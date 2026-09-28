/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "iot_knob.h"
#include "app_events.h"
#include "app_input.h"

#define LONG_PRESS_MS 2000
#define KNOB_TIMEOUT_MS 500     /* a partial click older than this is forgotten */

static const char *TAG = "app_input";

static void button_cb(void *btn, void *usr_data)
{
    app_post_event((app_event_type_t)(intptr_t)usr_data, 0);
}

/* One knob event per half quadrature cycle: count them per direction and post one step per click
 * (same approach as the factory demo's knob_step_threshold). Runs in the knob timer task only. */
static void knob_cb(void *knob, void *usr_data)
{
    static int count;
    static int last_dir;
    static int64_t last_ms;

    int dir = (int)(intptr_t)usr_data;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (dir != last_dir || now_ms - last_ms > KNOB_TIMEOUT_MS) {
        count = 0;
        last_dir = dir;
    }
    last_ms = now_ms;

    if (++count >= CONFIG_APP_ENCODER_EVENTS_PER_STEP) {
        count = 0;
        app_post_event(APP_EV_KNOB, dir);
    }
}

static esp_err_t add_button(int gpio, app_event_type_t click_event, int long_event)
{
    const button_config_t btn_cfg = {
        .long_press_time = LONG_PRESS_MS,
    };
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio,
        .active_level = 0,
    };
    button_handle_t btn = NULL;
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn), TAG, "button GPIO%d failed", gpio);

    if (long_event >= 0) {
        /* With a long-press action, the short action fires on release so a long press does not trigger both */
        ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, button_cb,
                                                   (void *)(intptr_t)click_event), TAG, "cb failed");
        ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, button_cb,
                                                   (void *)(intptr_t)long_event), TAG, "cb failed");
    } else {
        ESP_RETURN_ON_ERROR(iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, button_cb,
                                                   (void *)(intptr_t)click_event), TAG, "cb failed");
    }
    return ESP_OK;
}

esp_err_t app_input_init(void)
{
    ESP_RETURN_ON_ERROR(add_button(CONFIG_APP_BTN_RECORD_GPIO, APP_EV_BTN_RECORD, -1), TAG, "");
    ESP_RETURN_ON_ERROR(add_button(CONFIG_APP_BTN_ARM_GPIO, APP_EV_BTN_ARM, -1), TAG, "");
    ESP_RETURN_ON_ERROR(add_button(CONFIG_APP_BTN_LED_GPIO, APP_EV_BTN_LED, APP_EV_BTN_LED_LONG), TAG, "");
    ESP_RETURN_ON_ERROR(add_button(CONFIG_APP_BTN_ENCODER_GPIO, APP_EV_BTN_ENCODER, APP_EV_BTN_ENCODER_LONG), TAG, "");

    const knob_config_t knob_cfg = {
        .default_direction = 0,
        .gpio_encoder_a = CONFIG_APP_ENCODER_A_GPIO,
        .gpio_encoder_b = CONFIG_APP_ENCODER_B_GPIO,
    };
    knob_handle_t knob = iot_knob_create(&knob_cfg);
    ESP_RETURN_ON_FALSE(knob, ESP_FAIL, TAG, "knob create failed");
    ESP_RETURN_ON_ERROR(iot_knob_register_cb(knob, KNOB_RIGHT, knob_cb, (void *)(intptr_t)1), TAG, "knob cb failed");
    ESP_RETURN_ON_ERROR(iot_knob_register_cb(knob, KNOB_LEFT, knob_cb, (void *)(intptr_t) - 1), TAG, "knob cb failed");

    ESP_LOGI(TAG, "Buttons: rec=GPIO%d auto=GPIO%d led=GPIO%d encoder=GPIO%d, knob A/B=GPIO%d/%d",
             CONFIG_APP_BTN_RECORD_GPIO, CONFIG_APP_BTN_ARM_GPIO, CONFIG_APP_BTN_LED_GPIO,
             CONFIG_APP_BTN_ENCODER_GPIO, CONFIG_APP_ENCODER_A_GPIO, CONFIG_APP_ENCODER_B_GPIO);
    return ESP_OK;
}
