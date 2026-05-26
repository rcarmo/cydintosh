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
- metadata-only LC ROM vector/window scanning plus ROM-header entry/trampoline
  hints via `make lc-rom-vectors` and matching firmware diagnostics;
- LC-only Musashi configuration and linked core for 68EC020/68020, selected only
  by the Tab5/P4 environment;
- CPU trace helper scaffolds for reset-vector candidates, exception vectors,
  illegal/unimplemented instructions, bus/address errors, and interrupt levels;
- a trace ring and lightweight performance counters for later panic/hang dumps;
- provisional 24-bit-first RAM/ROM/I/O address decoding, with 32-bit candidates
  logged only;
- bounded LC memory-bus harness with PSRAM guest RAM, mapped ROM reads, named
  generic I/O stub reads/writes, ROM write blocking, and unmapped access logging;
- LC Musashi callback bridge plus a RAM-only synthetic 68EC020 reset/execute
  smoke probe and a bounded ROM-entry micro-probe; full LC boot remains disabled;
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
- normal `esp32-p4-tab5-lc-color` diagnostic now initializes the BSP panel and visibly draws the LC indexed test pattern before continuing serial diagnostics;
- software-only 720×1280 physical-panel smoke pattern checksums;
- Tab5 touch reader scaffold: ST7123/GT911 probing, driver init, no-touch polling, and raw-panel to LC-viewport coordinate mapping.

Full LC boot is not enabled yet. The latest hardware diagnostic reflashed and
verified the LC ROM partition, then ran the firmware-side vector and entry
scanners plus a bounded ROM-entry micro-probe. The scanner confirmed offset 0 is
not a plausible SP/PC reset vector, found 13 heuristic vector-like pairs in the
first `0x4000` bytes, and logged best current vector-like candidate
`file_offset=0x00d58 sp=0x00186100 pc=0x00842f00 rom_base=0x40800000`. The entry
scan is more useful than the noisy SP/PC heuristic: it identifies ROM-header
PC-relative trampolines at file offsets `0x0000a`, `0x0000e`, and `0x0002a`
targeting `0x0008c`, which disassembles to `move #0x2700,sr`; it also logs jumps
to `0x01240`, `0x02310`, `0x02e00`, and a `reset` opcode at `0x000aa`.

The bounded on-device ROM-entry micro-probe now starts at `0x0040008c` in the
24-bit ROM window. It reaches the guest `RESET` instruction, advances into the
next ROM dispatcher, and records first explicit I/O stub accesses from ROM PCs
around `0x00403124`-`0x0040314a` to `0x00f01c00`, `0x00f21c00`, and
`0x00f41c00` (classified as `early-rom-probe-1c00-stride`). That loop is now
modeled as a provisional VIA-style IER alias: writes set/clear IER bits and
reads return bit 7 plus the current enable mask, which advances the ROM past the
previous repeated 2832-read/3776-write loop. A `0x00800000` masked ROM alias was
added after the guest switched toward the `0x40800000` ROM window and the
68EC020 callbacks fetched masked `0x008xxxxx` addresses. With a 20M-cycle bounded
probe, execution advances through the checksum loop, high-memory sizing probes,
and the newly named `0x00f14000`-class early device range. A 100M-cycle probe
then reaches the ROM's diagnostic/serial monitor loop around `0x408498ec` and
`0x40849fca`, with `d0=0x8000` indicating no input available. High addresses from
`0x00400000` up to the I/O window, plus the top 16 bytes of the 24-bit space, are
now modeled as non-present RAM-size probe locations so the ROM can discover the
configured 4MB RAM boundary without unexpected-write panics. Newly named early
VIA-like accesses include `0x00f01e00`, `0x00f00600`, `0x00f00400`, and
`0x00f00000`; the `0x00f14800` range is separated as `early-f14000-device`, and
`0x00f04000` is separated as an SCC-like `early-f04000-device` no-input
status/data block.
This establishes `0x0040008c` as the first guarded execution target, while the
real reset overlay/vector mechanism remains to be modeled. The latest diagnostic
also validates the memory-bus harness and Musashi callback bridge: 4MB PSRAM RAM
reads/writes, mapped ROM reads including the masked ROM alias, generic I/O stub
reads/writes, ROM write blocking, RAM-size probe handling, unmapped-read logging,
and a RAM-only synthetic 68EC020 reset/execute smoke test (`reset_pc=0x100`, `reset_sp=0x2000`,
`cpu_type=3`). It also now shows the LC indexed diagnostic pattern on the Tab5
panel in the normal LC target; user confirmation reported the test pattern
visible. The `0x0040008c` guarded entry appears to end in a ROM diagnostic
monitor, not yet in a normal Mac boot path. Alternate ROM-header probes show
`0x00402e00` and `0x00402f18` avoid that monitor, while `0x00402310` reaches the
same diagnostic monitor quickly and `0x00401240` falls into RAM-only trap/setup
code. The current diagnostic default is `0x00402e00`, now seeded with the caller
frame pointer/continuation used by the reset trampoline (`a6=0x004000b4`). Latest
hardware capture (`serial-capture-20260526-204602.log`) shows this avoids the
previous zero-filled RAM trap, sets `vbr=0x40846140`, runs RAM sizing/probe code,
uses no-input SCC-like status (`0x00f04000` offset `+0` returns `0x04`), and stops
on the known ROM diagnostic/serial-monitor dispatcher around
`0x40849eae`/`0x40849fca` with `d7=0x01000304` (bits 24, 9, 8, and 2 set). The earlier direct `0x00402e00` probe without the
caller `a6` seed jumped through `a4=0x40400000`, executed ROM header/fingerprint
bytes, raised an A-line exception with zero low vectors, and fell into zero RAM;
that is now treated as an invalid entry precondition rather than boot progress.
The next boot milestone is to understand why the reset path still selects the ROM
monitor/diagnostic path and whether specific hardware status bits or reset flags
can steer it toward normal boot without faking serial input.

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
24-bit and 32-bit ROM window candidates and now also prints ROM-header
entry/trampoline hints. The firmware runs the same style of bounded scans against
the mapped flash partition and records vector-like candidates into the trace ring.
It is a heuristic aid only; the current offset-0 words are not a plausible reset
SP/PC pair, so actual reset-vector/overlay mapping still needs runtime
verification. The firmware validates the flashed partition by checking that the
`rom` data partition is at least `0x80000` bytes and that the mapped first long is
`0x350EACF0`. See `docs/lc-boot-media.md` for
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
