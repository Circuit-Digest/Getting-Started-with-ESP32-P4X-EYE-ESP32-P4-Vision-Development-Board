# Flashing the ESP32-P4X-EYE Face-Triggered Recorder

Prebuilt firmware for the **ESP32-P4X-EYE** (ESP32-P4 chip revision v3.1 or newer, 16 MB flash).
Built with ESP-IDF v6.1.

## Files

| File | Flash address | Description |
|---|---|---|
| `bootloader.bin` | `0x2000` | Second-stage bootloader |
| `partition-table.bin` | `0x8000` | Partition table |
| `p4x_eye_face_recorder.bin` | `0x10000` | Application |
| `p4x_eye_face_recorder_merged.bin` | `0x0` | The three images above combined into one file |
| `SHA256SUMS.txt` | – | Checksums of all images |

Use **either** the merged image **or** the three separate images, not both.

Flash settings: DIO mode, 80 MHz, 16 MB.

## Before you start

1. Connect the board's **Debug** USB-C port to your computer. The other port, labelled USB 2.0, is not
   used for flashing.
2. Find the serial port:
   - Windows: `COMx` in Device Manager
   - macOS: `/dev/cu.usbmodem*`
   - Linux: `/dev/ttyACM*`
3. If the port does not appear, or flashing fails to connect: hold **Boot**, press and release
   **Reset**, then release **Boot**. This forces download mode.

## Option 1: esptool (Windows, macOS, Linux)

Install [esptool](https://docs.espressif.com/projects/esptool/) (v4.8 or newer):

```sh
pip install esptool
```

Flash the merged image:

```sh
esptool --chip esp32p4 -p <PORT> -b 460800 write-flash 0x0 p4x_eye_face_recorder_merged.bin
```

Or flash the separate images:

```sh
esptool --chip esp32p4 -p <PORT> -b 460800 write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
    0x2000  bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 p4x_eye_face_recorder.bin
```

With esptool older than v5, the commands are `esptool.py` and `write_flash`.

Press **Reset** after flashing if the board does not restart on its own.

## Option 2: Browser (Chrome or Edge)

1. Open the [Espressif ESP Launchpad](https://espressif.github.io/esp-launchpad/) or the
   [esptool-js flasher](https://espressif.github.io/esptool-js/).
2. Click **Connect** and select the board's serial port.
3. Add `p4x_eye_face_recorder_merged.bin` at address **0x0**.
4. Click **Program**, then reset the board when it finishes.

## Option 3: Espressif Flash Download Tool (Windows)

1. Chip type **ESP32-P4**, work mode **Develop**, load mode **UART**.
2. Add the three separate images at `0x2000`, `0x8000` and `0x10000` (tick each row), or only the
   merged image at `0x0`.
3. Set SPI speed 80 MHz, SPI mode DIO, flash size 16 MB, select the COM port and press **START**.

## After flashing

- Insert a FAT32 / exFAT microSD card. Clips (MJPEG video + AAC audio in MP4, about 3–5 MB per
  second) are saved to `FACEREC/` on the card. Use a U1 card at least; U3 / V30 is recommended.
- Serial log: 115200 baud on the Debug port.
- Upgrading from an older version keeps your saved settings. To reset them, hold Button 3 for 2 s.
  To wipe everything, run `esptool --chip esp32p4 -p <PORT> erase-flash` before flashing.

## Verifying the download

```sh
shasum -a 256 -c SHA256SUMS.txt      # macOS / Linux
certutil -hashfile p4x_eye_face_recorder_merged.bin SHA256   # Windows
```
