/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * On-device video player for the recorded clips (plays the REC_xxxxx.prv MJPEG preview
 * with the hardware JPEG decoder; the board has no speaker, so playback is silent).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_player_init(void);

/* Scan the card and start playing the newest clip */
void app_player_open(void);

/* Stop playback and close the files; waits until done (safe before unmounting the card) */
void app_player_close(void);

void app_player_toggle_pause(void);

/* Previous (-1) / next (+1) clip */
void app_player_step(int direction);

/* First call arms, a second call within 3 s deletes the current clip (.mp4 + .prv) */
void app_player_delete(void);

#ifdef __cplusplus
}
#endif
