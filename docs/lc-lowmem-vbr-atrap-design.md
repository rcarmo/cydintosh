# LC low-memory, VBR, and A-trap ownership design

This note is the handoff point between the current diagnostic hook pile and the
next implementation step: a RAM-owned low-memory initializer behind a feature
flag. It intentionally avoids new ROM patches or direct continuation bypasses.

Current comparison point: `logs/serial-capture-20260527-191451.log`. After the
initial disabled-by-default seed scaffold, `logs/serial-capture-20260527-194755.log`
still matches that frontier. An enabled-seed A/B capture,
`logs/serial-capture-20260527-195516.log`, logs the RAM-owned seed and also
matches the same frontier. In both runs the ROM reaches the diagnostic/serial
monitor guard at `0x40849ff8` with `d7=0x010a7b6e`, but the pre-monitor failure is
still:

```text
LC illegal instruction callback: opcode=0x7562 ppc=0x00007fba pc=0x00007fbc ...
LC diagnostic exception stack: sp=0x001ffff6 words=2719 0000 7fba 0010 0218 0000 9a96 0000
```

The design goal is to make low memory, the vector base, and trap tables normal
RAM state that the ROM and trap setters can mutate, instead of returning
PC-gated synthetic bytes from `lc_memory_bus_read8()`.

## Relevant facts from current code and ROM traces

### Musashi A-line frame shape

Musashi currently handles A-line instructions (`EXCEPTION_1010`) by stacking a
format-0 frame even for the 68020/68EC020 path:

```c
m68ki_stack_frame_0000(REG_PPC, sr, EXCEPTION_1010);
m68ki_jump_vector(EXCEPTION_1010);
```

The format-0 helper writes:

```text
SP+0: saved SR
SP+2: saved PC (the A-trap word address / REG_PPC)
SP+6: format/vector word, vector 10 -> 0x0028
```

This matches the LC ROM dispatcher at `0x408099b0`: after saving `a2` and `d2`,
it reads `sp@(10)` as the A-trap PC, fetches the trap word from there, and uses
the post-incremented `a2` as the post-trap continuation.

### Dispatcher stack transform

For high traps (`trap >= A800`), the dispatcher does not return with `RTE`.
Instead it rewrites the exception frame into an ordinary subroutine call:

```asm
408099b4  moveal %sp@(10),%a2   ; original exception PC
408099b8  movew  %a2@+,%d2      ; trap word, a2 now post-trap PC
...
408099c6  movel  @(0x1e00,%d2:w:4),%sp@(8)
408099ce  movel  %a2,%sp@(12)
408099d2  movel  %sp@+,%d2
408099d4  moveal %sp@+,%a2
408099d6  rts                   ; jump to handler, handler RTS -> post-trap PC
```

So the table entry must be a real handler whose return contract matches the
trap. Routing every high trap to a generic `moveq #0,d0; rts` makes the handler
return to the word after the trap, but if that post-trap address is itself an
inline ROM helper ending in `RTS` (for example `A895`/`ShutDown` at
`0x40800582 -> 0x40800584`), the second `RTS` consumes whatever long happens to
be next on the stack. That is consistent with the current low-RAM `0x00007fba`
illegal callback after the high-table read for `$1054` / `A895`.

### Low-memory cells currently synthesized

The current `lc_memory.c` hooks synthesize, or partially synthesize, these cells:

| Range/cell | Current synthetic role | RAM-owned target |
|---|---|---|
| `$0000..$03ff` | Exception vectors and high-trap alternate table reads when the ROM dispatcher indexes low memory. | Explicit vector table seeded with ROM exception dispatcher entries and normal diagnostic/autovector cells. |
| `$0400..$05ff` | Low OS trap table for `A000..A07f` reads. | RAM low-trap table; `SetTrapAddress` updates these cells. |
| `$0a50`, `$0a58` | `TopMapHndl` / `SysMap` for the synthetic Resource Manager map. | RAM globals pointing at RAM handle cells. |
| `$0db0..$0db3` | Basilisk dispatch magic. | Normal RAM cell initialized once. |
| `$0dbc..$0dbf` | Post-InitMMU callback, currently forced to a ROM `RTS`. | Normal RAM callback/vector cell with provenance logged. |
| `$0e00..$1dff` | High OS trap table for `A800..ABff`. | RAM high-OS table with trap-specific entries, not universal success. |
| `$1e00..$2dff` | High Tool trap table for `AC00..Afff`. | RAM high-tool table with trap-specific entries. |
| `$02ae`, `$031a`, `$0698`, `$069c`, `$07f0` | Resource Manager globals/trampolines. | RAM globals seeded with the Resource Manager prototype and then mutated normally. |

## Ownership model

Add a small LC low-memory subsystem in `lc_memory.c` first, then split it into a
separate file once it stops being experimental.

### State

Use explicit constants for every seeded cell. The implementation should avoid
anonymous magic ranges in the read path.

Suggested names:

```c
#define LC_LOW_MEMORY_FLAG_RAM_OWNED          0 /* default off until A/B passes */
#define LC_LOW_MEMORY_SIZE                    0x3000u
#define LC_LOW_VECTOR_A_LINE                  0x0028u
#define LC_LOW_VECTOR_F_LINE                  0x002cu
#define LC_LOW_TRAP_TABLE_OS_BASE             0x0400u
#define LC_LOW_TRAP_TABLE_HIGH_OS_BASE        0x0e00u
#define LC_LOW_TRAP_TABLE_HIGH_TOOL_BASE      0x1e00u
#define LC_LOW_TRAP_DEFAULT_UNIMPLEMENTED     0x40800d88u
#define LC_LOW_CALLBACK_POST_INITMMU          0x00000dbcu
```

The initializer should write through helpers (`seed8/16/32`) into `bus->ram`, so
normal bus reads see the bytes. It should also log a concise checksum or a list
of important seeded cells once per bus init.

### Initialization phases

1. **Cold bus init seed** — after guest RAM allocation and before `bus->initialized
   = true`, seed only cells that are legitimate reset starting state and are not
   expected to be created by destructive RAM tests later.
2. **Post-RAM-fill restore seed** — after the ROM's RAM-fill/copy test has run,
   restore cells that the current direct-entry probe cannot protect yet. This is
   an explicit transitional phase and should be gated/logged, not hidden as
   per-PC reads.
3. **Runtime mutation** — `SetTrapAddress`, Memory Manager, and Resource Manager
   calls update RAM cells/handles directly. The read path should then fall
   through to `bus->ram[decoded.offset]`.

The current branch can start with phase 1 plus a deliberately broad phase-2
function called from the first places that currently install synthetic values.
The important property is that the values become RAM state and subsequent reads
are not PC-gated.

## Initial seed contents

### Vectors

Seed only the vectors needed to remove synthetic trap-table reads, not a fake full
Mac OS vector table. Initial candidates:

| Vector | Offset | Initial value | Reason |
|---:|---:|---:|---|
| 4 illegal | `$0010` | existing ROM diagnostic/illegal vector if identified; otherwise leave zero and keep current diagnostics. | Do not hide bad execution with a fake success path. |
| 10 A-line | `$0028` | `0x408099b0` | LC ROM A-trap dispatcher currently reached and understood. |
| 11 F-line | `$002c` | diagnostic/ROM vector if identified. | Needed for no-FPU paths; do not fake success blindly. |
| autovectors 25/28 | `$0064`, `$0070` | ROM-installed values only after the ROM writes them. | VIA/SCC timer diagnostics should own these later. |

Open question: confirm from reset-subtest logs whether the ROM itself installs
vector 10 before the post-reset traps. If it does, do not pre-seed it; instead
trace and preserve the ROM write.

### A-trap tables

Do **not** keep the current universal default for all high traps as the final
state. It hides missing services and likely causes the `0x00007fba` double-return.

Seed strategy:

- Low OS table `$0400..$05ff`: initialize observed low traps to the current
  provisional default handler, but leave unobserved traps zero or a diagnostic
  handler that logs and intentionally drops into the existing illegal-path guard.
- `A047` / `SetTrapAddress`: when the ROM calls it, write the requested handler
  into the RAM table cell if the handler address is plausible. Log rejected fill
  patterns (for example `0xb6db6db6`) exactly as today.
- `A05D` / `SwapMMUMode`: remove the dispatcher NOP only after the RAM table and
  exception frame naturally return through the ROM path.
- High table `$0e00..$2dff`: seed trap-specific entries only for traps with a
  known safe contract. For `A895` / `ShutDown`, do not use the generic success
  helper as the final behavior; it should become a trap-specific terminal or ROM
  diagnostic path once the expected LC shutdown handler is identified.

### Resource Manager globals

The current resource-map prototype is useful. Move its globals into RAM before
adding new resource types:

```text
$02ae  ROMBase
$031a  resource offset mask
$0698  resource trampoline / success vector
$069c  resource error/epilogue vector
$07f0  resource load/realize vector
$0a50  TopMapHndl
$0a54  SysMapHndl
$0a58  SysMap
$0a5a  CurMap
$1000  TopMapHndl handle cell
$1020  resource map body
```

Once these are seeded, remove `lc_memory_should_read_post_reset_resource_map()`
entries for those cells in small A/B steps.

## Implementation sequence

1. Add seed helpers and a disabled-by-default `LC_ENABLE_RAM_OWNED_LOW_MEMORY`
   flag. This scaffold now exists in `lc_memory.c`; both the default build and a
   `PLATFORMIO_BUILD_FLAGS='-DLC_ENABLE_RAM_OWNED_LOW_MEMORY=1'` build/upload
   pass succeed. Captures `194755` (default) and `195516` (enabled seed) show no
   frontier movement and no regression, so the next step is to make selected
   synthetic reads decline under the flag one group at a time.
2. Seed `$0db0`, `$0dbc`, `$0400..$05ff`, `$0e00..$2dff`, vector 10, and the
   Resource Manager prototype into RAM under the flag, preserving current values
   byte-for-byte where possible. This is implemented.
3. When the flag is enabled, make the corresponding synthetic read hooks decline
   one group at a time and let normal RAM reads return the seeded bytes. The
   first retirement slice now makes `$0DB0` and `$0DBC` decline under
   `LC_ENABLE_RAM_OWNED_LOW_MEMORY=1`; Resource Manager low-memory globals are
   RAM-seeded before the Resource Manager path reads them. The Resource Manager
   map body is deliberately **not** seeded during cold bus init because the reset
   RAM-fill test can overwrite `0x00008000`; it is reseeded at the post-reset
   Resource Manager entry.
4. A/B test against `serial-capture-20260527-222958.log`:
   - no `pc=0xffffffff`
   - no `addr=0x01000000`
   - no `0xffffffd8`
   - no `0x001fdfec`
   - no `0x00007fba`
   - no diagnostic exception stack.

   `serial-capture-20260528-083039.log` passes this gate with
   `LC_ENABLE_RAM_OWNED_LOW_MEMORY=1`, no `$0DBC` synthetic read, and the same
   100M-cycle frontier at `pc_after=0x4081ab90`.
5. Move the post-reset memory-layout cells out of read-time hooks. The first
   attempt seeded the `0x001fffa0` stack/local window too early and regressed to
   the diagnostic monitor. The fixed version seeds at the second-pass entry
   (`0x408418e4..0x40841910`) and lets the no-MMU byte, dispatch selector, and
   second-pass locals fall through to RAM. `serial-capture-20260528-154630.log`
   preserves the clean frontier at `pc_after=0x4081ab92` with no no-MMU,
   dispatch-kind, or second-pass-local synthetic reads.
6. Start low A-trap table retirement only for RAM-owned entries. A047 was first
   proven safe in `serial-capture-20260528-084005.log`. The next safe set was
   A047, A03F, A051, A02D, A01E, A02E, A022, A023, A00A, A011, A001, A01F, and
   A06E; `serial-capture-20260528-155655.log` left only A019/InitZone on the
   synthetic low-trap table path, with the regression gates clear and the new
   frontier in the A06E/SlotManager loop at `pc_after=0x40805fb4`.
7. Add a minimal RAM-owned InitZone/Memory Manager seed for A019. The first
   attempt proved why the A-line vector must be part of the same low-memory
   model: seeding the zone header without refreshing vector 10 regressed to
   `pc=0x00000000` after A02D. The fixed slice seeds the line-A vector, A019 and
   A02D table entries, low-memory `MemTop`/`BufPtr`/`HeapEnd`/`TheZone`/
   `ApplLimit`/`MemErr`, and a small classic zone header at the ROM table's
   requested start (`0x00003800`). `serial-capture-20260528-191240.log` removes
   the final A019 low-trap table synthetic read (`trap=0x019`: 0; `A-trap table
   synthetic read`: 0), keeps the regression gates clear, and reaches the same
   SlotManager frontier at `pc_after=0x40805fac`, `stopped_on_monitor=0`.
8. Add a bounded RAM-owned SlotManager no-PDS surface for the reset slot scan.
   The first version advanced past the A06E loop but exposed two ownership bugs:
   arbitrary stack-backed InitZone calls must remain no-ops until multiple zones
   are modeled, and the tiny heap allocator needed an overflow guard for huge
   provisional sizes. The fixed slice keeps the initial ROM-table InitZone seed,
   treats later stack-range InitZone calls as no-ops, bounds the synthetic heap,
   and lets A023/A024/A025/A026/A029/A02A/A055 table entries fall through to RAM
   because the bus-side Memory Manager surface already owns them. With
   `serial-capture-20260528-201344.log`, the gate is clean (`A-trap table
   synthetic read`: 0, no `0x00007fba`, no diagnostic exception stack, no illegal
   instruction callback) and the frontier moves from SlotManager to a Resource
   Manager/Memory Manager handle path at `pc_after=0x4080db72`. The follow-up
   `serial-capture-20260528-202210.log` allocates an empty master pointer for the
   observed huge provisional NewHandle size so the later SetHandleSize operates
   on a real handle instead of a stale Ptr block; it preserves the clean gate and
   advances the same handle-validation path to `pc_after=0x4080dc26`. The current
   validated build (`serial-capture-20260528-215618.log`, with tracing disabled
   for speed) keeps the gate clean and reaches the Memory Manager compaction scan
   at `pc_after=0x4080ea0a`. The follow-up
   `serial-capture-20260529-071512.log` replaces the rejected one-field
   `zone+0x34` patch with the ROM-observed classic layout: the zone header ends
   at `+0x34`, `+0x30` is the allocation rover, and `zone+0x34` owns a real
   free-block length. Repeated reset-table `InitZone` calls are now idempotent so
   they do not wipe Resource Manager allocations; RAM-parameter temporary zones
   remain no-ops after a test seed regressed to the diagnostic monitor. The clean
   100M frontier is now the Resource Manager/high-trap path at
   `pc_after=0x408099f0`; a 250M diagnostic stayed clean but remained in the
   same Resource Manager scan/dispatch loop. The next resource-global cleanup
   seeds `SysMapHndl` and `CurMap` beside `TopMapHndl`/`SysMap`; this preserves
   the clean frontier but does not by itself break the Resource Manager loop. A
   first attempt to make `DisposePtr` reuse freed synthetic blocks regressed to
   the older `0x4080dc14` handle path, so block reuse remains disabled until the
   Memory Manager owns real block headers/free lists rather than only host-side
   records. The next Memory Manager slice adds classic-style allocated block
   headers for synthetic Ptr/Handle data blocks: Ptr data is returned after an
   8-byte header, Handle data after a 12-byte header, the low 24 bits carry the
   total block size for the ROM's `$031A` mask, and the high byte marks allocated
   blocks. This keeps the gate clean; a 250M diagnostic
   (`serial-capture-20260529-082728.log`) stays in the ROM Resource Manager scan
   at `pc_after=0x4081ab92` instead of falling into the older invalid-execution
   paths. The following low-memory device-global slice seeds `$01D4`/VIA,
   `$01D8`/SCCRd, `$01DC`/SCCWr, and `$01E0`/IWM-SWIM to their LC 24-bit I/O
   bases so ROM code no longer falls back to low-RAM pseudo-device addresses;
   `serial-capture-20260529-091302.log` preserves the clean gate at the current
   100M frontier. A zero-filled `ReadXPRam` implementation was tested and
   rejected because the ROM Resource Manager scan regressed to the `0x4080dc04`
   handle-validation path; PRAM bytes need a reference-backed model, not a blank
   buffer. The first accepted PRAM slice is narrower: mac-rom `ResourceMgr.a`
   shows ReDoMap reading xPRAM `$AE` as a resource-combo index where zero means
   use `ProductInfo.DefaultRSRCs`, so the synthetic A051 helper now only writes
   `$AE = 0` and leaves `$8A` to the ROM/default path. The same slice registers
   the RAM-backed ROM map handle (`0x8000 -> 0x8020`, size `0x320`) with the
   tiny Memory Manager record table. The follow-up Resource Manager handle slice
   adds a RAM-backed ROM-resource master-pointer slab at `0x8400..0x8700` for
   `HandleZone(RomMapHndl)`: parsed LC ROM resource RelHandles are small offsets
   (`0x5c..0x24c`), and returning the real zone base lets ReDoMap write resource
   master pointers into the zone free-block header. `serial-capture-20260529-195930.log`
   preserves the clean 100M gate and `serial-capture-20260529-194728.log`
   preserves the clean 250M gate; the frontier remains `pc_after=0x4081ab92`.
   Non-zero synthetic `ProductInfo.DefaultRSRCs` values `2`/`4`, xPRAM `$AE=1`,
   reserving the master-pointer area inside the zone, and copying the ROM
   candidate ProductInfo record at ROM offset `0x6de60` with `DefaultRSRCs=1`
   were rejected because the incomplete ProductInfo/resource-map model drove the
   probe into earlier handle-validation or diagnostic-monitor illegal-instruction
   paths. A probe log confirms low-memory `$0DD8` contains the RAM-fill pattern
   `0xdb6db6db` by `0x40805e48`, so the fallback descriptor remains an explicit
   direct-entry scaffold until the real reset/UniversalInfo path survives the RAM
   test. A bounded `_MaxBlock` (`A061`) helper now returns available synthetic
   heap space below the ROM's top-of-RAM stack reserve if Resource Manager growth
   code reaches it; current 100M/250M/500M traces do not hit it before the
   frontier, and it preserves the clean gates. The current accepted 500M capture
   (`serial-capture-20260529-222234.log`, matching the earlier
   `serial-capture-20260529-214440.log`) ends cleanly at `pc_after=0x4081abf0`
   in `CountCombos`. The added `CountCombos` trace shows a complete 63-entry
   ROM-resource scan producing the expected approximate map size `0x1ab0`, then a
   fresh second invocation, so the remaining issue is repeated
   `InitResources`/ReDoMap ownership rather than a single corrupt linked-list
   walk. A post-header free-list reuse trial for disposed Ptr blocks was rejected
   after `serial-capture-20260529-213042.log` and
   `serial-capture-20260529-213456.log` stopped in low RAM at `pc_after=0x0000011c`;
   actual `DisposePtr` reuse must wait until `$0DB8`/dispatch-table, zone, and
   master-pointer lifetime are modeled together. A controlled fallback
   ProductInfo default-resource trial (`LC_PRODUCTINFO_DEFAULT_RSRCS=1`,
   `serial-capture-20260529-221650.log`) let the ROM add resources but regressed
   to the diagnostic monitor at `pc_after=0x40849ff8`, with an intermediate
   execution checkpoint at the attribute-tagged address `0x58091a06`. A
   reference-backed `_StripAddress` correction now strips the candidate address in
   `D0`, which matches the `ResourceMgr.a` RelHandle/RLocn call sites and keeps
   the default-zero path clean (`serial-capture-20260529-225849.log`). Retrying
   combo 1 after that correction still reaches the same monitor path. A
   2026-05-30 diagnostic rerun moved the useful symptom earlier: before the
   `0x58091a06` checkpoint, the CPU returns from the low A-trap dispatcher at
   `0x40809a18` into `pc=0x5807b400` with `d2=0x5807b4c0` and `a4=0x000030b8`.
   Restoring Resource Manager high-dispatch entries, aliasing `$58xxxxxx` as ROM
   offsets, and stubbing a ROM-resource `_Read` did not produce an accepted
   combo-1 path and were left out of the accepted firmware. Non-zero defaults
   therefore need a complete ROM map identity / `RomMapHndl` / handle-high-byte
   lifetime model, not just trap-register or address-alias fixes.
9. Replace the `A05D`, `A001`, and high-trap return repair hooks only after the
   RAM-owned path preserves or improves the frontier.

## `0x00007fba` working hypothesis

The strongest current hypothesis is no longer "bad ROM continuation" but "bad
high-trap / terminal-helper contract":

1. The ROM dispatcher sees a high trap in a helper-shaped ROM sequence, including
   the `A895`/`ShutDown` helper at `0x40800582`.
2. The first fix stopped masking plausible ROM-owned high-trap table entries:
   `serial-capture-20260527-200034.log` showed `$1054 -> 0x4080ed7e` in RAM, so
   `lc_memory.c` now lets high-table reads use RAM when the handler points into
   ROM.
3. That advanced through additional Resource Manager work and changed the final
   monitor stop to `0x40849fca`, but `serial-capture-20260527-200914.log` showed
   the remaining bad transfer explicitly: `prev_pc=0x40800584` executed an `RTS`,
   popped `0x00004080`, ran RAM-fill pattern words, and eventually reached the
   illegal callback at `0x00007fba`.
4. The direct cause was then narrowed to two ownership gaps, not a bad ROM
   continuation: the synthetic Resource Manager map/GDevice chain overlapped the
   low RAM address that the `ShutDown` helper was popping, and the tiny synthetic
   Memory Manager heap was growing up into the top-of-RAM stack during repeated
   Resource Manager `SetHandleSize` calls.
5. The current fix keeps the Resource Manager prototype away from the popped
   low-RAM address, seeds the ROM's post-reset probe dispatch globals
   (`$0DB8`/`$0DD8`) before the `0x40805e48` probe, and starts the tiny synthetic
   heap at `0x00100000` instead of near the stack. `serial-capture-20260527-215107.log`
   runs the full 100M-cycle micro-probe with no `0x00007fba`, no diagnostic
   exception stack, no `pc=0xffffffff`, no `addr=0x01000000`, and no monitor
   stop. It ends in a Resource Manager/INIT-loading loop at `pc_after=0x4081abec`.
6. The follow-up `serial-capture-20260527-222958.log` moves `$0DB0`/`$0DBC` and
   the Resource Manager low-memory globals into RAM-owned seed cells, removes the
   per-PC Resource Manager map read hook, and preserves the 100M-cycle frontier
   at `pc_after=0x4081ab90` with the same no-monitor/no-illegal regression gates.

This completes the `0x00007fba` explanation/elimination without a direct branch
around `0x40800584` or a forced continuation to `0x40800218`. The next work is to
turn the remaining synthetic low-memory reads and Memory/Resource Manager host
side effects into coherent RAM-owned services so the ROM can progress beyond the
`0x4081ab90` Resource Manager loop.

### 2026-05-30 combo-1 ROM-map lifetime diagnostics

- `LC_PRODUCTINFO_DEFAULT_RSRCS=1` no longer fails first as an opaque
  `$58xxxxxx` jump once the low A-trap dispatcher return ring is enabled: the
  first `$5807b400` return is preceded by Resource Manager ROM-map growth at
  `0x4081aba2` (`HandleZone`/`StripAddress`) and `0x4081bfcc`/`0x4081bfe0`
  (`SetHandleSize`/`BlockMove`).  The tagged value was map/resource bytes being
  used as a return address after synthetic Memory Manager allocation overlapped
  the live direct-probe stack.
- The transitional Memory Manager now keeps dynamic master-pointer cells in a
  reserved slab below the zone start (`0x2800..0x37ff`), tracks a small reusable
  free-list for released Ptr/Handle bodies, and caps synthetic heap growth below
  `ApplLimit`/current stack/top-of-RAM reserve.  Reallocated handle extensions
  are zero-filled so newly exposed resource-map fields are not RAM-fill pattern
  bytes.
- Combo-1 remains diagnostic-only: the latest stack-floor run
  `logs/serial-capture-20260530-024922.log` avoids the `$58xxxxxx` return and
  ROM-window writes, but still returns through low RAM after Resource Manager map
  growth (`pc=0x00000000`, `prev_pc=0x001fb714`).  This confirms the next slice
  is still ROM-map/Memory-Manager lifetime/initialization, not a ROM patch.
- Default-zero firmware was restored/flashed after the combo run and partially
  rechecked in `logs/serial-capture-20260530-025245.log`: no attr-tagged/zero
  return, synthetic A-trap table read, invalid-exec, illegal-instruction, or
  diagnostic-exception markers appeared during the 130 s capture (to
  `executed_total=163644837`).  A full 500M default-zero gate is still required
  before accepting this slice.

### 2026-05-30 follow-up gates and cap-detail probe

- Full default-zero ProductInfo regression gate passed in
  `logs/serial-capture-20260530-025630.log`:
  `cycles=504234789`, `pc_after=0x4081abf0`,
  `stopped_on_zero_ram=0`, `stopped_on_monitor=0`.  The searched regression
  markers remained zero: attr-tagged/zero-target dispatcher returns, A-trap
  table synthetic reads, `0x00007fba`, `pc=0xffffffff`, `addr=0x01000000`,
  `0xffffffd8`, `0x001fdfec`, diagnostic exception stack, illegal instruction,
  and post-reset invalid execution trace.
- The next combo-1 probe with deeper cap logging is
  `logs/serial-capture-20260530-030942.log`.  It shows the remaining failure is
  no longer a `$58xxxxxx` dispatcher return: Resource Manager reaches
  `0x4081beb4` with `a4=0x00002b50` and `a4@=0x001fb6c0`, but that target begins
  with the resource type bytes `43555253` (`'CURS'`) rather than a map header.
  In other words, the active map handle has become pointed at the type-list
  entry/type data (`map_hdr=43555253 ...`, `a0_m4=38004355`, `a0=52530000`), so
  the ROM reads the first type as the type-count word and the defensive DBRA cap
  fires.  The next implementation slice should fix map-handle/body base
  preservation during the temporary `NewHandle`/`HLock`/`SetHandleSize` path
  rather than widening the cap.

### 2026-05-30 locked-map handle attempt

- Added a reference-backed Memory Manager correction for locked handles: when a
  handle is locked, `_SetHandleSize` now keeps the master pointer stable and
  treats the original allocation as capacity, updating only the logical size
  when the requested size fits.  The handle tracking table was expanded to cover
  the whole temporary master-pointer slab.  This matches the ROM Resource
  Manager's expectation that its HLocked ROM map handle does not move while
  `ResizeMap`/`MoveNames` adjust offsets.
- A combo-1 run after this change (`logs/serial-capture-20260530-070353.log`)
  still reached the bad map state (`map_hdr=43555253`, the first type entry) and
  later low-RAM/diagnostic monitor.  A follow-up caller-aware clamp run
  (`logs/serial-capture-20260530-070809.log`) changed the failure to an earlier
  line-A/vector-style jump (`pc=0xffff5807`, `prev_pc=0x4081aba0`) while adding a
  resource via `_HandleZone`; this confirms the map/header corruption is still
  upstream of the defensive copy cap and should not be accepted.
- Leave the new locked-handle model as diagnostic until the next default-zero
  gate proves it, then continue by finding the first write that changes the ROM
  map header/type-list offsets rather than adding more return/vector guards.
