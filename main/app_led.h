/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * Indicator LED: solid while a face is in view, blinking (or solid) while recording.
 *   - CONFIG_APP_LED_GPIO (default 34, expansion header): always active
 *   - CONFIG_APP_FLASH_LED_GPIO (default 23, on-board flash): steady on while recording, if enabled
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_LED_OFF,
    APP_LED_FACE,
    APP_LED_RECORDING,
} app_led_state_t;

esp_err_t app_led_init(void);
void app_led_set_state(app_led_state_t state);

/* Enable / disable the on-board flash LED as a fill light (steady on while recording) */
void app_led_set_flash_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
