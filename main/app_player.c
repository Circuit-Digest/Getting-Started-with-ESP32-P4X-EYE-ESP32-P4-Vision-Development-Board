/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/param.h>
#include <errno.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/jpeg_decode.h"
#include "bsp/esp-bsp.h"

#include "app_preview_file.h"
#include "app_ui.h"
#include "app_player.h"

#define JPEG_IN_BUF_SIZE        (64 * 1024)
#define MAX_FRAME_LEN           JPEG_IN_BUF_SIZE
#define DELETE_CONFIRM_MS       3000
#define READ_CHUNK              4096
#define CLIP_DIR                BSP_SD_MOUNT_POINT "/" CONFIG_APP_RECORD_DIR

static const char *TAG = "app_player";

typedef enum {
    CMD_OPEN,
    CMD_CLOSE,
    CMD_TOGGLE,
    CMD_STEP,
    CMD_DELETE,
} player_cmd_type_t;

typedef struct {
    player_cmd_type_t type;
    int arg;
} player_cmd_t;

typedef struct {
    uint32_t offset;
    uint32_t pts_ms;
    uint32_t len;
} frame_entry_t;

static QueueHandle_t s_cmd_queue;
static SemaphoreHandle_t s_close_done;

/* Player task state */
static jpeg_decoder_handle_t s_jpeg;
static uint8_t *s_jpeg_in;
static uint8_t *s_read_chunk;   /* internal DMA-capable bounce buffer for SD reads */
static size_t s_jpeg_in_size;

static int *s_clips;            /* clip numbers, ascending */
static int s_clip_count;
static int s_clip_idx;

static FILE *s_fp;
static frame_entry_t *s_frames;
static int s_frame_count;
static int s_frame_cap;
static int s_cur_frame;
static int64_t s_base_us;       /* esp_timer time that corresponds to pts 0 */

static app_ui_player_info_t s_info;
static int64_t s_delete_armed_until_us;

static void publish_info(void)
{
    s_info.index = s_clip_idx;
    s_info.count = s_clip_count;
    s_info.delete_armed = s_delete_armed_until_us > esp_timer_get_time();
    app_ui_set_player_info(&s_info);
}

static void show_black(void)
{
    size_t size;
    uint8_t *buf = app_ui_player_acquire(&size);
    memset(buf, 0, size);
    app_ui_player_commit();
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void scan_clips(void)
{
    free(s_clips);
    s_clips = NULL;
    s_clip_count = 0;

    DIR *dir = opendir(CLIP_DIR);
    if (!dir) {
        return;
    }
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        int num;
        char ext[8] = {0};
        if (sscanf(e->d_name, APP_CLIP_PREFIX "%d.%7s", &num, ext) == 2 && strcasecmp(ext, "mp4") == 0) {
            if (s_clip_count == cap) {
                cap = cap ? cap * 2 : 32;
                int *n = realloc(s_clips, cap * sizeof(int));
                if (!n) {
                    break;
                }
                s_clips = n;
            }
            s_clips[s_clip_count++] = num;
        }
    }
    closedir(dir);
    qsort(s_clips, s_clip_count, sizeof(int), cmp_int);
}

static void close_clip(void)
{
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }
    s_frame_count = 0;
    s_cur_frame = 0;
}

static bool show_frame(int i)
{
    const frame_entry_t *f = &s_frames[i];
    /* Read through an internal RAM buffer: the SD card never DMAs straight into PSRAM */
    bool ok = fseek(s_fp, f->offset, SEEK_SET) == 0;
    for (uint32_t done = 0; ok && done < f->len; ) {
        size_t n = MIN(f->len - done, READ_CHUNK);
        ok = fread(s_read_chunk, 1, n, s_fp) == n;
        if (ok) {
            memcpy(s_jpeg_in + done, s_read_chunk, n);
            done += n;
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "Frame %d read failed (offset %" PRIu32 ", len %" PRIu32 ", errno %d)", i, f->offset, f->len, errno);
        clearerr(s_fp);
        return false;
    }
    size_t out_size;
    uint8_t *out = app_ui_player_acquire(&out_size);
    const jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded = 0;
    if (jpeg_decoder_process(s_jpeg, &cfg, s_jpeg_in, f->len, out, out_size, &decoded) != ESP_OK) {
        ESP_LOGW(TAG, "Frame %d decode failed", i);
        return false;
    }
    app_ui_player_commit();
    s_info.pos_ms = f->pts_ms;
    return true;
}

/* Build the frame index of the current clip's preview file */
static bool load_preview_index(const char *path)
{
    s_fp = fopen(path, "rb");
    if (!s_fp) {
        return false;
    }
    app_prv_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, s_fp) != 1 || hdr.magic != APP_PRV_MAGIC) {
        return false;
    }

    uint32_t offset = sizeof(hdr);
    app_prv_frame_t fh;
    while (fseek(s_fp, offset, SEEK_SET) == 0 && fread(&fh, sizeof(fh), 1, s_fp) == 1) {
        if (fh.len == 0 || fh.len > MAX_FRAME_LEN) {
            break;
        }
        if (s_frame_count == s_frame_cap) {
            int cap = s_frame_cap ? s_frame_cap * 2 : 128;
            frame_entry_t *n = realloc(s_frames, cap * sizeof(frame_entry_t));
            if (!n) {
                break;
            }
            s_frames = n;
            s_frame_cap = cap;
        }
        s_frames[s_frame_count++] = (frame_entry_t) {
            .offset = offset + sizeof(fh),
            .pts_ms = fh.pts_ms,
            .len = fh.len,
        };
        offset += sizeof(fh) + APP_PRV_PAD4(fh.len);
    }
    /* A truncated last frame (power loss while recording) is dropped by the fread check in show_frame */
    return s_frame_count > 0;
}

static void start_playing_from(int frame)
{
    s_cur_frame = frame;
    s_base_us = esp_timer_get_time() - (int64_t)s_frames[frame].pts_ms * 1000;
    s_info.state = APP_UI_PLAYER_PLAYING;
}

static void load_clip(void)
{
    close_clip();
    memset(&s_info, 0, sizeof(s_info));

    if (s_clip_count == 0) {
        s_info.state = APP_UI_PLAYER_EMPTY;
        show_black();
        publish_info();
        return;
    }

    int num = s_clips[s_clip_idx];
    snprintf(s_info.name, sizeof(s_info.name), APP_CLIP_PREFIX "%05d" APP_CLIP_EXT, num);

    char path[96];
    snprintf(path, sizeof(path), CLIP_DIR "/" APP_CLIP_PREFIX "%05d" APP_PRV_EXT, num);
    if (!load_preview_index(path)) {
        close_clip();
        s_info.state = APP_UI_PLAYER_NO_PREVIEW;
        show_black();
        publish_info();
        return;
    }

    s_info.duration_ms = s_frames[s_frame_count - 1].pts_ms;
    start_playing_from(0);      /* autoplay: the task loop shows frame 0 right away */
    publish_info();
}

static void delete_current(void)
{
    if (s_clip_count == 0) {
        return;
    }
    int num = s_clips[s_clip_idx];
    char path[96];

    close_clip();
    snprintf(path, sizeof(path), CLIP_DIR "/" APP_CLIP_PREFIX "%05d" APP_CLIP_EXT, num);
    int r1 = unlink(path);
    snprintf(path, sizeof(path), CLIP_DIR "/" APP_CLIP_PREFIX "%05d" APP_PRV_EXT, num);
    unlink(path);   /* may not exist */
    ESP_LOGI(TAG, "Deleted clip %d (%s)", num, r1 == 0 ? "ok" : "mp4 not found");

    memmove(&s_clips[s_clip_idx], &s_clips[s_clip_idx + 1], (s_clip_count - s_clip_idx - 1) * sizeof(int));
    s_clip_count--;
    if (s_clip_idx >= s_clip_count) {
        s_clip_idx = s_clip_count > 0 ? s_clip_count - 1 : 0;
    }
    load_clip();
}

static void handle_cmd(const player_cmd_t *cmd, bool *open)
{
    bool was_armed = s_delete_armed_until_us > esp_timer_get_time();
    if (cmd->type != CMD_DELETE) {
        s_delete_armed_until_us = 0;
    }

    switch (cmd->type) {
    case CMD_OPEN:
        *open = true;
        scan_clips();
        s_clip_idx = s_clip_count > 0 ? s_clip_count - 1 : 0;   /* newest first */
        load_clip();
        break;

    case CMD_CLOSE:
        *open = false;
        close_clip();
        xSemaphoreGive(s_close_done);
        break;

    case CMD_TOGGLE:
        if (!*open || s_frame_count == 0) {
            break;
        }
        if (s_info.state == APP_UI_PLAYER_PLAYING) {
            s_info.state = APP_UI_PLAYER_PAUSED;
        } else if (s_cur_frame >= s_frame_count - 1) {
            start_playing_from(0);  /* at the end: replay from the start */
        } else {
            start_playing_from(s_cur_frame);
        }
        publish_info();
        break;

    case CMD_STEP:
        if (!*open || s_clip_count == 0) {
            break;
        }
        s_clip_idx = (s_clip_idx + cmd->arg + s_clip_count) % s_clip_count;
        load_clip();
        break;

    case CMD_DELETE:
        if (!*open || s_clip_count == 0) {
            break;
        }
        if (was_armed) {
            s_delete_armed_until_us = 0;
            delete_current();
        } else {
            s_delete_armed_until_us = esp_timer_get_time() + DELETE_CONFIRM_MS * 1000LL;
            if (s_info.state == APP_UI_PLAYER_PLAYING) {
                s_info.state = APP_UI_PLAYER_PAUSED;
            }
            publish_info();
        }
        break;
    }
}

static void player_task(void *arg)
{
    bool open = false;
    bool armed_shown = false;

    while (true) {
        TickType_t wait = portMAX_DELAY;
        int64_t now = esp_timer_get_time();

        if (open && s_info.state == APP_UI_PLAYER_PLAYING && s_fp) {
            int64_t due = s_base_us + (int64_t)s_frames[s_cur_frame].pts_ms * 1000;
            wait = due > now ? pdMS_TO_TICKS((due - now) / 1000) : 0;
        }
        if (armed_shown) {
            wait = MIN(wait, pdMS_TO_TICKS(100));
        }

        player_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, wait) == pdTRUE) {
            handle_cmd(&cmd, &open);
        } else if (open && s_info.state == APP_UI_PLAYER_PLAYING && s_fp) {
            if (!show_frame(s_cur_frame) || ++s_cur_frame >= s_frame_count) {
                s_cur_frame = s_frame_count - 1;
                s_info.pos_ms = s_info.duration_ms;
                s_info.state = APP_UI_PLAYER_PAUSED;
            }
            publish_info();
        }

        /* Delete confirmation timed out */
        bool armed = s_delete_armed_until_us > esp_timer_get_time();
        if (armed_shown && !armed && open) {
            s_delete_armed_until_us = 0;
            publish_info();
        }
        armed_shown = armed;
    }
}

static void send_cmd(player_cmd_type_t type, int arg)
{
    player_cmd_t cmd = {
        .type = type,
        .arg = arg,
    };
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

esp_err_t app_player_init(void)
{
    s_cmd_queue = xQueueCreate(8, sizeof(player_cmd_t));
    s_close_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_cmd_queue && s_close_done, ESP_ERR_NO_MEM, TAG, "no mem");

    const jpeg_decode_engine_cfg_t eng_cfg = {
        .timeout_ms = 100,
    };
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&eng_cfg, &s_jpeg), TAG, "JPEG decoder init failed");
    const jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    s_jpeg_in = jpeg_alloc_decoder_mem(JPEG_IN_BUF_SIZE, &mem_cfg, &s_jpeg_in_size);
    ESP_RETURN_ON_FALSE(s_jpeg_in, ESP_ERR_NO_MEM, TAG, "JPEG input buffer alloc failed");
    s_read_chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(s_read_chunk, ESP_ERR_NO_MEM, TAG, "read buffer alloc failed");

    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "player", 4096, NULL, 4, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

void app_player_open(void)
{
    send_cmd(CMD_OPEN, 0);
}

void app_player_close(void)
{
    xSemaphoreTake(s_close_done, 0);
    send_cmd(CMD_CLOSE, 0);
    xSemaphoreTake(s_close_done, pdMS_TO_TICKS(2000));
}

void app_player_toggle_pause(void)
{
    send_cmd(CMD_TOGGLE, 0);
}

void app_player_step(int direction)
{
    send_cmd(CMD_STEP, direction);
}

void app_player_delete(void)
{
    send_cmd(CMD_DELETE, 0);
}
