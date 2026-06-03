# Reference-backed Macintosh LC bring-up design

This note is the reference handoff requested before more LC ROM poking. It maps
BasiliskII/macemu and MAME LC/V8 behavior onto the current
`cydintosh-lc-color` scaffolding, and defines the order in which temporary
synthetic hooks should be retired.

Current local comparison point: `logs/serial-capture-20260527-222958.log` runs
100M cycles without the old diagnostic monitor stop, `0x00007fba`,
`pc=0xffffffff`, `addr=0x01000000`, `0xffffffd8`, `0x001fdfec`, or a diagnostic
exception stack. It expires around `pc_after=0x4081ab90` in the Resource Manager /
INIT-loading path. The remaining work should preserve that frontier while moving
state ownership out of PC-gated read hooks.

## References read

### BasiliskII / macemu

- `BasiliskII/src/uae_cpu_2026/newcpu.cpp`
  - 32-bit ROM startup sets the initial PC to `ROMBaseMac + 0x2a` rather than
    treating ROM offset 0 as a raw reset-vector pair.
- `BasiliskII/src/uae_cpu_2026/basilisk_glue.cpp`
  - `ROM_VERSION_32` maps `RAMBaseMac = 0` and `ROMBaseMac = 0x40800000`.
- `BasiliskII/src/emul_op.cpp`
  - `M68K_EMUL_OP_RESET` constructs `BootGlobs` at top of RAM, fills the RAM
    bank list, loads `UniversalInfo`-derived registers, clears the FPU flag when
    no FPU is present, and initializes the boot stack/ISP/MSP.
  - `M68K_EMUL_OP_PATCH_BOOT_GLOBS` writes `MemTop` and no-MMU flags into the
    BootGlobs-adjacent structure.
  - `M68K_EMUL_OP_FIX_DISPATCH_MAGIC` seeds `$0DB0 = 0x5A932BC7`, gives `$0120`
    a handler if empty, sets `$02AE = ROMBaseMac`, and saves SR at `$0C74`.
- `BasiliskII/src/rom_patches.cpp`
  - Finds `UniversalInfo` in the 32-bit ROM by scanning for the universal table
    signature near `0x3400..0x3c00`.
  - Patches the reset entry at ROM offset `0x8c` to an EMUL_OP and jump in
    BasiliskII, and disables or redirects hardware probes it does not emulate.
  - Uses real OS mechanisms such as `Execute68kTrap(0xa247)`
    (`SetOSTrapAddress`) and driver-install traps for service installation.

BasiliskII is useful here as a list of required low-memory/BootGlobs invariants,
not as a license to patch the LC ROM in Cydintosh. Its patches identify places
where Cydintosh must provide either real LC hardware behavior or coherent
RAM-owned startup state.

### MAME LC / V8 / pseudo-VIA

- `tmp/mame/maclc.cpp`
  - LC-class machines are based on V8-family system controllers with a 10MB RAM
    hard limit.
  - The LC address map uses `map.global_mask(0x80ffffff)`, so ROM constants such
    as `0x50f04000` alias to the active low 24-bit I/O window `0x00f04000`.
  - Base V8-mapped ranges plus LC devices:
    - `0x00f00000..0x00f01fff` — V8 VIA1.
    - `0x00f04000..0x00f05fff` — SCC85C30.
    - `0x00f06000..0x00f07fff` — SCSI pseudo-DMA / DRQ.
    - `0x00f10000..0x00f11fff` — NCR53C80 SCSI.
    - `0x00f12000..0x00f13fff` — SCSI pseudo-DMA / DRQ mirror.
    - `0x00f14000..0x00f15fff` — ASC sound in V8 submap.
    - `0x00f16000..0x00f17fff` — SWIM1 floppy controller.
    - `0x00f24000..0x00f25fff` — ARIEL RAMDAC.
    - `0x00f26000..0x00f27fff` — V8 pseudo-VIA.
    - `0x00f40000..0x00fbffff` — V8 VRAM.
  - MAME models the LC as 68020 + HMMU, NCR53C80 SCSI, SCC85C30, V8, Egret,
    MACADB, SWIM1, and ARIEL.
- `tmp/mame/v8.cpp`
  - Reset starts with CPU halted until Egret wakes it, enables a ROM overlay,
    and installs a ROM mirror at address zero.
  - The first `rom_switch_r()` read disables the overlay and calls `ram_size()`.
  - V8 interrupt priority is SCC level 4, pseudo-VIA level 2, VIA1 level 1.
  - VIA1 register accesses decode as `(offset >> 8) & 0x0f`; MAME duplicates
    VIA byte reads into both lanes of the 16-bit return value.
  - `via_in_a()` returns `0xd4 | config_bit0`; VIA PB bits connect to Egret
    handshake (`PB4`, `PB5`) and PB3 reads Egret state.
  - Pseudo-VIA config reads return `m_config | 0x04`; video-config reads return
    monitor type shifted into bits 5:3; PB bit 3 controls HMMU enable.
  - Video mode is selected by `m_video_config & 7`. Monitor type `2` is the LC
    12-inch RGB geometry: `512x384`. VRAM rows are indexed with a 1024-byte
    stride; 8bpp reads one byte per pixel and maps through ARIEL pens.
- `tmp/mame/pseudovia.cpp`
  - V8 pseudo-VIA has a 6522-compatible-ish layout but no timers, shift register,
    or DDRs.
  - V8 pseudo-VIA decodes the base RBV-style registers; IFR/IER bit 7 reads as
    zero, not as a normal 6522 VIA IER-set indicator.
  - V8 pseudo-VIA makes ASC IRQ level-triggered and does not clear that bit via
    normal IFR acknowledgement.
- `tmp/mame/macscsi.cpp`
  - Macintosh SCSI Manager uses NCR5380 pseudo-DMA; blind transfers rely on DRQ
    wait-state style behavior and may install temporary bus-error handlers.
  - A pragmatic first model can mediate bytes/FIFOs and timeouts rather than
    trying to execute all SCSI wait-state timing literally.

### Local ROM inspection

`tools/inspect_lc_rom.py vendor/mac-lc.rom --vector-scan --entry-scan --io-scan`
confirms:

- ROM size/hash: `0x80000`, SHA256
  `129391cc72f84c2b321709cad8281e30a45e50b3cf6e7afe7434c4d32c7b9d5b`.
- ROM offset 0 is not a plausible SP/PC reset-vector pair.
- Header/entry hints include:
  - offset `0x0002a`: PC-relative jump to `0x0008c`.
  - offset `0x0008c`: `move #0x2700,sr` target reached by the header trampolines.
  - offset `0x000aa`: `RESET` instruction.
  - offset `0x000b0`: jump to `0x02e00`.
  - offset `0x000b4`: branch to `0x008e0`.
- I/O scan finds 122 `0x50fxxxxx` constants, 65 unique values, including the MAME
  LC/V8 ranges listed above.

## Design principles from the references

1. **No more per-PC ROM symptom fixes.** BasiliskII patches are clues about
   startup state and missing hardware; Cydintosh should model that state in RAM
   or devices, not alter ROM bytes or branch around individual failures.
2. **Reset entry must be a machine model, not a direct subroutine probe.** The LC
   ROM is a 32-bit clean ROM with a header/trampoline; offset 0 is metadata, not
   the reset SP/PC. The model should own reset overlay, stack, ROMBase,
   BootGlobs, and RAM bank layout coherently.
3. **Low memory is RAM state.** `$0DB0`, `$0120`, `$02AE`, `$0C74`, `$0DBC`, trap
   tables, Resource Manager globals, and BootGlobs-derived cells should be seeded
   or mutated through one low-memory subsystem, then read normally.
4. **A-traps should use the ROM trap dispatcher contract.** Trap tables should
   contain trap-specific handlers or real installed entries; synthetic universal
   success handlers are only diagnostic scaffolding and can corrupt return-stack
   contracts.
5. **The LC hardware map should be named after MAME/V8 devices.** The current
   `early-f04000-device`, `early-f10000-device`, `early-f14000-device`, and
   similar generic stubs should be retired behind VIA1, SCC, SCSI, ASC, SWIM,
   ARIEL, pseudo-VIA, and VRAM modules.
6. **First video target is exactly aligned with Tab5 scaffolding.** MAME's V8
   monitor type 2 is `512x384`, 8bpp uses 1024-byte VRAM row stride and ARIEL
   pens. That maps cleanly to the existing Tab5 512x384 indexed renderer once
   guest video globals/VRAM/CLUT are owned.

## Mapping to Cydintosh subsystems

| Reference behavior | Current Cydintosh area | Required owner | Hooks retired when done |
|---|---|---|---|
| ROMBase `0x40800000`, ROM header entry around `+0x2a/+0x8c`, ROM overlay at zero | `lc_cpu.c`, `lc_memory_decode_address()` ROM aliases, direct `0x02e00` probe seed | `lc_reset` / V8 overlay state | Direct reset-body caller-frame seeds, reset-continuation trap skips, masked ROM shadow special cases that only exist for direct entry |
| BootGlobs at top of RAM, RAM bank list, `MemTop`, no-MMU flags | `lc_memory_should_read_synthetic_ram_test_list()`, BootGlobs/RAM-test synthetic reads | `lc_lowmem` + `lc_v8_ram` bank layout | Synthetic RAM-test list reads; post-reset descriptor/finalizer data substitutions |
| `$0DB0`, `$0120`, `$02AE`, `$0C74`, `$0DBC` | `lc_memory_seed_post_reset_resource_lowmem_ram()`, `$0DBC` read fallback | RAM-owned low-memory initializer | PC-gated basilisk dispatch magic and low-memory callback reads |
| A-line vector and `$0400/$0e00/$1e00` trap tables | `post_reset_atrap_table_overrides`, synthetic trap-table reads, A05D/A001/high-trap repairs | Trap table RAM + trap dispatcher integration | Synthetic low/high A-trap read window; A05D NOP; A001 skip; high-trap return repair |
| Memory Manager handles and BlockMove | `lc_musashi_bus_maybe_apply_post_reset_memory_trap()`, tiny heap | Minimal RAM-backed Memory Manager | Host-side trap register rewrites once traps dispatch through real table entries |
| ROM Resource Manager map and type/ref lists | Resource globals/map seed in `lc_memory.c` plus lookup/copy caps in `lc_musashi_bus.c` | Resource Manager prototype backed by Memory Manager handles | Resource map globals/read hooks, copy-loop caps, fake GDevice/resource chains |
| VIA1 at `0xf00000`, real 6522 timers/IFR/IER | `early-rom-probe-1c00-stride`, `early-lc-via-register`, synthetic level-1 pulses | `lc_via1` module | Generic VIA IER/IFR stubs and exact-PC level-1 pulses |
| SCC85C30 at `0xf04000`, level-4 interrupt | `early-f04000-device`, synthetic SCC timer pulses | `lc_scc` module | Reset SCC register/local-loopback shortcuts and exact-PC level-4 pulses |
| V8 pseudo-VIA at `0xf26000`, Egret handshakes, VBL/ASC IRQs | Mostly absent; some `early-f14000`/slot/video assumptions | `lc_pseudovia` + `lc_egret_adb` | Generic `f14000`/slot-video assumptions; fake video-default trap path |
| NCR53C80 + pseudo-DMA at `0xf10000/0xf06000/0xf12000` | `early-f10000-device` and disk trace scaffolding | `lc_scsi` read-only boot path | Generic SCSI no-op status/data stubs once boot-block reads are logged |
| SWIM1 at `0xf16000` | `early-f16000-device` | `lc_swim` if floppy boot is chosen | Generic SWIM stub |
| ARIEL + V8 VRAM at `0xf24000`/`0xf40000..0xfbffff` | Tab5 indexed framebuffer scaffold, fake video globals | `lc_video_v8` | SlotManager/video-default trap skips and fake GDevice chain |

## Concrete bring-up sequence

### 1. Reset/overlay/entry cleanup

- Add an explicit reset state object: ROM overlay active, CPU held/released state,
  V8 RAM config, boot stack, and selected ROM entry.
- Treat `ROMBaseMac = 0x40800000` as the 32-bit ROM base for ROM-owned pointers,
  but also support the `0x00fxxxxx` active I/O aliases produced by
  `0x50fxxxxx & 0x80ffffff`.
- Stop treating ROM offset 0 as a reset vector. Use the ROM header/trampoline
  model: the meaningful early path is around `ROMBase + 0x2a -> +0x8c`, with the
  current `+0x2e00` direct probe only a diagnostic fallback.
- Preserve current regression gates before retiring direct-entry seeds:
  no `0x00007fba`, no `pc=0xffffffff`, no `addr=0x01000000`, no `0xffffffd8`,
  no `0x001fdfec`.

Implemented checkpoint: `lc_cpu.c` now has an explicit `lc_reset_state_t` logged
at ROM-entry probe start. It records ROM overlay active, CPU-held-for-Egret,
`ROMBase=0x40800000`, the 24-bit alias, header entry `+0x2a`, trampoline target
`+0x8c`, reset opcode `+0xaa`, boot stack `0x00010000`, RAM bank 0, and the
current `+0x2e00` direct-entry fallback as `diagnostic_only=yes`. Capture
`logs/serial-capture-20260528-131420.log` preserves the clean frontier at
`pc_after=0x4081ab92` with the existing bad-PC regression gates clear.

### 2. Low-memory and trap-table ownership

- Promote `LC_ENABLE_RAM_OWNED_LOW_MEMORY` from a passive seed to the normal path
  one group at a time:
  1. `$0DB0/$0120/$02AE/$0C74/$0DBC`.
  2. A-line vector and low OS trap table.
  3. High OS/tool trap tables.
  4. Resource Manager globals and handle cells.
- `SetTrapAddress` should write RAM table cells directly. Keep plausibility logs
  for rejected fill patterns, but stop serving table bytes from the read path.
- Replace universal `0x40800d88` default entries with trap-specific services or
  an intentional diagnostic trap. In particular, terminal/helper-shaped traps
  such as `A895`/ShutDown must not share a generic success handler that causes a
  second accidental `RTS`.
- Audit Musashi's A-line exception frame only against what the LC ROM dispatcher
  actually consumes. The existing dispatcher reads the trap PC at `sp@(10)` after
  saving `a2/d2`; any bus-side return-slot repair should be deleted only after a
  natural trap frame/table path reaches the same post-trap continuation.

### 3. Minimal Memory Manager and Resource Manager

- Keep the current synthetic Resource Manager map structure as the prototype, but
  allocate it as real RAM-owned handles:
  - handle cell(s), pointed-to map, map size, type-list offset, name-list offset,
    ref-list records, resource data offsets.
- Move `NewPtr`, `NewHandle`, `HLock`, `SetHandleSize`, `GetHandleSize`,
  `StripAddress`, and `BlockMove` behind a small Memory Manager surface that owns
  RAM blocks and handle metadata.
- Retire copy-loop caps only after Resource Manager map growth operates on valid
  RAM-owned lengths and destinations.

### 4. V8/VIA/SCC/pseudo-VIA device model

- Replace generic I/O classifier names with MAME-derived devices:
  - `lc_via1` for `0xf00000..0xf01fff`.
  - `lc_scc` for `0xf04000..0xf05fff`.
  - `lc_scsi` for `0xf06000`, `0xf10000`, `0xf12000`.
  - `lc_asc` placeholder for `0xf14000`.
  - `lc_swim` for `0xf16000`.
  - `lc_ariel` for `0xf24000`.
  - `lc_pseudovia` for `0xf26000`.
  - `lc_vram` for `0xf40000..0xfbffff`.
- Do not conflate VIA1 and pseudo-VIA IER behavior: real VIA1 can use 6522-style
  IER behavior; V8 pseudo-VIA IFR/IER bit 7 reads as zero.
- Model interrupt priority as MAME does: SCC level 4 first, pseudo-VIA level 2,
  VIA1 level 1. Replace exact-PC interrupt pulses with device timers/IFR causes.
- Model enough Egret handshake through V8 PB3/PB4/PB5 and CB1/CB2 to stop using
  fake ADB/video trap shortcuts. Full keyboard/mouse can wait; Tab5 touch should
  become ADB mouse only after this line discipline is stable.

### 5. Boot media

- Prefer a read-only NCR53C80/SCSI hard-disk path first if the local image is a
  hard-disk/System image. MAME's LC config defaults SCSI ID 6 to a hard disk and
  already uses a helper for pseudo-DMA complications.
- Implement enough status/DRQ/command/data behavior to log the first boot-block
  read, with writes blocked or ignored according to the existing read-only disk
  policy.
- SWIM1 at `0xf16000` remains the alternative if the target image is floppy-like,
  but it should not block the SCSI-first path.

### 6. Video and Tab5 output

- Implement V8 VRAM as a guest-visible region at `0xf40000..0xfbffff` with the
  MAME row model: 1024 bytes per row for indexed modes.
- Implement ARIEL CLUT enough for 1/2/4/8bpp lookup. For the first visible Mac
  screen, monitor type `2` (`512x384`) and video config `3` (8bpp) map directly
  to the existing Tab5 512x384 indexed flush path.
- Retire fake GDevice/video globals and SlotManager/video-default trap skips only
  after the ROM's own video resource/global setup can point at real VRAM/CLUT
  state.

## Retirement order for current scaffolding

1. **Keep observability.** Preserve ROM watchpoints, bad-PC logging, I/O summary
   logs, and regression checks.
2. **Retire low-memory read hooks first.** `$0DB0/$0DBC/$02AE`, trap table reads,
   and Resource Manager globals must become RAM bytes.
3. **Retire A-trap return repairs next.** Remove `A05D`, `A001`, and high-trap
   return fixes only after trap tables and exception frames naturally carry the
   current frontier.
4. **Retire Memory/Resource Manager host-side effects.** Move handle/map/copy
   behavior into owned RAM structures and minimal services.
5. **Retire generic I/O stubs.** Replace them by named V8, VIA, SCC, pseudo-VIA,
   SCSI/SWIM, ARIEL, and VRAM devices.
6. **Retire video-default and fake GDevice traps.** Do this after V8 video and
   Resource Manager state can expose a real 512x384 framebuffer path.
7. **Only then add Tab5 touch as ADB mouse.** ADB depends on Egret/V8 handshake
   stability, so touch injection is intentionally late.

## Immediate next implementation checkpoint

The next code tranche should not add a new ROM-PC exception. It should pick one
small retirement slice and A/B it against `serial-capture-20260527-222958.log`:

1. Add named V8 address-range decoding aliases alongside the current generic I/O
   names, with no behavior change.
2. Under `LC_ENABLE_RAM_OWNED_LOW_MEMORY=1`, decline the `$0DB0/$0DBC/$02AE` and
   Resource Manager global synthetic read hooks first, proving normal RAM bytes
   preserve the frontier.
3. Then decline low A-trap table reads for only traps that the RAM table already
   owns via seed or `SetTrapAddress`.

Success for that checkpoint is not a desktop yet; it is the same or later
`0x4081ab90` Resource Manager/INIT-loading frontier with the existing bad-PC
regression gates still clean and with fewer PC-gated reads in the trace.
