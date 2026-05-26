# Macintosh LC color emulation plan for ESP32-P4 Tab5

This branch (`feat/mac-lc-color`) is an experimental Macintosh LC/color bring-up
branch for **ESP32-P4 / M5Stack Tab5 only**. It is deliberately separated from
the existing Mac Plus ESP32/CYD and ESP32-S3 firmware paths.

## Current branch status

Completed setup:

- Worktree: `/workspace/projects/cydintosh-lc-color`
- Branch: `feat/mac-lc-color`
- Target board: M5Stack Tab5 / ESP32-P4
- Local-only ROM path: `vendor/mac-lc.rom` (ignored by git)
- Original Tab5 flash: backed up before experiments

No LC boot is claimed yet. The current firmware target is a diagnostic skeleton,
not a Macintosh LC emulator loop. It currently provides:

- ESP32-P4/chip/heap/PSRAM diagnostics;
- LC ROM partition probing plus read-only `esp_partition_mmap()` validation for
  the first 512KB;
- metadata-only LC ROM vector/window scanning via `make lc-rom-vectors`;
- LC-only Musashi configuration for 68EC020/68020, selected only by the Tab5/P4
  environment;
- CPU trace helper scaffolds for reset-vector candidates, exception vectors,
  illegal/unimplemented instructions, bus/address errors, and interrupt levels;
- a trace ring and lightweight performance counters for later panic/hang dumps;
- provisional 24-bit-first RAM/ROM/I/O address decoding, with 32-bit candidates
  logged only;
- 4MB guest RAM PSRAM allocation probe, 2MB fallback probe, separate indexed VRAM
  probe, and DMA-capable RGB565 strip-buffer probe;
- panic-on-unexpected-write policy for ROM/unmapped writes while early ranges are
  still being discovered;
- read-only LC disk partition policy and disk I/O trace scaffolding;
- 512×384×8-bit indexed video scaffold with debug CLUT, dirty rows, RGB565 strip
  conversion, checksums, and off-device PPM rendering;
- Tab5 GPIO22/LEDC backlight scaffold;
- temporary `esp32-p4-tab5-bootdiag` no-PSRAM GPIO22/PI4IOE isolation image;
- vendored M5Tab5 BSP display-smoke image for real 720×1280 MIPI-DSI panel fills, visually confirmed on hardware, now exposing reusable full and dirty-row LC indexed framebuffer flush paths;
- software-only 720×1280 physical-panel smoke pattern checksums;
- Tab5 touch reader scaffold: ST7123/GT911 probing, driver init, no-touch polling, and raw-panel to LC-viewport coordinate mapping.

Guest LC ROM code is not executed yet. The next boot milestone is to verify the
actual reset-vector mapping, select `M68K_CPU_TYPE_68EC020` at runtime, and start
recording first hardware accesses through the LC trace ring.

## ROM metadata

The supplied ROM is stored only under ignored `vendor/` storage. Do not commit
ROM contents or derived binary chunks.

Non-copyrighted metadata for the local file:

| Field | Value |
|---|---|
| Expected local path | `vendor/mac-lc.rom` |
| Size | `524288` bytes (`0x80000`) |
| First big-endian long | `0x350EACF0` |
| SHA256 | `129391cc72f84c2b321709cad8281e30a45e50b3cf6e7afe7434c4d32c7b9d5b` |
| MD5 | `5d8662dfab70ac34663d6d54393f5018` |
| First 16 bytes, metadata only | `35 0e ac f0 00 00 00 2a 06 7c 4e fa 00 80 4e fa` |

Inspect local ROM/disk metadata and flash the ROM explicitly with:

```bash
python3 tools/inspect_lc_rom.py vendor/mac-lc.rom
make lc-rom-info
make lc-rom-vectors
make lc-disk-info
make flash-tab5-lc-rom
```

`make lc-rom-vectors` scans metadata-only SP/PC pairs against the provisional
24-bit and 32-bit ROM window candidates. It is a heuristic aid only; the current
offset-0 words are not a plausible reset SP/PC pair, so actual reset-vector
mapping still needs runtime verification. The firmware validates the flashed
partition by checking that the `rom` data partition is at least `0x80000` bytes
and that the mapped first long is `0x350EACF0`. See `docs/lc-boot-media.md` for
the local-only read-only disk image workflow around `vendor/lc-disk.img` and the
firmware-side disk I/O trace/write-blocking scaffold. See
`docs/lc-via-scc-audit.md` for the current Mac Plus VIA/SCC reuse audit before LC
hardware stubs are implemented.

## Scope

In scope:

- ESP32-P4 / M5Stack Tab5 board support;
- Macintosh LC-class color machine investigation;
- 512KB LC ROM loading;
- 68020-first Musashi configuration;
- built-in LC-style color framebuffer work;
- ADB mouse/keyboard model sufficient for boot;
- read-only boot media during bring-up;
- display/touch smoke tests on Tab5.

Out of scope for this branch:

- changing the already validated Mac Plus/CYD/S3 paths on `main`;
- adding LC support to ESP32 or ESP32-S3 profiles;
- committing copyrighted ROM or disk images;
- claiming functional LC boot before reset-vector and hardware-stub milestones
  are reached.

## Architecture direction

The LC path is an explicit machine backend, not a mutation of the Mac Plus path:

```text
CYD_MACHINE_MAC_PLUS   -> current umac/Mac Plus path
CYD_MACHINE_MAC_LC     -> new LC memory map, ROM loader, color video, ADB stubs
```

`include/cyd_machine.h` enforces exactly one machine selection and ensures the
Macintosh LC model only builds with the M5Stack Tab5 ESP32-P4 LC target. The
current LC skeleton uses `src/machine_lc/` for ROM partition diagnostics so Mac
Plus ROM patch offsets are not applied to the LC ROM.

## Initial platform target

PlatformIO environment:

```text
esp32-p4-tab5-lc-color
```

Board selection:

```text
m5stack-tab5-p4
```

The attached Tab5 reports ESP32-P4 rev v1.3, so the branch uses PlatformIO's
M5Stack Tab5 ES/pre-rev300 board definition at 360MHz. The generic
`esp32-p4_r3` board was tested and rejected for this unit because it generated
rev3/400MHz linker and bootloader settings that trapped with `Illegal
instruction` before `app_main`. The current sdkconfig defaults explicitly select
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, min rev `100`, max rev `199`, and
360MHz CPU, matching the patched official `M5Tab5-UserDemo` that runs on this
hardware.

Initial skeleton source:

```text
src/tab5_lc_smoke.c
```

This skeleton intentionally does not start current umac or Mac Plus emulation.
It exists to validate ESP32-P4 toolchain support and provide safe diagnostics
before display/touch and LC emulation are added.

## Milestones

1. ESP32-P4 Tab5 skeleton builds.
2. Skeleton flashes and logs chip/heap/partition diagnostics.
3. Tab5 display smoke test shows known colors/orientation markers and the scaled LC indexed debug pattern.
4. Tab5 touch smoke test logs calibrated coordinates.
5. LC ROM partition maps and validates size/first-long metadata.
6. Provisional LC RAM/ROM map and guest-RAM allocation diagnostics are logged.
7. LC address decoder and throttled unmapped-access logger report candidate ranges.
8. LC reset vector executes under a conservative 68EC020/68020 configuration.
9. Missing hardware accesses are decoded and stubbed iteratively.
10. Fixed color framebuffer displays diagnostic writes.
11. ROM/System reaches a stable boot/probe phase.
12. Read-only boot media begins loading.
13. Finder desktop appears in color.

## Safety rules

- Keep original Tab5 flash backup immutable.
- Use explicit P4 target names for all flash commands.
- Do not use CYD/S3 serial ports for Tab5 flash targets.
- Keep disk images read-only until write path is validated.
- Add logs and metadata before adding functional claims.
