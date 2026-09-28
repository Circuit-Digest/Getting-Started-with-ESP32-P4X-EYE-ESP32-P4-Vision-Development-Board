/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * 240x240 LCD user interface (LVGL): live preview with face boxes, recording
 * progress, microSD status, microphone level and button hints.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Preview keeps the 16:9 camera aspect ratio */
#define APP_UI_PREVIEW_W 240
#define APP_UI_PREVIEW_H 135
/* Player frame buffers hold whole 16-line JPEG MCU rows (135 -> 144) */
#define APP_UI_PLAYER_BUF_H 144

typedef enum {
    APP_UI_MODE_LIVE,
    APP_UI_MODE_PLAYER,
    APP_UI_MODE_USB,
} app_ui_mode_t;

typedef enum {
    APP_UI_PLAYER_EMPTY,        /* no recordings on the card */
    APP_UI_PLAYER_NO_PREVIEW,   /* clip has no .prv playback file */
    APP_UI_PLAYER_PAUSED,
    APP_UI_PLAYER_PLAYING,
} app_ui_player_state_t;

typedef struct {
    app_ui_player_state_t state;
    int index;                  /* 0 based */
    int count;
    char name[24];
    uint32_t pos_ms;
    uint32_t duration_ms;
    bool delete_armed;          /* waiting for the delete confirmation press */
} app_ui_player_info_t;

typedef struct {
    bool armed;
    bool led_enabled;
    uint32_t clip_seconds;
} app_ui_settings_t;

esp_err_t app_ui_init(void);

void app_ui_set_settings(const app_ui_settings_t *settings);

/* Temporary message in the status line */
void app_ui_toast(const char *text, uint32_t color_hex, uint32_t duration_ms);

/* Live view vs. video player screen */
void app_ui_set_mode(app_ui_mode_t mode);
bool app_ui_is_live(void);

/* Camera preview double buffer: fill the returned RGB565 buffer, then commit */
uint8_t *app_ui_preview_acquire(size_t *size);
void app_ui_preview_commit(void);

/* Player: frame double buffer (APP_UI_PREVIEW_W x APP_UI_PLAYER_BUF_H RGB565) and status */
uint8_t *app_ui_player_acquire(size_t *size);
void app_ui_player_commit(void);
void app_ui_set_player_info(const app_ui_player_info_t *info);

#ifdef __cplusplus
}
#endif
