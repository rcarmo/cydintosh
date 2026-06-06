# Macintosh LC color emulation plan for ESP32-P4 Tab5

This branch (`feat/mac-lc-color`) is an experimental Macintosh LC/color bring-up
branch for **ESP32-P4 / M5Stack Tab5 only**. It is deliberately separated from
the existing Mac Plus ESP32/CYD and ESP32-S3 firmware paths.

## Execution plan reset: boot a real LC, not a growing diagnostic shim

Current supporting notes for the reset are:

- [`lc-temporary-hooks-inventory.md`](lc-temporary-hooks-inventory.md) — the
  temporary `lc_memory.c` / `lc_musashi_bus.c` hooks classified as keep, replace,
  or delete.
- [`lc-rom-range-annotations.md`](lc-rom-range-annotations.md) — annotated ROM
  ranges for the current A-trap/low-memory/Resource Manager/monitor frontier.
- [`lc-lowmem-vbr-atrap-design.md`](lc-lowmem-vbr-atrap-design.md) — design for
  replacing PC-gated low-memory/trap-table reads with RAM-owned state.
- [`lc-reference-bringup-design.md`](lc-reference-bringup-design.md) —
  BasiliskII/macemu and MAME LC/V8 reference-backed bring-up map plus retirement
  order for synthetic hooks.

The immediate objective is to turn the current bounded ROM-entry micro-probe into
a coherent Macintosh LC machine model that can execute the ROM's reset path,
reach boot-device probing, load a read-only System disk, and draw the first real
Macintosh screen on the Tab5. The direction from this point is:

1. **Own reset, low memory, VBR, and trap tables as a single subsystem.** Stop
   adding broad read-time substitutions for low-memory cells. Build one startup
   initializer that seeds the ROM's expected low-memory globals, exception
   vectors, A-trap dispatch tables, ROMBase/MemTop/BootGlobs, and no-FPU/no-MMU
   state, then let normal RAM reads see that state.
2. **Replace dispatcher bypasses with correct A-line/exception behavior.** Audit
   Musashi's 68020 exception frames and the ROM's high/low trap dispatchers, then
   make A-traps return through the same stack shape the LC ROM expects instead
   of repairing individual post-failure return slots.
3. **Promote the synthetic Memory Manager and Resource Manager into coherent
   minimal services.** Handles, movable blocks, `TopMapHndl`/`SysMap`, resource
   type/ref/name lists, and map growth must live in RAM with stable ownership,
   not as per-PC synthetic reads.
4. **Implement enough real LC hardware to reach boot media.** Prioritize VIA/RTC
   and SCC/ADB glue, then SCSI/SWIM read-only boot media, while keeping current
   I/O summaries to identify the next missing device register.
5. **Keep the Tab5 display path as a proven output sink.** The indexed 512x384
   framebuffer already reaches the panel; after ROM boot reaches video globals,
   wire the guest framebuffer/CLUT into that path, then add touch as ADB mouse.

The current validated frontier is now `logs/serial-capture-20260528-215618.log`:
the 100M-cycle ROM-entry micro-probe no longer reaches the diagnostic monitor,
no longer hits the low-RAM illegal callback at `0x00007fba`, no longer needs any
low A-trap table synthetic reads for the retired set including A019/InitZone and
the observed Memory Manager traps, and advances past the repeated A06E/
SlotManager scan using a bounded no-PDS SlotManager surface. It now allocates an
empty master pointer for the observed huge provisional `NewHandle` size so the
following `SetHandleSize` operates on a real handle, with larger handle metadata
tracking for ptr/size/lock state. The previous cycle budget expired in the
Memory Manager compaction scan at `pc_after=0x4080ea0a`; the current validated
slice (`serial-capture-20260529-071512.log`) seeds the ROM-observed classic zone
layout instead of a one-field free-block patch: the zone header ends at `+0x34`,
`+0x30` is the allocation rover, and `zone+0x34` contains a real free block.
Repeated reset-table `InitZone` calls are idempotent so the Resource Manager does
not lose allocations. The clean 100M frontier is now the Resource Manager/high-
trap path at `pc_after=0x408099f0`, with no `pc=0xffffffff`, no
`addr=0x01000000`, no `0xffffffd8`, no `0x001fdfec`, no diagnostic exception
stack, no illegal instruction callback, and no A-trap table synthetic reads. A
RAM-parameter temporary-zone seed was tested and rejected because it regressed to
the diagnostic monitor; those calls stay as no-ops until multiple-zone ownership
is implemented coherently. The follow-up Resource Manager-global cleanup seeds
`SysMapHndl` and `CurMap` beside `TopMapHndl`/`SysMap` and improves high-trap
logging so repeated `InitResources`/`ShutDown`/`Enqueue` dispatches are visible,
but the frontier remains the same clean Resource Manager/high-trap loop. A
`DisposePtr` block-reuse experiment regressed to the older `0x4080dc14` handle
path and was reverted to no-op disposal until real Memory Manager free-list
ownership exists. The next Memory Manager slice adds classic-style allocated
block headers for synthetic Ptr/Handle data blocks (8-byte Ptr header, 12-byte
Handle-data header, low-24-bit block size, allocated high-byte flag). The clean
250M diagnostic `serial-capture-20260529-082728.log` remains in the ROM Resource
Manager scan at `pc_after=0x4081ab92`. The current low-memory device-global
slice seeds `$01D4`/VIA, `$01D8`/SCCRd, `$01DC`/SCCWr, and `$01E0`/IWM-SWIM to
LC 24-bit I/O bases, preserving the clean gate while moving later ROM code off
low-RAM pseudo-device addresses; a zero-filled `ReadXPRam` implementation was
rejected because it regressed the Resource Manager scan to the `0x4080dc04`
handle-validation path. The next reference-backed PRAM slice uses mac-rom
`ResourceMgr.a`/`ReDoMap` and the 68kMLA LC475 ROM notes: xPRAM byte `$AE` is a
resource-combo index, and `0`/out-of-range makes ReDoMap fall back to
`ProductInfo.DefaultRSRCs`. Handling only `$AE` as combo index `0` and registering
the RAM-backed ROM map handle with the tiny Memory Manager preserves the clean
`serial-capture-20260529-175305.log` gate at `pc_after=0x4080dd28`. The next
Resource Manager-owned handle slice adds a RAM-backed ROM-resource master-pointer
slab at `0x8400..0x8700` for `HandleZone(RomMapHndl)`, because LC ROM resource
relative handles are small offsets (`0x5c..0x24c`) and returning the actual zone
base lets ReDoMap write master pointers into the zone free-block header at
`zone+0x34`. The slab preserves the clean 100M gate in
`serial-capture-20260529-195930.log` and the clean 250M gate in
`serial-capture-20260529-194728.log`, but does not yet move the frontier past
`pc_after=0x4081ab92`. Forcing the separate `$8A` PRAM byte to
BasiliskII/SheepShaver defaults, seeding non-zero `ProductInfo.DefaultRSRCs`
values (`2` or `4`), returning xPRAM `$AE=1` (`AppleTalk1`), reserving the
master-pointer range inside the zone, and copying the ROM candidate ProductInfo
record at ROM offset `0x6de60` with `DefaultRSRCs=1` were all rejected because
they regressed into earlier handle-validation or diagnostic-monitor paths. The
post-reset probe confirms `$0DD8` has been overwritten by the RAM-fill pattern
(`0xdb6db6db`) by the time the Slot Manager probe needs it, so the fallback
`0x9100` descriptor remains deliberate until the real reset/UniversalInfo path is
preserved. A bounded synthetic `_MaxBlock` (`A061`) Memory Manager surface was
added for later Resource Manager growth code, but the current 100M/250M traces do
not hit it before the frontier, so it preserves rather than advances the clean
frontier. The 2026-05-29 long run `serial-capture-20260529-214440.log` confirms
the current accepted slice is still clean at 500M cycles, now ending at
`pc_after=0x4081abf0` inside the ROM Resource Manager `CountCombos` scan, with no
synthetic A-trap table reads, no illegal callback, no diagnostic exception stack,
no low-RAM invalid-execution trace, and no zero-RAM or monitor stop. Follow-up
instrumentation in `serial-capture-20260529-222234.log` confirms `CountCombos`
completes a full 63-entry ROM-resource scan with an approximate map size of
`0x1ab0`, then is invoked again; the failure mode is repeated
`InitResources`/ReDoMap ownership rather than one stuck list walk. A renewed
allocation-record/free-list reuse trial after block headers was also rejected:
reusing disposed Ptr blocks (notably the `$0DB8` dispatch-table allocation)
stopped the 100M run in low RAM at `pc_after=0x0000011c`, so `DisposePtr` remains
a no-op until zone/master-pointer ownership is modeled coherently. A controlled
`LC_PRODUCTINFO_DEFAULT_RSRCS=1` trial was also rejected: it let ReDoMap add ROM
resources, but later executed through an attribute-tagged resource-map address
and fell into the diagnostic monitor at `pc_after=0x40849ff8`. A narrow
reference-backed `_StripAddress` fix now strips `D0` (matching `ResourceMgr.a`'s
RelHandle/RLocn call sites) instead of deriving `D0` from `A0`; this is accepted
under the default-zero path but does not by itself make combo 1 safe. Non-zero
ProductInfo defaults therefore remain gated off until the ROM map
reference/handle/high-byte semantics are modeled completely. Follow-up rejected
combo-1 probes on 2026-05-30 showed the first bad execution earlier than the
monitor: the low A-trap dispatcher returned from `0x40809a18` into
`pc=0x5807b400` with Resource Manager state such as `d2=0x5807b4c0`,
`a0=0x5807ba80`, and `a4=0x000030b8` before later checkpoints reached
`0x58091a06` and the same `0x40849ff8` monitor. Narrow experiments that restored
high-dispatch table entries, treated `$58xxxxxx` as ROM-resource offsets, or
stubbed a ROM-resource `_Read` did not change the rejection. That points back to
map-handle identity / `RomMapHndl` lifetime: the ROM is falling into ordinary
file-read/resource execution paths instead of consistently recognizing the ROM
map and using `DoRomEntry`. The next step is to replace host-side
allocation records with guest-visible free-list/zone ownership and a complete
ProductInfo/ROM-resource-map model, so the Resource Manager can stop rescanning
and proceed toward boot-device probing. The plan
below treats all current low-memory/trap/resource-manager special cases as
temporary diagnostics to be retired behind coherent subsystems.

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

The bounded on-device ROM-entry micro-probe first established `0x0040008c` in the
24-bit ROM window as a guarded execution target. It reaches the guest `RESET`
instruction, advances into the next ROM dispatcher, and records first explicit
I/O stub accesses from ROM PCs around `0x00403124`-`0x0040314a` to
`0x00f01c00`, `0x00f21c00`, and `0x00f41c00` (classified as
`early-rom-probe-1c00-stride`). That loop is now modeled as a provisional
VIA-style IER alias: writes set/clear IER bits and reads return bit 7 plus the
current enable mask, which advances the ROM past the previous repeated
2832-read/3776-write loop. A `0x00800000` masked ROM alias was added after the
guest switched toward the `0x40800000` ROM window and the 68EC020 callbacks
fetched masked `0x008xxxxx` addresses. High addresses from `0x00400000` up to the
I/O window, plus the top 16 bytes of the 24-bit space, are now modeled as
non-present RAM-size probe locations so the ROM can discover the configured 4MB
RAM boundary without unexpected-write panics. Newly named early VIA-like accesses
include `0x00f01e00`, `0x00f00600`, `0x00f00400`, and `0x00f00000`; the
`0x00f14800` range is separated as `early-f14000-device`, and `0x00f04000` is
separated as an SCC-like `early-f04000-device` no-input status/data block when
monitor paths reach it.
This establishes `0x0040008c` as the first guarded execution target, while the
real reset overlay/vector mechanism remains to be modeled. The latest diagnostic
also validates the memory-bus harness and Musashi callback bridge: 4MB PSRAM RAM
reads/writes, mapped ROM reads including the masked ROM alias, generic I/O stub
reads/writes, ROM write blocking, RAM-size probe handling, unmapped-read logging,
and a RAM-only synthetic 68EC020 reset/execute smoke test (`reset_pc=0x100`, `reset_sp=0x2000`,
`cpu_type=3`). It also now shows the LC indexed diagnostic pattern on the Tab5
panel in the normal LC target; user confirmation reported the test pattern
visible. The current diagnostic default is `0x00402e00`, seeded with the caller
frame pointer/continuation used by the reset trampoline (`a6=0x004000b4`). The
earlier direct `0x00402e00` probe without that caller `a6` seed jumped through
`a4=0x40400000`, executed ROM header/fingerprint bytes, raised an A-line
exception with zero low vectors, and fell into zero RAM; that is now treated as an
invalid entry precondition rather than boot progress. A local macemu/BasiliskII reference search matched this range to the same
`0x50f00000 / 0x50f14000` physical NuBus/slot video-probe family documented in
`BasiliskII/docs/AARCH64_JIT_BRINGUP.md`; `BasiliskII/src/rom_patches.cpp` skips
those physical probes because BasiliskII provides video through its generated
Slot Manager declaration ROM (`slot_rom.cpp`) and EMUL_OP video driver. Cydintosh
keeps the LC ROM unpatched, so the current diagnostic reports only the observed
ready/complete bits at `early-f14000-device` offset `+0x0804`. A one-bit trial
advanced out of the inner write/poll loop but stopped in the outer wait at
`0x40845e3a`; reporting bits 0 and 1 (`0x03`) advances through the slot/video
probe. The `serial-capture-20260526-212157.log` hardware capture reached the
broad ROM diagnostic/serial-monitor guard at `0x40849eae` after 49.5M cycles,
with `d7=0x01000304` (bits 24, 9, 8, and 2 set), first `0x00f04000` status read
value `0x04`, and `stopped_on_monitor=1`. The latest hardware capture
(`serial-capture-20260526-213538.log`) narrows that result: after a conservative
SCC-like transmit-ready/no-input status stub, the bounded probe advances through
monitor initialization and stops at `0x40849fca`, the serial command/read poll,
with `d0=0x00008000` (no input), `d7=0x01020304`, and no fake receive data.
Follow-up ROM watchpoint instrumentation (`serial-capture-20260526-214830.log`,
`serial-capture-20260526-215201.log`, `serial-capture-20260526-220805.log`, and
`serial-capture-20260526-221437.log`) confirms the seeded reset body returns to
`0x408000b4`, branches to the normal reset continuation at `0x408008e0`, enters
reset dispatch at `0x4084641c`, reaches the `0x40845c0c` slot/video probe, then
runs a long RAM-fill/check at `0x40846850`. The first version of the VIA model
kept the ORA/no-handshake bit 0 latched high and skipped D7 bit 26 at
`0x40846494`; modeling ORA reads as external-pin state with bit 0 low now reaches
`0x40846462` and carries D7 bit 26 into the later preflight path. This is progress in the reset-dispatch model, and a later diagnostic showed why
the previous RAM-fill path went through a bogus illegal exception: the seeded
entry path's RAM-region descriptor list is at top of RAM (`0x043fffe4`, masked to
`0x003fffe4`) and was overwritten by the destructive RAM fill. A diagnostic-only
synthetic descriptor-list read for the reset-region loop now avoids the odd-PC
illegal at `0x40846905`. The later captures through `serial-capture-20260527-074655.log` extend that
synthetic list through the reset copy/vector-relocation reads, so the RAM
lane/copy test at `0x40846c5c` returns with `d6=0` instead of `0x00007fff`, and
VBR is no longer seeded from the RAM fill pattern when the reset subtests install
autovectors. The provisional `early-f04000-device` SCC model now supports
selected-register read/write, local loopback data, and diagnostic timer IRQ
pulses; the ROM passes the `d7=0x84`, `0x86`, `0x85`, `0x87`, `0x88`, `0x89`,
`0x8b`, `0x8c`, and Basilisk-informed no-FPU `0x8d` reset diagnostics. Later
captures (`serial-capture-20260527-103318.log` and
`serial-capture-20260527-103821.log`) show the normal BootGlobs walk at
`0x40800a90` and the post-reset memory-layout entry/header at
`0x4084168e`/`0x4084172c`. A narrow synthetic post-reset address-map dispatch
now carries the second pass through `0x408418e4` and `0x40841922` instead of the
CritError video/monitor path. A one-bank synthetic record table at the observed
`-0x58(A5)` local also prevents the expanded record-copy loop
`0x40841b1e`–`0x40841b3e` from walking past the 4MB RAM top into the 24-bit ROM
window at `0x00400000+`. A provisional finalizer descriptor table at
`0x00600000` also avoids the first `0x40841cbe` misdispatch and returns through
`0x40841cda`. Additional watchpoints and a provisional loop cap showed that the
expanded post-reset memory-layout pack/compress path can be forced through
`0x40841bd4`, but BasiliskII's `InitMMU` patch points to a cleaner no-MMU path
for this 68EC020 scaffold. The current hardware captures now synthesize the
observed no-MMU flag at `0x4084169a`, seed the direct-probe threaded return frame
for `0x408416a2 -> 0x4080130a`, skip the immediate reset-continuation `A001`
trap at `0x40801314`, and provide low-memory callback `$0DBC` as a ROM `RTS`.
This avoids the earlier `0x408416e0` FPU frontier. Follow-up captures add a
narrow `A05D`/SwapMMUMode dispatcher bypass for the EC020 exception-frame
mismatch, split 24-bit ROM-window writes from masked ROM instruction-shadow
writes, and seed a minimal Resource Manager `TopMapHndl`/`SysMap` chain. The ROM
now advances beyond the previous `0x40809a04` trap-table jump and the
`0x01000000` resource-map walk. `serial-capture-20260527-145248.log` also fixes
the later bad Resource Manager map/ref-list fall-through (`pc=0xffffffff`, read
`addr=0x01000000`) by making the synthetic `CURS`/`FONT`/`KMAP` reference IDs
match the observed reset lookup ID (`d2=8`). The bounded probe now reaches the
ROM diagnostic/serial monitor guard at `0x40849ff8` with `d7=0x010a7a6e` and no
`pc=0xffffffff` access. A reverted narrowing experiment in
`serial-capture-20260527-150848.log` proved that the broad temporary
`0x40809a04` dispatcher NOP was masking invalid low-memory trap-table entries:
when only the observed `A05D` dispatch was NOPed, the next trap vectored through
low address `0x00000002` and looped in an illegal-instruction stack spiral. The
`serial-capture-20260527-152004.log` tranche replaced that broad NOP with a
bounded synthetic low-memory A-trap table for dispatcher reads; it logs observed
early traps `0x047`, `0x03f`, `0x051`, and `0x019`, routes them through a ROM
`moveq #0,d0; rts` handler, and leaves only the `A05D` dispatch on the
EC020-frame NOP path. The follow-up captures through `serial-capture-20260527-154729.log` added a
bounded `A02E`/BlockMove side effect for plausible RAM destinations/counts and a
plausibility-gated `A047`/SetTrapAddress side effect. This let the ROM's early
copied table at `0x408006f0 -> 0x001fdfb2` materialize while refusing later
bogus negative-size Resource Manager copies and rejecting a fill-pattern
`SetTrapAddress(A05D, 0xb6db6db6)`. RAM-execution tracing then showed the later
illegal callback was execution of copied table/data around
`0x001fdfb0`/`0x001fdfec`. The `serial-capture-20260527-155855.log`
tranche adds a tiny synthetic Memory Manager surface for the observed post-reset
traps (`NewPtr`, `NewHandle`, `HLock`, `SetHandleSize`, `GetHandleSize`, and
`StripAddress`). That moves Resource Manager state away from the copied table and
eliminates the `0x001fdfec` illegal callback. The
`serial-capture-20260527-180650.log` tranche then makes the synthetic resource
map closer to a real Resource Manager map by adding the map header fields and
post-RAM-fill synthetic low-memory reads for unaligned `$02AE`/`ROMBase` and
`$031A`/resource-offset mask. This changes the first Resource Manager map-growth
copy from a bogus negative count (`0x00ffffb6`) to a bounded in-map copy
(`0x000002d6`). `serial-capture-20260527-181413.log` goes one step further:
the synthetic resource map is seeded into RAM, the name-list offset is moved past
the type/ref-list records, and synthetic `SetHandleSize` preserves handle
contents. Resource Manager growth copies now operate on RAM-backed map addresses
with bounded counts (`0x000002b8`, `0x00000018`, `0x0000000c`) instead of the
previous negative counts. The probe still reaches the ROM diagnostic/serial
monitor at `0x40849fca` with `d7=0x010a7a6e` and no
`pc=0xffffffff`/`addr=0x01000000` regression. `serial-capture-20260527-185030.log`
confirms the next malformed trap-return symptom and repair: the ROM's high
A-trap dispatcher at `0x408099d6` was about to `RTS` through low-memory
vector-table data at `0x0000003e`, which branched to `0xffffffd8` and raised an
F-line diagnostic frame (`format_vector=0x002c`). The current diagnostic guard
rewrites only that observed invalid return target to the ROM `moveq #0,d0; rts`
helper, clearing the `0xffffffd8` path. The expanded stack log shows the
exception-frame continuation candidate `0x40800218`, but a rejected
`serial-capture-20260527-184743.log` experiment that resumed directly there
regressed to `pc=0xffffffff`/`addr=0x01000000`, so it was reverted. The
`serial-capture-20260527-191451.log` tranche expands the synthetic high A-trap
read window to the ROM dispatch table range `$0E00..$2DFF`; this covers observed
`0x408099c6` table reads such as `$1054` for `A895`/ShutDown and removes the
need for the previous narrow high-trap return repair on that path. The exposed
frontier remains an illegal instruction callback at `pc=0x00007fba`
(`opcode=0x7562`, `format_vector=0x0010`) before the ROM still reaches the
serial monitor guard at `0x40849ff8` with `d7=0x010a7b6e`. A tested direct
`A895` frame-bypass experiment briefly advanced past the low-RAM callback but
regressed to `pc=0xffffffff`/`addr=0x01000000`, so it was reverted. The next
milestone is to replace the temporary post-reset finalizer/pack/compress,
trap-table, Memory-Manager, and resource-map probes with a coherent
low-memory/VBR/address-map model rather than masking the monitor path. The first
RAM-owned low-memory seed scaffold is now present behind
`LC_ENABLE_RAM_OWNED_LOW_MEMORY=0`; `serial-capture-20260527-194755.log` confirms
that the default build still reaches the same `0x40849ff8` / `d7=0x010a7b6e`
frontier, with no `pc=0xffffffff`, `addr=0x01000000`, `0xffffffd8`, or
`0x001fdfec` regression. An enabled-seed A/B flash/capture
(`serial-capture-20260527-195516.log`) logs the RAM-owned seed and also reaches
the same frontier, proving the seed scaffold itself is neutral before synthetic
read hooks are retired. The next tranche lets high A-trap table reads use
plausible ROM handlers already present in RAM instead of the universal synthetic
success handler. Captures through `serial-capture-20260527-201224.log` then
advance through additional Resource Manager map growth and prove the remaining
low-RAM path starts when `0x40800584` (`RTS` after `A895`/`ShutDown`) pops
`0x00004080` from the stack and executes RAM-fill pattern words before the
`0x00007fba` illegal callback. The `serial-capture-20260527-215107.log` tranche
eliminates that callback without a direct continuation bypass by keeping the
Resource Manager/GDevice scaffolds out of the popped low-RAM path, seeding the
post-reset probe dispatch globals `$0DB8`/`$0DD8`, and moving the synthetic heap
below the top-of-RAM stack. The `serial-capture-20260527-222958.log` tranche then
moves `$0DB0`/`$0DBC` and the Resource Manager globals (`$02AE`, `$031A`,
`$0698`, `$069C`, `$07F0`, `$0A50`, `$0A58`, `$0A60`, `$0AF2`) into RAM-owned
seed cells and removes the per-PC Resource Manager map read hook while preserving
the 100M-cycle frontier. The follow-up `serial-capture-20260528-191240.log`
tranche adds a minimal RAM-owned InitZone/Memory Manager seed, refreshes the
line-A vector and immediate A019/A02D table entries as part of that same low-
memory model, and removes the final A019 low-trap table synthetic read. The
`serial-capture-20260528-201344.log` tranche then adds a bounded no-PDS
SlotManager result surface, makes later stack-range InitZone calls no-ops until
multi-zone ownership exists, guards the tiny heap against size wraparound, and
retires the observed Memory Manager low-trap table entries through A024/A029.
The follow-up `serial-capture-20260528-202210.log` makes huge provisional
`NewHandle` calls allocate an empty master pointer for the later SetHandleSize,
pushing the clean frontier to `0x4080dc26`; `serial-capture-20260528-215618.log`
then reaches the Memory Manager compaction scan at `0x4080ea0a` with the same
clean gate. The remaining per-PC scaffolds are now the dispatcher/return repairs,
Resource Manager/video prototypes, and earlier address-map/BootGlobs probes
rather than low-trap table reads.

## 2026-06-06 host boot_3 progress

The host LC harness now gets past the boot_3 progress-picture high-PC return
that previously jumped from the progress SetPort restore path into values such
as `0xf4800000`. The narrow fixes are:

- handle `_DrawPicture` as site-aware: caller-cleaned sites that immediately
  execute `ADDQ #8,SP` keep their two long arguments on the stack; progress
  sites without caller cleanup let the Toolbox trap pop the PicHandle and Rect;
- keep `_CloseResFile` on the normal 2-byte refNum signature in this progress
  path instead of over-popping the adjacent scratch frame;
- skip uppercase `PTCH` resources as well as lowercase `ptch` until the patch
  loader/resource execution model is safe;
- add minimal `_TextFace`, `_StringWidth`, and `_GetString` signatures needed by
  the progress text path.

Verification log: `logs/host-lc-final-stackfix-20260606-135912.log` using
`HOST_LC_ENTRY_BASE=0x40800000u`, `HOST_LC_ENTRY_OFFSET=0x0008cu`, 16MiB guest
RAM, HD200MB, and `--basilisk-compat`. The run reaches later copied boot_3/PTCH
control flow and stops on a new low-PC illegal-instruction frontier
(`opcode=0x017a` around `0x00bf9fec`/`0x00bea3d6`), with the framebuffer still in
status-overlay mode rather than a Mac desktop.

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
