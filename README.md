# ESP32-P4X-EYE Face-Triggered Recorder

A small security-camera style firmware for the Espressif **ESP32-P4X-EYE**. It watches the scene, and
when a face shows up it records a short clip, **MJPEG video + AAC audio in an `.mp4`**, to the
microSD card. An external LED tells you when a face is seen and when it is recording. The 1.54" LCD
shows a live preview with face boxes, and you can play clips back on the device or copy them to a
computer over USB without pulling the card.

Everything runs on the ESP32-P4's hardware blocks: the ISP for the camera, the JPEG encoder for
recording (MJPEG in MP4, the same format as the ESP32-P4-EYE factory demo), the PPA for scaling and
the JPEG decoder for the on-device player. That leaves the CPU for face detection.

## Features

- Face detection with Espressif's [esp-dl](https://github.com/espressif/esp-dl) `human_face_detect` model
- Clips of 1280×720 @ 25 fps, MJPEG (quality 80) + AAC (16 kHz mono) in MP4; plays on any PC or phone
- Clip length adjustable from 1 to 60 s with the rotary encoder (default 5 s)
- Indicator LED on the expansion header: on while a face is in view, blinking while recording
- On-board flash LED as an optional fill light while recording
- Live preview, face boxes, recording countdown, free space and a microphone level meter on the LCD
- On-device player: play, pause, previous / next clip, delete
- USB drive mode: the microSD card shows up as a USB drive on a PC or Mac
- microSD hot-plug using the card-detect switch
- Settings survive a power cycle (stored in NVS)

## Hardware

| Item | Notes |
|---|---|
| ESP32-P4X-EYE | ESP32-P4 rev 3.1, 32 MB PSRAM, 16 MB flash, OV2710 camera, ST7789 240×240 LCD, PDM mic |
| microSD card | FAT32 or exFAT. MJPEG writes about 3–5 MB/s, so use a U1 card at least; U3 / V30 is recommended |
| LED + 330 Ω resistor | Optional: GPIO34 on the expansion header → resistor → LED → GND |

The **ESP32-P4X-EYE** is the ESP32-P4-EYE with the newer v3.1 chip, so this project uses the official
[`esp32_p4_eye`](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_p4_eye) board support package.
The board runs fine from USB power alone; no battery is needed.

### Pins used

| Function | GPIO |
|---|---|
| Indicator LED (expansion header) | 34 |
| Flash / fill light LED (on board) | 23 |
| Button 1 / 2 / 3 | 3 / 4 / 5 |
| Encoder push / A / B | 2 / 48 / 47 |
| microSD card detect | 45 |

Camera, LCD, microphone and microSD use the pins defined by the BSP. Every GPIO in the first table can
be changed in `menuconfig` (see [Configuration](#configuration)). The other free header pins are
6, 7, 8, 10, 37, 38, 50, 51, 52, 53 and 54.

## Using it

Insert a microSD card and power the board. The camera starts in **auto** mode: two consecutive
detections of a face start a clip, and after a clip there is a short pause (3 s) before it can trigger
again.

### Controls

| Control | Live view | Player |
|---|---|---|
| Button 1 | Record a clip now / stop recording | Play / pause |
| Button 2 | Auto (face-triggered) recording on / off | Delete clip (press twice within 3 s) |
| Button 3 | Flash fill light on / off | Back to live view |
| Button 3, hold 2 s | Reset settings to defaults | – |
| Encoder, rotate | Clip length (1 s per click) | Previous / next clip (one per click) |
| Encoder, press | Open the player | Play / pause |
| Encoder, hold 2 s | USB drive mode on / off | USB drive mode on |

### LEDs

| State | Indicator LED (GPIO34) | Flash LED (GPIO23) |
|---|---|---|
| Face in view | On | Off |
| Recording | Blinking | On if enabled with Button 3, otherwise off |
| Idle | Off | Off |

### Files on the card

```
/FACEREC/REC_00001.mp4   the clip (MJPEG + AAC)
/FACEREC/REC_00001.prv   small MJPEG preview used by the on-device player
```

Numbering continues from the highest existing file, so clips are never overwritten.

Every video frame is a complete JPEG, so the files are larger than H.264 would be: about 3–5 MB per
second at quality 80, roughly 20 MB for a 5 s clip. Lower **MJPEG video quality** in menuconfig for
smaller files. MJPEG in MP4 plays in VLC, mpv, ffmpeg-based players and most video editors.

### On-device player

Decoding full 1280×720 frames only to shrink them for the 240×240 LCD would waste time, so every clip
also gets a 240×135 MJPEG preview (`.prv`) that the hardware JPEG decoder plays at full frame rate. Playback on the device is silent because the board has no speaker.
Deleting a clip removes both files.

### USB drive mode

1. Hold the encoder for 2 s. The screen shows **USB DRIVE MODE** and recording pauses.
2. Connect the board's **USB 2.0** port to your computer. This is the port *not* labelled Debug; the
   Debug port is only for flashing and logs.
3. The card appears as a normal drive; the clips are in `FACEREC/`.
4. Eject the drive on the computer, then press Button 3 (or hold the encoder again) to go back to the
   camera.

The PC has exclusive access to the card while USB mode is on, so the camera cannot record at the same
time.

## Building

Requires **ESP-IDF v6.1** (v5.5+ should work but is untested). All other components are pulled from the
[Espressif Component Registry](https://components.espressif.com) on the first build and pinned in
`dependencies.lock`.

```sh
git clone <this repository>
cd ESP32-P4X-EYE-Face-Recorder
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

Use the board's **Debug** USB port for flashing and the serial monitor.

### Prebuilt firmware

If you don't want to build, prebuilt binaries are on the [Releases](../../releases) page. Flash them
with [esptool](https://docs.espressif.com/projects/esptool/):

```sh
# single merged image
esptool --chip esp32p4 -p <PORT> -b 460800 write-flash 0x0 p4x_eye_face_recorder_merged.bin

# or the individual images
esptool --chip esp32p4 -p <PORT> -b 460800 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x2000  bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 p4x_eye_face_recorder.bin
```

## Configuration

All project options are under `idf.py menuconfig` → **Face Recorder (ESP32-P4X-EYE)**:

| Menu | Options |
|---|---|
| Indicator LED | LED GPIO, active high / low, blink or solid while recording, blink rate, flash LED GPIO |
| Recording | Default clip length, pause between clips, folder name, video quality, audio bitrate, mic gain |
| Face detection | Auto mode at boot, minimum confidence, detections needed to trigger |
| Storage | Use the card-detect pin |
| Camera and display | Mirror / flip the image, rotate the LCD by 180°, LCD brightness |
| Buttons and encoder | GPIO of every button and of the encoder, knob events per click |

Settings changed with the buttons (auto mode, flash LED, clip length) are saved on the device and
override the menuconfig defaults until you reset them (hold Button 3 for 2 s).

### Rotary encoder

The encoder is read with Espressif's [`knob`](https://components.espressif.com/components/espressif/knob)
component, which reports one event per half quadrature cycle. As in the factory demo, events in the same
direction are counted and one step is made per click (**Knob events per click**, default 2). A partial
click is forgotten after 500 ms or when the direction changes. If one click moves two clips, raise the
value; if two clicks are needed per clip, set it to 1.

## How it works

```
OV2710 ──MIPI-CSI──▶ ISP ──YUV420 1280×720──┬──▶ PPA ──▶ 240×135 RGB565 ──▶ LCD preview ──▶ JPEG ──▶ .prv
                                            ├──▶ PPA ──▶ 320×180 RGB565 ──▶ face detector (esp-dl)
                                            └──▶ JPEG encoder ──┐
PDM mic ──▶ 16 kHz PCM ──▶ AAC encoder ─────────────────────────┴──▶ MP4 muxer ──▶ microSD
```

- **Camera task**: takes each frame from the camera and hands it to the preview, the detector (only when
  it is idle) and, while recording, the JPEG encoder.
- **Face task**: runs the detector on core 1 and posts the result to the controller.
- **Audio task**: reads the microphone continuously (for the level meter) and encodes AAC while recording.
- **Writer task**: owns the MP4 file, so slow SD card writes never stall the camera.
- **Controller** (`main.c`): trigger logic, LEDs, buttons, the live / player / USB modes and settings.

| File | Purpose |
|---|---|
| `main/main.c` | Controller and settings |
| `main/app_camera.c` | Camera capture and frame fan-out |
| `main/app_face.cpp` | Face detection |
| `main/app_recorder.c` | MJPEG + AAC encoding, MP4 writing, `.prv` preview |
| `main/app_player.c` | On-device player |
| `main/app_storage.c` | microSD hot-plug, file naming, USB drive mode |
| `main/app_ui.c` | LCD user interface (LVGL) |
| `main/app_input.c` | Buttons and rotary encoder (knob) |
| `main/app_led.c` | Indicator and flash LEDs |

## Known limitations

- Playback on the device uses the `.prv` preview, so it is low resolution and silent. The full-quality
  video with sound is the `.mp4`.
- Clips recorded while the card was removed or the storage was full are not saved; the screen shows
  *Recording failed*.
- Recording is paused while USB drive mode is on.
- MJPEG clips are large (see [Files on the card](#files-on-the-card)); a slow card drops frames, which
  shows up as `Writer backlog full` in the serial log.

## License

Released under the [MIT License](LICENSE).
Copyright © 2026 Jobit Joseph, Semicon Media.

The ESP-IDF framework and the Espressif components pulled in at build time (BSP, esp_video, esp_h264,
esp-dl, esp_muxer, esp_audio_codec, TinyUSB, LVGL and others) are distributed under their own licenses.
