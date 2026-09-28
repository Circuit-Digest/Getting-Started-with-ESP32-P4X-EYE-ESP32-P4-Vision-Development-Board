/*
 * SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
 * SPDX-License-Identifier: MIT
 */

/*
 * Preview side-car file (REC_xxxxx.prv) written next to every REC_xxxxx.mp4.
 *
 * On-device playback uses a small MJPEG copy of the clip (the LCD preview frames, hardware
 * JPEG encoded) so the player never has to decode full 1280x720 frames for a 240x240 screen.
 *
 *   header  app_prv_header_t
 *   frame   app_prv_frame_t followed by `len` bytes of JPEG, padded to 4 bytes
 *   ...
 */
#pragma once

#include <stdint.h>

#define APP_PRV_MAGIC       0x56525045u     /* "EPRV" little endian */
#define APP_PRV_VERSION     1
#define APP_PRV_EXT         ".prv"
#define APP_CLIP_EXT        ".mp4"
#define APP_CLIP_PREFIX     "REC_"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t reserved0;
    uint32_t frame_count;   /* 0 if the file was not closed properly: scan to EOF */
    uint32_t duration_ms;
    uint32_t reserved[3];
} app_prv_header_t;

typedef struct {
    uint32_t pts_ms;
    uint32_t len;
} app_prv_frame_t;

#define APP_PRV_PAD4(n) (((n) + 3u) & ~3u)
