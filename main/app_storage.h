/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * microSD storage: hot-plug handling (card-detect pin), clip folder and file naming,
 * and USB drive mode (card exported to a PC through TinyUSB MSC).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called (from the storage task) right before the card is unmounted,
 * so an ongoing recording can be closed first. */
typedef void (*app_storage_unmount_cb_t)(void);

esp_err_t app_storage_init(app_storage_unmount_cb_t before_unmount);

bool app_storage_is_mounted(void);

/* Free / total space in MiB, returns false when nothing is mounted */
bool app_storage_get_space(uint32_t *free_mb, uint32_t *total_mb);

/* Build the path of the next clip, e.g. "/sdcard/FACEREC/REC_00012.mp4" (creates the folder) */
esp_err_t app_storage_next_clip_path(char *path, size_t len);

/* Ask the storage task to probe / re-mount the card */
void app_storage_rescan(void);

/* USB drive mode: export the microSD card to a PC over the USB 2.0 port (asynchronous,
 * APP_EV_USB_MODE reports the result). The application has no card access meanwhile. */
void app_storage_set_usb_mode(bool enable);
bool app_storage_is_usb_mode(void);

#ifdef __cplusplus
}
#endif
