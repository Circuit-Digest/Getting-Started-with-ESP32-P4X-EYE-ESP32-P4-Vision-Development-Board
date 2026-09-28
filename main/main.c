/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * ESP32-P4X-EYE face triggered recorder: application controller.
 *
 * Owns the settings and the live / player / USB drive modes, turns button and detector events
 * into recorder actions and drives the indicator LEDs. See README.md for the controls.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"

#include "app_events.h"
#include "app_led.h"
#include "app_storage.h"
#include "app_recorder.h"
#include "app_face.h"
#include "app_camera.h"
#include "app_ui.h"
#include "app_input.h"
#include "app_player.h"

#define FACE_HOLD_MS            600     /* keep "face present" this long after the last detection */
#define NO_STORAGE_RETRY_MS     5000
#define CLIP_MIN_S              1
#define CLIP_MAX_S              60

#define NVS_NAMESPACE           "facerec"

#define COLOR_INFO              0x4FC3F7
#define COLOR_OK                0x2ECC71
#define COLOR_ERR               0xE53935
#define COLOR_WARN              0xF5A623

static const char *TAG = "main";

static QueueHandle_t s_events;
static volatile bool s_player_mode;
static bool s_usb_mode;

static struct {
    bool armed;
    bool led_enabled;
    uint8_t clip_s;
} s_settings;

void app_post_event(app_event_type_t type, int32_t value)
{
    app_event_t ev = {
        .type = type,
        .value = value,
    };
    if (s_events && xQueueSend(s_events, &ev, 0) != pdTRUE) {
        ESP_LOGD(TAG, "event queue full, event %d dropped", type);
    }
}

static void settings_defaults(void)
{
    s_settings.armed = CONFIG_APP_AUTO_RECORD_DEFAULT;
    s_settings.led_enabled = true;
    s_settings.clip_s = CONFIG_APP_RECORD_SECONDS;
}

static void settings_load(void)
{
    nvs_handle_t nvs;
    settings_defaults();
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "armed", &v) == ESP_OK) {
            s_settings.armed = v;
        }
        if (nvs_get_u8(nvs, "led", &v) == ESP_OK) {
            s_settings.led_enabled = v;
        }
        if (nvs_get_u8(nvs, "clip_s", &v) == ESP_OK && v >= CLIP_MIN_S && v <= CLIP_MAX_S) {
            s_settings.clip_s = v;
        }
        nvs_close(nvs);
    }
}

static void settings_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "armed", s_settings.armed);
        nvs_set_u8(nvs, "led", s_settings.led_enabled);
        nvs_set_u8(nvs, "clip_s", s_settings.clip_s);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void settings_apply(void)
{
    app_led_set_flash_enabled(s_settings.led_enabled);
    app_ui_settings_t ui = {
        .armed = s_settings.armed,
        .led_enabled = s_settings.led_enabled,
        .clip_seconds = s_settings.clip_s,
    };
    app_ui_set_settings(&ui);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool start_clip(const char *reason)
{
    esp_err_t ret = app_recorder_start(s_settings.clip_s * 1000);
    char msg[64];

    switch (ret) {
    case ESP_OK:
        ESP_LOGI(TAG, "Recording started (%s): %s", reason, app_recorder_last_file());
        return true;
    case ESP_ERR_NOT_FOUND:
        app_ui_toast(LV_SYMBOL_WARNING " No microSD card!", COLOR_ERR, 2500);
        break;
    case ESP_ERR_INVALID_STATE:
        break;
    default:
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING " Record error: %s", esp_err_to_name(ret));
        app_ui_toast(msg, COLOR_ERR, 2500);
        break;
    }
    return false;
}

/* Called by the storage task right before the microSD card is unmounted */
static void before_unmount(void)
{
    app_recorder_stop_sync();
    app_player_close();
}

static void enter_player(void)
{
    if (app_recorder_is_busy()) {
        app_ui_toast("Busy recording...", COLOR_WARN, 1500);
        return;
    }
    if (!app_storage_is_mounted()) {
        app_ui_toast(LV_SYMBOL_WARNING " No microSD card", COLOR_ERR, 2000);
        app_storage_rescan();
        return;
    }
    s_player_mode = true;
    app_ui_set_mode(APP_UI_MODE_PLAYER);
    app_player_open();
}

static void exit_player(void)
{
    app_player_close();
    s_player_mode = false;
    app_ui_set_mode(APP_UI_MODE_LIVE);
}

static void enter_usb_mode(void)
{
    if (!app_storage_is_mounted()) {
        app_ui_toast(LV_SYMBOL_WARNING " No microSD card", COLOR_ERR, 2000);
        return;
    }
    if (s_player_mode) {
        exit_player();
    }
    s_usb_mode = true;
    app_ui_set_mode(APP_UI_MODE_USB);
    app_storage_set_usb_mode(true);     /* stops a running recording before the card is released */
}

static void exit_usb_mode(void)
{
    app_storage_set_usb_mode(false);
    app_ui_toast("Leaving USB mode...", COLOR_INFO, 1500);
}

/* Button / encoder handling while the player is on screen */
static void handle_player_event(const app_event_t *ev)
{
    switch (ev->type) {
    case APP_EV_BTN_RECORD:
    case APP_EV_BTN_ENCODER:
        app_player_toggle_pause();
        break;
    case APP_EV_BTN_ARM:
        app_player_delete();
        break;
    case APP_EV_BTN_LED:
        exit_player();
        break;
    case APP_EV_KNOB:
        app_player_step(ev->value);
        break;
    default:
        break;
    }
}

static void controller_task(void *arg)
{
    int face_streak = 0;
    int64_t last_face_ms = -100000;
    int64_t next_trigger_ms = 0;        /* cooldown / retry gate for face triggered clips */
    char msg[64];

    while (true) {
        app_event_t ev;
        bool have_ev = xQueueReceive(s_events, &ev, pdMS_TO_TICKS(100)) == pdTRUE;
        int64_t now = now_ms();

        if (have_ev && s_usb_mode) {
            switch (ev.type) {
            case APP_EV_BTN_ENCODER_LONG:
            case APP_EV_BTN_LED:
                exit_usb_mode();
                have_ev = false;
                break;
            case APP_EV_USB_MODE:
            case APP_EV_STORAGE:
            case APP_EV_REC_DONE:
                break;          /* handled below */
            case APP_EV_FACE:
                face_streak = 0;
                have_ev = false;
                break;
            default:
                have_ev = false; /* all other controls are inactive in USB mode */
                break;
            }
        }

        if (have_ev && s_player_mode) {
            switch (ev.type) {
            case APP_EV_BTN_RECORD:
            case APP_EV_BTN_ENCODER:
            case APP_EV_BTN_ARM:
            case APP_EV_BTN_LED:
            case APP_EV_KNOB:
                handle_player_event(&ev);
                have_ev = false;
                break;
            case APP_EV_BTN_LED_LONG:
                have_ev = false;
                break;
            case APP_EV_FACE:
                face_streak = 0;        /* no face triggered clips while browsing */
                have_ev = false;
                break;
            case APP_EV_STORAGE:
                if (!ev.value) {
                    exit_player();
                }
                break;
            default:
                break;
            }
        }

        if (have_ev) {
            switch (ev.type) {
            case APP_EV_FACE:
                if (ev.value > 0) {
                    face_streak++;
                    last_face_ms = now;
                } else {
                    face_streak = 0;
                }
                if (s_settings.armed && face_streak >= CONFIG_APP_FACE_MIN_FRAMES &&
                        !app_recorder_is_busy() && now >= next_trigger_ms) {
                    if (!start_clip("face")) {
                        next_trigger_ms = now + NO_STORAGE_RETRY_MS;
                    }
                }
                break;

            case APP_EV_BTN_RECORD:
                if (app_recorder_is_busy()) {
                    app_recorder_stop();
                    app_ui_toast("Stopping...", COLOR_INFO, 1000);
                } else {
                    start_clip("button");
                }
                break;

            case APP_EV_BTN_ARM:
                s_settings.armed = !s_settings.armed;
                face_streak = 0;
                app_ui_toast(s_settings.armed ? LV_SYMBOL_EYE_OPEN " Auto record ON" : LV_SYMBOL_EYE_CLOSE " Auto record OFF",
                             s_settings.armed ? COLOR_OK : COLOR_WARN, 1500);
                settings_apply();
                settings_save();
                break;

            case APP_EV_BTN_LED:
                s_settings.led_enabled = !s_settings.led_enabled;
                app_ui_toast(s_settings.led_enabled ? "Flash LED ON" : "Flash LED OFF", COLOR_INFO, 1500);
                settings_apply();
                settings_save();
                break;

            case APP_EV_KNOB: {
                int clip = s_settings.clip_s + ev.value;
                clip = clip < CLIP_MIN_S ? CLIP_MIN_S : (clip > CLIP_MAX_S ? CLIP_MAX_S : clip);
                if (clip != s_settings.clip_s) {
                    s_settings.clip_s = clip;
                    snprintf(msg, sizeof(msg), "Clip length: %d s", clip);
                    app_ui_toast(msg, COLOR_INFO, 1200);
                    settings_apply();
                    settings_save();
                }
                break;
            }

            case APP_EV_BTN_ENCODER:
                enter_player();
                break;

            case APP_EV_BTN_ENCODER_LONG:
                enter_usb_mode();
                break;

            case APP_EV_USB_MODE:
                if (ev.value == 1) {
                    ESP_LOGI(TAG, "USB drive mode active");
                } else {
                    s_usb_mode = false;
                    app_ui_set_mode(APP_UI_MODE_LIVE);
                    app_ui_toast(ev.value == 0 ? LV_SYMBOL_OK " USB mode off" : LV_SYMBOL_WARNING " USB mode failed",
                                 ev.value == 0 ? COLOR_OK : COLOR_ERR, 2000);
                    next_trigger_ms = now_ms() + CONFIG_APP_RECORD_COOLDOWN_S * 1000;
                }
                break;

            case APP_EV_BTN_LED_LONG:
                settings_defaults();
                settings_apply();
                settings_save();
                app_ui_toast("Settings reset to defaults", COLOR_WARN, 2000);
                break;

            case APP_EV_REC_DONE:
                if (ev.value == ESP_OK) {
                    snprintf(msg, sizeof(msg), LV_SYMBOL_OK " Saved %s", app_recorder_last_file());
                    app_ui_toast(msg, COLOR_OK, 3000);
                } else {
                    app_ui_toast(LV_SYMBOL_WARNING " Recording failed", COLOR_ERR, 3000);
                }
                next_trigger_ms = now + CONFIG_APP_RECORD_COOLDOWN_S * 1000;
                face_streak = 0;
                break;

            case APP_EV_STORAGE:
                app_ui_toast(ev.value ? LV_SYMBOL_SD_CARD " microSD ready" : LV_SYMBOL_WARNING " microSD removed",
                             ev.value ? COLOR_OK : COLOR_WARN, 2000);
                if (ev.value) {
                    next_trigger_ms = 0;
                }
                break;
            }
        }

        /* Indicator LED */
        if (app_recorder_is_busy()) {
            app_led_set_state(APP_LED_RECORDING);
        } else if (!s_player_mode && !s_usb_mode && now - last_face_ms < FACE_HOLD_MS) {
            app_led_set_state(APP_LED_FACE);
        } else {
            app_led_set_state(APP_LED_OFF);
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_events = xQueueCreate(32, sizeof(app_event_t));
    settings_load();

    ESP_ERROR_CHECK(app_led_init());
    ESP_ERROR_CHECK(app_ui_init());
    settings_apply();
    app_ui_toast("Starting camera...", COLOR_INFO, 3000);

    ESP_ERROR_CHECK(app_storage_init(before_unmount));

    uint32_t width, height, fps;
    ESP_ERROR_CHECK(app_camera_init(&width, &height, &fps));
    ESP_ERROR_CHECK(app_recorder_init(width, height, fps));
    ESP_ERROR_CHECK(app_face_init(width / 4, height / 4));
    ESP_ERROR_CHECK(app_camera_start());
    ESP_ERROR_CHECK(app_player_init());
    ESP_ERROR_CHECK(app_input_init());

    xTaskCreatePinnedToCore(controller_task, "controller", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Face recorder running: clip %d s, auto record %s, LED GPIO%d",
             s_settings.clip_s, s_settings.armed ? "armed" : "paused", CONFIG_APP_LED_GPIO);
}
