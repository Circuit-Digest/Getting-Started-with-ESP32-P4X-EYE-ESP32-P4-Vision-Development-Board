/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "bsp/esp-bsp.h"
#include "app_events.h"
#include "app_storage.h"

#define STORAGE_POLL_MS         500
#define STORAGE_RETRY_MS        3000   /* re-probe period when a card is present but did not mount */
#define SD_LDO_CHANNEL          4      /* on-chip LDO powering the microSD IOs on the P4-EYE */
#define CLIP_PREFIX             "REC_"
#define CLIP_EXT                ".mp4"

/* Task notification bits */
#define NOTIFY_RESCAN           BIT0
#define NOTIFY_USB_ON           BIT1
#define NOTIFY_USB_OFF          BIT2

static const char *TAG = "app_storage";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static app_storage_unmount_cb_t s_before_unmount;
static volatile bool s_mounted;     /* FAT mounted for the application at BSP_SD_MOUNT_POINT */
static volatile bool s_usb_mode;    /* card exported to the PC over USB */
static int s_next_clip = -1;        /* cached next clip number, -1 = scan the folder */

/* Normal operation: the BSP mounts the card (FAT on /sdcard).
 * USB mode: the BSP mount is released and the card is re-initialised as a raw block device for
 * TinyUSB MSC, so only the PC accesses it. */
static bool s_present_mounted;      /* a card is present and handled (BSP mount or USB export) */
static sdmmc_card_t *s_card;
static sdmmc_host_t s_host;
static sd_pwr_ctrl_handle_t s_ldo;
static tinyusb_msc_storage_handle_t s_msc;

static bool card_present(void)
{
#if CONFIG_APP_SD_USE_CARD_DETECT
    return gpio_get_level(BSP_SD_DET) == 0;
#else
    return true;
#endif
}

static void usb_card_close(void)
{
    if (s_msc) {
        tinyusb_msc_delete_storage(s_msc);      /* unmounts FAT if mounted to the app */
        s_msc = NULL;
    }
    if (s_card) {
        s_host.deinit();
        free(s_card);
        s_card = NULL;
    }
    if (s_ldo) {
        sd_pwr_ctrl_del_on_chip_ldo(s_ldo);
        s_ldo = NULL;
    }
}

static esp_err_t usb_card_open(void)
{
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_SD, true), TAG, "SD power enable failed");

    const sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = SD_LDO_CHANNEL,
    };
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_ldo), TAG, "SD LDO init failed");

    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &s_host);
    s_host.pwr_ctrl_handle = s_ldo;
    sdmmc_slot_config_t slot;
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);

    s_card = calloc(1, sizeof(sdmmc_card_t));
    ESP_GOTO_ON_FALSE(s_card, ESP_ERR_NO_MEM, fail, TAG, "no mem");
    ESP_GOTO_ON_ERROR(s_host.init(), fail_host, TAG, "SDMMC host init failed");
    ESP_GOTO_ON_ERROR(sdmmc_host_init_slot(s_host.slot, &slot), fail, TAG, "SDMMC slot init failed");
    ESP_GOTO_ON_ERROR(sdmmc_card_init(&s_host, s_card), fail, TAG, "card init failed");

    const tinyusb_msc_storage_config_t msc_cfg = {
        .medium.card = s_card,
        .fat_fs = {
            .base_path = BSP_SD_MOUNT_POINT,
            .do_not_format = true,      /* never wipe a card with an unknown filesystem */
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    ESP_GOTO_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&msc_cfg, &s_msc), fail, TAG, "MSC storage create failed");
    return ESP_OK;

fail_host:
    free(s_card);
    s_card = NULL;
fail:
    usb_card_close();
    return ret;
}

static bool app_mount(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "microSD mount failed: %s", esp_err_to_name(ret));
        bsp_sdcard_unmount();   /* releases the LDO / host, errors are expected here */
        return false;
    }
    sdmmc_card_print_info(stdout, bsp_sdcard_get_handle());
    return true;
}

/* Release the card from the application: stop the recorder / player first */
static void app_side_release(void)
{
    if (s_before_unmount) {
        s_before_unmount();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mounted) {
        bsp_sdcard_unmount();
    }
    s_mounted = false;
    s_next_clip = -1;
    xSemaphoreGive(s_lock);
}

static void usb_mode_enter(void)
{
    if (s_usb_mode) {
        return;
    }
    if (!s_mounted) {
        ESP_LOGW(TAG, "USB mode needs a microSD card");
        app_post_event(APP_EV_USB_MODE, -1);
        return;
    }
    app_side_release();

    esp_err_t ret = usb_card_open();
    if (ret == ESP_OK) {
        const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
        ret = tinyusb_driver_install(&tusb_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB start failed: %s", esp_err_to_name(ret));
            usb_card_close();
        }
    }
    if (ret != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mounted = app_mount();
        xSemaphoreGive(s_lock);
        app_post_event(APP_EV_USB_MODE, -1);
        return;
    }
    s_usb_mode = true;
    ESP_LOGI(TAG, "USB mode: microSD exported as a USB drive");
    app_post_event(APP_EV_USB_MODE, 1);
}

static void usb_mode_exit(bool remount)
{
    if (!s_usb_mode) {
        return;
    }
    tinyusb_driver_uninstall();
    usb_card_close();
    s_usb_mode = false;
    if (remount) {
        /* files may have changed on the PC: the next clip number is rescanned */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_mounted = app_mount();
        s_present_mounted = s_mounted;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "USB mode off, microSD %s", s_mounted ? "back in the application" : "not mounted");
    app_post_event(APP_EV_USB_MODE, 0);
}

static void storage_task(void *arg)
{
    TickType_t last_try = 0;
    uint32_t notify = NOTIFY_RESCAN;

    while (true) {
        bool present = card_present();
        bool rescan = notify & NOTIFY_RESCAN;

        if (s_present_mounted && (!present || (rescan && !s_usb_mode))) {
            /* Card removed, or explicit re-scan (remount so a swapped card is picked up) */
            if (!present) {
                ESP_LOGW(TAG, "microSD removed");
            }
            usb_mode_exit(false);
            app_side_release();
            s_present_mounted = false;
            app_post_event(APP_EV_STORAGE, 0);
        }

        if (!s_present_mounted && present &&
                (rescan || (xTaskGetTickCount() - last_try) >= pdMS_TO_TICKS(STORAGE_RETRY_MS))) {
            last_try = xTaskGetTickCount();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_mounted = app_mount();
            xSemaphoreGive(s_lock);
            s_present_mounted = s_mounted;
            if (s_mounted) {
                ESP_LOGI(TAG, "microSD mounted at %s", BSP_SD_MOUNT_POINT);
                app_post_event(APP_EV_STORAGE, 1);
            }
        }

        if (notify & NOTIFY_USB_ON) {
            usb_mode_enter();
        }
        if (notify & NOTIFY_USB_OFF) {
            usb_mode_exit(true);
        }

        notify = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify, pdMS_TO_TICKS(STORAGE_POLL_MS));
    }
}

esp_err_t app_storage_init(app_storage_unmount_cb_t before_unmount)
{
    s_before_unmount = before_unmount;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mem");

#if CONFIG_APP_SD_USE_CARD_DETECT
    const gpio_config_t det_conf = {
        .pin_bit_mask = BIT64(BSP_SD_DET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&det_conf), TAG, "card detect GPIO config failed");
#endif

    /* MSC class driver; the USB device itself is only started in USB mode */
    const tinyusb_msc_driver_config_t msc_drv_cfg = {
        .user_flags.auto_mount_off = 1,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&msc_drv_cfg), TAG, "MSC driver install failed");

    BaseType_t ok = xTaskCreatePinnedToCore(storage_task, "storage", 4096, NULL, 4, &s_task, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

bool app_storage_is_mounted(void)
{
    return s_mounted;
}

bool app_storage_is_usb_mode(void)
{
    return s_usb_mode;
}

bool app_storage_get_space(uint32_t *free_mb, uint32_t *total_mb)
{
    uint64_t total = 0, free_bytes = 0;
    bool ok = false;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    if (s_mounted && esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        *free_mb = (uint32_t)(free_bytes >> 20);
        *total_mb = (uint32_t)(total >> 20);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static int scan_next_clip_number(const char *dir_path)
{
    int max_num = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return 1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int num;
        if (sscanf(entry->d_name, CLIP_PREFIX "%d", &num) == 1 && num > max_num) {
            max_num = num;
        }
    }
    closedir(dir);
    return max_num + 1;
}

esp_err_t app_storage_next_clip_path(char *path, size_t len)
{
    char dir_path[64];
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_mounted) {
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }

    snprintf(dir_path, sizeof(dir_path), "%s/%s", BSP_SD_MOUNT_POINT, CONFIG_APP_RECORD_DIR);
    struct stat st;
    if (stat(dir_path, &st) != 0 && mkdir(dir_path, 0775) != 0) {
        ESP_LOGE(TAG, "Cannot create %s", dir_path);
        ret = ESP_FAIL;
        goto out;
    }

    if (s_next_clip < 0) {
        s_next_clip = scan_next_clip_number(dir_path);
    }
    snprintf(path, len, "%s/" CLIP_PREFIX "%05d" CLIP_EXT, dir_path, s_next_clip++);

out:
    xSemaphoreGive(s_lock);
    return ret;
}

void app_storage_rescan(void)
{
    if (s_task) {
        xTaskNotify(s_task, NOTIFY_RESCAN, eSetBits);
    }
}

void app_storage_set_usb_mode(bool enable)
{
    if (s_task) {
        xTaskNotify(s_task, enable ? NOTIFY_USB_ON : NOTIFY_USB_OFF, eSetBits);
    }
}
