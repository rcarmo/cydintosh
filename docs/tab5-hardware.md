# M5Stack Tab5 ESP32-P4 hardware notes

This branch targets the **M5Stack Tab5 / ESP32-P4 only** for Macintosh LC color
experiments. Existing ESP32/CYD and ESP32-S3 Mac Plus profiles are intentionally
out of scope for LC bring-up.

## Connected device fingerprint

Detected with `esptool v5.2.0` via USB-Serial/JTAG:

| Field | Value |
|---|---|
| Board class | M5Stack Tab5 |
| SoC | ESP32-P4 |
| Chip revision | v1.3 |
| CPU | Dual Core + LP Core, 400MHz reported by esptool |
| Crystal | 40MHz |
| USB mode | USB-Serial/JTAG |
| MAC | `80:f1:b2:d1:46:0d` |
| Flash manufacturer/device | `0x46 / 0x4018` |
| Detected flash size | 16MB |
| Host serial path | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D1:46:0D-if00` |

PlatformIO currently lists board `m5stack-tab5-p4` as:

```text
m5stack-tab5-p4  ESP32P4  360MHz  16MB  500KB  M5STACK Tab5 esp32-p4 Board (ES pre rev.300)
```

The attached device reports ESP32-P4 rev v1.3. A first build attempt with
`m5stack-tab5-p4` selected the `esp32p4_es` chip variant and failed to link
(`sram_low`/`sram_high` linker region mismatch). The branch therefore keeps the
Tab5-specific environment name but uses PlatformIO board `esp32-p4_r3` until a
rev-v1.3 Tab5 board definition is available.

## Original flash backup

A full 16MB backup was read before any experimental flashing:

| Artifact | Value |
|---|---|
| Raw image | `/workspace/backups/m5stack-tab5/m5stack-tab5-esp32p4-flash-20260525-151803Z.bin` |
| Archive | `/workspace/backups/m5stack-tab5/m5stack-tab5-esp32p4-flash-20260525-151803Z.tar.xz` |
| Raw SHA256 | `a260eda93e4e5ddc1d3abddd361b1eb326c07d4248b67d1b493feba057fde231` |
| Backup command | `esptool --port <tab5-port> --baud 921600 read-flash 0x0 0x1000000 <image.bin>` |

Restore command, if needed:

```bash
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D1:46:0D-if00
IMG=/workspace/backups/m5stack-tab5/m5stack-tab5-esp32p4-flash-20260525-151803Z.bin
/workspace/.venvs/pio/bin/python -m esptool --chip esp32p4 --port "$PORT" --baud 921600 write-flash 0x0 "$IMG"
```

Do not overwrite the backup artifacts from automated scripts.

## Display/touch hardware profile

Hardware details from the official M5Stack Tab5 documentation (`https://docs.m5stack.com/en/core/Tab5`):

| Area | Detail |
|---|---|
| Display | 5-inch 1280×720 IPS touchscreen |
| Display interface | MIPI-DSI |
| Display controller/path | `ILI9881C / ST7123` |
| Backlight | ESP32-P4 GPIO22, documented as `LEDA` |
| Touch controller/path | `GT911 (0x14) / ST7123 (0x55)` |
| Touch bus | I2C, SDA GPIO31, SCL GPIO32 |
| Touch interrupt | GPIO23 (`TP_INT`) |
| Flash | 16MB |
| PSRAM | 32MB hex/16-line PSRAM documented by M5Stack |
| Wireless coprocessor | ESP32-C6-MINI-1U, out of scope for initial LC bring-up |

The branch board header records these values in
`include/boards/m5stack_tab5_esp32p4_lc.h`. Display and touch drivers are not
implemented yet; next hardware milestone is a Tab5 display smoke test using the
MIPI-DSI panel path.

Open items:

- identify the ESP-IDF/M5Stack component stack needed for `ILI9881C / ST7123` MIPI-DSI;
- implement display init and backlight control for GPIO22;
- implement a display smoke test before enabling any LC emulation;
- implement touch probing for GT911/ST7123 on GPIO31/GPIO32;
- map touch to ADB mouse packets after the LC input model exists.

## Initial LC flash layout

Proposed `partitions-esp32p4-tab5-lc.csv` layout:

| Offset | Size | Label | Purpose |
|---:|---:|---|---|
| `0x009000` | `0x006000` | `nvs` | NVS |
| `0x00f000` | `0x001000` | `phy_init` | compatibility/reserved |
| `0x010000` | `0x400000` | `factory` | app |
| `0x410000` | `0x100000` | `rom` | LC ROM partition, 1MB aligned/reserved for 512KB ROM |
| `0x510000` | `0xAF0000` | `disk` | LittleFS/disk images and diagnostics |

Milestone 0 build result:

```text
pio run -e esp32-p4-tab5-lc-color
# SUCCESS with board=esp32-p4_r3, ESP-IDF 5.5.2, firmware.bin generated
```

The LC ROM itself is not committed. Use local `vendor/mac-lc.rom` only.
Validate and flash it explicitly with:

```bash
make lc-rom-info
make flash-tab5-lc-rom
```

`flash-tab5-lc-rom` writes only the local 512KB ROM image to the Tab5 LC ROM
partition at `0x410000`; it does not erase the full device or commit/copy ROM
contents into the repository.
