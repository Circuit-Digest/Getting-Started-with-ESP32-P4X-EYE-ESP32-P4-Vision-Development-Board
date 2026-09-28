/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "app_face.h"
#include "app_recorder.h"
#include "app_storage.h"
#include "app_ui.h"

#define ALIGN_UP(n, a)      (((n) + ((a) - 1)) & ~((a) - 1))
#define BUF_ALIGN           128
#define DRAW_BUF_LINES      40

#define TOP_BAR_H           26
#define PREVIEW_Y           (TOP_BAR_H + 2)
#define UI_REFRESH_MS       100
#define SPACE_REFRESH_MS    3000

#define COLOR_BG            0x0E1116
#define COLOR_BAR           0x1C222B
#define COLOR_TEXT          0xE6E8EB
#define COLOR_DIM           0x8A93A0
#define COLOR_GREEN         0x2ECC71
#define COLOR_RED           0xE53935
#define COLOR_ORANGE        0xF5A623
#define COLOR_FACE_BOX      0x39FF88
#define COLOR_BLUE          0x4FC3F7

static const char *TAG = "app_ui";

static lv_obj_t *s_lbl_arm;
static lv_obj_t *s_lbl_sd;
static lv_obj_t *s_img;
static lv_obj_t *s_rec_badge;
static lv_obj_t *s_rec_badge_lbl;
static lv_obj_t *s_face_box[APP_FACE_MAX_BOXES];
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_bar_rec;
static lv_obj_t *s_bar_mic;
static lv_obj_t *s_lbl_hint;
static lv_obj_t *s_mic_icon;
static lv_obj_t *s_play_badge;
static lv_obj_t *s_play_badge_lbl;
static lv_obj_t *s_usb_panel;

static uint8_t *s_prev_buf[2];
static size_t s_prev_buf_size;
static lv_image_dsc_t s_prev_dsc[2];
static int s_prev_back;            /* index of the buffer the camera writes next */

static uint8_t *s_play_buf[2];
static size_t s_play_buf_size;
static lv_image_dsc_t s_play_dsc[2];
static int s_play_back;

static volatile app_ui_mode_t s_mode = APP_UI_MODE_LIVE;
static app_ui_player_info_t s_player;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static app_ui_settings_t s_settings;
static char s_toast[64];
static uint32_t s_toast_color;
static int64_t s_toast_until_us;

static uint32_t s_free_mb;
static uint32_t s_total_mb;
static bool s_space_valid;
static int64_t s_space_time_us;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int y, int h, uint32_t color)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 200, h);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_BAR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    return bar;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);

    /* Top bar: auto-record state (left), microSD (right) */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, BSP_LCD_H_RES, TOP_BAR_H);
    lv_obj_set_style_bg_color(top, lv_color_hex(COLOR_BAR), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    s_lbl_arm = make_label(top, &lv_font_montserrat_14, COLOR_GREEN);
    lv_obj_align(s_lbl_arm, LV_ALIGN_LEFT_MID, 6, 0);
    s_lbl_sd = make_label(top, &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_align(s_lbl_sd, LV_ALIGN_RIGHT_MID, -6, 0);

    /* Camera preview */
    s_img = lv_image_create(scr);
    lv_obj_set_size(s_img, APP_UI_PREVIEW_W, APP_UI_PREVIEW_H);
    lv_obj_set_pos(s_img, 0, PREVIEW_Y);
    lv_obj_set_style_bg_color(s_img, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_img, LV_OPA_COVER, 0);

    /* Face boxes on top of the preview */
    for (int i = 0; i < APP_FACE_MAX_BOXES; i++) {
        lv_obj_t *box = lv_obj_create(scr);
        lv_obj_remove_style_all(box);
        lv_obj_set_style_border_color(box, lv_color_hex(COLOR_FACE_BOX), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 3, 0);
        lv_obj_set_hidden(box, true);
        s_face_box[i] = box;
    }

    /* REC badge */
    s_rec_badge = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rec_badge);
    lv_obj_set_size(s_rec_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_rec_badge, lv_color_hex(COLOR_RED), 0);
    lv_obj_set_style_bg_opa(s_rec_badge, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_rec_badge, 4, 0);
    lv_obj_set_style_pad_hor(s_rec_badge, 6, 0);
    lv_obj_set_style_pad_ver(s_rec_badge, 2, 0);
    lv_obj_set_pos(s_rec_badge, 6, PREVIEW_Y + 6);
    s_rec_badge_lbl = make_label(s_rec_badge, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_set_hidden(s_rec_badge, true);

    /* Player: play badge in the middle of the picture while paused */
    s_play_badge = lv_obj_create(scr);
    lv_obj_remove_style_all(s_play_badge);
    lv_obj_set_size(s_play_badge, 48, 48);
    lv_obj_set_style_bg_color(s_play_badge, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_play_badge, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_play_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_play_badge, (APP_UI_PREVIEW_W - 48) / 2, PREVIEW_Y + (APP_UI_PREVIEW_H - 48) / 2);
    s_play_badge_lbl = make_label(s_play_badge, &lv_font_montserrat_14, 0xFFFFFF);
    lv_label_set_text(s_play_badge_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(s_play_badge_lbl);
    lv_obj_set_hidden(s_play_badge, true);

    /* Status line, recording progress, microphone level, hints */
    int y = PREVIEW_Y + APP_UI_PREVIEW_H + 4;
    s_lbl_status = make_label(scr, &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_set_width(s_lbl_status, BSP_LCD_H_RES - 8);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, y);

    s_bar_rec = make_bar(scr, y + 22, 6, COLOR_RED);

    s_mic_icon = make_label(scr, &lv_font_montserrat_12, COLOR_DIM);
    lv_label_set_text(s_mic_icon, LV_SYMBOL_AUDIO);
    lv_obj_align(s_mic_icon, LV_ALIGN_TOP_LEFT, 8, y + 30);
    s_bar_mic = make_bar(scr, y + 34, 4, COLOR_GREEN);
    lv_obj_set_width(s_bar_mic, 196);
    lv_obj_align(s_bar_mic, LV_ALIGN_TOP_LEFT, 30, y + 34);

    s_lbl_hint = make_label(scr, &lv_font_montserrat_12, COLOR_DIM);
    lv_obj_align(s_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* USB drive mode: full screen panel */
    s_usb_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_usb_panel);
    lv_obj_set_size(s_usb_panel, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_usb_panel, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_usb_panel, LV_OPA_COVER, 0);
    lv_obj_t *icon = make_label(s_usb_panel, &lv_font_montserrat_28, COLOR_BLUE);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *title = make_label(s_usb_panel, &lv_font_montserrat_20, COLOR_TEXT);
    lv_label_set_text(title, "USB DRIVE MODE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_t *body = make_label(s_usb_panel, &lv_font_montserrat_14, COLOR_DIM);
    lv_obj_set_width(body, BSP_LCD_H_RES - 20);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(body, "Connect the USB 2.0 port\nto a computer.\nClips are in /" CONFIG_APP_RECORD_DIR ".\n\nRecording is paused.\nEject on the PC, then exit.");
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_t *hint = make_label(s_usb_panel, &lv_font_montserrat_12, COLOR_ORANGE);
    lv_label_set_text(hint, "Hold knob 2 s or press B3 to exit");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_hidden(s_usb_panel, true);
}

static void set_live_widgets_visible(bool live)
{
    lv_obj_set_hidden(s_mic_icon, !live);
    lv_obj_set_hidden(s_bar_mic, !live);
    if (live) {
        lv_obj_set_hidden(s_play_badge, true);
    } else {
        lv_obj_set_hidden(s_rec_badge, true);
        for (int i = 0; i < APP_FACE_MAX_BOXES; i++) {
            lv_obj_set_hidden(s_face_box[i], true);
        }
    }
    lv_obj_set_style_bg_color(s_bar_rec, lv_color_hex(live ? COLOR_RED : COLOR_BLUE), LV_PART_INDICATOR);
}

static void refresh_player(bool toast_active, const char *toast, uint32_t toast_color)
{
    app_ui_player_info_t pl;
    portENTER_CRITICAL(&s_lock);
    pl = s_player;
    portEXIT_CRITICAL(&s_lock);

    lv_label_set_text(s_lbl_arm, LV_SYMBOL_VIDEO " PLAYBACK");
    lv_obj_set_style_text_color(s_lbl_arm, lv_color_hex(COLOR_BLUE), 0);
    lv_label_set_text_fmt(s_lbl_sd, "%d / %d", pl.count > 0 ? pl.index + 1 : 0, pl.count);
    lv_obj_set_style_text_color(s_lbl_sd, lv_color_hex(COLOR_TEXT), 0);

    lv_obj_set_hidden(s_play_badge, pl.state != APP_UI_PLAYER_PAUSED);
    lv_bar_set_value(s_bar_rec, pl.duration_ms ? LV_MIN(pl.pos_ms * 100 / pl.duration_ms, 100) : 0, LV_ANIM_OFF);

    if (toast_active) {
        lv_label_set_text(s_lbl_status, toast);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(toast_color), 0);
    } else if (pl.delete_armed) {
        lv_label_set_text(s_lbl_status, LV_SYMBOL_TRASH " Press B2 again to delete");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_RED), 0);
    } else if (pl.state == APP_UI_PLAYER_EMPTY) {
        lv_label_set_text(s_lbl_status, "No recordings");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_DIM), 0);
    } else if (pl.state == APP_UI_PLAYER_NO_PREVIEW) {
        lv_label_set_text_fmt(s_lbl_status, "%s (no preview)", pl.name);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_ORANGE), 0);
    } else {
        lv_label_set_text_fmt(s_lbl_status, "%s  %" PRIu32 ":%02" PRIu32 " / %" PRIu32 ":%02" PRIu32, pl.name,
                              pl.pos_ms / 60000, (pl.pos_ms / 1000) % 60,
                              pl.duration_ms / 60000, (pl.duration_ms / 1000) % 60);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_TEXT), 0);
    }

    lv_label_set_text(s_lbl_hint, "B1 Play  B2 Delete  B3 Back  Knob < >");
}

static void ui_refresh_cb(lv_timer_t *timer)
{
    static app_ui_mode_t shown_mode = APP_UI_MODE_LIVE;
    app_ui_settings_t set;
    char toast[sizeof(s_toast)];
    uint32_t toast_color;
    bool toast_active;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    set = s_settings;
    toast_active = now < s_toast_until_us;
    memcpy(toast, s_toast, sizeof(toast));
    toast_color = s_toast_color;
    portEXIT_CRITICAL(&s_lock);

    app_ui_mode_t mode = s_mode;
    if (mode != shown_mode) {
        shown_mode = mode;
        lv_obj_set_hidden(s_usb_panel, mode != APP_UI_MODE_USB);
        if (mode != APP_UI_MODE_USB) {
            set_live_widgets_visible(mode == APP_UI_MODE_LIVE);
        }
    }
    if (mode == APP_UI_MODE_USB) {
        return;
    }
    if (mode == APP_UI_MODE_PLAYER) {
        refresh_player(toast_active, toast, toast_color);
        return;
    }

    bool sd = app_storage_is_mounted();
    if (sd && (!s_space_valid || now - s_space_time_us > SPACE_REFRESH_MS * 1000LL)) {
        s_space_valid = app_storage_get_space(&s_free_mb, &s_total_mb);
        s_space_time_us = now;
    } else if (!sd) {
        s_space_valid = false;
    }

    /* Top bar */
    lv_label_set_text(s_lbl_arm, set.armed ? LV_SYMBOL_EYE_OPEN " AUTO" : LV_SYMBOL_EYE_CLOSE " PAUSED");
    lv_obj_set_style_text_color(s_lbl_arm, lv_color_hex(set.armed ? COLOR_GREEN : COLOR_DIM), 0);
    if (sd && s_space_valid) {
        if (s_free_mb >= 10240) {
            lv_label_set_text_fmt(s_lbl_sd, LV_SYMBOL_SD_CARD " %" PRIu32 " GB free", s_free_mb / 1024);
        } else {
            lv_label_set_text_fmt(s_lbl_sd, LV_SYMBOL_SD_CARD " %" PRIu32 ".%" PRIu32 " GB free",
                                  s_free_mb / 1024, (s_free_mb % 1024) * 10 / 1024);
        }
        lv_obj_set_style_text_color(s_lbl_sd, lv_color_hex(COLOR_TEXT), 0);
    } else {
        lv_label_set_text(s_lbl_sd, sd ? LV_SYMBOL_SD_CARD " SD" : LV_SYMBOL_WARNING " NO SD");
        lv_obj_set_style_text_color(s_lbl_sd, lv_color_hex(sd ? COLOR_TEXT : COLOR_ORANGE), 0);
    }

    /* Face boxes (detector image -> preview coordinates) */
    app_face_result_t faces;
    app_face_get_result(&faces);
    for (int i = 0; i < APP_FACE_MAX_BOXES; i++) {
        if (i < faces.count && faces.img_w && faces.img_h) {
            const app_face_box_t *b = &faces.box[i];
            lv_obj_set_pos(s_face_box[i], b->x * APP_UI_PREVIEW_W / faces.img_w,
                           PREVIEW_Y + b->y * APP_UI_PREVIEW_H / faces.img_h);
            lv_obj_set_size(s_face_box[i], LV_MAX(b->w * APP_UI_PREVIEW_W / faces.img_w, 4),
                            LV_MAX(b->h * APP_UI_PREVIEW_H / faces.img_h, 4));
            lv_obj_set_hidden(s_face_box[i], false);
        } else {
            lv_obj_set_hidden(s_face_box[i], true);
        }
    }

    /* Recording badge + progress */
    uint32_t elapsed_ms, duration_ms;
    bool rec = app_recorder_is_busy();
    app_recorder_get_progress(&elapsed_ms, &duration_ms);
    if (rec) {
        bool blink = (now / 500000) % 2 == 0;
        lv_label_set_text_fmt(s_rec_badge_lbl, "%s REC %" PRIu32 "s", blink ? LV_SYMBOL_STOP : " ",
                              (duration_ms - elapsed_ms + 999) / 1000);
        lv_obj_set_hidden(s_rec_badge, false);
        lv_bar_set_value(s_bar_rec, duration_ms ? elapsed_ms * 100 / duration_ms : 0, LV_ANIM_OFF);
    } else {
        lv_obj_set_hidden(s_rec_badge, true);
        lv_bar_set_value(s_bar_rec, 0, LV_ANIM_OFF);
    }
    lv_bar_set_value(s_bar_mic, app_recorder_mic_level(), LV_ANIM_OFF);

    /* Status line: toast has priority */
    if (toast_active) {
        lv_label_set_text(s_lbl_status, toast);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(toast_color), 0);
    } else if (rec) {
        lv_label_set_text_fmt(s_lbl_status, "Recording %s", app_recorder_last_file());
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_RED), 0);
    } else if (!sd) {
        lv_label_set_text(s_lbl_status, "Insert a microSD card");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_ORANGE), 0);
    } else if (faces.count > 0) {
        lv_label_set_text_fmt(s_lbl_status, "Face detected (%d)", faces.count);
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_GREEN), 0);
    } else if (set.armed) {
        lv_label_set_text(s_lbl_status, "Watching for faces...");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_TEXT), 0);
    } else {
        lv_label_set_text(s_lbl_status, "Auto record paused");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(COLOR_DIM), 0);
    }

    lv_label_set_text_fmt(s_lbl_hint, "B1 Rec B2 Auto B3 Flash:%s Knob %" PRIu32 "s",
                          set.led_enabled ? "on" : "off", set.clip_seconds);
}

static lv_display_t *display_start(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t bsp_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(bsp_display_new(&bsp_cfg, &panel, &io));
    esp_lcd_panel_disp_on_off(panel, true);
#if CONFIG_APP_DISPLAY_ROTATE_180
    esp_lcd_panel_mirror(panel, true, true);
#endif

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
#if CONFIG_APP_DISPLAY_ROTATE_180
            .mirror_x = true,
            .mirror_y = true,
#endif
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = BSP_LCD_BIGENDIAN,
        },
    };
    return lvgl_port_add_disp(&disp_cfg);
}

esp_err_t app_ui_init(void)
{
    s_prev_buf_size = ALIGN_UP(APP_UI_PREVIEW_W * APP_UI_PREVIEW_H * 2, BUF_ALIGN);
    for (int i = 0; i < 2; i++) {
        s_prev_buf[i] = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_prev_buf_size, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_prev_buf[i], ESP_ERR_NO_MEM, TAG, "preview buffer alloc failed");
        s_prev_dsc[i] = (lv_image_dsc_t) {
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .w = APP_UI_PREVIEW_W,
                .h = APP_UI_PREVIEW_H,
                .stride = APP_UI_PREVIEW_W * 2,
            },
            .data_size = APP_UI_PREVIEW_W * APP_UI_PREVIEW_H * 2,
            .data = s_prev_buf[i],
        };
    }

    s_play_buf_size = ALIGN_UP(APP_UI_PREVIEW_W * APP_UI_PLAYER_BUF_H * 2, BUF_ALIGN);
    for (int i = 0; i < 2; i++) {
        s_play_buf[i] = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_play_buf_size, MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_play_buf[i], ESP_ERR_NO_MEM, TAG, "player buffer alloc failed");
        s_play_dsc[i] = s_prev_dsc[0];
        s_play_dsc[i].data = s_play_buf[i];
    }

    lv_display_t *disp = display_start();
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "display start failed");

    lvgl_port_lock(0);
    build_screen();
    lv_timer_create(ui_refresh_cb, UI_REFRESH_MS, NULL);
    lvgl_port_unlock();

    bsp_display_brightness_set(CONFIG_APP_LCD_BRIGHTNESS);
    return ESP_OK;
}

void app_ui_set_settings(const app_ui_settings_t *settings)
{
    portENTER_CRITICAL(&s_lock);
    s_settings = *settings;
    portEXIT_CRITICAL(&s_lock);
}

void app_ui_toast(const char *text, uint32_t color_hex, uint32_t duration_ms)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_toast, text, sizeof(s_toast));
    s_toast_color = color_hex;
    s_toast_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    portEXIT_CRITICAL(&s_lock);
}

uint8_t *app_ui_preview_acquire(size_t *size)
{
    *size = s_prev_buf_size;
    return s_prev_buf[s_prev_back];
}

void app_ui_preview_commit(void)
{
    /* Skip the frame rather than stall the camera if LVGL is busy */
    if (!lvgl_port_lock(20)) {
        return;
    }
    lv_image_set_src(s_img, &s_prev_dsc[s_prev_back]);
    lv_obj_invalidate(s_img);
    lvgl_port_unlock();
    s_prev_back ^= 1;
}

void app_ui_set_mode(app_ui_mode_t mode)
{
    s_mode = mode;
}

bool app_ui_is_live(void)
{
    return s_mode == APP_UI_MODE_LIVE;
}

uint8_t *app_ui_player_acquire(size_t *size)
{
    *size = s_play_buf_size;
    return s_play_buf[s_play_back];
}

void app_ui_player_commit(void)
{
    if (!lvgl_port_lock(50)) {
        return;
    }
    lv_image_set_src(s_img, &s_play_dsc[s_play_back]);
    lv_obj_invalidate(s_img);
    lvgl_port_unlock();
    s_play_back ^= 1;
}

void app_ui_set_player_info(const app_ui_player_info_t *info)
{
    portENTER_CRITICAL(&s_lock);
    s_player = *info;
    portEXIT_CRITICAL(&s_lock);
}
