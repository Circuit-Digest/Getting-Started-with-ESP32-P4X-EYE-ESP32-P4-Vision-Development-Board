/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * Clip recorder: camera YUV420 frames -> hardware JPEG (MJPEG), PDM microphone -> AAC,
 * both muxed into an MP4 file on the microSD card by a dedicated writer task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_recorder_init(uint32_t width, uint32_t height, uint32_t fps);

/* Start a clip of duration_ms. Errors: ESP_ERR_INVALID_STATE (busy), ESP_ERR_NOT_FOUND (no microSD) */
esp_err_t app_recorder_start(uint32_t duration_ms);

/* Request an early stop (asynchronous, APP_EV_REC_DONE is posted when the file is closed) */
void app_recorder_stop(void);

/* Stop and wait until the file is closed (used before the card is unmounted) */
void app_recorder_stop_sync(void);

bool app_recorder_is_busy(void);

/* Progress of the current clip */
void app_recorder_get_progress(uint32_t *elapsed_ms, uint32_t *duration_ms);

/* Name (without folder) of the last clip that was started */
const char *app_recorder_last_file(void);

/* Microphone level 0..100 (updated continuously, also when not recording) */
int app_recorder_mic_level(void);

/* Feed one camera frame (O_UYY_E_VYY YUV420, width x height). Called from the camera task. */
void app_recorder_push_video_frame(const uint8_t *yuv420, size_t len, int64_t timestamp_us);

/* Feed one LCD preview frame (RGB565). JPEG encoded into the REC_xxxxx.prv playback file. */
void app_recorder_push_preview_frame(const uint8_t *rgb565, uint16_t width, uint16_t height, int64_t timestamp_us);

#ifdef __cplusplus
}
#endif
