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

## 2026-06-10 BasiliskII oracle + early-boot patch parity

Goal reframed: *port BasiliskII to the ESP32, simplified for the LC.* Stood up
the reference BasiliskII (already built at `projects/macemu/BasiliskII/src/Unix/
BasiliskII`, aarch64) as a **live boot oracle** using our exact ROM + disk:

- Prefs `/tmp/b2-oracle-prefs`: `rom vendor/mac-lc.rom`, `disk HD200MB`,
  `ramsize 8388608`, `modelid 14`, headless via `SDL_VIDEODRIVER=dummy`.
- Trace env: `B2_ROM_HARNESS`, `B2_TRACE_BOOT_STAGE`, `B2_TRACE_EMULOPFLOW`.

Key findings (decisive):

- The reference BasiliskII **boots our exact ROM+disk to the OS** and **never**
  visits ROM offset `0x42xxx`/`0x41xxx`. Our long-standing stuck frontier
  `0x4084271c` (TextServices/Component walk) is a **dead path** the real boot
  never executes — chasing it was wasted effort.
- Real native-call boot recipe: RESET -> CLKNOMEM -> PATCH_BOOT_GLOBS ->
  SPECIALTIES -> IRQ -> SCSI_DISPATCH -> CHECKLOAD -> DISK_PRIME -> System.
- Our boot skipped CLKNOMEM entirely and diverged into high-ROM hardware-
  detection paths because of **three early-boot patch gaps** vs `patch_rom_32`.

Fixes landed (all mirror BasiliskII `rom_patches.cpp` exactly):

1. **GetHardwareInfo / VIA-init skip** — NOP ROM `0xc2` (2 words) + `0xc6`
   (15 words). Without it, the threaded-init `jmp 0x2f18` at `0xc2` walks into
   the machine-id / hardware-detection dispatch the emulator can't satisfy.
2. **ClkNoMem signature fallback** — `find_rom_trap(0xa053)` returns the
   `jmp (a5)` thunk on ROM32; locate the real routine by signature
   `{40 c2 00 7c 07 00 48 42}` in `0xb0000..0xb8000` before patching.
3. **`find_rom_trap` signed-offset bug** — branch-table offsets are *signed*
   `int16` (`((b<<8)|next)<<1`). Our port added them as unsigned, so any offset
   with the top bit set resolved the wrong direction. This mis-resolved
   `_ClkNoMem` to `0x4b1e4` instead of `0xb1e4` (and corrupted every trap
   patched this way). Fixed to signed accumulation.

Result: boot now follows the oracle rail RESET -> CLKNOMEM (fires 79x, was 0) and
proceeds much further before the next divergence. Verified over 50M and 500M
Basilisk-compat runs: `HOST_LC_OK`, illegal=0, unknown=0, getpic0=0,
stopped_on_zero_ram/monitor/zero_rom=0, VRAM writes=4 preserved.
Patch match improved patterns_found 18->19, missing 14->13.

**Next divergence (open):** after CLKNOMEM, boot reaches `0xb27c` -> `0x15274`
-> `0x41xxx` (post-reset memory-layout), which the oracle avoids. Continue
porting the remaining `patch_rom_32` early-init patches (SPECIALTIES path,
remaining hardware-skip / memory-layout patches) using the oracle diff.

Methodology to reuse: run the oracle, diff its BOOT_STAGE/EMULOPFLOW + PC visits
against our `BOOTTRACE`, fix at the *earliest* divergence rather than at the
final stuck PC. The `lc_basilisk` `LC patch MISS ...` log lines surface every
unmatched `patch_rom_32` signature on our ROM.

### PatchHWBases ported (hardware bases -> scratch RAM)

The ROM decoderInfo (at offset 0x348c, reached via UniversalInfo+rel) holds the
hardware register base addresses (VIA/IWM/SCC/ASC/SCSI...) at 0x50fxxxxx. Walk
the table at ROM 0x94a (pairs of offset/4 + lowMem-global, terminated by 0xffff)
and rewrite every slot (except the ASC base, lmg 0xcc0) to scratch RAM
`0x00f00000`, exactly like `patch_rom_32` PatchHWBases. This corrects the
post-CLKNOMEM `a1` from `0x50f00000` (unmapped hardware) to `0x00f00000`
(backed scratch), matching the oracle. Verified 50M+500M: all invariants hold
(HOST_LC_OK, illegal/unknown/getpic0=0, zero-ram/monitor/zero-rom=0, VRAM=4),
CLKNOMEM fires 79x.

**Open limb (next):** boot still reaches the dead high-ROM `post-reset-memory-
layout` path (0x41xxx) via the V8 memory-controller detection routine at ROM
`0x15274` (called from `0xb27c`, itself from the threaded-init stage at `0xf2`).
That routine reads `a0@(8)` (a decoderInfo slot) and branches on `d1 & 0x70 ==
0x20`; with scratch hardware returning 0, `d1` likely mis-selects the RAM-config
path. Cracking this needs a PC-level oracle diff (instrument the reference
BasiliskII to log the `0x15274` register state / branch taken, or use
`--break 0x815274`) so we mirror the exact V8 memory-controller contract rather
than guessing. This is the highest-leverage next step.

### Oracle PC+register diff built; CLKNOMEM XPRAM ported; RAM-sizing isolated

Used the reference BasiliskII's windowed PC trace (`B2_TRACE_PC_START` /
`B2_TRACE_PC_END` / `B2_TRACE_LIMIT`, prints full D0-D7/A0-A7 per instruction)
as a register-level oracle. Findings:

- At ROM `0x15274` our `d1` and branch decision **match** the oracle (both take
  `bne 0x152fa`); `0x15274` is NOT the divergence.
- The divergence is the **RAM-sizing probe**: at `0x15308` (after the threaded
  ClkNoMem/PRAM chain at `0xb288`) the oracle has `a0=0x00800000` (top of its
  8MB RAM) while ours has `a0=0xffffffff` (failed probe). The oracle keeps
  `a0=decoderInfo` through the whole `0xb288` chain, so the RAM-top value is
  computed right after the chain returns. Our run executes a *single* pass of
  the `0x152xx` sizing loop and exits to the dead `0x41xxx` path; the oracle
  loops it once per RAM bank.
- Ported BasiliskII's **CLKNOMEM** emul_op faithfully: a persistent 256-byte
  XPRAM (with BasiliskII's default signature/values), proper PRAM read/write
  persistence, RTC time-byte reads, and result returned in `d2` only. Fixed two
  real bugs in the old stub: it clobbered `d1` with the result (BasiliskII never
  touches d1) and ignored PRAM writes (returning hardcoded values), so
  write-then-read returned wrong data. Verified 50M+500M: all invariants hold
  (HOST_LC_OK, illegal/unknown/getpic0=0, zero-ram/monitor/zero-rom=0, VRAM=4,
  CLKNOMEM 79x). This did not by itself move `a0`, confirming the RAM-top value
  comes from the RAM-probe logic, not the XPRAM contents.

**Next:** trace the exact instruction in/after the `0x152xx` sizing loop where
`a0` is set (widen the oracle window past the `0xb288` chain return) and model
the V8 RAM-bank aliasing so our 16MB RAM probe computes a coherent top-of-RAM
instead of `0xffffffff`. The `post-reset-memory-layout` watchpoint cluster in
`lc_musashi_bus.c` (0x4167e..) maps the dead path we must avoid by sizing RAM
correctly here.

#### Correction: 0x4168e is a trampoline, not a dead path; RAM table flows via BootGlobs

Further oracle register tracing corrected two earlier assumptions:

- ROM `0x4168e` (the `post-reset-memory-layout-entry` watchpoint) is NOT a dead
  path. It is a `4efb` computed-jump trampoline: `0x1306 (jmp base+0x4038e) ->
  0x4168e -> 0x15308`. The oracle passes through it too. The watchpoint cluster
  names in `lc_musashi_bus.c` are misleading; that region is normal boot.
- The RAM size is NOT read from a hardware probe. BasiliskII's RESET emul_op
  builds a RAM **bank table** at BootGlobs (`RAMBaseMac+RAMSize-0x1c`):
  `+0=bank0 base (0)`, `+4=bank0 size (RAMSize)`, `+8=0xffffffff (end marker)`,
  `+0xc=0`. The boot reads it at `0x800aa0` (`movem.l fp@,d3-d4/a3-a4; exg
  d4,a3`). Our RESET already writes this table correctly, and our boot reaches
  `0x800a70 -> 0x801300 -> 0x4168e -> 0x15308` just like the oracle.

The remaining divergence is in the **per-bank RAM-walk loop** around
`0x15274/0x152fa/0x15308`. At `0x15308` the oracle has `d3=0, d4=0xffffffff`
(bank-table end marker reached) while ours has `d3=1, d4=RAMSize` (still mid-walk
with a phantom extra bank). Setting our harness RAM to 8MB to match the oracle
did NOT help — it regressed to `stopped_on_zero_ram=1` at low PC `0x88a` — so the
bug is in the bank-walk loop state (the `d3` bank counter / loop termination),
not the RAM size per se. Next: step-diff the `0x152xx` loop body between oracle
and ours to find where `d3` diverges (where the loop fails to see the
`0xffffffff` end marker and terminate after one bank).

#### Root cause + FIX: PATCH_BOOT_GLOBS clobbered the RAM bank table

Step-level register trace of ROM `0x800a70..0x800ab0` plus the bank-table reads
found the real cause (the earlier "8MB cap" theory was wrong — 8MB also showed
`d3=1`).

Our RESET builds the BootGlobs RAM bank table at `boot_globs = ram_size-0x1c`:
`+0=base(0)`, `+4=size`, `+8=0xffffffff (end)`, `+0xc=0`. But the inline
PATCH_BOOT_GLOBS work in RESET forced `a4 = ram_size` (top) and then wrote the
MemTop/No-MMU fields relative to `a4`:

- `a4-25 = boot_globs+3` <- No-MMU flag `|= 1` set the bank-table **base** byte3
  to 1, so `[boot_globs] = 0x00000001` (phantom bank -> `d3=1`).
- `a4-20 = boot_globs+8` <- MemTop write overwrote the **end marker**
  `0xffffffff` with `ram_size` (-> `d4=ram_size`).

That is exactly why the ROM RAM-walk at `0x800aa0`/`0x15274` saw `d3=1,
d4=ram_size` instead of the oracle's `d3=0, d4=0xffffffff`, diverging the size
computation. Fix: re-assert the four bank-table words after the PATCH_BOOT_GLOBS
writes in RESET, so the table RESET set up survives. Verified at `0x15308`:
now `d3=0, d4=0xffffffff, a3=0x1000000` (our 16MB; oracle's `0x800000` is just
its 8MB), matching the oracle's bank-walk result exactly. 50M+500M green:
HOST_LC_OK, illegal/unknown/getpic0=0, zero-ram/monitor/zero-rom=0, VRAM=4.

The RAM-bank walk is now faithful to the oracle. The probe pc still ends in the
later `0x42xxx` region after 50M/500M; the next divergence is downstream of the
(now-correct) RAM sizing — continue the oracle PC+register diff forward from
`0x15308` to the next point our path leaves the oracle rail.

#### Forward diff status (post bank-table fix)

With the bank table fixed, the boot init cascade at ROM `0x100..0x176`
(`bsr 0x910/0xa30/0x9c0/0x9a0` etc.) proceeds healthily and matches the oracle's
flow (registers `d0/a3/sp` sane throughout). Note: our Musashi instr-callback
fires at a finer granularity than the oracle's UAE dispatch, so apparent
"mid-instruction" PCs like `0x122` in our trace are callback-granularity
artifacts, not real divergences.

The dead linked-list walker at `0x426c4..0x42722` (`a3 = [a3]` until `a3==0`,
checking node+28 bit3 / node+24 / node+22 — a Toolbox queue walk) is now reached
much later (icnt ~37761, after correct RAM sizing) with **bogus state**:
`a3=0xffffffff`, `fp=0x41edfe70` (not a valid RAM/ROM address), and garbage
return addresses on the stack. The oracle never executes `0x42xxx`. So the
upstream corruption happens between the healthy cascade (icnt ~4000) and the
walker entry (icnt ~37761). Next: forward-diff that window to find where `a3`/
`fp` first go bogus (likely a Toolbox/Component/queue call reached with an
uninitialized handle), and model that contract the BasiliskII way.

#### ROOT CAUSE: harness boot-fixture scaffolding vs BasiliskII disk boot

Forward-diffing the corruption window pinned the root cause. Tracing entry into
the `0x840xxx` component routine (icnt ~37600) and the `0x42xxx` walker
(icnt ~37761) shows both are reached from the **copied boot_3 image** at
`0x00bexxxx` with `a3=0x0004ff08` (the boot_3 handle), `a1=0xf3381aec` /
`a0=0xf3374aec` (bogus component-table pointers). Crucially, the reference
BasiliskII **never visits `0x840xxx` or `0x842xxx`** (0 hits across 60k traced
ROM instructions).

The chain `copied boot_3 (0x00bexxxx) -> component/TextServices (0x840xxx) ->
queue walker (0x426c4) -> 0x4084271c` originates entirely from the harness's
**injected `boot_3.bin` fixture**. `host_load_boot_resources()` in
`tools/host_lc_harness/host_esp_stubs.c` loads static `fixtures/boot_2.bin`
(648B @ handle 0x4ff00) and `fixtures/boot_3.bin` (31420B @ handle 0x4ff08) into
guest RAM, and serves System resources from a host-side fork on demand. That
`boot_3` code resource runs Component Manager / TextServices logic that
dead-ends because the surrounding System environment is faked, not the real
HFS-resident System.

BasiliskII does NOT do this: it lets the ROM read the real boot blocks / System
file from the HFS volume on the disk image via the patched `.Sony`/`.Disk`
driver (DISK_PRIME, which we already have), and the real System + Process
Manager take over in low RAM (oracle PCs `0x0003xxxx`/`0x0007xxxx`). That is why
the oracle never touches the `0x840xxx`/`0x842xxx` component dead path.

**Architectural pivot for the next phase (faithful BasiliskII boot):** retire
the `boot_2.bin`/`boot_3.bin` fixture injection and the on-demand host-side
resource arena; instead drive the ROM's normal HFS boot — read boot blocks from
the HD200MB volume via the disk driver, let the ROM load `boot 1/2/3` and the
System file from the volume, and run the real System. The early-init port work
landed this session (find_rom_trap, GetHardwareInfo/VIA skip, ClkNoMem+XPRAM,
PatchHWBases, bank table) is exactly what gets the ROM to the point where it
can do that disk boot coherently. This is the single highest-leverage next
step and directly serves “port BasiliskII, simplified for the LC.”

#### Faithful-disk-boot scaffold (env-gated) + first result

Added `LC_FAITHFUL_DISK_BOOT` env gate in `lc_musashi_bus.c` that skips all three
`lc_musashi_bus_stage_boot_resources()` calls (RESET, INSTALL_DRIVERS, and the
post-boot-block DISK_PRIME hook) and the `$0dbc` boot_2 trampoline. Default
(unset) preserves the working baseline exactly (50M green: HOST_LC_OK,
illegal/getpic0/zero-ram=0, VRAM=4, ends in the dead walker `0x426d0`).

With `LC_FAITHFUL_DISK_BOOT=1` the ROM now runs the **real boot path**: it reads
the real boot blocks from the HFS volume via DISK_PRIME (`$800=0x4c4b` 'LK'
signature), executes the real `boot 1` code, and jumps to the boot-2 staging
area `0x900000` — which is empty because we no longer inject `boot_2.bin`, so it
stops at `pc=0x900010` (`stopped_on_zero_ram`). This is the real boot reaching
the point where `boot 1` must load `boot 2`/the System file from the HFS volume.

**Next:** make `boot 1`'s System-file load work over the HFS volume — i.e., the
ROM's File Manager / Resource Manager reading the System file via the disk
driver (BasiliskII relies on the real ROM Toolbox for this, reading the volume
through the patched `.Sony`/`.Disk` driver). Bring up enough of the HFS read
path that `boot 1` loads and jumps into the real `boot 2`, then `boot 3` +
System, instead of the empty staging area. Iterate behind the env gate so the
baseline stays green until the faithful path overtakes it.

#### boot 1 trap sequence traced (HFS volume bring-up)

Under `LC_FAITHFUL_DISK_BOOT=1`, the real `boot 1` code (boot blocks at `$800`,
entered from ROM `0x11c8`) executes this trap sequence before jumping to the
boot-2 staging area `0x900000`:

| trap | name | role |
|---|---|---|
| `0xa02e` x3 | BlockMove | copy System/Finder/debugger names from boot blocks |
| `0xa06d` | InitEvents | init Event Manager |
| `0xa06c` | **InitFS** | init File System |
| `0xa00f` | **MountVol** | mount the HFS boot volume |
| `0xa207` | **HGetVInfo** | get volume info |
| `0xa995` | **InitResources** | init Resource Manager |
| `0xa00e` | UnmountVol | |

The critical four (InitFS / MountVol / HGetVInfo / InitResources) must run the
ROM's REAL File Manager / Resource Manager so the HFS volume is mounted and the
System file is readable. Today our A-trap dispatcher (installed wholesale at the
A-line vector `0x408099b0`) **stubs** these (e.g. `0xa06c` is even misidentified
as `_FreeMem` at lc_musashi_bus.c:5813; `InitResources` stubbed at :6561), so
the volume is never really mounted and `boot 1` jumps to an empty staging area.

**Architectural shift for faithful boot:** stop intercepting the A-line vector
wholesale. Let the ROM's own A-trap dispatcher + File/Resource Manager run
(reading the HFS volume through the patched `.Sony`/`.Disk` driver = DISK_PRIME,
which already works), and only keep native EMUL_OP hooks for the specific
driver/hardware traps BasiliskII patches. That is exactly BasiliskII's model and
the last big structural change before a real System load. Stage it behind
`LC_FAITHFUL_DISK_BOOT` so the fixture baseline stays green during bring-up.

#### Real ROM dispatcher path for File Manager traps (gated) + dependency found

Disassembled the ROM A-trap dispatcher at `0x99b0`: OS traps without the
auto-pop bit `jsr` through `[$0400 + (trap&0xff)*4]`; our no-op trap-table seed
matches that layout. ROM handler offsets (via the corrected signed
find_rom_trap): InitFS `0xf368`, MountVol `0xf60e`, UnmountVol `0xfe18`,
InitEvents `0x22e0`, InitResources `0x1aa32` (toolbox), HGetVInfo unimplemented.

Under `LC_FAITHFUL_DISK_BOOT` we now (a) seed those real OS handler addresses
into the trap table and (b) stop intercepting InitFS/MountVol/UnmountVol/
InitEvents in our dispatcher, letting the ROM dispatcher `jsr` the real handlers.
Result: boot 1's File Manager traps run the REAL ROM code without crashing and
advance the boot pc from `0x900010` to `0x900074` (still the empty boot-2
staging area). Default baseline unchanged (50M green, VRAM=4).

**Dependency confirmed:** MountVol ran but did NOT read the volume header (only
the boot-block reads happened), because the real File Manager needs the real
**Memory Manager** (NewHandle/NewPtr building the VCB and disk buffers) and the
real heap (InitZone/SysZone) — which our scaffolding fakes. So faithful boot is
all-or-mostly: the ROM's real OS init (InitZone -> InitMem -> InitFS -> MountVol
-> InitResources) must run as a chain, not piecemeal. Next: under the gate,
progressively let the ROM's real Memory Manager + zone init run (un-stub
NewPtr/NewHandle/InitZone, seed their real handlers) so MountVol can allocate
and actually mount the HFS volume, then read the System file.

#### Real Memory Manager wired in (gated) — boot 1 advances further

Key gotcha: the OS trap table region `$0400-$0600` is a **dynamic read** in
`lc_memory.c` (returns `post_reset_atrap_table_overrides[trap]` or the default
`0x40800d88`), so raw `ram_write32` seeds are ignored. Switched the faithful
seeding to `lc_memory_set_post_reset_atrap_handler(trapword, handler)` which
registers the override the read path honors, and added the Memory Manager
handlers: InitZone `0xcf08`, InitApplZone `0xcd52`, MaxApplZone `0xd2dc`, NewPtr
`0xd360`, DisposPtr `0xd3a0`, NewHandle `0xd448`, DisposHandle `0xd484`,
SetPtrSize `0xd4d0`, SetHandleSize `0xd4b4`, HLock `0xd628`, HUnlock `0xd64e`,
FreeMem `0xd174`, ResrvMem `0xd18a`, CompactMem `0xd0a8` (Sys variants share the
low-byte slot). The dispatcher passthrough was extended to these low bytes.

Progression of the faithful boot pc as the Toolbox chain is un-stubbed (all
advances are real ROM handlers running, no crash; default baseline stays green
at 50M, VRAM=4):

- fixtures only: jumps straight to dead component path
- + real File Manager traps: `0x900010 -> 0x900074`
- + real Memory Manager traps: `0x900074 -> 0x9000a8`

Still ends in the empty boot-2 staging area: MountVol now allocates via the real
MM but does not yet read the HFS volume header. Remaining chain for an actual
mount + System-file load: the **drive queue / DCE** registration (so MountVol's
drive lookup finds the boot drive and issues volume-header reads through the
disk driver), then **InitResources** (toolbox trap `0xa995`, handler `0x1aa32`)
and the System-file open/`GetResource('boot',2)` path. Continue un-stubbing the
chain behind the gate until `boot 1` loads the real `boot 2`.

#### ToExtFS hook fixed — faithful boot runs cleanly past the mount worker

Traced the real MountVol (`0xf60e`) -> mount worker (`0xef94`): at `0xef94` it
reads `[$03E2]` (ToExtFS, the external file-system hook) and `jsr`s through it
when nonzero. Our scaffolding left garbage there, so the worker jumped into
garbage. Seeding `ToExtFS ($03E2) = 0` (no external FS, gated) fixes it: the
faithful boot no longer crashes in the empty boot-2 staging area — it now runs
the full 50M cycles **cleanly** (illegal=0, unknown=0, zero-ram=0, getpic0=0)
along the real boot path and reaches `0x426d0`. Default baseline unchanged
(VRAM=4).

Status: the mount worker proceeds, but the volume header is still not read (1
disk read = boot blocks only), so the System is not yet loaded; boot falls into
the ROM component/queue walker at `0x426d0` (VRAM=0). The next blocker is the
**drive queue / DCE registration**: MountVol's worker needs the boot drive in
the drive queue (`DrvQHdr $0308`, normally populated by the disk driver's
Open/`AddDrive`) so its drive lookup issues volume-header reads through the disk
driver. Bring up the drive-queue entry (drive #, driver refNum, FSID) so
MountVol reads the HFS volume header and builds the VCB, then opens the System.

#### FS low-memory globals were being clobbered by SYS-table seeding

Traced the mount worker further: at `0xf01e` it does `movea.l $0362,a0; movea.l
a0@(8),a1; jsr (a1)` — dispatching via FSQHead (`$0362`). We saw `[$0362] =
0x0d884080`, which is the **misaligned overlap of `0x40800d88`**: our RESET
SYS-table seed ran `for i in 0x02..0x100: [i*4] = ROM+0x0d88`, writing the no-op
handler across `$08-$3FC`. But 68k exception vectors are only `$00-$FF`
(`$00-$FC`); `$100-$3FF` are Mac OS **low-memory globals** (FSQHead `$0362`,
etc.). The seed was clobbering the File Manager globals. Under the faithful gate
the seed now stops at vector `0x40` (`$100`), leaving `$100-$3FF` for the ROM.

With FS globals no longer clobbered, boot 1 returns to the **correct rail**: it
mounts (worker no longer jumps through a garbage FSQHead) and jumps to the
boot-2 staging area `0x900000` expecting `boot 2` — instead of falling into the
dead component walker. Staging is still empty (`boot 2` not loaded), so it stops
there; disk activity is still only the boot-block reads (sector 0 x2 across boot
1's two passes). Default baseline unchanged (50M green, VRAM=4).

Remaining to actually mount + load: MountVol still doesn't issue the volume-
header read (block 2) — the **drive queue / DCE** is still the gap (the boot
drive must be registered so the worker's drive lookup calls the disk driver for
the volume header). After that: build VCB, open System file,
`GetResource('boot',2)`, jump to the real boot 2. This remains a multi-step
ROM-OS-init bring-up; each scaffolding global we stop faking moves boot 1 closer
to the real System-load rail, with the fixture baseline staying green.

#### FS dispatch reaches real ROM; drive lookup returns nsDrvErr (-57)

With FS globals intact, the mount worker's FS dispatch at `0xf01e`
(`a0=[$0362]=0x800000`, `a1=[a0+8]=0x4080f662`, `jsr a1`) now calls a **real ROM
FS routine** (`0xf662`) instead of garbage. That routine does the drive-queue
lookup and returns `d0 = -57 (nsDrvErr, "no such drive")`, so MountVol bails
before issuing the volume-header read.

We already seed a drive-queue entry (DQE at `$8A40`: drive 1, refNum -63, in
`DrvQHdr $0308`) and `BootDrive ($0210)=1`, but the ROM's lookup still fails.
Next: make the drive lookup succeed — verify the requested drive number
(`MountVol` param block `ioVRefNum`/drive at `a0@(22)`) matches `dQDrive`, the
DQE field layout/`dQDrvSz` word order, and the `DrvQHdr` qHead/qTail linkage are
exactly what the ROM's `0xf662` traversal expects (cross-check against Inside
Macintosh DV-05 "Drive Queue Elements" and BasiliskII's AddDrive call:
`d0=(num<<16)|refNum`, `a0=status+dsQLink`). Once the lookup returns the right
DQE/refNum, MountVol issues `_Read` (our `$A002 -> DISK_PRIME`) for the volume
header and can build the VCB.

#### Root fix: seed the FULL OS trap table with real ROM handlers

The nsDrvErr turned out to be one instance of a systemic problem: the File
Manager reaches many internal routines through **patchable OS-trap-slot
vectors** (e.g. `0xfc6c` does `move.l $6c8,-(sp); rts`, where `$6c8` is OS trap
slot `0xb2` = the drive-queue lookup at `0xfc72`). Our scaffolding seeded the
whole OS trap table ($0400-$07FF) with the no-op handler `0x40800d88`, clobbering
every one of these vectors, so the FM broke as soon as it tried to use one.

Fix (gated): at RESET under `LC_FAITHFUL_DISK_BOOT`, resolve the real ROM handler
for every OS trap via the ROM trap dispatch table (exposed
`lc_basilisk_find_rom_trap`, using the corrected signed branch-table walk) and
seed it — traps 0x00-0x7f through the `$0400-$0600` override window, 0x80-0xff by
direct RAM write to `$0600-$07FF`. Unimplemented traps keep the no-op.

Result: the faithful boot no longer crashes in the empty boot-2 staging area.
It now runs **real ROM code cleanly for the full 50M cycles** (illegal=0,
unknown=0, zero-ram=0, getpic0=0), reaching a list/queue-traversal loop at ROM
`0x142fc` (`cmpa.l a4@(8),a2; cmpl a4@(18),d2` walking a linked structure).
Default baseline unchanged (50M green, VRAM=4). Disk activity is still just the
boot-block reads, so MountVol still hasn't read the volume header — it is now
stuck in the `0x142fc` traversal loop instead. Next: trace the `0x142fc` loop
(which queue/list it walks and why it doesn't terminate) to continue toward the
actual volume mount.

#### 0x142fc loop walks a list whose head is NULL (a3=0)

Traced the `0x142fc` loop (a circular list walk: `a4=a3` head, `a4=[a4]` until
`a4==a3`, matching `+8==a2` / `+18==d2`, flags at `+26`). It is invoked from the
MountVol chain (return address `0xf042`). The list head **`a3 = 0`**, so the
walk dereferences `[0]=0x10 -> [0x10]=0x7f2 -> garbage -> 0xffffffff` and never
returns to the head — an infinite walk over low memory / reset-vector bytes
(`a2=0x001001b8`, `d2=2` are the search keys). The head should be a valid
FS/HFS structure pointer; it is null because that structure (a queue/control
block the File Manager expects MountVol to have populated) isn't initialized,
which ultimately traces back to the volume header still not being read.

So MountVol now gets much deeper into the real File Manager (full real OS trap
table in place) but still hasn't issued the volume-header read; it dereferences
a not-yet-initialized list head and loops. Next: identify which list head this
is (which low-memory global / control-block field `a3` is loaded from in the
caller) and why MountVol reaches this search before reading/parsing the volume —
then close the gap so the volume-header read happens and the structure is real.
This remains the same multi-step ROM File Manager bring-up; the boot is now
executing real FM code rather than scaffolding, but reaching an actual mounted
volume requires the read+VCB+catalog chain to complete.

#### Deepest progress yet: a VCB is allocated; FCB array (FCBSPtr) is null

The `0x142fc` loop's list head `a3` is loaded from **FCBSPtr (`$034E`)** (via
`movea.l $034e,a1; ... a3=a1`) — the File Control Block array pointer. Dumping
the FS globals when the loop is reached:

```
FCBSPtr  ($034E) = 0x00000000   <- FCB array NOT allocated (null head -> loop)
DefVCBPtr($0352) = 0x001001b8   <- a real VCB WAS allocated by MountVol!
VCBQHead ($0358) = 0x00000010   <- new VCB not linked into the VCB queue
SysZone  ($02A6) = 0x00380000
TheZone  ($0118) = 0x00380000
```

This is the deepest real-File-Manager progress so far: **MountVol allocated a
VCB at `0x001001b8`** (via the real Memory Manager) and set `DefVCBPtr`. The
remaining gaps: (1) the **FCB array** was never allocated, so `FCBSPtr=0` and
the FCB search at `0x142fc` walks a null head and loops; (2) the new VCB is not
linked into the VCB queue (`$0358` should be `0x001001b8`, not `0x10`).

FCBSPtr is normally set during InitFS/early startup by allocating the FCB array
in the system heap (its first word is the array length). Our boot runs the real
InitFS but the FCB-array allocation didn't happen / didn't store `$034E`. Next:
ensure the FCB array is allocated and `FCBSPtr` set (and the VCB linked into
`$0358`) so the FCB search terminates and MountVol proceeds to read the volume
catalog. The Memory Manager works (the VCB alloc succeeded), so this is about
running the specific InitFS/startup step that builds the FCB array.

#### FCB array now allocated (FSFCBLen guard) — boot 1 reaches the boot-2 handoff

InitFS (`0xf368`) DOES allocate the FCB array (`NewPtr` at `0xf39c`, then
`move.l a0,$034e`), but only when **`$03F6` (FSFCBLen) is negative**:
`tst.w $3f6; bpl 0xf44e` skips the whole allocation when `$3F6 >= 0`. Our zeroed
RAM left `$3F6 = 0`, so InitFS skipped the FCB allocation and `FCBSPtr` stayed
null (causing the `0x142fc` loop). Seeding `$03F6 = 0xFFFF` (-1) under the gate
before boot makes the first InitFS build the FCB array and set `FCBSPtr`.

Result: boot 1 no longer loops in the FCB search — it completes its FS init
(FCB array allocated, VCB created) and jumps to the boot-2 staging area
(`0x9000d4`). Default baseline unchanged (50M green, VRAM=4).

New frontier: the boot-2 staging area is still empty (`stopped_on_zero_ram`),
because MountVol creates the VCB but **still doesn't read the volume header**
(block 2 / the HFS MDB) — disk activity is still only the sector-0 boot-block
reads. So boot 1 finishes FS init and tries to run boot 2, but the System file
was never loaded (the VCB has no real catalog/extent info from disk). Next:
find why MountVol creates the VCB without issuing the block-2 read (does it
build a default VCB and return, or bail before the read?), and make the volume
MDB read happen so the VCB is populated and boot 1 can open the System file and
load the real boot 2.

#### MountVol now does drive lookup + VCB alloc; FS worker 0x14224 returns ioErr

With the FCB array allocated, MountVol (`0xf60e`) gets much further:

- `0xfc6c` drive-queue lookup now returns 0 (drive found) — the FCBSPtr fix also
  unblocked the lookup chain.
- `0xf690` allocates a real 178-byte VCB at `0x00104070` (via the real MM).
- `0xf6f6` `jsr 0x14224` (the FS mount worker, called with `d0=-1, d1=0, d2=2`).

`0x14224` is itself a patchable-trap trampoline (`move.l $XXX,-(sp); rts` ->
`0x14230`).  With `d0=-1` it takes the `blt 0x142d6` path into the FCB/VCB queue
search (`a4=a3`, `a4=[a4]`), which now terminates (FCB array is real).  But the
worker returns **`ioErr (-36)`** to MountVol (`0xf6fa bne 0xf97e` bails), so the
volume mount fails before the block-2 MDB read ever reaches the disk driver
(still only the sector-0 boot-block reads).

So the remaining blocker is *inside* `0x14224`: it returns ioErr from the
FCB/VCB search or a sub-step, before the catalog/MDB read.  Next: trace within
`0x14224` (and its sub-calls `0x11f0e` / `0x14620`) to find exactly where `d0`
becomes -36 and what structure it is missing, then close that so MountVol issues
the block-2 read, populates the VCB, and boot 1 can open the System file.

#### Oracle diff at 0x14224: param-block pointer (A0) differs

Used the reference BasiliskII (which boots this same ROM+disk) as a register-
level oracle at `0x14224`. It does hit `0x14224` (3x) and MountVol `0xf662`, so
the same code path. Comparing entry state to ours:

```
            D0      D1   D2   A0         A1       A3
oracle:  0000ffff  0    2    00400a4c   00009018 00004c1e
ours:    0000ffff  0    2    00800000   ?        ?
```

`D0/D1/D2` MATCH. The divergence is **`A0` (the MountVol param block)**: the
oracle's is a real System-heap allocation at `0x00400a4c`, while ours is
`0x00800000` — a suspiciously round value that looks like leftover scaffolding /
a mis-computed pointer rather than a real allocation. The `0x14224` worker reads
the volume/FS request fields out of this param block, so wrong contents make it
return `ioErr`. (`0x14224`'s internal routine at `0x1440c` is the WDCB array
builder — `NewPtr` of `d2` working-directory control blocks — confirming this is
FS setup, not yet the disk read.)

Next: trace where the MountVol param-block pointer (`A0`) is established in our
boot and why it is `0x800000` instead of a real heap allocation like the
oracle's `0x400a4c`; fixing the param block (real allocation + correct fields)
should let the `0x14224` worker succeed and MountVol proceed to the block-2 MDB
read. The register-level oracle diff is the fast way to keep pinpointing these.

#### Control-flow diff: trap-0xc1 FS vector ($0704) resolves differently

Deeper oracle diff: `0x14224` is a self-patching trampoline
(`move.l $0704,-(sp); rts`), where `$0704` is OS trap slot `0xc1`. The ROM
*patches* `[$0704]` at runtime (InitFS-era), so the seeded value is overwritten:

- `find_rom_trap(0xa0c1)` (our seed) = `0x1422a`
- our runtime `[$0704]` -> `0x14230` (FCB-search entry)
- oracle runtime `[$0704]` -> `0x1440c` (WDCB-builder entry)

So at this point our boot dispatches trap 0xc1 to a *different FS operation*
than the oracle (FCB search vs WDCB build), which is why the worker returns
ioErr. The trap vector itself is fine; the divergence is upstream state that
drives the ROM's runtime patch of `[$0704]` (and the param block `A0`).

**Strategic assessment.** We have reached the point of diminishing returns with
the piecemeal retrofit: each fix (FS globals, FCB array, trap table, ToExtFS,
etc.) corrects one structure but the ROM's real InitFS/InitOS sets up *many*
interdependent structures (trap vectors, FCB/VCB/WDCB queues, the heap zone) as
a coherent whole, and our scaffolding + per-item seeding keeps producing subtly
different state than a real InitOS run. The structurally correct next step is to
let the ROM's **complete** OS init run for the faithful path (rather than
faking globals and retrofitting), so all FS state is internally consistent —
then the MountVol chain (param block, trap-0xc1 dispatch, volume read) matches
the oracle by construction. That is a larger change (unwinding the scaffolding
behind the gate) but avoids the unbounded vector-by-vector chase. The fixture
baseline stays green (50M, VRAM=4) throughout; all faithful work remains gated.

#### ROOT of the ioErr: read-only disk -> wPrErr on MountVol's mount write

The `0x14224` worker's ioErr was a mapped error: the FS-dispatch epilogue at
`0x146f4` takes the real operation result `d0`, and if it is not 0 or -65 it
returns ioErr. Tracing the real `d0` at `0x146e6` gave **`-44` (wPrErr,
write-protect)**. MountVol issues a **write** during mount (updating the volume
bitmap / MDB to mark the volume mounted), and our `DISK_PRIME` rejected all
writes on the read-only image with wPrErr — which the FS turned into ioErr and
bailed. BasiliskII's disk is writable, so its mount write succeeds; this was a
genuine read-only-disk parity gap, not a micro-divergence.

Fix (gated): added a sparse **RAM copy-on-write overlay** to `DISK_PRIME` —
under `LC_FAITHFUL_DISK_BOOT`, writes go to RAM (512-byte sectors, lazily
allocated, up to 8192 sectors) and reads check the overlay before the read-only
image. The volume now looks writable without modifying the disk file. Result:
the mount write succeeds, MountVol no longer fails with ioErr, and the boot
advances to a new state (spinning near `0x11c6`, `stopped_on_zero_ram=0`).
Default baseline unchanged (50M green, VRAM=4, no wPrErr).

New frontier: MountVol still does not read the **MDB (block 2)** — disk activity
is read/write of **sector 0** only, then a spin near `0x11c6`. So MountVol's
MDB read targets the wrong sector (position computed as 0 instead of block 2 /
byte 0x400), or it loops re-reading the boot blocks. Next: trace the MDB-read
param block / `DISK_PRIME` position so the read targets HFS block 2, populating
the VCB from the real MDB.

#### ioPosMode fix: MountVol now reads the MDB (HFS block 2)

`DISK_PRIME` computed the read position from `dCtlPosition` (`dce+16`) and only
used `ioPosOffset` (`pb+46`) when `ioPosMode & 0x100` — but the Mac positioning
mode is in **ioPosMode bits 0-1** (`fsFromStart=1`), not bit 8. So MountVol's MDB
read (`fsFromStart`, `ioPosOffset = 0x400` = HFS block 2) fell through to
`dCtlPosition (0)` and re-read the boot blocks at sector 0. Fixed (gated): when
`(ioPosMode & 3) == 1` use `ioPosOffset` as the absolute byte position.

Result: disk activity now includes **`scsi-read sector=2` and `scsi-write
sector=2`** — MountVol reads (and updates, via the RAM overlay) the real Master
Directory Block. Default baseline unchanged (50M green, VRAM=4).

New frontier: after reading the MDB, the boot still ends in the empty boot-2
staging area (`0x900010`, `stopped_on_zero_ram`); the **catalog B-tree** reads
(to locate/open the System file) and the System-file read haven't happened yet.
Next: confirm the MDB read built a valid VCB (HFS signature, catalog extent),
then trace the catalog read so boot 1 can open the System file and load the real
boot 2. The disk-position handling may need the other ioPosMode cases
(fsFromLEOF/fsFromMark) for the catalog/extent reads.

#### MountVol validates the MDB but the data never reaches its buffer (a5@=0)

MountVol validates the MDB at `0xf706`: `cmpi.w #0x4244,a5@` (HFS 'BD') else
`-57 extFSErr` -> dispose VCB -> fail -> the whole boot retries in a loop (trap
sequence repeats). Tracing `a5` at `0xf706` shows **`a5@ = 0x00000000`** — the
MDB data never reached MountVol's validation buffer (`a5 = 0x00101a88`).
`0xf702 jsr 0x1447c` is *not* the read — it is yet another patchable FS
trampoline (`move.l $070c,-(sp); rts` = trap 0xc3, body `0x14482`, a flag-setter
for `d1=2`). The worker's earlier `_Read` (`0x1469a`, sector 2) should have
filled `a5`, but its buffer (`ioBuffer` = `pb+32`) and MountVol's `a5` don't
match, and on the failing pass the worker never reached the read at all.

**This is now the FOURTH instance of the same systemic pattern** — trap 0xb2
(`$06c8`), trap 0xc1 (`$0704`), trap 0xc3 (`$070c`), plus the MDB buffer/param
block — all FS-internal vectors/buffers the ROM's InitFS sets up as a coherent
whole, which our scaffolding + per-item retrofit cannot reproduce consistently.
The piecemeal File Manager bring-up has reached its practical limit.

**Decision: the next meaningful step is the structural pivot** — let the ROM's
*complete* InitOS/InitFS run for the faithful path (unwind the scaffolding:
faked SysZone/trap-table/FS-globals/drive-queue) so every FS vector, buffer,
queue and the heap zone are internally consistent by construction, matching the
oracle. Then MountVol's MDB read lands in the buffer it validates, the catalog
reads follow, and the System file loads — instead of chasing each divergent FS
vector individually. The fixture baseline (50M green, VRAM=4) stays as the
fallback while this is built behind `LC_FAITHFUL_DISK_BOOT`.

#### Confirmed: the ROM's InitOS/InitZone/trap-builder never run (suppressed)

Instrumented the faithful boot for the ROM init routines. Only **InitEvents**
(`0x22e0`) runs (called by boot 1 at icnt ~4177). **InitZone (`0xcf08`),
InitApplZone (`0xcd52`), and the trap-table builder (`0x9a9a`) never execute.**
So the real heap zone and trap table are never built by the ROM — our
scaffolding fakes them, which is the root of the cumulative FS-state divergence
(the four patchable FS vectors, the MDB buffer, etc.).

Why they are suppressed (the interlocked scaffolding, with
`LC_ENABLE_RAM_OWNED_LOW_MEMORY`):
1. **Dispatch magic** `$0DB0 = 0x5A932BC7` is RAM-seeded (lc_memory.c:691/807).
   The ROM's dispatch setup reads it and, seeing the “table initialized” cookie,
   skips InitOS / the trap-table builder.
2. **Trap table** `$0400-$0600` is a *dynamic read* (override table or the
   `0x40800d88` no-op default), so even if the ROM built a table its writes
   there would be ignored.
3. **Wholesale A-line interception** at `0x408099b0` handles every A-trap in our
   switch, bypassing the ROM's own dispatcher (which would consult the table).
4. **Faked SysZone/ApplZone globals** stand in for InitMemMgr/InitZone.

**Concrete pivot plan (gated, staged, baseline kept green):**
- (a) Under the gate, stop seeding the dispatch magic and let `$0DB0` read 0 so
  the ROM runs InitOS and its trap-table builder (`0x9a9a`).
- (b) Make `$0400-$0600` a normal RAM region (drop the dynamic-read override) so
  the ROM-built trap table persists and is what the dispatcher reads.
- (c) Stop intercepting at `0x408099b0`; let the ROM's real A-trap dispatcher
  run against the ROM-built table, keeping only the EMUL_OP hooks for the
  specific driver/hardware traps BasiliskII patches (Sony/Disk/SCSI/IRQ/etc.).
- (d) Let InitMemMgr/InitZone build the real SysZone instead of the faked
  globals (or call them explicitly), so NewPtr/NewHandle operate on a real zone.
Each step is testable in isolation behind `LC_FAITHFUL_DISK_BOOT`; expect early
regressions (the boot will re-derive state the ROM way) that converge toward the
oracle. This replaces the unbounded per-vector retrofit with one coherent init.

#### Definitive: our boot bypasses the ENTIRE ROM OS-init phase

Verified by oracle diff (reference BasiliskII, same ROM+disk):
- Oracle runs **InitZone (`0xcf08`) 3x** and the **trap-table builder (`0x9a9a`)
  1x** during startup.
- Our faithful boot runs **neither** (only InitEvents `0x22e0`, called by boot 1).

Steps (a) and the `0x586->0x1142` shortcut were both tried and are **inert** —
gating the dispatch magic or that jump changed nothing (identical 55685-cycle
run, same `0x900010`), and `0x586` is never even executed. The reason: our boot
*enters the boot flow downstream of the OS-init phase entirely* — it never
traverses the ROM code that would call InitMemMgr/InitOS/InitZone/trap-builder.
So individual scaffolding levers have no effect; the boot path itself is the
scaffold.

Implication: the pivot is not a few flag flips but a **boot-flow restructure** —
the faithful path must enter the ROM's normal startup so it runs InitMemMgr ->
InitOS (trap build) -> InitZone -> ... -> StartBoot, the way the oracle does,
instead of our scaffolded “seed fake OS state, jump to boot 1” entry. The next
concrete investigation is to trace the oracle's caller chain into `0x9a9a` /
`0xcf08` (who calls the OS-init, from where) and make our faithful boot reach
that same caller, rather than the scaffolded shortcut. This is a substantial
restructure; the fixture baseline (50M green, VRAM=4) remains the fallback.

#### Localized: InitOS lives at 0x999e-0x9a90, adjacent to the intercepted dispatcher

Tracing the oracle's trap-builder showed the InitOS/trap-table-build code lives
at ROM `0x999e..0x9a90` (entered with `A0=0x9332`, `D0=0x80b520`), immediately
**adjacent to the A-line dispatcher at `0x99b0`** that our faithful path
intercepts wholesale in `cpu_instr_callback`. The oracle executes this whole
region during InitOS (building the dispatch table); our boot never reaches it.

This sharpens the restructure scope into two coupled requirements:
1. **Reach InitOS**: our boot must traverse the ROM startup that calls the
   `0x999e` InitOS routine (the scaffolded entry jumps past it). Find the caller
   of `0x999e` in the oracle and ensure our faithful boot follows the same path.
2. **Stop intercepting `0x99b0`**: the wholesale A-line interception sits on top
   of the dispatcher that InitOS builds/uses; with InitOS running it must be
   removed (let the ROM dispatcher run against the ROM-built table), keeping
   only the EMUL_OP driver/hardware hooks.
Both are gated behind `LC_FAITHFUL_DISK_BOOT`; the fixture baseline stays green.
This is the concrete entry point for the boot-flow restructure.

#### Restructure step 1 LANDED: InitOS now runs (un-NOP the $160 dispatch init)

Found the exact skip: our compat NOPs the whole `$0160-$01d3` block (“dispatch/
SCC/VIA init”), which also removes the call that runs the trap-dispatch build
(InitOS at `0x999e` / builder `0x9a9a`).  BasiliskII instead lets that block run
and surgically RTS's only the hardware routines (IWM `0x9c0`, SCSI `0x9a0`, SCC
`0xa30`, VIA NOPs).  Gating the `$0160` NOP off under `LC_FAITHFUL_DISK_BOOT`
(keeping the surgical hardware RTS's) makes the ROM run its real OS init:

- `0x999e` (InitOS entry) and `0x9a9a` (trap-table builder) now execute.
- Boot advances from ~55,685 cycles to **~177,970**, reaching ROM `0x49fca`
  (the NuBus/slot probe — `btst #17,d7`) and stopping on the ROM monitor
  (no NuBus hardware), `stopped_on_zero_ram=0`.
- Default baseline unchanged (50M green, VRAM=4).

So the ROM now builds its **real** trap table instead of our seed — the
foundation for coherent FS state. New frontier: the NuBus/slot init at `0x49fca`
panics into the ROM monitor with no slot hardware. Next: port BasiliskII's slot
probe skips for this path (it already has `0xb27c`/`0xba0b0`/`0xb9874`; the
`0x49fxx` slot scanner needs the same treatment), then continue down the real
init toward StartBoot — with InitOS/trap-table now real, the MountVol FS state
should finally be coherent.

#### Step-1 frontier characterized: post-InitOS hardware scan -> serial monitor

The `0x49fca` stop is the ROM **serial monitor** no-input command poll (waits
forever for debug input) — i.e. the boot *panicked* into the monitor. Root: after
InitOS, the real init runs a hardware/diagnostic scan that reads the actual
hardware bases (`a2=0x50f00000`, `a3=0x50f04000`, plus `0x50f04000`/`0x50f14000`
via our I/O stubs returning `0xff`).  At `0x49ece` it does `move.b (d16,a2),d0`
from `0x50f00000` and the result sets **`d7` bit 17**, which routes the boot into
the monitor at `0x49fca`.

Notably the bases are the *real* `0x50fxxxxx` here, not the PatchHWBases scratch
(`0x00f00000`) — the InitOS-driven path re-derives them from the decoderInfo, so
our earlier scratch redirect doesn't cover this scan.  Two ways forward:
- Make the hardware read at/around `0x49ece` (the `0x50f00000`/`0x50f14000`
  device) return a value that does NOT set `d7` bit 17, so the diagnostic scan
  passes instead of dropping to the monitor; or
- Port BasiliskII's hardware/slot-probe skips for this `0x49xxx`/`0x45exx` scan
  (it patches the NuBus/RBV probes out), so the scan is bypassed entirely.
This is genuine forward progress: the boot now runs the real OS init and reaches
the ROM's hardware-diagnostic phase (3.2x more cycles); the remaining work is
the hardware-probe contract for the `0x50f00000`/`0x50f14000` (VIA/RBV) devices
that the post-InitOS scan reads. Fixture baseline stays green (50M, VRAM=4).

#### The monitor is reached via a NuBus/slot scanner (0x4988e) the oracle avoids

Traced the monitor entry: the boot reaches `0x4988e` — a NuBus/slot scanner whose
entry is `lea -256(sp),sp` (`4fef ff00`), the *same signature* as BasiliskII's
`0xb9874` scanner.  It is reached unconditionally as the last step of a
device-record dispatch chain (`0x2450 -> 0x2488 -> 0x24fe -> 0x246e -> jmp
0x4988e`); the scanner probes absent slot hardware and ends at the monitor poll.
The **oracle never executes `0x4988e`/`0x49fca`/`0x4a02a`** — so our boot takes a
divergent device/slot path the reference avoids; the divergence is *upstream* of
the `0x2450` chain.

Tried BasiliskII's scanner-skip (patch `0x4988e` entry with `JMP(A6)`): it is
**wrong here** — `0x4988e` is the *end* of a dispatch chain (jumped to, not
called), so `A6` is not a valid return; the skip mis-returns into a mid-`bsr`
byte at `0x108` (illegal opcode `0x00ff`) and lands in empty staging.  Reverted.
Without the skip the boot stops *cleanly* at the monitor (no illegal), which is
the better state.

So the right fix is NOT to skip the scanner but to address why our boot enters
the `0x2450` device-record chain at all (the oracle doesn't) — or to give the
scanner a graceful “no slots present” hardware contract so it completes and falls
through to StartBoot instead of the monitor. Next: trace the caller of the
`0x2450` chain and diff against the oracle to find the upstream device/slot
decision that diverges.

#### Divergence localized to the Slot Manager scan (0x235e / 0xa06e)

Traced the jmp-to-scanner: `0x246e` is reached from `0x238a`, which is the exit
of a **Slot Manager scan loop** at `0x235e-0x238a`:
```
2360: ori.b  #6,(0x6e,a2,a2.w)     ; slot flags
2366: bne    0x2384                ; -> 0x238a -> 0x246e -> scanner 0x4988e
2368: move.b #1,a0@(50)
236e: moveq  #5,d0
2370: A06E                          ; _SlotManager
2372: movea.l a0@,a1
2374: cmpi.w #1,a1@(32)             ; slot record check
237a: beq    0x238e                ; slot OK -> continue
237c: addq.w #1,d3 ; ... ; bra 0x235e   ; next slot
```
So the boot enumerates slots via `_SlotManager` (`0xa06e`) and, depending on the
slot records, either continues (`a1@(32)==1`) or falls through to the scanner ->
monitor.  Our `0xa06e` handler / slot-ROM records drive it down the scanner
path; the oracle's slot enumeration keeps it on the boot rail (it never reaches
`0x4988e`).

We DO install a minimal slot ROM (“Basilisk minimal Slot ROM ... frame=0xa0000000
512x384”) and seed a real `_SlotManager` handler (`0xa06e -> 0x6e16`) at RESET.
The contract between that handler / slot-ROM record format and what this scan
loop expects (`a1@(32)==1`, the `0x6e` slot flags) is the next thing to get
right — so the LC's built-in video slot enumerates cleanly and the scan falls
through to StartBoot instead of the NuBus scanner.  Next: trace the `0xa06e`
results and `a1`/`a0@(50)` slot records here vs the oracle, and align the slot
record the LC built-in-video slot should present.

#### Oracle never runs the slot scan (confirmed); two options

Confirmed at harness 2000: the oracle **never executes `0x235e`/`0x2360`** (the
slot scan loop) — so our boot diverges into it from upstream; the reference takes
a slot path that bypasses this loop entirely. Two ways to converge:

1. **Targeted (likely simplest):** the loop continues (avoids the scanner) when
   the slot record satisfies `a1@(32) == 1` at `0x2374` (`a1 = [a0]`, `a0` from
   `_SlotManager`). Make our built-in-video slot record present that field so
   the scan loop falls through to `0x238e` (continue) instead of `0x2384` ->
   scanner -> monitor. Requires getting our slot-ROM record / `_SlotManager`
   (`0xa06e`) output to match the `0x6e`/`+32` fields this loop reads.
2. **Upstream:** find why our boot enters this slot-scan routine at all when the
   oracle's slot init avoids it (likely a different sResource/board-sResource
   record or PRAM slot-config the LC presents), and match that.

The InitOS restructure (real ROM trap table) stands as the session's key
advance; the boot now runs real Mac OS init and the remaining frontier is this
Slot Manager record contract before StartBoot. Fixture baseline green (50M,
VRAM=4); all gated behind `LC_FAITHFUL_DISK_BOOT`.

#### ROOT CAUSE: slot scanner is downstream of a CPU exception (rts-to-0x40)

Back-walked the EXACT divergence chain instead of treating the slot scanner as
the frontier.  The scanner / serial monitor is reached only because our boot
takes an **unexpected CPU exception** the oracle never hits.  Chain (each link
confirmed by register-level trace + oracle diff):

```
serial monitor 0x49fca
  <- scanner 0x4988e
  <- 0x246e (SysError dispatch)
  <- 0x2740: cmpi.w #13,d0 ; bne   (our d0=3 = vector index, != expected 13)
  <- exception dispatch table 0x26f0 (row of `bsr 0x26a0`); 0x26a0 computes
     d0 = (return-0x26f0)/2 = vector index 3, stacks faulting PC = 0x108
  <- ILLEGAL INSTRUCTION executing low memory: pc walked 0x40 -> 0x108
  <- 0xd8a rts returned to 0x40 (garbage); oracle's same rts returns to 0x104a
  <- trampoline 0xd360: `move.l #0x40,-(sp); move.l (0x1ee8),-(sp); rts`
     -> jumps through (0x1ee8), leaving 0x40 as the return address
  <- (0x1ee8) = 0x40800d88 = the null stub `moveq #0,d0; rts` (WRONG)
  <- trap dispatched via InitOS 0x9a22 `jsr [table+d2*4]` -> handler 0xd360
```

**The real bug:** the entire OS dispatch region `0x1e00..0x1f00` (incl. the
sub-vectors `0x1ee0/0x1ee4/0x1ee8`) is **default-filled with the null stub
0xd88**.  ROM routine **`0xcc60`** (entry pointer at ROM offset 0x54 =
`0x0000cc60`) is what should build the real, ROMBase-relocated dispatch table at
`0x1e00`/`0x1f00` (40 entries each, `add.l (0x2ae),...`) AND install the real
OS-utility handlers `0x1ee0=0xdce4, 0x1ee4=0xdc2e, 0x1ee8=0xdd1e`
(`0xccbc/c4/cc`).  `0xcc60` reads XPRAM (`_ReadXPRam` 0xa051), writes reset
vectors `0x0`/`0x4`, then installs those handlers.  **Our faithful boot never
runs `0xcc60`**, so when a trap dispatches to the `0xd360` trampoline it jumps
through the uninitialized `0x1ee8` stub, returns into the exception-vector table,
and faults.

The trap handler entry `0xd360` itself IS correctly installed (by the InitOS
trap-builder enabled in `3edc921`); only the `0xcc60`-owned sub-vectors are
missing.  Same class as the original InitOS gap: an early-init step is still
skipped under faithful mode.  Next: find `0xcc60`'s caller (ROM-header entry
0x54) and why our boot doesn't run it (likely a surgical IWM/SCSI/SCC/VIA RTS or
an early-init NOP cuts the path), then let it run so the dispatch table +
`0x1ee0/4/8` handlers are built before any trap dispatches through them.

Debug aid added (gated behind `LC_FAITHFUL_DISK_BOOT`, inert in fixture mode):
`LC_TRACE_PC_START`/`LC_TRACE_PC_END`/`LC_TRACE_PC_LIMIT` env-driven PC-window
trace for register-level boot forensics.  Fixture baseline re-verified green
(HOST_LC_OK, stopped_on_*=0).

#### SlotManager video-default check now passes (selector 21 / fake built-in video sResource)

After the MM-dispatch fix (`e6db888`), the boot still entered the ROM routine
`0x2310..0x238a` that the oracle avoids.  The first `_SlotManager` call in that
routine is selector **21** at `0x2352`; the real ROM Slot Manager returned
`0xfed4` (`smEmptySlot`), so `0x2354 bne -> 0x2386 -> 0x238a -> 0x246e` dropped
straight into diagnostics.

There was already a narrow helper for this video-default path, but it was called
too late for this A-line path.  The fix now invokes it directly at the actual
trap instruction PCs `0x2352`, `0x2364`, and `0x2370` under
`LC_FAITHFUL_DISK_BOOT`:

- selector 21 returns `noErr` instead of `smEmptySlot`;
- the follow-up selector 6 succeeds;
- selector 5 returns a tiny fake built-in-video sResource whose `+0x20` field is
  `1`, satisfying `0x2374: cmpi.w #1,a1@(32)`;
- execution falls through to `0x238e` and continues the setup/unpack routines
  instead of immediately jumping to diagnostics.

A/B: faithful frontier advances **180,544 -> 216,983 cycles**.  The remaining
halt is still monitor `0x49fca` with d7 bit17, reached later via the same ROM
video/diagnostic path.  A trace of the upstream exception/diagnostic selector
shows low-memory `0x120=0` and `0x2ba=0`, causing the default handler `0x280e`
path; the oracle avoids this whole `0x2310..0x2520` family, so the ideal fix is
still upstream, but this models the LC built-in-video SlotManager contract enough
to expose the next dependency.

Validation: 50M fixture baseline remains `HOST_LC_OK` with
`stopped_on_zero_ram=0 stopped_on_monitor=0 stopped_on_zero_rom=0`.  50M/500M
faithful runs both stop at the new 216,983-cycle frontier.  A 500M fixture run
returns `HOST_LC_OK` but also reaches the long-run monitor guard (pre-existing
fixture behavior, not caused by this faithful-gated change).

#### Boot-block loop escapes monitor/zero-RAM; current frontier is real MountVol `-113`

Next upstream trace showed the ROM diagnostic path came from low-memory boot-code
at `0x902..0x98e`, not from the oracle rail (the oracle never executes
`0x900..0x940`).  The specific fault chain was:

- boot code called trap `A06D` at `0x926` (this path expects our MaxMem-style
  stub semantics, not the real ROM handler);
- before the call, the next words were valid boot code
  (`303a ff50 a06c 2047 ...`);
- after the real ROM handler returned, `0x928` had been overwritten with garbage
  (`cfc5 00af 5f93 ... fdef`), causing a bogus F-line trap into
  `0x2702 -> 0x280e -> 0x2310..` diagnostics.

Because this path is already non-oracle/synthetic, stop letting the real `A06D`
handler run here under faithful mode; the generic `_MaxMem` trap intercept now
returns to `0x928` without clobbering the boot-code area.  Also gate the old
fixture-only boot-block re-entry shortcut (`0x88a -> boot_2 at 0x00900000`) off
under `LC_FAITHFUL_DISK_BOOT`; otherwise, once the diagnostic crash is fixed, the
faithful boot is redirected into the dead fixture staging area.

The MM fix was also made more faithful: instead of only installing sub-vectors
`0x1ee0/4/8`, the lazy `0xcc60` model now copies/relocates the full 40-entry
`0x1e00` and `0x1f00` dispatch tables from ROM table `0xcb20`, then installs
`0x1ee0=0xdce4`, `0x1ee4=0xdc2e`, `0x1ee8=0xdd1e`.

Result: faithful boot now reaches the cycle budget rather than monitor/zero-RAM:

- 50M faithful: `HOST_LC_OK`, `cycles=50275866`, `pc_after=0x40809ac0`, all
  stop flags 0.
- 500M faithful: `HOST_LC_OK`, `cycles=502755944`, `pc_after=0x40809abc`, all
  stop flags 0.
- 50M fixture baseline unchanged: `HOST_LC_OK`, all stop flags 0.
- 500M fixture baseline unchanged: `HOST_LC_OK`, reaches the pre-existing
  long-run monitor guard.

The next real frontier was the boot-block retry loop around `_MountVol`: real
`_MountVol` at `0x936` returned `d0=0xffffff8f` (`-113`), so the boot block
branched to `0x98e` and retried, shrinking the stack by 2 each time.  A trace of
`MountVol` showed the error came from its internal `_NewPtr(178)` at `0xf690`
(the VCB allocation): the real MM handler is present, but the fully initialized
zone state still is not, so it returned `-113` before any MDB/VCB work could
complete.

A narrow faithful model now handles only that `0xf690` `_NewPtr` with a small
zeroed bump allocation.  This lets the real MountVol path progress past VCB
allocation and into the File Manager worker.  A temporary experiment forcing the
whole `_MountVol` call to `noErr` showed the following boot-block failure would
be `fnfErr (43)` after `_InitResources`/resource lookup, so do not stub MountVol
success; keep fixing the real MountVol/VCB/System-file state.

With the narrow `0xf690` allocation model:

- 50M faithful: `HOST_LC_OK`, `cycles=50231129`, `pc_after=0x4081431a`, all stop
  flags 0.
- 500M faithful: `HOST_LC_OK`, `cycles=502308543`, `pc_after=0x408142f2`, all
  stop flags 0.

The new current frontier is the File Manager linked-list scan at
`0x142e0..0x14402`; 50M/500M end around `0x1431a`/`0x142f2` with no monitor or
zero-RAM.  Next: identify which FCB/VCB/list header this scan is walking and
populate/repair the real structure so MountVol proceeds to the MDB/System-file
path instead of spinning in the list scan.

Follow-up trace of the list scan showed the immediate structural bug:
`0x14224` dispatches to a worker that does `movea.l a1,a3` at `0x14232`, and
`a3` is **zero** at entry. The scan then treats low memory address 0 as a
circular list header and walks exception-vector/table contents as list nodes
(`0x10 -> 0x26f4 -> 0x40800d88 -> 0x70004e75 -> ...`). The missing list head is
not created because real `_InitFS` is still incomplete: `_InitFS` should set
`0x37c` at `0xf42c`, but it fails much earlier at its first `_NewPtr` call
(`0xf39c`, size `0x0eb2`) with `d0=-113`, so it exits before initializing the
FCB/list globals (`0x34e`, `0x372`, `0x37c`, `0x380`). A temporary broad model
for all InitFS `NewPtr` sites reached `0xf42c` but later fell back to the monitor
path by 50M, so do **not** commit that broad allocation shortcut. A second
experiment manually seeded low-memory circular/free-node list structures for
`0x37c`/`0x380` plus minimal `0x34e`/`0x372`; this made the worker take a free
node and reach the I/O helper (`0x14620`, returning `d0=0`), but the frame/return
path still fell into the monitor before returning to the expected worker
continuation. So do **not** commit the manual list-seed shortcut either. The next
clean fix should either faithfully initialize enough Memory Manager zone state
for real InitFS allocations, or narrowly model the InitFS-created structures with
all verified frame/list invariants (not just head links), then re-test
MountVol's `0x142e0` scan.

Additional MM-zone trace: at `0xf39c`, lowmem points both `TheZone`/`SysZone` at
`0x00380000`, but the zone header there is zero in the current validated code,
so the real ROM MM handler returns `-113`. Seeding a basic zone header plus the
ROM validation fields at `zone+0x38`/`zone+0x3c` gets past the first validator
(`0xdbf8`) but still falls into a later MM error/monitor path during allocation.
So a partial zone header is not enough; the real fix needs the full block/free
list invariants the ROM allocator expects, or a deliberately complete modeled
InitFS structure set.

Oracle comparison for `_InitFS` confirms the intended allocation is low-heap:
when the reference hits `0xf39c` (`_NewPtr(0x0eb2)`), it returns `d0=0` and
`a0=0x00007734`. That is not a high `0x0038xxxx` ApplZone pointer. Attempts to
move our `TheZone`/`SysZone` to a low zone were informative but still not valid:
`0x2800` overlaps existing master-pointer/scaffold state and is clobbered before
`0xf39c`; `0x3800` survives and can pass the first `0xdbf8` validator if
`zone+0x38/0x3c` are seeded, but then the allocator still falls into a later MM
error/monitor path. This strongly suggests the missing contract is the **complete
classic Memory Manager zone/free-block format**, not just the low-memory zone
pointers.

One more rejected shortcut: replaying the oracle's observed low allocation
addresses for the InitFS/File Manager `NewPtr` sites (`0xf39c -> 0x7734`,
`0xf3b2 -> 0x85f4`, `0xf3da -> 0x8884`, `0xf3e8 -> 0x8da0`, `0x14426 ->
0x8de4/0x9018`) gets the early InitFS sequence moving, but it still regresses
back into the video-default/diagnostic family by longer runs. Those addresses
also overlap current scaffolded low-memory areas such as the boot DCE/DQE, so
hard-coding them is not safe. Use the oracle addresses as evidence for low
SysZone behavior, not as a committed allocation table.

A complete-structure seed experiment (manual FCB array plus `0x372/0x378/0x37c/
0x380/0x386/0x3ea` structures in a safe low-memory range) also failed: it made
the File Manager worker consume the synthetic free node and reach the I/O helper,
but the control-frame/return convention still corrupted the stack, ending in the
same `0xd8a RTS -> lowmem vector table (0x44/0xEA)` bad-return family. So the
structure model must include the exact ROM calling-frame/list side effects, not
just plausible list contents.

A dynamic safe-low bump allocator for the same InitFS/File Manager `NewPtr`
sites (starting at `0x0000b000` to avoid the oracle-address/scaffold overlap)
was also rejected: like fixed-address replay, it moves the early InitFS sequence
but regresses back toward the `0x280e` video-default/diagnostic family by 50M.
That rules out simple “allocate the right sizes somewhere low” as sufficient.

Temporary oracle instrumentation captured the real SysZone at the same `0xf39c`
allocation:

```
TheZone/SysZone/ApplZone = 0x00002000
zone+00 = 0x00011be0   zone+08 = 0x00002074   zone+0c = 0x00007fec
zone+20 = 0x00001f00   zone+30 = 0x00007564   zone+34 = 0x40000000
zone+38 = 0x0000010c   zone+3c = 0x00002000
[0x2074] = 0x00002070  [0x2078] = 0x00007d8c
[0x7564] = 0x40000000  [0x7568] = 0x000001c4
```

After allocation, oracle has `a0=0x00007734`, `zone+0c=0x00007130`,
`zone+30=0x00007728`, and `[0x7728]=0x40000002/[0x772c]=0x00000ec0`.
The parsed before/after mutation also shows the free-list growth/split:
`[0x2078] 0x00007d8c -> 0x0000a254` and the new allocated 12-byte block header
at `0x7728`, with user pointer `0x7734`. This confirms the first InitFS
allocation is not merely a bump pointer; the ROM allocator updates multiple
free-list/rover fields around the low SysZone.
Replaying this visible SysZone snapshot late at `0xf39c` still falls into the
monitor before `NewPtr` returns, so the snapshot is incomplete: the ROM allocator
also depends on hidden/surrounding block/link state, not just the visible zone
header and two block headers. A wider replay experiment copied the oracle's
`0x2000..0x12000` band before our `0xf39c`; it still returned `-113`. Replaying
all `0x0000..0x12000` immediately clobbered the active A-line/vector/trap state
and jumped into zero RAM, so it is not a valid test. The missing contract likely
spans the zone band plus low globals/trap/MM-table/frame state below `0x2000`,
or must be reconstructed as true allocator invariants rather than copied bytes.

Static review of the current scaffold also found a direct obstacle to using the
oracle's `SysZone=0x2000`: the boot scaffold pre-populated the toolbox trap table
over `0x0e00..0x2e00`, which collides with a low SysZone at `0x2000` and with the
MM dispatch tables at `0x1e00/0x1f00`. The Musashi-side setup now caps faithful
toolbox prefill at `0x1e00`, while non-faithful fixture behavior still fills to
`0x2e00`. A matching cap in the lower-level RAM-owned low-memory seeder was
tested and **reverted** (`c9c330b`): it prevented the old `0x1ee8=d88` stub state
that our lazy MM installer used as a trigger, and faithful boot regressed to the
monitor/zero-RAM family (`0x49fca` / `0x100`) even after trying a zero-or-stub
installer condition. Therefore the RAM-owned seed layer remains part of the
current MM bootstrap dependency. A faithful low-SysZone implementation still has
to make both setup layers coherent, not just cap the later Musashi-side fill, and
move several other scaffolds out of the oracle allocation band. Known oracle allocation addresses vs current static
scaffold ranges:

```
0x7734: free by static ranges
0x85f4: CLEARED by relocating faithful RESOURCE_ROM_MASTER_PTR slab to 0x1c000..0x1c300
0x8884: dynamically free in faithful 50M (disk PB uses ~0x8800..0x8830; DQE starts 0x8a40; no writes seen in 0x8860..0x88a0)
0x8da0: free by static ranges
0x8de4: free by static ranges
0x9018: CLEARED by relocating faithful post-reset dispatch/probe tables to 0x1d000..0x1d120
0x9468: CLEARED by relocating faithful SRT records to 0x1e000..0x1e0dc
```

So the Musashi-side trap-table cap is necessary but not sufficient: the faithful
low heap also requires keeping the remaining low range free or choosing a
deliberately modeled layout that does not pretend to match the oracle's exact
low addresses. Three collisions are now cleared under `LC_FAITHFUL_DISK_BOOT`: the ROM resource master-pointer scratch
slab moves from `0x8400..0x8700` to `0x1c000..0x1c300`, the post-reset probe
dispatch/ProductInfo tables move from `0x9000..0x9120` to `0x1d000..0x1d120`,
and the synthetic SRT records move from `0x9400..0x94c0` to
`0x1e000..0x1e0dc`. Fixture mode keeps the old addresses. Validation after these
layout fixes:

- 50M fixture: `HOST_LC_OK`, all stop flags 0.
- 50M faithful: `HOST_LC_OK`, `cycles=50231129`, `pc_after=0x4081431a`, all stop
  flags 0.
- 500M faithful: `HOST_LC_OK`, `cycles=502308543`, `pc_after=0x408142f2`, all
  stop flags 0.

This does not solve `_InitFS` allocation by itself, but it removes real scaffold
collisions. The reverted RAM-owned prefill-cap experiment proves a coherent
low-SysZone/MM-table layout must be applied consistently across both low-memory
seed layers and the lazy MM-table installer.





























### RAM-sizing probe traced to ROM 0x800a70 + 0x800aa0 (V8 bank aliasing)

Widened the oracle register trace across the whole 0x800000-0x820000 range. The
RAM top the boot uses comes from a probe, not the `0x15274` loop:

- Oracle: `d4=0x00800000` (its 8MB RAM) is established by step ~3694 at ROM
  `0x800a82`; then `a3/a0 = 0x00800000` at `0x800aa0` (`movem.l fp@,d3-d4/a3-a4;
  exg d4,a3`). It reaches `0x15308` with `a0=0x00800000` via the `0x1300 ->
  0x1306` path (called from the main boot `bsr 0x1300` at `0x10a`).
- The routine at ROM `0xa70` (`bsr 0xa70` from boot `0x106`) *caps* the size:
  `d3 = fp@(4) >> 1`; if `fp@ == 0` load `d4 = 0x00800000` (8MB max) and
  `d3 = min(d3, d4)`. So 0x800000 is an 8MB ceiling; the real per-bank probe
  result arrives via the `fp` parameter block from upstream.
- Ours instead reaches `0x15308` via `0xb288 -> 0x15300` with `a0=0xffffffff`
  (failed probe), does a single sizing pass, and exits to the dead 0x41xxx path.

The upstream probe writes/reads RAM-bank boundaries and relies on **V8 memory-
controller bank aliasing** (a write past populated RAM wraps/aliases) to detect
size. Our flat 16MB RAM with no aliasing makes the probe return -1. Next step:
identify the exact probe loop (just before `0x800a70`/`fp` setup) and either
model V8 bank aliasing in `lc_memory.c` so a 16MB (or LC-typical 2/4/10MB) probe
resolves coherently, or inject the size the BasiliskII way.

**Also revisit:** our `0x490` CompBootStack patch (CompBootStack + FIX_MEMSIZE,
the BasiliskII RAM-size override) is currently clobbered by a later
`moveq #0,d0; rts` NOP-out (an old anti-recursion workaround in
`lc_basilisk_compat.c`). With the find_rom_trap fix and early patches now in
place, re-evaluate whether that workaround is still needed; restoring real
CompBootStack/FIX_MEMSIZE may be required once the probe reaches it.


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

## 2026-06-09 copied boot_3 INIT/CFM resource progress

After the video/GDevice tranche, the next crash was in copied boot_3 startup
resource/INIT handling.  The dirty `external/umac` state was audited and found
to be the expected `make prepare` patch/symlink state (`umac` hot-path/eject
patches plus `external/Musashi/m68kconf.h -> ../../../../include/m68kconf.h`),
not a new source change to vendor.

The host trap model now records host-side resource handle sizes for PICT and
resource handles, implements `_SizeRsrc`, corrects `_CurResFile`/`_UseResFile`
stack signatures, skips optional `INIT` resources until native startup-extension
execution is safe, and adds stack-correct no-op/failure handling for copied
boot_3 `_AUXDispatch`, `_ScriptUtil`, `_ExpansionBusDispatch`, and the early
Code Fragment Manager dispatch sequence.  CFM selector 3/5 calls deliberately
return a non-zero result so copied boot_3 skips native thunk execution instead
of jumping through uninitialized callback locals.

A follow-up corrected the resource/open-file split: `_HOpenResFile` is now
handled on its real `A81A` trap, while `A9C9` is treated as register-based
`_SysError` except at the three known copied-boot HOpen-shaped shim sites
(`0x007f8416`, `0x00bf0f74`, `0x00be138e`).  This avoids broadly popping a fake
12-byte HOpen frame from ordinary `SysError` checks, which had been corrupting
later copied-boot stack frames.

The copied boot_3 progress-picture loop was then traced to two stack/resource
contract errors rather than missing PICT data.  `A997` is `_OpenResFile`, not
`_CountResources`; returning refNum/result `0` for the optional `PTCH` file probe
made boot_3 request `GetPicture(0)` repeatedly.  The model now handles `A997` as
missing optional `PTCH`/`ptch`, moves `_CountResources` to `A99C`, and keeps
patch-resource counts at zero until patch execution is safe.  Repeated real PICT
lookups now return stable cached handles.  The remaining repeated QuickDraw loop
was a stack-shape issue in the script/text helper: `_ScriptUtil` uses a 6-byte
argument block and 4-byte result in this path, and `AA54`/`_TextServicesDispatch`
selector 14 consumes the long pointer while the caller cleans the adjacent word
and scratch record.  With those signatures, the 500M-cycle run exits the copied
boot_3 progress-picture loop and reaches ROM code around `0x408426dc` with no
unknown Toolbox traps and no `GetPicture(0)` requests.

Verification logs use the same Basilisk-compatible 16MiB/HD200MB settings.
`logs/host-lc-init-cfm-skip-20260609-075542.log` removed the previous low-PC
invalid-stack loop at `pc_after=0x00000003` and advanced through the first copied
boot_3 INIT/CFM helper into a later copied boot_3/video pass.  The current
follow-up log, `logs/host-lc-a9c9-sites-20260609-080321.log`, reaches the later
pass with site-specific A9C9 handling and stops after the copied-boot resource
manager path at `pc_after=0x00be9a6e`, `sp_after=0x00be8f56`; this is still
zero-filled RAM.  The current progress-loop verification log is
`logs/host-lc-scriptutil-textservices-500m-20260609-144421.log`: it reports
`HOST_LC_OK`, `stopped_on_zero_ram=0`, `stopped_on_monitor=0`,
`stopped_on_zero_rom=0`, `GetPicture(0)=0`, and only the real progress PICT IDs
`-16503`, `-20235`, and `-20241`.  It still has no guest VRAM writes or ROM
serial output, so the framebuffer remains in host status-overlay mode rather
than a Mac desktop.

A follow-on TextServices slice models copied boot_3's optional selector-14 path
more narrowly.  Gestalt now returns explicit LC/68K answers for the selectors the
copied boot probes (`sysa`, `mach`, `cput`, `vers`, `vm  `, `pgsz`, `dply`,
`scsi`, `hdwr`) and reports undefined optional managers instead of leaving stale
`A0` values.  `_NMInstall`/`_NMRemove` are no-op successes.  The first selector-14
attempt wrongly treated the ROM-side list walk as an absent-manager list and
escaped it; that advanced to a bad trap-dispatch/`pc=0xffffffff` loop.  The
correct narrower stack finding is that copied boot_3 already caller-cleans the
`AA54` selector-14 word+pointer frame with `ADDQ #6,SP`, so the trap shim must
consume no Pascal parameters.  Verification log
`logs/host-lc-aa54-clean-final-50m-20260609-194653.log` keeps `GetPicture(0)=0`,
zero unknown Toolbox traps, and no zero-RAM/monitor/zero-ROM stop, and exposes
the next real frontier: ROM copy helper `$40809256..$40809260` waiting on an
uninitialized/absent descriptor (`A4=0`, source from low-memory vector `$48`,
`A2=$0000000c`, `D2=$00fe6800`).  Attempts to force that wait complete or skip
its RTS target overwrite low memory or return to low PC, so the next fix must
model the descriptor source rather than patch the wait loop.

A later slice narrowed that source further: `_ScriptUtil` is not one fixed
signature in copied boot_3.  Most sites use a word result plus one long selector;
only copied offsets `$11c0/$11d4` use the long-result script/font forms.  Making
that distinction, handling `_HideCursor`/`_ShowCursor` as no-op procedures, and
skipping optional `wart`, `lmgr`, `ndlc`, and `ndrv` code resources avoids the bad
`$40809256` copy-helper path, preserves `GetPicture(0)=0`, and stops the long-run
stack collapse.  The current verification logs are
`logs/host-lc-skiplmgr-50m-20260609-205407.log` and
`logs/host-lc-skipndlc-500m-20260609-205848.log`: both report `HOST_LC_OK`, no
zero-RAM/monitor/zero-ROM stop, zero unknown Toolbox traps, and no `GetPicture(0)`.
The 500M run remains a blocker for desktop because it loops near
`0x00be9f06`/`0x00be9f08` in copied boot_3 heap-bound checking with periodic early
illegal-instruction callbacks, but the previous low-stack failure is gone
(`sp_after=0x00be8f8e`).

The next stack/scratch relocation slice traced the apparent dynamic code at
`0x00be8f50..0x00be8f60` to copied boot_3 local stack frames being built inside
the same heap range as the second copied boot_3 image.  Rather than repairing the
bytes or forcing the later zeroing loop, the host now models Basilisk's boot_3
stack relocation contract for that second copy: when execution is inside
`0x00be8934..0x00bf03f0` and SP still points inside the same copied-code range,
SP is lifted just above the copied image before local frames overwrite helper
code.  Verification logs `logs/host-lc-stacklift-50m-20260610-154108.log` and
`logs/host-lc-stacklift-500m-20260610-154235.log` both report `HOST_LC_OK`, zero
unknown Toolbox traps, zero illegal-instruction callbacks, `GetPicture(0)=0`,
and the first guest VRAM writes (`writes=4`, first PC `0x40840a34`).  Desktop is
still not reached; the new stable frontier is the ROM TextServices/component list
walk near `0x4084271c`, with the framebuffer still in status-overlay mode.

## 2026-06-09 copied boot_3 video/GDevice contract progress

The host LC harness now seeds a coherent Basilisk-style video contract instead
of relying only on fake trap success for the copied boot_3 video path.  The
scaffold creates a synthetic `GDevice` handle/pointer, matching `PixMap`, CLUT,
video DCE/unit-table entry, and low-memory globals (`ScrnBase`, `MainDevice`,
`DeviceList`, `TheGDevice`, cursor/source devices).  `_Control`/`_Status` calls
for the synthetic video refnum now decode the `csCode` and return coherent mode,
base-address, CLUT, geometry, and connection data for the 512×384×8-bit LC
framebuffer model.

The same tranche adds narrow QuickDraw/window/display contract support needed by
copied boot_3: seeded WMgr/Desk ports, rectangular vis/clip regions,
`_InitGraf`/`_InitCPort`/`_SetPort`/`_GetPort` port state, a stack-correct
`_DisplayDispatch` result frame for selectors 1830/2031, and a site-specific
`_SysError` no-op for the copied boot_3 heap-bound warning path at
`0x00bf9afe`.  The per-copy boot_3 A5 port at `A5+112` is repaired to point at
the same PixMap as `MainDevice`, preventing the copied helper from following
stale `0x6a042f..` port data while reading `portPixMap`.

Verification log: `logs/host-lc-video-gdevice-qdstacks8-20260609-071501.log`
with `HOST_LC_ENTRY_BASE=0x40800000u`, `HOST_LC_ENTRY_OFFSET=0x0008cu`, 16MiB
guest RAM, HD200MB, and `--basilisk-compat`.  This moves past the previous
`A205`/`A204` GDevice/gamma loop and the earlier DisplayDispatch stack fallthrough:
`_DisplayDispatch` at `0x00bff82e` returns with `param_bytes=14`,
`result_bytes=2`, and the second copied boot_3 instance seeds `A5+112` at
`0x00bf200e`.  The run advances into later INIT/resource-manager work before
corrupting into a low-PC/invalid-stack loop (`pc_after=0x00000003`,
`sp_after=0x39265f27`), so the framebuffer is still the host status overlay and
not yet a Mac desktop.

## 2026-06-08 host boot_3 patch-loader progress

The copied boot_3/PTCH illegal-instruction loop at `opcode=0x017a` was traced to
boot_3's optional patch-loader interpreter entering its jump-table data after
loading patch-support resources (`gusd`/`lodr`/`gtbl`/`gpch`).  Following the
Basilisk-style approach of emulating only the boot contract needed now, the host
harness now keeps `System.rsrc` host-side and copies requested resource data into
a bounded guest scratch arena instead of mapping the entire 5MB fork into guest
RAM near the ROM-created heap.  `_CountResources('PTCH'/'ptch')` reports zero
until patch execution is safe, and the copied boot_3 patch-loader entry is
bypassed as an optional helper.

Verification log: `logs/host-lc-final-patchloader-skip-20260608-093344.log` with
`HOST_LC_ENTRY_BASE=0x40800000u`, `HOST_LC_ENTRY_OFFSET=0x0008cu`, 16MiB guest
RAM, HD200MB, and `--basilisk-compat`.  The previous `0x017a` illegal loop is
gone in the 10M-cycle run.  The next blocker is a later GDevice/display-status
loop around `_Status`/`_Control` (`A205`/`A204`) at the copied boot_3 video/gamma
helper (`pc_after≈0x00bfabda`), and the framebuffer is still a host status
overlay rather than a Mac desktop.

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
RAM, HD200MB, and `--basilisk-compat`. That run reached later copied boot_3/PTCH
control flow and stopped on the then-new low-PC illegal-instruction frontier
(`opcode=0x017a` around `0x00bf9fec`/`0x00bea3d6`), which the 2026-06-08 tranche
then moved forward. The framebuffer was still in status-overlay mode rather than
a Mac desktop.

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
