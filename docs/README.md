# Cydintosh documentation index

This directory contains branch-specific notes for the Cydintosh fork.

## Stable Mac Plus / board-profile docs

The top-level [`README.md`](../README.md) remains the main entry point for the
existing Mac Plus ESP32/CYD and ESP32-S3 targets.

## ESP32-P4 / M5Stack Tab5 Macintosh LC work

The LC/color work lives on `feat/mac-lc-color` and is **Tab5/ESP32-P4 only**. No
LC boot is claimed yet.

| Document | Use it for |
|---|---|
| [`mac-lc-color-plan.md`](mac-lc-color-plan.md) | Scope, current status, safety rules, and milestone plan. |
| [`tab5-hardware.md`](tab5-hardware.md) | Tab5 hardware fingerprint, original flash backup, flash layout, and current scaffolds. |
| [`tab5-display-component-audit.md`](tab5-display-component-audit.md) | M5Stack demo/BSP display and touch audit for ILI9881C/ST7123/GT911. |
| [`musashi-lc-cpu-audit.md`](musashi-lc-cpu-audit.md) | Musashi 68EC020/68020 configuration and CPU trace scaffold notes. |
| [`lc-boot-media.md`](lc-boot-media.md) | Local-only LC disk workflow and read-only disk trace policy. |
| [`lc-via-scc-audit.md`](lc-via-scc-audit.md) | Why Mac Plus VIA/SCC code cannot be reused directly and what LC hardware gaps remain. |
| [`lc-temporary-hooks-inventory.md`](lc-temporary-hooks-inventory.md) | Inventory of diagnostic hooks in `lc_memory.c` / `lc_musashi_bus.c`, classified as keep/replace/delete. |
| [`lc-rom-range-annotations.md`](lc-rom-range-annotations.md) | Annotated disassembly for the current `0x00007fba` / monitor-frontier ROM ranges. |
| [`lc-lowmem-vbr-atrap-design.md`](lc-lowmem-vbr-atrap-design.md) | RAM-owned low-memory, VBR, and A-trap table design for retiring PC-gated synthetic reads. |
| [`lc-reference-bringup-design.md`](lc-reference-bringup-design.md) | Reference-backed BasiliskII/MAME LC bring-up map and retirement order for synthetic hooks. |
| [`host-lc-harness.md`](host-lc-harness.md) | Native Linux LC core harness for tight ROM/memory/Musashi/video regression loops before Tab5 flashing. |

## Local-only assets

Do not commit these assets:

| Asset | Expected local path |
|---|---|
| Macintosh LC ROM | `vendor/mac-lc.rom` |
| Macintosh LC boot disk | `vendor/lc-disk.img` |
| Mac Plus ROM | `vendor/rom.bin` |
| Mac Plus boot disk | `vendor/disk.img` or `data/disk.img` |

Relevant metadata-only commands:

```bash
make lc-rom-info
make lc-rom-vectors
make lc-disk-info
make lc-video-test-pattern
make host-lc-smoke
make host-lc-rom-probe
```

Relevant Tab5 build/flash commands:

```bash
make build-tab5-lc
make flash-tab5-lc
make flash-tab5-lc-rom
make capture-tab5-logs
```

Only run Tab5 flash commands when the USB/JTAG path from `tab5-hardware.md` is
present.
