/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "human_face_detect.hpp"
#include "app_events.h"
#include "app_face.h"

#define ALIGN_UP(n, a) (((n) + ((a) - 1)) & ~((a) - 1))
#define BUF_ALIGN      128

static const char *TAG = "app_face";

static HumanFaceDetect *s_detect;
static TaskHandle_t s_task;
static uint8_t *s_input;
static size_t s_input_size;
static uint16_t s_width;
static uint16_t s_height;
static volatile bool s_busy = true;     /* true until the model is loaded */

static portMUX_TYPE s_result_lock = portMUX_INITIALIZER_UNLOCKED;
static app_face_result_t s_result;

static void face_task(void *arg)
{
    /* Load the model here so the (slow) load does not block app_main */
    s_detect = new HumanFaceDetect(static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL), false);
    ESP_LOGI(TAG, "Face model loaded, input %ux%u", s_width, s_height);
    s_busy = false;

    const float min_score = CONFIG_APP_FACE_SCORE_PCT / 100.0f;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        dl::image::img_t img = {
            .data = s_input,
            .width = s_width,
            .height = s_height,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
        int64_t t0 = esp_timer_get_time();
        std::list<dl::detect::result_t> &results = s_detect->run(img);
        int64_t t1 = esp_timer_get_time();

        app_face_result_t res = {};
        res.img_w = s_width;
        res.img_h = s_height;
        res.infer_ms = (uint16_t)((t1 - t0) / 1000);
        for (const auto &r : results) {
            if (r.score < min_score || res.count >= APP_FACE_MAX_BOXES) {
                continue;
            }
            app_face_box_t &b = res.box[res.count++];
            b.x = (int16_t)r.box[0];
            b.y = (int16_t)r.box[1];
            b.w = (int16_t)(r.box[2] - r.box[0]);
            b.h = (int16_t)(r.box[3] - r.box[1]);
            b.score = (uint8_t)(r.score * 100.0f);
        }

        portENTER_CRITICAL(&s_result_lock);
        s_result = res;
        portEXIT_CRITICAL(&s_result_lock);

        s_busy = false;
        app_post_event(APP_EV_FACE, res.count);
    }
}

extern "C" esp_err_t app_face_init(uint16_t width, uint16_t height)
{
    s_width = width;
    s_height = height;
    s_input_size = ALIGN_UP((size_t)width * height * 2, BUF_ALIGN);
    s_input = (uint8_t *)heap_caps_aligned_calloc(BUF_ALIGN, 1, s_input_size, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_input, ESP_ERR_NO_MEM, TAG, "input buffer alloc failed");

    BaseType_t ok = xTaskCreatePinnedToCore(face_task, "face_det", 16 * 1024, NULL, 3, &s_task, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

extern "C" uint8_t *app_face_acquire_input(size_t *size)
{
    if (s_busy) {
        return NULL;
    }
    s_busy = true;
    *size = s_input_size;
    return s_input;
}

extern "C" void app_face_submit(void)
{
    xTaskNotifyGive(s_task);
}

extern "C" void app_face_cancel(void)
{
    s_busy = false;
}

extern "C" void app_face_get_result(app_face_result_t *out)
{
    portENTER_CRITICAL(&s_result_lock);
    *out = s_result;
    portEXIT_CRITICAL(&s_result_lock);
}
