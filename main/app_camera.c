/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "bsp/esp-bsp.h"

#include "app_face.h"
#include "app_recorder.h"
#include "app_ui.h"
#include "app_camera.h"

#define CAM_BUF_COUNT       3
#define PREVIEW_DIVIDER     2       /* update the LCD every Nth frame */
#define DETECT_DIVIDER      4       /* detector input = camera size / 4 */

static const char *TAG = "app_camera";

static int s_fd = -1;
static uint8_t *s_buf[CAM_BUF_COUNT];
static uint32_t s_width;
static uint32_t s_height;
static uint32_t s_fps;
static bool s_limited_range;
static ppa_client_handle_t s_ppa;

static void __attribute__((unused)) set_ctrl(uint32_t id, int32_t value, const char *name)
{
    struct v4l2_ext_control ctrl = {
        .id = id,
        .value = value,
    };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count = 1,
        .controls = &ctrl,
    };
    if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG, "Sensor does not support %s", name);
    }
}

/* YUV420 camera frame -> scaled RGB565 picture */
static esp_err_t convert_frame(const uint8_t *yuv, uint8_t *out, size_t out_size,
                               uint32_t out_w, uint32_t out_h, float scale)
{
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = yuv,
            .pic_w = s_width,
            .pic_h = s_height,
            .block_w = s_width,
            .block_h = s_height,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = s_limited_range ? PPA_COLOR_RANGE_LIMIT : PPA_COLOR_RANGE_FULL,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = out,
            .buffer_size = out_size,
            .pic_w = out_w,
            .pic_h = out_h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale,
        .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

static void process_frame(const uint8_t *yuv, size_t len, int64_t ts_us, uint32_t frame_no)
{
    /* 1. LCD preview (also JPEG encoded into the clip's playback file while recording).
     *    Skipped while the video player owns the screen. */
    if (frame_no % PREVIEW_DIVIDER == 0 && app_ui_is_live()) {
        size_t size;
        uint8_t *dst = app_ui_preview_acquire(&size);
        if (convert_frame(yuv, dst, size, APP_UI_PREVIEW_W, APP_UI_PREVIEW_H,
                          (float)APP_UI_PREVIEW_W / s_width) == ESP_OK) {
            app_recorder_push_preview_frame(dst, APP_UI_PREVIEW_W, APP_UI_PREVIEW_H, ts_us);
            app_ui_preview_commit();
        }
    }

    /* 2. Face detector (only when it finished the previous frame) */
    size_t det_size;
    uint8_t *det = app_face_acquire_input(&det_size);
    if (det) {
        if (convert_frame(yuv, det, det_size, s_width / DETECT_DIVIDER, s_height / DETECT_DIVIDER,
                          1.0f / DETECT_DIVIDER) == ESP_OK) {
            app_face_submit();
        } else {
            app_face_cancel();
        }
    }

    /* 3. Recorder (does nothing unless a clip is being recorded) */
    app_recorder_push_video_frame(yuv, len, ts_us);
}

static void camera_task(void *arg)
{
    uint32_t frame_no = 0;
    int64_t fps_t0 = esp_timer_get_time();
    uint32_t fps_frames = 0;

    while (true) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t ts_us = esp_timer_get_time();

        if (buf.flags & V4L2_BUF_FLAG_DONE) {
            process_frame(s_buf[buf.index], buf.bytesused, ts_us, frame_no++);
            fps_frames++;
        }

        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF failed");
        }

        if (ts_us - fps_t0 >= 10 * 1000000LL) {
            ESP_LOGI(TAG, "Camera %.1f fps", fps_frames * 1e6 / (double)(ts_us - fps_t0));
            fps_t0 = ts_us;
            fps_frames = 0;
        }
    }
}

esp_err_t app_camera_init(uint32_t *width, uint32_t *height, uint32_t *fps)
{
    ESP_RETURN_ON_ERROR(bsp_camera_start(NULL), TAG, "camera (esp_video) init failed");

    s_fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    ESP_RETURN_ON_FALSE(s_fd >= 0, ESP_FAIL, TAG, "cannot open %s", BSP_CAMERA_DEVICE);

#if CONFIG_APP_CAMERA_HFLIP
    set_ctrl(V4L2_CID_HFLIP, 1, "horizontal mirror");
#endif
#if CONFIG_APP_CAMERA_VFLIP
    set_ctrl(V4L2_CID_VFLIP, 1, "vertical flip");
#endif

    /* Keep the sensor's native resolution (selected in menuconfig, default 1280x720) */
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_G_FMT, &fmt) == 0, ESP_FAIL, TAG, "G_FMT failed");
    s_width = fmt.fmt.pix.width;
    s_height = fmt.fmt.pix.height;

    /* YUV420 (O_UYY_E_VYY) is what the JPEG engine and the PPA consume directly.
     * Full range BT.601 is what JPEG (JFIF) players expect. */
    struct v4l2_format yuv_fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = s_width,
        .fmt.pix.height = s_height,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420,
        .fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE,
        .fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601,
    };
    s_limited_range = false;
    if (ioctl(s_fd, VIDIOC_S_FMT, &yuv_fmt) != 0) {
        ESP_LOGW(TAG, "Full range BT.601 YUV not accepted (errno %d), using the sensor default", errno);
        yuv_fmt.fmt.pix.quantization = V4L2_QUANTIZATION_DEFAULT;
        yuv_fmt.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
        s_limited_range = false;
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_S_FMT, &yuv_fmt) == 0, ESP_FAIL, TAG, "S_FMT YUV420 failed (errno %d)", errno);
    }

    struct v4l2_streamparm parm = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    s_fps = 25;
    if (ioctl(s_fd, VIDIOC_G_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator) {
        s_fps = parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator;
    }

    struct v4l2_requestbuffers req = {
        .count = CAM_BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS failed");

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF failed");
        s_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(s_buf[i] && s_buf[i] != MAP_FAILED, ESP_FAIL, TAG, "mmap failed");
        ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF failed");
    }

    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_ppa), TAG, "PPA client register failed");

    ESP_LOGI(TAG, "Camera %" PRIu32 "x%" PRIu32 " @ %" PRIu32 " fps, YUV420 %s range",
             s_width, s_height, s_fps, s_limited_range ? "limited" : "full");
    *width = s_width;
    *height = s_height;
    *fps = s_fps;
    return ESP_OK;
}

esp_err_t app_camera_start(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(s_fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON failed");

    BaseType_t ok = xTaskCreatePinnedToCore(camera_task, "camera", 6144, NULL, 6, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}
