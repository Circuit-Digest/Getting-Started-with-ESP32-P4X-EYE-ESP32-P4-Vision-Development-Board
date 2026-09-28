/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * Events delivered to the application controller (main.c).
 * Producers: buttons/encoder, face detector, recorder, storage.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EV_BTN_RECORD,          /* Button 1: manual record / stop */
    APP_EV_BTN_ARM,             /* Button 2: arm / disarm auto recording */
    APP_EV_BTN_LED,             /* Button 3: flash LED on / off */
    APP_EV_BTN_ENCODER,         /* Encoder push: open the player */
    APP_EV_BTN_ENCODER_LONG,    /* Encoder long push: USB drive mode on / off */
    APP_EV_BTN_LED_LONG,        /* Button 3 long push: reset settings */
    APP_EV_KNOB,                /* Encoder rotated, value = detents (+ right, - left) */
    APP_EV_FACE,                /* Detector result, value = number of faces */
    APP_EV_REC_DONE,            /* Recording finished, value = esp_err_t */
    APP_EV_STORAGE,             /* microSD mounted / removed, value = 1 / 0 */
    APP_EV_USB_MODE,            /* USB drive mode entered / left / failed, value = 1 / 0 / -1 */
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    int32_t value;
} app_event_t;

/* Post an event to the controller. Safe from any task (not from ISR). */
void app_post_event(app_event_type_t type, int32_t value);

#ifdef __cplusplus
}
#endif
