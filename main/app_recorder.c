/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"

#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "esp_aac_enc.h"
#include "esp_muxer.h"
#include "mp4_muxer.h"
#include "driver/jpeg_encode.h"

#include "app_events.h"
#include "app_storage.h"
#include "app_recorder.h"
#include "app_preview_file.h"

#define AUDIO_SAMPLE_RATE       16000   /* PDM microphone rate set by the BSP */
#define AUDIO_CHANNELS          1
#define AUDIO_BITS              16
#define AAC_FRAME_SAMPLES       1024

#define PKT_QUEUE_LEN           96
#define START_TIMEOUT_MS        3000
#define STOP_DRAIN_TIMEOUT_MS   1000
#define MUXER_CACHE_SIZE        (32 * 1024)
#define MAX_WRITE_ERRORS        5
#define PREVIEW_JPEG_QUALITY    70
#define PREVIEW_JPEG_BUF_SIZE   (64 * 1024)

static const char *TAG = "app_recorder";

typedef enum {
    MSG_START,
    MSG_VIDEO,
    MSG_AUDIO,
    MSG_PREVIEW,
} msg_type_t;

typedef struct {
    msg_type_t type;
    uint32_t session;
    uint8_t *data;
    uint32_t len;
    uint32_t pts;
    bool key_frame;
} rec_msg_t;

static struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;

    QueueHandle_t queue;
    SemaphoreHandle_t start_done;
    esp_timer_handle_t stop_timer;

    /* Session state shared between tasks */
    volatile bool busy;             /* from start() until the file is closed */
    volatile bool active;           /* producers may generate packets */
    volatile bool stop_req;
    volatile uint32_t session;
    volatile int in_flight;         /* producers currently encoding a packet */
    int64_t start_us;
    uint32_t duration_ms;
    esp_err_t start_result;
    char path[128];
    char last_name[128];

    /* Writer task only */
    esp_muxer_handle_t muxer;
    int video_idx;
    int audio_idx;
    int write_errors;
    uint32_t video_frames;
    FILE *prv_fp;
    uint32_t prv_frames;
    uint32_t prv_last_pts;
    uint16_t prv_width;
    uint16_t prv_height;

    /* Camera task only */
    esp_h264_enc_handle_t enc;
    uint32_t enc_session;
    uint8_t *enc_out;
    uint32_t enc_out_size;
    uint32_t last_video_pts;
    jpeg_encoder_handle_t jpeg;
    uint8_t *jpeg_out;
    size_t jpeg_out_size;

    /* Audio task only */
    esp_codec_dev_handle_t mic;
    void *aac;
    int aac_in_size;
    int aac_out_size;
    uint32_t audio_session;
    uint64_t audio_samples;

    volatile int mic_level;
} s_rec;

/* Producer guard: the writer only closes the file once no producer is in the middle of a packet */
static bool producer_enter(uint32_t *session)
{
    __atomic_add_fetch(&s_rec.in_flight, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&s_rec.active, __ATOMIC_SEQ_CST)) {
        __atomic_sub_fetch(&s_rec.in_flight, 1, __ATOMIC_SEQ_CST);
        return false;
    }
    *session = s_rec.session;
    return true;
}

static void producer_exit(void)
{
    __atomic_sub_fetch(&s_rec.in_flight, 1, __ATOMIC_SEQ_CST);
}

static void queue_packet(msg_type_t type, uint32_t session, const uint8_t *data, uint32_t len,
                         uint32_t pts, bool key_frame)
{
    rec_msg_t msg = {
        .type = type,
        .session = session,
        .len = len,
        .pts = pts,
        .key_frame = key_frame,
    };
    msg.data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!msg.data) {
        ESP_LOGW(TAG, "No memory for packet type %d (%" PRIu32 " B), dropped", type, len);
        return;
    }
    memcpy(msg.data, data, len);
    if (xQueueSend(s_rec.queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Writer queue full (storage too slow?), packet type %d dropped", type);
        heap_caps_free(msg.data);
    }
}

/* Video: hardware H.264 encoder (runs in the camera task) */
static void video_encoder_close(void)
{
    if (s_rec.enc) {
        esp_h264_enc_close(s_rec.enc);
        esp_h264_enc_del(s_rec.enc);
        s_rec.enc = NULL;
    }
}

static esp_err_t video_encoder_open(void)
{
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = s_rec.fps,
        .fps = s_rec.fps,
        .res = {
            .width = s_rec.width,
            .height = s_rec.height,
        },
        .rc = {
            .bitrate = CONFIG_APP_VIDEO_BITRATE_KBPS * 1000,
            .qp_min = 20,
            .qp_max = 40,
        },
    };
    if (esp_h264_enc_hw_new(&cfg, &s_rec.enc) != ESP_H264_ERR_OK) {
        s_rec.enc = NULL;
        return ESP_FAIL;
    }
    if (esp_h264_enc_open(s_rec.enc) != ESP_H264_ERR_OK) {
        esp_h264_enc_del(s_rec.enc);
        s_rec.enc = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_recorder_push_video_frame(const uint8_t *yuv420, size_t len, int64_t timestamp_us)
{
    uint32_t session;

    if (!producer_enter(&session)) {
        return;
    }

    /* The encoder is created at boot, while internal RAM still has the ~92 KB contiguous block its
     * reference frame needs. A new clip recreates it back-to-back (same task, the allocator hands
     * the freed blocks straight back) so the clip starts with an IDR frame + SPS/PPS. */
    if (s_rec.enc_session != session) {
        video_encoder_close();
        if (video_encoder_open() != ESP_OK) {
            ESP_LOGE(TAG, "H.264 encoder re-open failed (internal RAM free %u, largest block %u)",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            s_rec.enc_session = session;    /* don't retry every frame */
            producer_exit();
            return;
        }
        s_rec.enc_session = session;
        s_rec.last_video_pts = 0;
    }
    if (!s_rec.enc) {
        producer_exit();
        return;
    }

    int64_t rel_us = timestamp_us - s_rec.start_us;
    uint32_t pts = rel_us > 0 ? (uint32_t)(rel_us / 1000) : 0;
    if (s_rec.last_video_pts && pts <= s_rec.last_video_pts) {
        pts = s_rec.last_video_pts + 1;
    }

    esp_h264_enc_in_frame_t in_frame = {
        .raw_data = {
            .buffer = (uint8_t *)yuv420,
            .len = len,
        },
        .pts = pts,
    };
    esp_h264_enc_out_frame_t out_frame = {
        .raw_data = {
            .buffer = s_rec.enc_out,
            .len = s_rec.enc_out_size,
        },
    };
    esp_h264_err_t err = esp_h264_enc_process(s_rec.enc, &in_frame, &out_frame);
    if (err == ESP_H264_ERR_OK && out_frame.length > 0) {
        bool key = out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR || out_frame.frame_type == ESP_H264_FRAME_TYPE_I;
        queue_packet(MSG_VIDEO, session, out_frame.raw_data.buffer, out_frame.length, pts, key);
        s_rec.last_video_pts = pts;
    } else if (err != ESP_H264_ERR_OK) {
        ESP_LOGW(TAG, "H.264 encode failed (%d)", err);
    }
    producer_exit();
}

void app_recorder_push_preview_frame(const uint8_t *rgb565, uint16_t width, uint16_t height, int64_t timestamp_us)
{
    uint32_t session;

    if (!s_rec.jpeg || !producer_enter(&session)) {
        return;
    }
    const jpeg_encode_cfg_t cfg = {
        .width = width,
        .height = height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = PREVIEW_JPEG_QUALITY,
    };
    uint32_t jpeg_len = 0;
    if (jpeg_encoder_process(s_rec.jpeg, &cfg, rgb565, (uint32_t)width * height * 2,
                             s_rec.jpeg_out, s_rec.jpeg_out_size, &jpeg_len) == ESP_OK && jpeg_len > 0) {
        int64_t rel_us = timestamp_us - s_rec.start_us;
        uint32_t pts = rel_us > 0 ? (uint32_t)(rel_us / 1000) : 0;
        s_rec.prv_width = width;
        s_rec.prv_height = height;
        queue_packet(MSG_PREVIEW, session, s_rec.jpeg_out, jpeg_len, pts, false);
    }
    producer_exit();
}

/* Audio: PDM microphone -> gain -> AAC (own task, runs continuously for the level meter) */
static void audio_process_pcm(int16_t *pcm, int samples)
{
    int peak = 0;
    for (int i = 0; i < samples; i++) {
        int32_t v = (int32_t)pcm[i] * CONFIG_APP_MIC_GAIN;
        if (v > INT16_MAX) {
            v = INT16_MAX;
        } else if (v < INT16_MIN) {
            v = INT16_MIN;
        }
        pcm[i] = (int16_t)v;
        int a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    /* Map -60 dBFS .. 0 dBFS to 0 .. 100 */
    int level = 0;
    if (peak > 0) {
        float db = 20.0f * log10f((float)peak / 32768.0f);
        level = (int)((db + 60.0f) * 100.0f / 60.0f);
        level = level < 0 ? 0 : (level > 100 ? 100 : level);
    }
    s_rec.mic_level = level;
}

static void audio_task(void *arg)
{
    int16_t *pcm = heap_caps_malloc(s_rec.aac_in_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *aac_buf = heap_caps_malloc(s_rec.aac_out_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm || !aac_buf) {
        ESP_LOGE(TAG, "Audio buffers alloc failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (esp_codec_dev_read(s_rec.mic, pcm, s_rec.aac_in_size) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        audio_process_pcm(pcm, s_rec.aac_in_size / sizeof(int16_t));

        uint32_t session;
        if (!producer_enter(&session)) {
            continue;
        }
        if (session != s_rec.audio_session) {
            s_rec.audio_session = session;
            s_rec.audio_samples = 0;
            esp_aac_enc_reset(s_rec.aac);
        }
        esp_audio_enc_in_frame_t in_frame = {
            .buffer = (uint8_t *)pcm,
            .len = s_rec.aac_in_size,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = aac_buf,
            .len = s_rec.aac_out_size,
        };
        if (esp_aac_enc_process(s_rec.aac, &in_frame, &out_frame) == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
            uint32_t pts = (uint32_t)(s_rec.audio_samples * 1000 / AUDIO_SAMPLE_RATE);
            queue_packet(MSG_AUDIO, session, aac_buf, out_frame.encoded_bytes, pts, true);
        }
        s_rec.audio_samples += AAC_FRAME_SAMPLES;
        producer_exit();
    }
}

static esp_err_t audio_init(void)
{
    s_rec.mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_rec.mic, ESP_FAIL, TAG, "Microphone init failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_rec.mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "Mic open failed");

    esp_aac_enc_config_t aac_cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
    aac_cfg.sample_rate = AUDIO_SAMPLE_RATE;
    aac_cfg.channel = AUDIO_CHANNELS;
    aac_cfg.bits_per_sample = AUDIO_BITS;
    aac_cfg.bitrate = CONFIG_APP_AUDIO_BITRATE;
    aac_cfg.adts_used = true;
    ESP_RETURN_ON_FALSE(esp_aac_enc_open(&aac_cfg, sizeof(aac_cfg), &s_rec.aac) == ESP_AUDIO_ERR_OK,
                        ESP_FAIL, TAG, "AAC encoder open failed");
    esp_aac_enc_get_frame_size(s_rec.aac, &s_rec.aac_in_size, &s_rec.aac_out_size);
    ESP_LOGI(TAG, "AAC encoder: in %d B, out %d B per frame", s_rec.aac_in_size, s_rec.aac_out_size);

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "rec_audio", 6144, NULL, 8, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio task create failed");
    return ESP_OK;
}

/* Writer task: owns the MP4 muxer, all file I/O happens here */
static int muxer_url_cb(esp_muxer_slice_info_t *info, void *ctx)
{
    snprintf(info->file_path, info->len, "%s", (const char *)ctx);
    return 0;
}

static void preview_file_open(void)
{
    char prv_path[sizeof(s_rec.path)];
    strlcpy(prv_path, s_rec.path, sizeof(prv_path));
    char *ext = strrchr(prv_path, '.');
    if (!ext) {
        return;
    }
    strcpy(ext, APP_PRV_EXT);

    s_rec.prv_frames = 0;
    s_rec.prv_last_pts = 0;
    s_rec.prv_fp = fopen(prv_path, "wb");
    if (!s_rec.prv_fp) {
        ESP_LOGW(TAG, "Cannot create preview file %s", prv_path);
        return;
    }
    app_prv_header_t hdr = {
        .magic = APP_PRV_MAGIC,
        .version = APP_PRV_VERSION,
    };
    fwrite(&hdr, sizeof(hdr), 1, s_rec.prv_fp);
}

static void preview_file_write(const rec_msg_t *msg)
{
    static const uint8_t pad[4] = {0};
    if (!s_rec.prv_fp || msg->session != s_rec.session) {
        return;
    }
    app_prv_frame_t frame = {
        .pts_ms = msg->pts,
        .len = msg->len,
    };
    if (fwrite(&frame, sizeof(frame), 1, s_rec.prv_fp) != 1 ||
            fwrite(msg->data, 1, msg->len, s_rec.prv_fp) != msg->len) {
        ESP_LOGW(TAG, "Preview write failed");
        fclose(s_rec.prv_fp);
        s_rec.prv_fp = NULL;
        return;
    }
    fwrite(pad, 1, APP_PRV_PAD4(msg->len) - msg->len, s_rec.prv_fp);
    s_rec.prv_frames++;
    s_rec.prv_last_pts = msg->pts;
}

static void preview_file_close(void)
{
    if (!s_rec.prv_fp) {
        return;
    }
    app_prv_header_t hdr = {
        .magic = APP_PRV_MAGIC,
        .version = APP_PRV_VERSION,
        .width = s_rec.prv_width,
        .height = s_rec.prv_height,
        .frame_count = s_rec.prv_frames,
        .duration_ms = s_rec.prv_last_pts,
    };
    if (fseek(s_rec.prv_fp, 0, SEEK_SET) == 0) {
        fwrite(&hdr, sizeof(hdr), 1, s_rec.prv_fp);
    }
    fclose(s_rec.prv_fp);
    s_rec.prv_fp = NULL;
    ESP_LOGI(TAG, "Playback preview: %" PRIu32 " frames %ux%u", s_rec.prv_frames, s_rec.prv_width, s_rec.prv_height);
}

static esp_err_t muxer_open(void)
{
    mp4_muxer_config_t cfg = {
        .base_config = {
            .muxer_type = ESP_MUXER_TYPE_MP4,
            .slice_duration = s_rec.duration_ms + 60000,    /* one file per clip */
            .url_pattern_ex = muxer_url_cb,
            .ctx = s_rec.path,
            .ram_cache_size = MUXER_CACHE_SIZE,
        },
        .display_in_order = true,
        .moov_before_mdat = false,
    };
    s_rec.muxer = esp_muxer_open(&cfg.base_config, sizeof(cfg));
    ESP_RETURN_ON_FALSE(s_rec.muxer, ESP_FAIL, TAG, "esp_muxer_open failed");

    esp_muxer_video_stream_info_t video = {
        .codec = ESP_MUXER_VDEC_H264,
        .width = s_rec.width,
        .height = s_rec.height,
        .fps = s_rec.fps,
        .min_packet_duration = 1000 / s_rec.fps,
    };
    esp_muxer_audio_stream_info_t audio = {
        .codec = ESP_MUXER_ADEC_AAC,
        .channel = AUDIO_CHANNELS,
        .bits_per_sample = AUDIO_BITS,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .min_packet_duration = AAC_FRAME_SAMPLES * 1000 / AUDIO_SAMPLE_RATE,
    };
    if (esp_muxer_add_video_stream(s_rec.muxer, &video, &s_rec.video_idx) != ESP_MUXER_ERR_OK ||
            esp_muxer_add_audio_stream(s_rec.muxer, &audio, &s_rec.audio_idx) != ESP_MUXER_ERR_OK) {
        ESP_LOGE(TAG, "Failed to add muxer streams");
        esp_muxer_close(s_rec.muxer);
        s_rec.muxer = NULL;
        return ESP_FAIL;
    }
    s_rec.write_errors = 0;
    s_rec.video_frames = 0;
    preview_file_open();
    return ESP_OK;
}

static void muxer_write(const rec_msg_t *msg)
{
    esp_muxer_err_t ret;

    if (msg->type == MSG_PREVIEW) {
        preview_file_write(msg);
        return;
    }
    if (!s_rec.muxer || msg->session != s_rec.session) {
        return;     /* late packet of an already closed clip */
    }
    if (msg->type == MSG_VIDEO) {
        esp_muxer_video_packet_t pkt = {
            .data = msg->data,
            .len = msg->len,
            .pts = msg->pts,
            .dts = msg->pts,
            .key_frame = msg->key_frame,
        };
        ret = esp_muxer_add_video_packet(s_rec.muxer, s_rec.video_idx, &pkt);
        if (ret == ESP_MUXER_ERR_OK) {
            s_rec.video_frames++;
        }
    } else {
        esp_muxer_audio_packet_t pkt = {
            .data = msg->data,
            .len = msg->len,
            .pts = msg->pts,
        };
        ret = esp_muxer_add_audio_packet(s_rec.muxer, s_rec.audio_idx, &pkt);
    }
    if (ret != ESP_MUXER_ERR_OK) {
        ESP_LOGW(TAG, "Muxer write error %d", ret);
        if (++s_rec.write_errors >= MAX_WRITE_ERRORS) {
            ESP_LOGE(TAG, "Too many write errors, stopping clip");
            app_recorder_stop();
        }
    }
}

static void writer_finish_clip(void)
{
    /* Wait for producers that are in the middle of a packet */
    int64_t t0 = esp_timer_get_time();
    while (__atomic_load_n(&s_rec.in_flight, __ATOMIC_SEQ_CST) > 0 &&
            esp_timer_get_time() - t0 < STOP_DRAIN_TIMEOUT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    /* Flush whatever is still queued */
    rec_msg_t msg;
    while (xQueueReceive(s_rec.queue, &msg, 0) == pdTRUE) {
        if (msg.type != MSG_START) {
            muxer_write(&msg);
            heap_caps_free(msg.data);
        }
    }

    esp_err_t result = s_rec.write_errors >= MAX_WRITE_ERRORS ? ESP_FAIL : ESP_OK;
    if (s_rec.muxer) {
        if (esp_muxer_close(s_rec.muxer) != ESP_MUXER_ERR_OK) {
            result = ESP_FAIL;
        }
        s_rec.muxer = NULL;
    }
    preview_file_close();
    if (s_rec.video_frames == 0) {
        result = ESP_FAIL;
    }
    ESP_LOGI(TAG, "Clip %s closed: %" PRIu32 " video frames, %s", s_rec.path, s_rec.video_frames,
             result == ESP_OK ? "OK" : "FAILED");

    s_rec.busy = false;
    app_post_event(APP_EV_REC_DONE, result);
}

static void writer_task(void *arg)
{
    rec_msg_t msg;

    while (true) {
        if (xQueueReceive(s_rec.queue, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (msg.type == MSG_START) {
                s_rec.start_result = muxer_open();
                xSemaphoreGive(s_rec.start_done);
            } else {
                muxer_write(&msg);
                heap_caps_free(msg.data);
            }
        }
        if (s_rec.busy && s_rec.stop_req && s_rec.muxer) {
            writer_finish_clip();
        }
    }
}

static void stop_timer_cb(void *arg)
{
    app_recorder_stop();
}

esp_err_t app_recorder_init(uint32_t width, uint32_t height, uint32_t fps)
{
    s_rec.width = width;
    s_rec.height = height;
    s_rec.fps = fps ? fps : 25;

    s_rec.queue = xQueueCreate(PKT_QUEUE_LEN, sizeof(rec_msg_t));
    s_rec.start_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_rec.queue && s_rec.start_done, ESP_ERR_NO_MEM, TAG, "no mem");

    /* Reserve the H.264 engine's internal RAM now (see app_recorder_push_video_frame) */
    ESP_RETURN_ON_ERROR(video_encoder_open(), TAG, "H.264 encoder create failed");
    s_rec.enc_session = 0;

    /* Encoded frame buffer: DMA target of the H.264 engine, must be cache aligned */
    s_rec.enc_out = esp_h264_aligned_calloc(128, 1, width * height, &s_rec.enc_out_size, ESP_H264_MEM_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rec.enc_out, ESP_ERR_NO_MEM, TAG, "H.264 out buffer alloc failed");

    const esp_timer_create_args_t timer_args = {
        .callback = stop_timer_cb,
        .name = "rec_stop",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_rec.stop_timer), TAG, "timer create failed");

    /* Hardware JPEG encoder for the playback preview (non fatal if unavailable) */
    const jpeg_encode_engine_cfg_t jpeg_cfg = {
        .timeout_ms = 100,
    };
    const jpeg_encode_memory_alloc_cfg_t jpeg_mem = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    s_rec.jpeg_out = jpeg_alloc_encoder_mem(PREVIEW_JPEG_BUF_SIZE, &jpeg_mem, &s_rec.jpeg_out_size);
    if (!s_rec.jpeg_out || jpeg_new_encoder_engine(&jpeg_cfg, &s_rec.jpeg) != ESP_OK) {
        ESP_LOGW(TAG, "JPEG encoder unavailable, clips will have no playback preview");
        s_rec.jpeg = NULL;
    }

    ESP_RETURN_ON_FALSE(mp4_muxer_register() == ESP_MUXER_ERR_OK, ESP_FAIL, TAG, "MP4 muxer register failed");

    BaseType_t ok = xTaskCreatePinnedToCore(writer_task, "rec_writer", 8192, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "writer task create failed");

    ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio init failed");

    ESP_LOGI(TAG, "Internal RAM: %u free, largest block %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Recorder ready: H.264 %" PRIu32 "x%" PRIu32 "@%" PRIu32 " %d kbps + AAC %d Hz mono",
             width, height, s_rec.fps, CONFIG_APP_VIDEO_BITRATE_KBPS, AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t app_recorder_start(uint32_t duration_ms)
{
    if (s_rec.busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = app_storage_next_clip_path(s_rec.path, sizeof(s_rec.path));
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : ret;
    }

    s_rec.busy = true;
    s_rec.stop_req = false;
    s_rec.duration_ms = duration_ms;
    s_rec.session++;
    xSemaphoreTake(s_rec.start_done, 0);

    rec_msg_t msg = { .type = MSG_START };
    if (xQueueSendToFront(s_rec.queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE ||
            xSemaphoreTake(s_rec.start_done, pdMS_TO_TICKS(START_TIMEOUT_MS)) != pdTRUE ||
            s_rec.start_result != ESP_OK) {
        ESP_LOGE(TAG, "Could not create %s", s_rec.path);
        s_rec.busy = false;
        return ESP_FAIL;
    }

    const char *name = strrchr(s_rec.path, '/');
    snprintf(s_rec.last_name, sizeof(s_rec.last_name), "%s", name ? name + 1 : s_rec.path);

    s_rec.start_us = esp_timer_get_time();
    __atomic_store_n(&s_rec.active, true, __ATOMIC_SEQ_CST);
    esp_timer_start_once(s_rec.stop_timer, (uint64_t)duration_ms * 1000);

    ESP_LOGI(TAG, "Recording %" PRIu32 " ms to %s", duration_ms, s_rec.path);
    return ESP_OK;
}

void app_recorder_stop(void)
{
    if (!s_rec.busy || s_rec.stop_req) {
        return;
    }
    esp_timer_stop(s_rec.stop_timer);
    __atomic_store_n(&s_rec.active, false, __ATOMIC_SEQ_CST);
    s_rec.stop_req = true;
}

void app_recorder_stop_sync(void)
{
    app_recorder_stop();
    for (int i = 0; i < 300 && s_rec.busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool app_recorder_is_busy(void)
{
    return s_rec.busy;
}

void app_recorder_get_progress(uint32_t *elapsed_ms, uint32_t *duration_ms)
{
    uint32_t elapsed = 0;
    if (s_rec.busy && s_rec.active) {
        elapsed = (uint32_t)((esp_timer_get_time() - s_rec.start_us) / 1000);
        if (elapsed > s_rec.duration_ms) {
            elapsed = s_rec.duration_ms;
        }
    } else if (s_rec.busy) {
        elapsed = s_rec.duration_ms;
    }
    *elapsed_ms = elapsed;
    *duration_ms = s_rec.duration_ms;
}

const char *app_recorder_last_file(void)
{
    return s_rec.last_name;
}

int app_recorder_mic_level(void)
{
    return s_rec.mic_level;
}
