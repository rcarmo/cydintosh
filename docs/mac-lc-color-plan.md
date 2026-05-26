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

No LC boot is claimed yet. The first firmware target is a skeleton that logs
ESP32-P4/heap/partition state and verifies whether an LC ROM partition exists,
then maps the first 512KB of that partition read-only through ESP-IDF flash
`mmap` for diagnostics. It also logs a provisional 24-bit-first RAM/ROM memory
map and probes whether a 4MB guest RAM allocation fits in PSRAM, with a 2MB
fallback probe. It also probes indexed VRAM allocation in PSRAM and a DMA-capable
RGB565 strip buffer in internal RAM to guide the first color renderer. It includes
a provisional LC address decoder for RAM, ROM-window
candidates, I/O-window candidates, throttled unmapped-access logging, and a small
trace ring buffer that can dump recent diagnostic events on panic/hang paths in
future reset-vector execution. CPU trace helpers now exist for exception-vector
hits, illegal/unimplemented instructions, bus/address errors, and interrupt
levels. Lightweight performance counters now exist for future CPU-loop,
video-update, host-render, and display-flush timing. A diagnostic 512×384×8-bit
indexed video scaffold can generate a CLUT-backed test pattern, track dirty rows,
convert dirty strips to RGB565, checksum both indexed/RGB paths, and render the
same debug pattern off-device as a PPM image without using the Tab5 display
driver yet. A Tab5-only GPIO22/LEDC backlight scaffold and a software-only
720×1280 physical-panel smoke-pattern generator are wired into startup diagnostics
ahead of full DSI panel init. A Tab5 I2C touch probe scaffold checks GT911/ST7123
presence but does not yet read coordinates or emit ADB events. The memory scaffold also has an early bring-up
write policy: RAM and I/O-candidate writes are allowed, while ROM/unmapped writes
are flagged by the panic-on-unexpected-write policy. The branch now also has an
LC-only Musashi config
scaffold for 68EC020/68020, selected only by the Tab5/P4 LC environment, plus LC
CPU diagnostics that log the intended 68EC020 mode and raw ROM vector candidates
without executing guest code.

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
firmware-side disk I/O trace/write-blocking scaffold.

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
esp32-p4_r3
```

The environment remains Tab5-specific, but `esp32-p4_r3` is used because the
connected Tab5 reports ESP32-P4 rev v1.3 and PlatformIO's `m5stack-tab5-p4`
entry currently targets the ES/pre-rev chip variant.

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
3. Tab5 display smoke test shows known colors/orientation markers.
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
