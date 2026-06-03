# Host-native Macintosh LC harness

The host LC harness builds the Macintosh LC core as a native Linux binary, using
small ESP-IDF compatibility stubs instead of flashing the M5Stack Tab5.

It is intended for tight regression loops around the ROM map, LC memory bus,
Musashi callbacks, and framebuffer conversion. It does **not** emulate Tab5
hardware such as MIPI-DSI, touch, PI4IOE, real PSRAM/cache behavior, or camera
validation.

## Targets

```sh
make host-lc-harness
make host-lc-smoke
make host-lc-rom-probe
```

Outputs:

- binary: `build/host-lc/host_lc_harness`
- default video artifact: `artifacts/host-lc-video-test-pattern.ppm`

## Smoke run

```sh
make host-lc-smoke
```

This validates, on the host:

- `vendor/mac-lc.rom` partition stub metadata and first-long fingerprint
- LC memory map decoder and allocation probes
- LC memory-bus read/write harness
- Musashi 68EC020 synthetic RAM-only execution smoke
- indexed LC video test-pattern generation
- RGB565 conversion and PPM export

Expected terminal marker:

```text
HOST_LC_OK
```

## Bounded ROM-entry probe

```sh
make host-lc-rom-probe
```

This runs the same native smoke plus `lc_cpu_probe_rom_entry_execution()`.
Default bounded cycle count:

```make
HOST_LC_CYCLES ?= 200000u
```

Override example:

```sh
make host-lc-rom-probe HOST_LC_CYCLES=5000000u
```

Historical hardware gates used much larger values, for example
`500000000u`; use those only for deliberate long host runs.

## Useful variables

```sh
make host-lc-smoke \
  HOST_LC_ROM_IMAGE=vendor/mac-lc.rom \
  HOST_LC_DISK_IMAGE=vendor/lc-disk.img \
  HOST_LC_PPM=artifacts/host-lc-video-test-pattern.ppm
```

Tracing is compiled off by default to match the accepted low-noise firmware
regression gates:

```make
HOST_LC_TRACE ?= 0
```

Enable trace ring logging for local diagnostics:

```sh
make host-lc-rom-probe HOST_LC_TRACE=1
```

## Scope boundaries

Use this harness for fast local logic tests. Still use Tab5 hardware for:

- display BSP / MIPI-DSI init
- backlight and touch
- real flash/partition behavior
- ESP32-P4 PSRAM/cache interactions
- final camera validation of the LC desktop
