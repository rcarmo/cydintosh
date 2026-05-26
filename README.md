# Cydintosh

> Branch note: `feat/mac-lc-color` is an experimental Macintosh LC/color branch
> for **ESP32-P4 / M5Stack Tab5 only**. The current Mac Plus ESP32/CYD and
> ESP32-S3 profiles remain separate and should not be treated as LC targets.
> See [`docs/mac-lc-color-plan.md`](docs/mac-lc-color-plan.md),
> [`docs/tab5-hardware.md`](docs/tab5-hardware.md), and the LC/Tab5 docs under
> [`docs/`](docs/).

Cydintosh is a Macintosh Plus emulator for small ESP32 display boards. It runs
[umac](https://github.com/likeablob/umac) with the Musashi 68000 core, renders a
patched Mac framebuffer to the attached LCD, and stores the emulated boot disk in
LittleFS.

This fork focuses on repeatable multi-board builds, browser-flashable artifacts,
and reliable bring-up on both the original CYD2USB board and the larger
ESP32-S3 800×480 RGB panel board.

## Current status

| Target | Status |
|---|---|
| ESP32-2432S028 / CYD2USB | Build-compatible legacy target. Uses 128KB emulated Mac RAM and the original 240×320 display path. |
| ESP32-2432S028 Mac512×384 rotated-fit | Experimental CYD2USB profile for a larger square-pixel Mac framebuffer downsampled to the 240×320 LCD. |
| ESP32-8048S043C | Hardware-validated ESP32-S3 target. Boots System 6 + After Dark 2 from a 1.44MB HFS image, fills the 800×480 panel in portrait mode, and runs at roughly 65–67 FPS in recent serial captures. |
| M5Stack Tab5 / ESP32-P4 LC color | Experimental branch-only target. Builds a diagnostic Macintosh LC/color skeleton with 512KB ROM mmap validation, 68EC020/68020 Musashi scaffold, 4MB RAM/VRAM probes, indexed-color renderer scaffolding, Tab5 backlight/touch/display smoke diagnostics, disk trace scaffolding, and bounded ROM-entry probes. Latest probe advances through the NuBus/slot-video probe and stops in the ROM serial monitor no-input poll; no claimed LC boot yet. |

Latest validated ESP32-8048S043C characteristics:

- ESP32-S3 bootloader at `0x0`, partition table at `0x8000`, app at `0x10000`.
- Patched Mac Plus ROM partition at `0x410000`.
- LittleFS disk partition at `0x430000`.
- 8MB octal PSRAM detected and tested by ESP-IDF.
- 480×800 emulated Mac framebuffer rotated clockwise to the 800×480 RGB panel.
- 1MB emulated Mac RAM on the ESP32-S3 profile.
- GT911 touch controller detected at `0x5d`.
- Disk image mounted read-only from LittleFS.
- Recent verified log: `logs/serial-capture-20260513-195248.log`.

## Repository layout

| Path | Purpose |
|---|---|
| `include/board_profiles.h` | Board-profile selector and compile-time sanity checks. |
| `include/boards/` | Per-board display, touch, memory, and storage profiles. |
| `src/` | ESP-IDF application, display, touch, disk, Wi-Fi/IPC, and hardware control code. |
| `external/umac/` | umac emulator submodule used by the firmware. |
| `tools/` | ROM patching, LC ROM/disk metadata inspection, LC video-pattern rendering, serial capture, MacBinary flag, and disk tooling helpers. |
| `data/` | Build-time LittleFS content directory. Generated/user-supplied disk images are ignored except `.gitkeep`. |
| `vendor/` | User-supplied Mac ROM/disk inputs. Ignored except `.gitkeep`. |
| `web/` | Browser flasher page and generated firmware artifacts. |
| `mac-app/` | Classic Mac app sources built with Retro68. |
| `enclosure/` | Enclosure CAD files and documentation. |

## LC/Tab5 documentation map

| Document | Purpose |
|---|---|
| `docs/README.md` | Documentation index for the LC/Tab5 branch notes. |
| `docs/mac-lc-color-plan.md` | Branch scope, current LC skeleton status, milestones, and safety rules. |
| `docs/tab5-hardware.md` | Tab5 hardware fingerprint, backup/restore commands, flash layout, and current diagnostics. |
| `docs/tab5-display-component-audit.md` | M5Stack Tab5 demo/BSP audit for ILI9881C/ST7123 MIPI-DSI and touch paths. |
| `docs/musashi-lc-cpu-audit.md` | Musashi 68020/68EC020 configuration audit and LC-only CPU scaffold notes. |
| `docs/lc-boot-media.md` | Local-only `vendor/lc-disk.img` workflow and disk trace/read-only policy. |
| `docs/lc-via-scc-audit.md` | Mac Plus VIA/SCC reuse audit and LC hardware-stub gaps. |

## Hardware targets

### ESP32-2432S028 / CYD2USB

Original Cheap Yellow Display target.

| Component | Detail |
|---|---|
| SoC | ESP32-D0WD class module |
| Flash | 4MB typical |
| PSRAM | Not used / not assumed |
| Display | ILI9341 SPI LCD, 240×320 |
| Touch | XPT2046 resistive touch, separate SPI bus |
| RGB LED | Active-low RGB LED on GPIO 4/16/17 |
| Emulated RAM | 128KB |
| Default Mac framebuffer | 240×320 |

Important pins:

| Function | GPIO |
|---|---:|
| TFT MOSI | 13 |
| TFT MISO | 12 |
| TFT CLK | 14 |
| TFT CS | 15 |
| TFT DC | 2 |
| TFT backlight | 21 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch CLK | 25 |
| Touch CS | 33 |

### Sunton ESP32-8048S043C

Validated ESP32-S3 board with an 800×480 RGB DPI panel.

| Component | Detail |
|---|---|
| Board | Sunton ESP32-8048S043C |
| SoC | ESP32-S3 QFN56, revision v0.2 observed |
| CPU | Dual-core Xtensa LX7, 240MHz |
| Flash | 16MB, DIO flash mode in this project |
| PSRAM | 8MB octal PSRAM, 80MHz |
| USB serial | CH340, typically `/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0` |
| Display | 800×480 RGB DPI LCD |
| Touch | GT911 capacitive touch on I2C |
| Backlight | GPIO2 PWM |
| Emulated RAM | 1MB |
| Mac framebuffer | 480×800 portrait, rotated clockwise to fill the physical panel |

RGB/touch pin mapping:

| Function | GPIO |
|---|---:|
| Backlight PWM | 2 |
| RGB DE | 40 |
| RGB HSYNC | 39 |
| RGB VSYNC | 41 |
| RGB PCLK | 42 |
| Blue data B0..B4 | 8, 3, 46, 9, 1 |
| Green data G0..G5 | 5, 6, 7, 15, 16, 4 |
| Red data R0..R4 | 45, 48, 47, 21, 14 |
| GT911 SDA | 19 |
| GT911 SCL | 20 |

### M5Stack Tab5 / ESP32-P4 LC color

Experimental branch-only target for Macintosh LC/color bring-up. This target is
separate from the Mac Plus firmware path and currently builds a diagnostic
skeleton, not a booting LC emulator.

| Component | Detail |
|---|---|
| Board | M5Stack Tab5 |
| SoC | ESP32-P4 rev v1.3 observed |
| CPU | Dual Core + LP Core; branch targets the rev-v1.x 360MHz PlatformIO board path |
| Flash | 16MB |
| PSRAM | 32MB documented by M5Stack |
| USB serial/JTAG | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D1:46:0D-if00` |
| Display | 5-inch MIPI-DSI path, physical target `720×1280` in the BSP/demo code |
| Display controller/path | ILI9881C and/or ST7123 path in M5Stack demo code |
| Backlight | GPIO22 / `LEDA`, via M5Stack BSP LEDC path in display-smoke image |
| Touch | ST7123 at `0x55` confirmed on current unit; GT911 fallback at `0x14`; I2C GPIO31/GPIO32, INT GPIO23 |
| LC ROM | local-only `vendor/mac-lc.rom`, 512KB, not committed |
| LC boot disk | local-only `vendor/lc-disk.img`, read-only workflow, not committed |
| Guest RAM target | 4MB initial PSRAM-backed target, 2MB fallback probe |
| Guest video target | 512×384, 8-bit indexed color, dirty-row RGB565 strip conversion and Tab5 partial-flush scaffold |

Current LC/P4 diagnostics include ROM partition mmap validation, on-device ROM
vector candidate scanning plus ROM-header entry hints, LC-only Musashi
68EC020/68020 configuration, Musashi callback wiring to the LC memory bus,
trace/perf counters, provisional memory decoder, a bounded LC memory-bus harness
with PSRAM RAM + mapped ROM + masked ROM alias + named generic I/O stubs, a
bounded ROM-entry micro-probe, read-only disk trace scaffolding, Tab5
backlight/touch probes,
software-only display pattern checksums, and a visually confirmed M5Stack-BSP-based
physical display path. The normal LC diagnostic now initializes the BSP panel and
renders the LC `512×384×8bpp` indexed debug pattern scaled into the Tab5 panel
through reusable BSP `init`/`flush_indexed` and
dirty-row `flush_indexed_dirty` paths. The dirty-row self-test currently inverts a
24-row LC band and updates only the corresponding centered-viewport physical rows
before entering the brightness heartbeat. The display-smoke heartbeat also
initializes the confirmed ST7123 touch driver and polls touch samples, mapping raw
`720×1280` panel coordinates into the centered LC `512×384` viewport for later ADB
mouse work. The latest LC skeleton capture verified the flashed LC ROM partition
after reflashing `vendor/mac-lc.rom`: first long `0x350eacf0`, 13 heuristic
vector-like pairs in the first `0x4000` bytes, and ROM-header trampolines that
point to `0x0040008c` as the first guarded 24-bit execution target. The bounded
ROM-entry micro-probe reaches the guest `RESET` instruction and advances into the
ROM dispatcher. The first explicit `early-rom-probe-1c00-stride` I/O probes at
`0x00f01c00`, `0x00f21c00`, and `0x00f41c00` now use provisional VIA-style IER
set/clear/readback behavior, which advances past the previous repeated
2832-read/3776-write loop. The decoder also maps the 68EC020-masked
`0x00800000` ROM alias after the guest moves toward `0x408xxxxx` PCs. The current
seeded `0x00402e00` reset-body probe uses the reset trampoline's caller
continuation (`a6=0x004000b4`); without that seed it falls into zero-filled RAM
through the ROM header. With the seed, local macemu/BasiliskII references identify the same
`0x50f00000 / 0x50f14000` address family as a physical NuBus/slot video probe
that BasiliskII skips with ROM patches and replaces with a Slot Manager video
resource. Cydintosh does not patch the LC ROM, so `early-f14000-device` now
reports only the observed ready/complete bits at `+0x0804`. The latest 100M-cycle
capture (`logs/serial-capture-20260526-212157.log`) advances through that probe
and reaches the ROM diagnostic/serial monitor guard at `0x40849eae` after
49.5M cycles, with `d7=0x01000304`. `0x00f04000` remains the next SCC-like
no-input status/data block. Addresses above the
configured 4MB RAM and below the I/O window, plus the top 16 bytes of the 24-bit
address space, are modeled as non-present RAM-size probes. The memory-bus harness validates 4MB PSRAM RAM
reads/writes, ROM window reads, generic I/O stub reads/writes, ROM write blocking,
RAM-size probe handling, and unmapped-read logging. A RAM-only synthetic 68EC020 smoke program validates
`m68k_init()`/`m68k_set_cpu_type()`/`m68k_pulse_reset()` and bounded
`m68k_execute()` through the LC bus callbacks (`reset_pc=0x100`, `reset_sp=0x2000`,
`cpu_type=3`); full LC boot remains disabled. Current hardware/user confirmation:
the normal `esp32-p4-tab5-lc-color` diagnostic visibly shows the LC test pattern
and uses the BSP display/backlight path, avoiding the raw GPIO22 LEDC fallback
unless panel init fails. The Tab5 USB/JTAG device must be present before flashing
or validating hardware output.

## Board profiles and PlatformIO environments

| PlatformIO environment | Board macro | Profile header | Notes |
|---|---|---|---|
| `esp32-cyd2usb` | `CYD_BOARD_ESP32_2432S028` | `include/boards/esp32_2432s028.h` | Default legacy ESP32 CYD target. |
| `esp32-cyd2usb-mac512x384-rotfit` | `CYD_BOARD_ESP32_2432S028_MAC512X384_ROTFIT` | `include/boards/esp32_2432s028_mac512x384_rotfit.h` | Experimental 512×384 framebuffer downsampled to CYD LCD. |
| `esp32dev` | extends `esp32-cyd2usb` | `include/boards/esp32_2432s028.h` | Backward-compatible alias for older scripts/docs. |
| `esp32-8048s043c` | `CYD_BOARD_ESP32_8048S043C` | `include/boards/esp32_8048s043c.h` | ESP32-S3 N16R8, 800×480 RGB panel, GT911 touch. |
| `esp32-p4-tab5-lc-color` | `CYD_BOARD_M5STACK_TAB5_ESP32P4_LC` + `CYD_MACHINE_MAC_LC` | `include/boards/m5stack_tab5_esp32p4_lc.h` | Experimental Tab5/P4-only Macintosh LC/color skeleton. Uses `m68kconf_lc.h` and does not start Mac Plus `umac`. |
| `esp32-p4-tab5-display-smoke` | `CYD_BOARD_M5STACK_TAB5_ESP32P4_LC` + `CYD_MACHINE_MAC_LC` | `include/boards/m5stack_tab5_esp32p4_lc.h` | Tab5-only M5Stack BSP physical MIPI-DSI smoke image; no LC emulation. |
| `esp32-p4-tab5-bootdiag` | `CYD_BOARD_M5STACK_TAB5_ESP32P4_LC` + `CYD_MACHINE_MAC_LC` | `include/boards/m5stack_tab5_esp32p4_lc.h` | Minimal GPIO22/PI4IOE boot isolation image; no LC emulation. |

Profile responsibilities:

- select exactly one LCD backend;
- select the touch backend and transform parameters;
- define physical panel dimensions;
- define render scale/rotation/offsets;
- define emulator task stack size;
- define board storage policy such as read-only disk image use.

`include/board_profiles.h` performs compile-time checks so incompatible display
or touch settings fail early.

## Flash layout

### ESP32-2432S028 / CYD2USB (`partitions.csv`)

| Offset | Content |
|---:|---|
| `0x001000` | ESP32 bootloader |
| `0x008000` | Partition table |
| `0x010000` | Application firmware |
| `0x210000` | Patched Mac Plus ROM |
| `0x230000` | LittleFS filesystem containing `disk.img` |

### ESP32-8048S043C (`partitions-16mb.csv`)

| Offset | Content |
|---:|---|
| `0x000000` | ESP32-S3 bootloader |
| `0x008000` | Partition table |
| `0x010000` | Application firmware |
| `0x410000` | Patched Mac Plus ROM |
| `0x430000` | LittleFS filesystem containing `disk.img` |

### M5Stack Tab5 / ESP32-P4 LC (`partitions-esp32p4-tab5-lc.csv`)

| Offset | Content |
|---:|---|
| `0x009000` | NVS |
| `0x00f000` | PHY/reserved |
| `0x010000` | Application firmware |
| `0x410000` | LC ROM partition, 1MB reserved for local 512KB `vendor/mac-lc.rom` |
| `0x510000` | `disk` LittleFS/data partition reserved for LC disk diagnostics/images |

Use board-specific full-flash images whenever possible. Firmware-only images do
not contain the ROM or disk filesystem, and ESP32, ESP32-S3, and ESP32-P4
bootloader/flash layouts differ.

## Requirements

Host tools used by this workspace:

- Bun/Node is not required for the firmware itself, but is available in this workspace.
- Python 3.
- PlatformIO (`pio`).
- ESP-IDF packages installed by PlatformIO.
- `esptool` for manual flash/verify operations.
- `hfsutils` (`hmount`, `hcopy`, `hls`, `hdel`, `humount`) for HFS image work.
- Docker only for the optional Retro68 Mac app build flow.

Project assets you must provide yourself:

- Mac Plus ROM v3, 128KB, checksum `4D1F8172`, placed at `vendor/rom.bin` for Mac Plus targets.
- A Mac Plus bootable HFS disk image placed at `vendor/disk.img` or `data/disk.img` for existing Mac Plus targets.
- Macintosh LC ROM, 512KB, first long `0x350EACF0`, placed at `vendor/mac-lc.rom` for the Tab5 LC target.
- Optional Macintosh LC boot disk image at `vendor/lc-disk.img`; the LC workflow keeps it read-only during bring-up.

The `vendor/` and generated disk/ROM images are intentionally ignored by git.

## Prepare

```bash
# Prepare umac/Musashi generated files and default local config
make prepare

# Or manually:
make -C external/umac prepare
cp include/user_config.h.tmpl include/user_config.h
```

Generate board-appropriate patched ROM images:

```bash
# Default CYD2USB 240×320 ROM patch
make prepare-rom PIO_ENV=esp32-cyd2usb

# ESP32-S3 480×800 ROM patch
make prepare-rom PIO_ENV=esp32-8048s043c

# Experimental CYD2USB 512×384 ROM patch
make prepare-rom PIO_ENV=esp32-cyd2usb-mac512x384-rotfit
```

Seed the LittleFS data directory:

```bash
mkdir -p data
cp vendor/disk.img data/disk.img
```

At runtime the firmware mounts the LittleFS partition at `/disk`, so this file is
opened by the emulator as `/disk/disk.img`.

For current ESP32-S3 testing, the disk image is a 1.44MB HFS image with System 6
and After Dark 2. The ESP32-S3 profile mounts it read-only because the minimal
Sony write path is not yet robust enough for guest desktop metadata writes.

## Build

```bash
# Default CYD2USB build
make build PIO_ENV=esp32-cyd2usb

# ESP32-S3 8048S043C build
make build PIO_ENV=esp32-8048s043c

# Experimental CYD2USB 512×384 rotated-fit build
make build PIO_ENV=esp32-cyd2usb-mac512x384-rotfit

# Experimental ESP32-P4 / M5Stack Tab5 LC-color diagnostic skeleton
make build-tab5-lc

# M5Stack BSP-based physical Tab5 display smoke image
make build-tab5-display-smoke

# Temporary minimal Tab5 boot/backlight isolation image
make build-tab5-bootdiag
```

Direct PlatformIO commands are also supported:

```bash
pio run -e esp32-cyd2usb
pio run -e esp32-8048s043c
pio run -e esp32-cyd2usb-mac512x384-rotfit
pio run -e esp32-p4-tab5-lc-color
pio run -e esp32-p4-tab5-display-smoke
pio run -e esp32-p4-tab5-bootdiag
```

Build just the LittleFS image:

```bash
make fs PIO_ENV=esp32-8048s043c
```

## Generate browser flasher artifacts

```bash
make stable-artifacts PIO_ENV=esp32-cyd2usb
make stable-artifacts PIO_ENV=esp32-8048s043c
make stable-artifacts PIO_ENV=esp32-cyd2usb-mac512x384-rotfit
# LC/Tab5 currently uses explicit build/flash targets; do not publish as a stable artifact yet.
```

Generated artifact names:

- `web/bootloader-<env>.bin`
- `web/partitions-<env>.bin`
- `web/firmware-<env>.bin`
- `web/littlefs-<env>.bin`
- `web/full-flash-<env>.bin`

The full-flash image is the recommended artifact for installation.

## Flash

Preferred Makefile flow:

```bash
# CYD2USB
make flash-stable \
  PIO_ENV=esp32-cyd2usb \
  SERIAL_PORT=<serial-port>

# ESP32-8048S043C
make flash-stable \
  PIO_ENV=esp32-8048s043c \
  SERIAL_PORT=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
```

Manual full-image flash:

```bash
# Erase first for a clean install
esptool --port <serial-port> --baud 460800 erase_flash

# CYD2USB full image
esptool --port <serial-port> --baud 460800 write_flash \
  0x0000 web/full-flash-esp32-cyd2usb.bin

# ESP32-8048S043C full image
esptool --port <serial-port> --baud 460800 write_flash \
  0x0000 web/full-flash-esp32-8048s043c.bin
```

For disk-only iteration on ESP32-8048S043C:

```bash
make fs PIO_ENV=esp32-8048s043c
esptool --chip esp32s3 --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --baud 460800 write_flash 0x430000 .pio/build/esp32-8048s043c/littlefs.bin
esptool --chip esp32s3 --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --baud 460800 verify_flash 0x430000 .pio/build/esp32-8048s043c/littlefs.bin
```

For the experimental Tab5 LC target, use only the explicit Tab5 targets and the
USB/JTAG path recorded in `docs/tab5-hardware.md`:

```bash
make build-tab5-lc
make lc-rom-info
make lc-rom-vectors
make lc-disk-info
make lc-video-test-pattern
make flash-tab5-lc
make flash-tab5-lc-rom
make capture-tab5-logs

# Physical DSI smoke test: M5Stack BSP init, 720x1280 RGB565 stripes,
# then 35%/100% brightness heartbeat. Press power once if the panel is asleep.
make flash-tab5-display-smoke

# Temporary black-screen isolation image: no PSRAM boot init, no LC probes,
# PI4IOE reset/output subset, then GPIO22 high/low loop.
make flash-tab5-bootdiag
```

`make flash-tab5-lc-rom` writes only `vendor/mac-lc.rom` to the LC ROM partition
at `0x410000`. It does not erase the device or copy ROM contents into git.

## Browser flasher

The web flasher lives in `web/`:

- `web/index.html`
- `web/manifest.json`
- `web/server.py`

Run a local server for testing:

```bash
cd web
python3 server.py
```

Then open the printed local URL in Chrome or Edge with Web Serial support.

## Serial logs

Capture a reset-and-boot log:

```bash
/workspace/.venvs/pio/bin/python tools/capture_serial_logs.py \
  --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
  --baud 115200 \
  --duration 20
```

Expected ESP32-8048S043C boot markers:

- `esp_psram: Found 8MB PSRAM device`
- `lcd_cyd: Install RGB panel driver (800x480)`
- `touch: GT911 found at 0x5d`
- `Found ROM partition at offset 0x410000`
- `LittleFS mounted`
- `Opened /disk/disk.img, size=1474560, ro=1`
- `Starting emulation`
- `FB offset: 0xf4100 (expected FB_START=0xf4100, DISP=480x800)`
- recurring `BENCH` reports

## Disk image notes

The Mac Plus emulator accesses the boot disk through `src/disc_lfs.c` and umac's Sony
disk interface.

Current constraints:

- ESP32-S3 testing uses a read-only LittleFS disk image.
- System 6 + After Dark 2 needs more than the CYD2USB profile's 128KB emulated RAM; use the ESP32-S3 profile for that image.
- CYD2USB remains best suited to the smaller original System/Finder disk image.
- If you change `data/disk.img`, rebuild and flash the board-specific LittleFS or full-flash image.
- The LC branch expects optional local-only boot media at `vendor/lc-disk.img`; inspect it with `make lc-disk-info` and keep it read-only until LC disk I/O is validated.

HFS utility examples:

```bash
hmount data/disk.img
hls -la :
hls -la ':System Folder'
humount
```

## Development notes

- Prefer `make` targets when available.
- Keep board-specific settings in `include/boards/*.h` and environment-specific
  build flags in `platformio.ini`.
- Keep ROM dimensions in sync with the selected display profile. The Makefile
  sets `ROM_WIDTH`/`ROM_HEIGHT` automatically from `PIO_ENV` for known targets.
- `external/umac/external/Musashi/m68kconf.h` must resolve to this project's
  `include/m68kconf.h`; `make prepare` handles this.
- Wi-Fi is skipped at runtime when `WIFI_SSID` remains unset/placeholder, which
  saves memory and avoids pointless scan/retry loops.

## Useful commands

```bash
# See all project targets
make help

# Build primary Mac Plus profiles and the LC/P4 skeleton
make build-cyd2usb
make build-8048s043c
make build-tab5-lc

# LC/Tab5 metadata and diagnostics
make lc-rom-info
make lc-rom-vectors
make lc-disk-info
make lc-video-test-pattern

# Inspect connected serial devices
pio device list
ls -l /dev/serial/by-id/

# Clean generated web artifacts
make clean
```

## Original README

A copy of the previous README is kept as `README.original.md` in this workspace
for reference while this documentation is reorganized.
