# SPDX-FileCopyrightText: 2026 Jobit Joseph, Semicon Media
# SPDX-License-Identifier: MIT

idf_component_register(
    SRCS
        "main.c"
        "app_camera.c"
        "app_face.cpp"
        "app_input.c"
        "app_led.c"
        "app_player.c"
        "app_recorder.c"
        "app_storage.c"
        "app_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_jpeg
        esp_driver_sdmmc
        sdmmc
        esp_driver_ppa
        esp_timer
        fatfs
        nvs_flash
        vfs
)
