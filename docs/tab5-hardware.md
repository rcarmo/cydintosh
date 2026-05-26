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

The attached device reports ESP32-P4 rev v1.3, so the branch now uses
`m5stack-tab5-p4` for Tab5 environments. A later attempt with the generic
`esp32-p4_r3` board produced rev3/400MHz linker and bootloader settings; on this
unit that trapped before `app_main` with `Illegal instruction` at the bootloader
entry. The working configuration matches the official `M5Tab5-UserDemo` after
patching it for ESP32-P4 rev <3.0: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`,
minimum rev `100`, maximum rev `199`, and 360MHz CPU.

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
`include/boards/m5stack_tab5_esp32p4_lc.h`. The branch now vendors the
M5Stack display BSP under `components_tab5/m5stack_tab5` and provides a separate
hardware display-smoke image (`esp32-p4-tab5-display-smoke`) that initializes
SYS-I2C/PI4IOE, detects GT911 vs ST7123, initializes the MIPI-DSI panel, draws a
physical `720×1280` RGB565 stripe/orientation pattern, and then pulses backlight
between 35% and 100%.

Open items:

- capture a user-touch coordinate sample now that the ST7123 reader initializes;
- calibrate/orient the touch-to-LC viewport transform with real touches;
- replace display-smoke debug draws with ROM/System-driven LC VRAM updates;
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

Milestone 0/1 build results:

```text
pio run -e esp32-p4-tab5-lc-color
# SUCCESS with board=m5stack-tab5-p4, ESP-IDF 5.5.2, firmware.bin generated

pio run -e esp32-p4-tab5-display-smoke
# SUCCESS; BSP display smoke image builds, serial logs backlight heartbeat,
# and hardware camera/user confirmation shows visible pattern rendering/flashing
```

Hardware flashing requires the Tab5 USB/JTAG path to be present under
`/dev/serial/by-id/`. On the current Tab5 USB-Serial/JTAG path, log capture reset
is reliable with DTR low/false during the RTS reset pulse; this captures the full
boot log including the ESP-ROM banner and early LC diagnostics. `make
capture-tab5-logs` uses PlatformIO's Python plus `--dtr-during-reset false
--no-clear-after-reset` so very early boot output is not discarded.

The LC ROM itself is not committed. Use local `vendor/mac-lc.rom` only.
Validate, inspect vector/window candidates, and flash it explicitly with:

```bash
make lc-rom-info
make lc-rom-vectors
make flash-tab5-lc-rom
```

`flash-tab5-lc-rom` writes only the local 512KB ROM image to the Tab5 LC ROM
partition at `0x410000`; it does not erase the full device or commit/copy ROM
contents into the repository.

The firmware maps the first `0x80000` bytes of that partition read-only with
`esp_partition_mmap()` and logs the mapped base, size, and first two big-endian
longwords. It also probes LC guest RAM allocation in PSRAM with a 4MB primary
request and a 2MB fallback request.

Current address-map diagnostics are deliberately provisional and log both
24-bit-first and 32-bit candidate windows:

| Guest range | Purpose | Status |
|---:|---|---|
| `0x00000000`–`0x003fffff` | 4MB RAM target | allocation probe only |
| `0x00400000`–`0x0047ffff` | 24-bit ROM candidate | guarded entry probe reaches ROM dispatcher from `0x0040008c` |
| `0x40800000`–`0x4087ffff` | 32-bit ROM candidate | guest PC can move here after early ROM setup |
| `0x00800000`–`0x0087ffff` | masked ROM alias | maps the same 512KB ROM for 68EC020 callback fetches from masked `0x408xxxxx` PCs |
| `0x00400000`–`0x00efffff` excluding ROM | non-present RAM-size probe | ignored writes/read `0xff` above configured 4MB so ROM can size RAM safely |
| `0x00fffff0`–`0x00ffffff` | top-of-24-bit RAM-size probe | ignored writes/read `0xff` for ROM top-of-space memory tests |
| `0x00f00000`–`0x00ffffff` | 24-bit I/O candidate | named stubs; first ROM accesses at VIA-like `0x00f?1c00`, then `0x00f01e00/0600/0400/0000`; `0x00f14000`-class range separated as `early-f14000-device` |
| `0x50000000`–`0x500fffff` | 32-bit I/O candidate | placeholder decoder only |

The decoder includes throttled unmapped and I/O-stub access loggers for bounded
CPU execution diagnostics. Full guest boot is not enabled; only a short ROM-entry
micro-probe is run to discover the next missing hardware ranges.

Display-memory diagnostics currently probe:

| Buffer | Size formula | Allocation caps | Purpose |
|---|---:|---|---|
| indexed LC VRAM | `512×384×8bpp` (`196608` bytes) | PSRAM, 8-bit | guest color framebuffer candidate |
| RGB565 DMA strip | `512×16×2` (`16384` bytes) | internal, DMA, 8-bit | preferred panel-transfer staging buffer |
| full RGB565 frame | `512×384×2` (`393216` bytes) | logged only | intentionally avoided until needed |

The first renderer should prefer dirty rows/strips rather than a full RGB565
shadow framebuffer.

A small LC trace ring buffer (`LC_TRACE_RING_SIZE`, default 128 entries) records
skeleton markers, CPU config, ROM vector candidates, decoded memory accesses, and
unmapped accesses. The current skeleton dumps the most recent entries at the end
of diagnostics; later CPU execution/panic paths can reuse the same dump routine.
The firmware-side ROM vector scanner now checks the mapped flash partition as
well as the host-side `make lc-rom-vectors` path. With the LC ROM partition
reflashed and verified, the hardware log found 13 heuristic vector-like pairs in
the first `0x4000` bytes and selected current best vector-like candidate
`file_offset=0x00d58 sp=0x00186100 pc=0x00842f00 rom_base=0x40800000`; this is
still considered noise compared with the ROM-header entry hints. The bounded
entry micro-probe originally started at `0x0040008c`, invoked the guest `RESET`
callback once, and recorded first ROM I/O-stub probes at `0x00f01c00`,
`0x00f21c00`, and `0x00f41c00`. Alternate ROM-header probes showed that a bare
`0x00402e00` entry is invalid because it reaches `jmp (a4)` at `0x40802e7a` with
`a4=0x40400000`, executes the ROM header/fingerprint bytes, trips an A-line
exception while low vectors are still zero, and falls into zero-filled RAM. The
current `0x00402e00` diagnostic seeds the reset trampoline continuation
(`a6=0x004000b4`), which avoids the zero-RAM trap and sets `vbr=0x40846140`. The
explicit `early-rom-probe-1c00-stride` stub now behaves as a provisional VIA IER
alias instead of constant `0xff`: the former repeated 2832-read/3776-write loop
advances after IER-style set/clear/readback behavior. The masked ROM alias then
lets the guest continue with `0x408xxxxx` PCs while Musashi fetch callbacks
request `0x008xxxxx` addresses. Addresses above the configured 4MB RAM and below
`0x00f00000`, plus the top 16 bytes of the 24-bit address space, are treated as
non-present RAM-size probes. Early VIA-like accesses are summarized at
`0x00f01e00`, `0x00f00600`, `0x00f00400`, and `0x00f00000`.

A local macemu/BasiliskII reference search identifies this `0x00f14000` 24-bit
alias as the same `0x50f14000` physical NuBus/slot video-probe family described
in `BasiliskII/docs/AARCH64_JIT_BRINGUP.md`; BasiliskII's `rom_patches.cpp` skips
these physical probes and installs a generated Slot Manager declaration ROM in
`slot_rom.cpp` instead. Cydintosh keeps the LC ROM unpatched, so the current
`early-f14000-device` stub reports only the observed ready/complete bits at
`+0x0804`. One-bit `0x02` status advanced out of the inner byte-output loop but
stopped in the outer wait at `0x40845e3a`; `0x03` status advances through the
slot/video probe. The `serial-capture-20260526-212157.log` capture then reached
the broad ROM diagnostic/serial monitor guard at `0x40849eae` after 49.5M cycles,
with `d7=0x01000304` and first `0x00f04000` status read value `0x04`.
`early-f14000-device` recorded 417 reads and 319 writes in that run; offset
`0x0804` was read 294 times with final value `0x03`. The `0x00f04000`-class range
is now modeled as SCC-like no-input status/data (`early-f04000-device`): `+0` and
`+4` return `0x04`, `+2` returns a one-shot `0x05` only after transmit-data writes
to `+6` and otherwise `0x04`, and `+6` returns `0x00`. The latest capture
(`serial-capture-20260526-213538.log`) advances through monitor initialization
and stops at `0x40849fca`, the serial command/read poll, with `d0=0x00008000`,
`d7=0x01020304`, and no fake receive input.

The previous `0x0080xxxx` masked-ROM write stream was reclassified as an artifact
of continuing through zero-filled RAM after the invalid unseeded `0x00402e00`
entry path, not as evidence of normal reset-body progress.

The early memory-write policy is controlled by `LC_PANIC_ON_UNEXPECTED_WRITE`
(default `1`). In this scaffold, RAM and I/O-candidate writes are expected; ROM
and unmapped writes are tagged as `would-panic` so the first CPU execution pass
can fail loudly before hardware stubs are relaxed. The memory-bus harness and
bounded ROM-entry micro-probe allocate 4MB PSRAM guest RAM, attach the mapped
512KB ROM, return `0xff` for generic I/O stub reads, accept/log generic I/O
writes, block ROM writes, and log unmapped reads. Hardware capture validates:

```text
LC memory bus initialized: ram=... size=0x400000 rom=... size=0x80000
LC memory bus harness: ram_write=ESP_OK ram_read=0x12345678 tail_write=ESP_OK tail_read=0xa5a55a5a
LC memory bus harness: rom24[0]=0x350eacf0 rom24[4]=0x0000002a rom32[0]=0x350eacf0
LC memory bus harness: io_read=0xff io_write=ESP_OK rom_write_blocked=ESP_ERR_INVALID_STATE unmapped_read=0xff
```

CPU trace helper hooks are available for exception-vector hits,
illegal/unimplemented instructions, bus errors, address errors, and interrupt
levels. Musashi is now linked into the Tab5 LC target and wired to the LC memory
bus through LC-specific callbacks (`cpu_read_*`, `cpu_write_*`, function-code,
reset, interrupt-ack, and instruction-hook surfaces). The hardware smoke probe
executes only a synthetic RAM program, not the LC ROM:

```text
lc_musashi_bus: wrote synthetic 68k smoke program: sp=0x00002000 pc=0x00000100
lc_cpu: LC synthetic 68EC020 bus probe: reset_pc=0x00000100 reset_sp=0x00002000 cycles=64 pc_after=0x00000104 sr=0x2704 cpu_type=3
lc_musashi_bus: Musashi callback stats: fc=6 reset_callbacks=0 irq_acks=0 instruction_callbacks=3
```

Actual LC ROM execution still waits for verified ROM overlay/reset-vector mapping
and device-specific hardware stubs.

Performance counter scaffolding (`src/machine_lc/lc_perf.c`) tracks count, total,
minimum, average, and maximum microseconds for future CPU loop, video update,
host render, and display flush phases, plus frame/FPS totals.

Backlight control (`src/machine_lc/tab5_backlight.c`) uses LEDC on GPIO22
(`LEDA`) with the same basic LEDC settings as the M5Stack demo BSP: low-speed
LEDC timer0/channel1, 5kHz, 12-bit duty. The safe default boot brightness is
`TAB5_BACKLIGHT_BOOT_PERCENT=20`. It is Tab5/P4-only and does not initialize the
MIPI-DSI panel. The skeleton initializes backlight at the start of `app_main`,
runs a strong 0%→100% boot pulse, and then enters a repeating heartbeat pulse at
the end of diagnostics so app startup can be observed even if USB console logs are
silent.

A separate minimal boot diagnostic environment is available as
`esp32-p4-tab5-bootdiag` / `make flash-tab5-bootdiag`. It disables boot-time
PSRAM init and skips LC probes, then loops forever toggling GPIO22 high/low with
1.5s periods. Before entering the loop it also initializes the Tab5 SYS-I2C
PI4IOE expanders with the same reset/output subset used by the M5Stack BSP,
releasing LCD/touch reset-related expander outputs. This is a temporary isolation
image for black-screen bring-up; it should not be mistaken for the normal LC
skeleton firmware.

A software-only physical-panel smoke scaffold (`src/machine_lc/tab5_display_smoke.c`)
generates RGB565 strip checksums for the Tab5 DSI target mode (`720×1280`). The
hardware smoke path (`src/machine_lc/tab5_bsp_display_smoke.c`) uses the vendored
M5Stack BSP to initialize the real panel. It first validated generic RGB565
stripes/orientation markers, then was updated to draw the LC `512×384×8bpp`
indexed debug pattern scaled to a `720×540` centered viewport on the `720×1280`
physical panel. It now exposes reusable `tab5_bsp_display_init()`,
`tab5_bsp_display_flush_indexed()`, and `tab5_bsp_display_flush_indexed_dirty()`
paths so later LC VRAM updates can use the same BSP panel handle. Serial capture
after flashing showed the app alive, flushed the LC indexed framebuffer with
checksum `0x3b4a1479`, validated a dirty-row partial update for LC rows `180-203`
(`34` physical rows, `3` strips, checksum `0x33892af9`), and repeatedly set
backlight to 35%/100%; camera/user confirmation verified the BSP pattern renders
and flashes. The normal LC diagnostic target now also initializes the same BSP
panel path and draws the LC indexed test pattern before continuing ROM/RAM/CPU
serial diagnostics. User confirmation on 2026-05-26: “I see the test pattern.”
When the BSP panel initializes, the LC target uses the BSP brightness path and
skips the raw GPIO22 backlight fallback to avoid LEDC ownership conflicts.

Touch support (`src/machine_lc/tab5_touch.c`) now reuses the M5Stack BSP system
I2C bus on GPIO31/GPIO32, probes GT911 at `0x14` plus ST7123 at `0x55`, and
initializes the matching Espressif `esp_lcd_touch` driver. The current Tab5 unit
ACKs ST7123 at `0x55`; serial capture shows firmware `3(1.71.1.3)`, max touch
coordinates `720×1280`, and max touches `10`. The display-smoke heartbeat polls
samples and maps panel coordinates into the centered LC `512×384` viewport
(`720×540` at offset `0,370`). A no-touch polling sample is validated; real touch
coordinate calibration and ADB mouse mapping are still pending.

Video scaffolding (`src/machine_lc/lc_video.c`) defines the first guest mode as
`512×384×8bpp`, `rowBytes=512`, `60Hz` VBL target, and separate PSRAM-backed
indexed VRAM. It initializes a deterministic debug RGB565 CLUT, generates a
border/ramp/stripe test pattern into the indexed framebuffer, marks rows dirty,
and converts dirty strips to RGB565 using a 16-line DMA-capable staging buffer.
The skeleton logs indexed and RGB565 checksums plus timing samples, validating
color-framebuffer logic without a Tab5 display driver. The same test pattern can
be rendered off-device as a PPM image with:

```bash
make lc-video-test-pattern
```
