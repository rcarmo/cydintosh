# LC bring-up reassessment against Basilisk II

Date: 2026-06-01

## Trigger

The current LC host harness has spent too much time adding one-off ROM-loop escapes. It now passes bounded gates, but it still does not render a guest-driven desktop and still reports no guest VRAM writes. That is the wrong success trajectory when Basilisk II already contains the relevant 32-bit-clean Mac II ROM bring-up machinery.

## Current state of our LC lane

Our host LC lane is effectively executing the LC/Mac II-class ROM directly and then compensating for missing hardware/manager behavior with per-PC synthetic shortcuts in `src/machine_lc/lc_musashi_bus.c`.

Observed state from the latest long probes:

```text
HOST_LC_VRAM_SNAPSHOT writes=0 reads=0 visible_nonzero=190333
```

The nonzero visible framebuffer is still host/synthetic evidence, not a Mac desktop. The repeated PC frontiers around ROM/Event/Slot/Memory Manager paths show that we are stuck in boot-manager plumbing rather than reaching a stable Display Manager/QuickDraw/video path.

## What Basilisk II already does for this ROM class

The local LC ROM is a 512 KiB ROM with version `0x067c`, which Basilisk II treats as a supported 32-bit-clean Mac II ROM:

- `vendor/mac-lc.rom`: size `0x80000`, version word at offset `0x08` is `0x067c`.
- Basilisk II `rom_patches.h` defines `ROM_VERSION_32 = 0x067c`.
- Basilisk II maps this ROM family at `ROMBaseMac = 0x40800000` in `BasiliskII/src/uae_cpu/basilisk_glue.cpp`.

Basilisk II does not try to emulate all LC hardware faithfully at reset. It creates a controlled paravirtual Mac II-compatible machine:

1. **Memory map**
   - RAM base is Mac address `0`.
   - 32-bit-clean ROM base is `0x40800000`.
   - Framebuffer base is `MacFrameBaseMac = 0xa0000000`.
   - In 24-bit mode it mirrors framebuffer writes from the final RAM page range into `MacFrameBaseHost`; in 32-bit mode it maps frame banks directly.

2. **ROM patching**
   - `rom_patches.cpp::patch_rom_32()` systematically patches the 0x067c ROM instead of relying on scattered runtime PC hooks.
   - It disables or replaces hardware init paths for VIA/SCC/IWM/SCSI/ASC/cache/MMU/slot interrupts.
   - It installs `M68K_EMUL_OP_*` opcodes for reset, clock/PRAM, Time Manager, SCSI dispatch, ADBOp, block move, driver installation, interrupts, etc.
   - It patches boot stack/memory sizing, BootGlobs, and hardware base assumptions.

3. **Device/driver substitution**
   - It replaces `.Sony`, `.Disk`, `.AppleCD`, serial, Time Manager, DebugUtil, ADBOp and SCSIDispatch with paravirtual handlers.
   - `emul_op.cpp` is the central dispatch for those synthetic operations.

4. **Video path**
   - `video.cpp` models monitors/video modes and implements the classic Mac video driver entry points.
   - `slot_rom.cpp` builds a Slot Manager declaration ROM and copies it into the Mac ROM, so MacOS discovers a video card through normal Slot Manager/Display Manager machinery.
   - `rom_patches.cpp` explicitly avoids mangling the framebuffer base and installs the slot ROM.

## Gap analysis: our project vs Basilisk II

| Area | Our current LC harness | Basilisk II reference | Consequence |
|---|---|---|---|
| ROM boot strategy | Raw ROM entry probe plus incremental PC-specific escapes | Full `patch_rom_32()` table for 0x067c ROMs | We keep discovering one hardware/manager blocker at a time. |
| Hardware model | Sparse LC bus stubs plus emergency Memory Manager/Resource Manager repairs | Paravirtual Mac II environment with known patched ROM contract | Our state drifts into invalid Resource/Slot/Event loops. |
| Video discovery | Host framebuffer/status rendering and LC VRAM counters | Slot declaration ROM + video driver + `MacFrameBaseMac` mapping | MacOS has no reliable guest-owned display device path, so VRAM writes stay zero. |
| Disk/boot path | Local disk image attached to custom LC harness | Basilisk `.Sony`/`.Disk` driver replacement and boot driver install | Even if CPU progresses, boot I/O is not aligned with the ROM's expected driver model. |
| Trap/manager hooks | Ad hoc synthetic A-trap and heap/handle workarounds | `M68K_EMUL_OP_*` central dispatch and patched ROM calls | Our fixes are fragile and accumulate hidden state corruption. |
| Portability to ESP | ESP-oriented C code, but not yet desktop-capable | Host-proven C++ emulator code, too large to drop directly into ESP | Need host reuse first, then selective C/ESP adaptation. |

## Reassessed direction

Stop adding new one-off ROM-loop escapes as the primary lane. The fastest credible path to a real desktop is to make the host LC harness converge toward Basilisk II's 0x067c Mac II-compatible contract, then selectively port the minimal pieces to the ESP/Tab5 target.

This does **not** require running Basilisk II. It requires reusing its source-level design and concrete patch tables.

## Proposed implementation plan

### Phase 1 — Host reference compatibility layer

Create a new host-only lane, separate from the current synthetic LC bus hacks:

- Add a `b2_compat` module under the LC host harness.
- Load `vendor/mac-lc.rom` into a writable copy for host testing.
- Apply a C/C++ port of the relevant `patch_rom_32()` operations for ROM version `0x067c`.
- Implement a Musashi illegal-instruction/EMUL_OP dispatch equivalent for the subset needed to boot.
- Seed BootGlobs/reset state following Basilisk II `M68K_EMUL_OP_RESET` semantics.

Initial success criteria:

```text
patched ROM boots past the current Resource/Slot/Event loops without per-PC loop caps
no monitor/zero execution stops
MacOS reaches driver installation / Display Manager path
```

### Phase 2 — Reuse Basilisk video discovery path

Port the minimal Basilisk video model rather than faking a desktop:

- Build a single monitor descriptor, initially 640x480 or our target LC mode, 8-bit indexed.
- Port enough of `slot_rom.cpp` to install a video Slot ROM into the ROM copy.
- Port enough of `video.cpp`/`video_defs.h` to answer driver open/control/status calls.
- Map guest framebuffer at the Basilisk-style `0xa0000000` host buffer first.
- Keep our PNG snapshot tool, but only report success on guest writes to this mapped framebuffer.

Success criteria:

```text
HOST_LC_VRAM_SNAPSHOT writes > 0
first/last write PCs are guest ROM/MacOS video/QuickDraw paths
PNG is generated from guest framebuffer, not host-rendered shell/status UI
```

### Phase 3 — Disk/boot driver alignment

Port or wrap Basilisk's `.Sony`/`.Disk` boot driver replacement so the ROM gets a known boot path instead of depending on partial LC storage hardware emulation.

Success criteria:

```text
System file loads from disk image
Finder/desktop draw happens in guest framebuffer
```

### Phase 4 — Remove ad hoc hooks

Once the Basilisk-compatible path boots, remove or quarantine the current PC-specific loop escapes:

- Resource Manager handle repairs
- Slot scan caps
- Event wait caps
- VBL/time hardware loop skip
- GetString startup shortcut
- Memory Manager emergency slab workarounds

Keep them only as diagnostics/regression gates until the replacement path is verified.

### Phase 5 — ESP/Tab5 port

Only after host desktop proof:

- Translate the minimal Basilisk-compatible patch/video/disk/EMUL_OP subset into ESP-safe C.
- Preserve Musashi CPU and the existing display output path.
- Rebuild and validate on ESP/Tab5.

## Immediate next work

1. Freeze the current loop-cap lane; do not add more PC-specific boot shortcuts.
2. Create `docs/lc-basiliskii-port-map.md` or an implementation checklist mapping each Basilisk function/patch to a Cydintosh module.
3. Start with `patch_rom_32()` and `M68K_EMUL_OP_RESET`, because those replace a large fraction of the current early-boot hacks.
4. Then port the Slot ROM/video declaration path; this directly targets the missing guest VRAM writes.

## Bottom line

The current harness has proven useful for instrumentation, but it is not the right boot strategy. Basilisk II already encodes the necessary ROM contract for this ROM family. The project should pivot from emulating/patching each failed ROM loop to reusing Basilisk II's systematic ROM patching, EMUL_OP dispatch, Slot ROM, and video-driver path, with the existing host harness becoming the verification shell around that compatibility layer.
