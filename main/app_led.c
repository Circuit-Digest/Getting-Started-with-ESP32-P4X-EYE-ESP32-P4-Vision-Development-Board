/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "app_led.h"

#define LED_TICK_MS 50

#if CONFIG_APP_LED_ACTIVE_HIGH
#define LED_ON_LEVEL 1
#else
#define LED_ON_LEVEL 0
#endif

#if CONFIG_APP_LED_REC_BLINK
#define LED_BLINK_TICKS (CONFIG_APP_LED_BLINK_PERIOD_MS / LED_TICK_MS > 0 ? CONFIG_APP_LED_BLINK_PERIOD_MS / LED_TICK_MS : 1)
#endif

static const char *TAG = "app_led";

static volatile app_led_state_t s_state = APP_LED_OFF;
static volatile bool s_flash_enabled = true;
static esp_timer_handle_t s_timer;

static void led_write(bool indicator_on, bool flash_on)
{
    gpio_set_level(CONFIG_APP_LED_GPIO, indicator_on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#if CONFIG_APP_FLASH_LED_GPIO >= 0 && CONFIG_APP_FLASH_LED_GPIO != CONFIG_APP_LED_GPIO
    gpio_set_level(CONFIG_APP_FLASH_LED_GPIO, flash_on);   /* flash LED is active high */
#endif
}

/* Single writer of the GPIO, so state changes from any task are race free */
static void led_tick(void *arg)
{
    static uint32_t tick;
    bool on = false;

    tick++;
    {
        switch (s_state) {
        case APP_LED_FACE:
            on = true;
            break;
        case APP_LED_RECORDING:
#if CONFIG_APP_LED_REC_BLINK
            on = (tick / LED_BLINK_TICKS) % 2 == 0;
#else
            on = true;
#endif
            break;
        default:
            break;
        }
    }
    /* Flash = fill light: steady on for the whole recording if the user enabled it */
    led_write(on, s_flash_enabled && s_state == APP_LED_RECORDING);
}

esp_err_t app_led_init(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(CONFIG_APP_LED_GPIO)
#if CONFIG_APP_FLASH_LED_GPIO >= 0 && CONFIG_APP_FLASH_LED_GPIO != CONFIG_APP_LED_GPIO
        | BIT64(CONFIG_APP_FLASH_LED_GPIO)
#endif
        ,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "LED GPIO config failed");
    led_write(false, false);

    const esp_timer_create_args_t timer_args = {
        .callback = led_tick,
        .name = "led",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_timer), TAG, "LED timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, LED_TICK_MS * 1000), TAG, "LED timer start failed");

    ESP_LOGI(TAG, "Indicator LED on GPIO%d (active %s), flash LED GPIO%d", CONFIG_APP_LED_GPIO,
             LED_ON_LEVEL ? "high" : "low", CONFIG_APP_FLASH_LED_GPIO);
    return ESP_OK;
}

void app_led_set_state(app_led_state_t state)
{
    s_state = state;
}

void app_led_set_flash_enabled(bool enabled)
{
    s_flash_enabled = enabled;
}
