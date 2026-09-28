/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * OV2710 MIPI-CSI camera capture (esp_video / V4L2, YUV420 from the ISP) and frame fan-out:
 *   - PPA scale + YUV->RGB565 for the LCD preview
 *   - PPA scale + YUV->RGB565 for the face detector
 *   - raw YUV420 to the MJPEG recorder
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the camera and reports the capture format (call before app_camera_start) */
esp_err_t app_camera_init(uint32_t *width, uint32_t *height, uint32_t *fps);

esp_err_t app_camera_start(void);

#ifdef __cplusplus
}
#endif
