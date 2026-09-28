/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * Face detection (esp-dl human_face_detect) running in its own task on a
 * down-scaled RGB565 copy of the camera frame.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_FACE_MAX_BOXES 4

typedef struct {
    int16_t x;      /* in detector image coordinates */
    int16_t y;
    int16_t w;
    int16_t h;
    uint8_t score;  /* 0..100 */
} app_face_box_t;

typedef struct {
    uint8_t count;
    app_face_box_t box[APP_FACE_MAX_BOXES];
    uint16_t img_w;
    uint16_t img_h;
    uint16_t infer_ms;
} app_face_result_t;

esp_err_t app_face_init(uint16_t width, uint16_t height);

/* Returns the (cache aligned) RGB565 input buffer if the detector is idle, else NULL */
uint8_t *app_face_acquire_input(size_t *size);

/* Hand the filled input buffer to the detector */
void app_face_submit(void);

/* Give the buffer back without running the detector (e.g. conversion failed) */
void app_face_cancel(void);

/* Copy of the latest result */
void app_face_get_result(app_face_result_t *out);

#ifdef __cplusplus
}
#endif
