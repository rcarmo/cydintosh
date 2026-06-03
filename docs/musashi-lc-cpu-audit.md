# Musashi CPU configuration audit for Macintosh LC bring-up

This note captures the CPU-core state for Macintosh LC bring-up. The branch now
has an LC-only 68EC020/68020 Musashi configuration scaffold and structured trace
helpers, but it still does not execute the LC ROM.

## Current state

Current branch source uses the existing umac/Musashi integration for the Mac Plus
path, and a separate ESP32-P4 LC skeleton for the Tab5 path.

Observed current Musashi config in `include/m68kconf.h`:

| Setting | Current value | LC implication |
|---|---:|---|
| `M68K_EMULATE_010` | `OPT_OFF` | 68010 opcodes/features disabled |
| `M68K_EMULATE_EC020` | `OPT_OFF` | 24-bit 68EC020 mode disabled |
| `M68K_EMULATE_020` | `OPT_OFF` | full 68020 mode disabled |
| `M68K_EMULATE_040` | `OPT_OFF` | 68040 mode disabled, acceptable for LC-first work |
| `M68K_FIXED_CPU_TYPE` | `CPU_TYPE_000` | CPU is compile-time fixed to 68000 |
| `M68K_EMULATE_FC` | `OPT_OFF` | function-code callbacks are disabled |
| `M68K_ILLG_HAS_CALLBACK` | `OPT_OFF` | illegal instruction hook is disabled |
| `M68K_EMULATE_ADDRESS_ERROR` | `OPT_OFF` | odd word/long access traps disabled |
| `M68K_EMULATE_INT_ACK` | `OPT_SPECIFY_HANDLER` | interrupt acknowledge callback already wired |
| `M68K_EMULATE_RESET` | `OPT_SPECIFY_HANDLER` | reset callback already wired |

Existing umac code in `external/umac/src/main.c` hard-codes 68000 behavior:

```c
m68k_init();
m68k_set_cpu_type(M68K_CPU_TYPE_68000);
m68k_pulse_reset();
```

The disassembler calls are also currently hard-coded to `M68K_CPU_TYPE_68000`.

## What LC likely needs first

A Macintosh LC-class ROM is expected to need at least 68020-class behavior. The
first experimental target should be **68EC020 / 24-bit addressing** rather than a
full 32-bit 68020, because early LC-class machines and System 6/7 boot paths are
more likely to start in a 24-bit-compatible mode.

Recommended first CPU target:

```text
M68K_CPU_TYPE_68EC020
```

Reasons:

- enables 68020 instruction decoding/execution;
- preserves 24-bit address masking (`0x00ffffff`) in Musashi;
- avoids PMMU expectations;
- gives a safer first pass for a Macintosh LC ROM than full 32-bit `68020`.

If reset/vector execution clearly expects 32-bit clean behavior, later evaluate:

```text
M68K_CPU_TYPE_68020
```

68030 should be deferred until evidence demands it.

## Required config changes before LC execution

Do not enable these globally for the Mac Plus path. Gate them behind the LC
machine target or a dedicated LC Musashi config.

Minimum expected LC config changes:

```c
#define M68K_EMULATE_EC020 OPT_ON
#define M68K_EMULATE_020   OPT_ON
#undef M68K_FIXED_CPU_TYPE
```

Optional/diagnostic changes to consider for early bring-up:

```c
#define M68K_ILLG_HAS_CALLBACK OPT_SPECIFY_HANDLER
#define M68K_EMULATE_FC        OPT_SPECIFY_HANDLER
```

Notes:

- Removing `M68K_FIXED_CPU_TYPE` is required if runtime `m68k_set_cpu_type()` is
to select `M68K_CPU_TYPE_68EC020` or `M68K_CPU_TYPE_68020`.
- `M68K_EMULATE_FC` may become important for supervisor/user and program/data
space behavior on 68010+ instructions such as `MOVES`.
- Illegal-instruction callbacks should be enabled for traceability before
running the ROM for real.
- Address error emulation is noted as only properly emulated for 68000 in the
Musashi comments, so do not turn it on blindly for 68020 bring-up.

## Required integration changes

The current external umac entry point assumes Mac Plus/68000. LC work needs an
LC-specific CPU setup boundary rather than changing the existing Mac Plus call
site directly. The current branch has LC CPU diagnostics and trace hooks, but not
the runtime `m68k_init()`/`m68k_set_cpu_type()` execution path.

Planned LC CPU setup should log:

- selected CPU type (`68EC020` first);
- address mask / 24-bit vs 32-bit mode;
- reset SP and PC read from the LC ROM vector table;
- exception/vector hits;
- illegal instruction opcodes and PC;
- bus/address/unmapped access context.

The current `src/machine_lc/` boundary is the right place to add this without
claiming LC boot yet.

## Open questions

- Does the supplied LC ROM start in a 24-bit-compatible path, or does it require
32-bit mode very early?
- Does the LC ROM require 68030-specific stack frames or control registers?
- Which exception frame formats does the current Musashi core produce for the
first ROM exceptions under 68EC020?
- How much of function-code handling is required before ROM hardware probes work?
- What are the first missing memory-mapped hardware ranges after the reset vector
starts executing?

## LC-only configuration scaffold

`include/m68kconf_lc.h` now exists as the LC-only Musashi configuration. It is
selected by the `esp32-p4-tab5-lc-color` PlatformIO environment with:

```text
-DMUSASHI_CNF=\"m68kconf_lc.h\"
```

The standard Mac Plus config remains `include/m68kconf.h` and stays fixed to
`CPU_TYPE_000`.

Current LC scaffold settings:

| Setting | LC scaffold value | Notes |
|---|---:|---|
| `M68K_EMULATE_010` | `OPT_ON` | Enables 68010+ core paths needed below 020 |
| `M68K_EMULATE_EC020` | `OPT_ON` | First runtime CPU target should be `M68K_CPU_TYPE_68EC020` |
| `M68K_EMULATE_020` | `OPT_ON` | Allows later full `M68K_CPU_TYPE_68020` experiments |
| `M68K_EMULATE_030` | `OPT_OFF` | 68030 deferred until evidence requires it |
| `M68K_EMULATE_040` | `OPT_OFF` | Not relevant for Macintosh LC-first work |
| `M68K_FIXED_CPU_TYPE` | not defined | LC CPU type can be selected at runtime |
| `M68K_EMULATE_FC` | `OPT_SPECIFY_HANDLER` | Prepared for 68010+ function-code handling |
| `M68K_ILLG_HAS_CALLBACK` | `OPT_OFF` | To be enabled with an LC trace callback later |

`m68kconf_lc.h` also has a guard that errors if used without
`CYD_MACHINE_MAC_LC`, so Mac Plus builds cannot accidentally pick it up.

## Current conclusion

The Mac Plus path remains 68000-fixed and unchanged. The LC/P4 path now has a
Tab5-only Musashi configuration and linked core for 68EC020/68020. LC ROM
execution is still pending verified ROM overlay/reset-vector mapping and hardware
stubs.

`src/machine_lc/lc_cpu.c` now logs the selected initial CPU target
(`M68K_CPU_TYPE_68EC020`), compile-time emulation switches, conservative quantum
settings, and raw first/second ROM longwords as reset-vector candidates. These
events are also recorded in the LC trace ring buffer (`src/machine_lc/lc_trace.c`)
for later panic/hang dumps. It exposes scaffold hooks for exception-vector hits,
illegal/unimplemented instructions, bus errors, address errors, and interrupt
levels.

`src/machine_lc/lc_musashi_bus.c` provides the LC-only Musashi callback bridge to
the new memory-bus harness. The current hardware smoke test writes a tiny RAM-only
synthetic program, selects `M68K_CPU_TYPE_68EC020`, pulses reset, and runs a
bounded `m68k_execute(64)` without executing LC ROM bytes. The latest serial log
shows `reset_pc=0x00000100`, `reset_sp=0x00002000`, `pc_after=0x00000104`,
`sr=0x2704`, and `cpu_type=3`.

The current CPU-core diagnostic seeds the `0x00402e00` reset-body probe with the
caller continuation used by the reset trampoline (`a6=0x004000b4`). That avoids
the invalid zero-RAM path from a bare entry and sets `vbr=0x40846140`. A local
macemu/BasiliskII reference search identifies the `0x00f14000`/`0x50f14000`
frontier as a physical NuBus/slot video-probe family. Reporting only the observed
`+0x0804` ready/complete bits (`0x03`) advances the unpatched LC ROM through that
probe; the `serial-capture-20260526-212157.log` 100M-cycle hardware capture
then reached the broad ROM-monitor guard at `0x40849eae` after 49.5M cycles with
`d2=0x50000304`, `d7=0x01000304`, and `stopped_on_monitor=1`. The guard is now
narrowed to no-input command polling sites so the monitor setup path can run.
With the SCC-like no-input/transmit-ready stub, `serial-capture-20260526-213538.log`
advances to `0x40849fca` with `d0=0x00008000`, `d7=0x01020304`, and
`stopped_on_monitor=1`. ROM watchpoint instrumentation in
`src/machine_lc/lc_musashi_bus.c` now confirms this seeded path does return to
`0x408000b4`, reaches the normal reset continuation at `0x408008e0`, enters reset
dispatch at `0x4084641c`, runs the `0x40845c0c` slot/video probe, performs a long
RAM-fill/check at `0x40846850`, and later enters monitor setup. With pure VIA
latches it reaches `0x408498da` via `0x40848d04` with `d6=0x00117b34`,
`d7=0x01000304`, and `usp=0x50000304` (`serial-capture-20260526-221437.log`).
After modeling the VIA ORA/no-handshake alias as external bit 0 low, reset
dispatch sets D7 bit 26 at `0x40846462`, but the first RAM-fill return exposed a
seeded-entry artifact: the ROM's RAM-region descriptor list lives at the top of
RAM (`0x043fffe4` masked to `0x003fffe4`) and was overwritten by the destructive
RAM fill, so the next descriptor became `0x6db6db6d` and the CPU took an illegal
instruction exception at odd `ppc=0x40846905` (`opcode=0xcbff`). A diagnostic-only
synthetic top-of-RAM descriptor-list read now preserves the intended
`[0x00000000, 0x00400000, 0xffffffff]` list for the reset-region loop and the
later reset copy/vector-relocation reads. Captures through
`serial-capture-20260527-074655.log` show the `0x40846c5c` byte/word/long RAM
lane test returning `d6=0`, followed by successful diagnostic SCC
selected-register (`d7=0x84`), timer-IRQ (`d7=0x86`), loopback (`d7=0x85`), VIA
IRQ (`d7=0x87`), `0x50f10000` register (`d7=0x88`), `0x50f14000` register/RAM
(`d7=0x89`), and `0x50f16000` shift/loopback (`d7=0x8c`) reset subtests. The
FPU/no-FPU (`d7=0x8d`) reset diagnostic after applying BasiliskII's no-FPU
capability behavior as a reference. Later captures show the normal BootGlobs
walk at `0x40800a90`, post-reset memory-layout entry/header at
`0x4084168e`/`0x4084172c`, second-pass dispatch at
`0x408418e4`/`0x40841922`, and record finalizer entry at `0x40841b8e`. The
current CPU/memory frontier is no longer an FPU exception or the record-copy ROM
write loop: a one-bank synthetic record table stops the writes to `0x00400000+`,
and a provisional `0x00600000` descriptor table avoids the first `0x40841cbe`
misdispatch. Follow-up hardware captures now take the BasiliskII-style no-MMU
path instead: a synthetic no-MMU flag at `0x4084169a` reaches the early return at
`0x408416a2`, a seeded direct-probe return frame lands at `0x4080130a`, the
immediate `A001` reset-continuation trap is skipped, and low-memory `$0DBC` is
supplied as a ROM `RTS` callback. That advances into the ROM A-trap dispatcher.
A narrow `A05D` / `0x40809a04` SwapMMUMode bypass plus an EC020-frame return-slot
repair moves beyond the RAM-fill trap-table jump, and splitting 24-bit ROM-window
shadow writes from masked ROM instruction-shadow writes prevents the diagnostic
copy path from corrupting executing ROM bytes. A minimal synthetic Resource
Manager `TopMapHndl`/`SysMap` chain also moves past the `0x01000000` resource-map
walk. A bounded cap on the `0x4081beb4` resource-copy loop exposed a later bad
Resource Manager map/ref-list path, but `serial-capture-20260527-145248.log`
now clears that `pc=0xffffffff` / read `addr=0x01000000` failure by aligning the
synthetic `CURS`/`FONT`/`KMAP` reference IDs with the observed reset lookup ID
(`d2=8`). The current CPU/memory frontier is the later diagnostic-monitor path:
the bounded probe reaches `0x40849ff8` with `d7=0x010a7a6e` after the provisional
post-reset finalizer/pack/compress state. A reverted dispatcher-narrowing probe
(`serial-capture-20260527-150848.log`) showed that the broad `0x40809a04` NOP was
hiding invalid low-memory trap-table entries: after only the `A05D` case was
NOPed, the next trap jumped through low address `0x00000002` and fell into
repeated illegal-instruction callbacks. The follow-up
`serial-capture-20260527-152004.log` replaces the broad NOP with a bounded
synthetic low-memory A-trap table for dispatcher reads, logs observed traps
`0x047`, `0x03f`, `0x051`, and `0x019`, and keeps only the `A05D` dispatcher on
the EC020-frame NOP path. Captures through `serial-capture-20260527-154729.log` add a bounded
`A02E`/BlockMove side effect for plausible RAM destinations/counts and a
plausibility-gated `A047`/SetTrapAddress side effect, allowing the early ROM
copy `0x408006f0 -> 0x001fdfb2`, skipping later bogus negative-size Resource
Manager copies, and rejecting a fill-pattern `SetTrapAddress(A05D,
0xb6db6db6)`. RAM execution tracing showed the later illegal callback executed
that copied table/data around `0x001fdfb0`/`0x001fdfec`. The
`serial-capture-20260527-155855.log` tranche adds a tiny synthetic Memory
Manager surface for observed `NewPtr`/`NewHandle`/`HLock`/`SetHandleSize`/
`GetHandleSize`/`StripAddress` traps; this eliminates the `0x001fdfec` illegal
callback. `serial-capture-20260527-180650.log` adds more coherent synthetic
Resource Manager map header fields plus post-RAM-fill low-memory reads for
unaligned `$02AE`/`ROMBase` and `$031A`/resource-offset mask. That turns the
first Resource Manager map-growth copy from an obviously invalid negative count
(`0x00ffffb6`) into a bounded in-map copy (`0x000002d6`) without regressing to
`pc=0xffffffff` or `addr=0x01000000`. `serial-capture-20260527-181413.log`
seeds the synthetic Resource Manager map into RAM, corrects the synthetic
name-list offset so it no longer overlaps the type/ref records, and makes
synthetic `SetHandleSize` preserve handle contents. The Resource Manager growth
copies now run against RAM-backed map addresses with bounded counts
(`0x000002b8`, `0x00000018`, `0x0000000c`) instead of negative sizes. The probe
still reaches the diagnostic monitor at `0x40849fca` with `d7=0x010a7a6e`.
`serial-capture-20260527-185030.log` confirms the next malformed high A-trap
return and narrow repair: `0x408099d6` was going to `RTS` through low-memory
vector-table data at `0x0000003e`, which branched to `0xffffffd8` and raised an
F-line diagnostic frame (`format_vector=0x002c`). The repair clears that
`0xffffffd8` path and exposes a later illegal-instruction callback at
`pc=0x00007fba` (`opcode=0x7562`, `format_vector=0x0010`). The expanded stack
log also reveals a plausible exception-frame continuation candidate
`0x40800218`; a rejected `serial-capture-20260527-184743.log` experiment that
resumed directly there regressed to `pc=0xffffffff`/`addr=0x01000000`, so it was
reverted. `serial-capture-20260527-191451.log` expands the synthetic high A-trap
read window to cover the full `$0E00..$2DFF` ROM dispatch table window (including
observed `0x408099c6` reads such as `$1054` for `A895`/ShutDown). Later captures
through `serial-capture-20260527-201224.log` prove that the `0x00007fba` path is
not a continuation to bypass: `0x40800584` (`RTS` after `A895`/ShutDown) pops a
low-RAM fill-pattern address and starts executing RAM-fill words. The
`serial-capture-20260527-215107.log` tranche eliminates that path without a direct
branch by letting plausible ROM high-trap table entries win, keeping
Resource/GDevice scaffolds away from the popped low-memory address, seeding the
post-reset `$0DB8`/`$0DD8` probe tables, and moving the tiny synthetic heap down
from the top-of-RAM stack. The current frontier is no longer a diagnostic monitor
or illegal callback: the 100M-cycle probe expires at `pc_after=0x4081abec` in the
Resource Manager/INIT-loading path, with no `0x00007fba`, `pc=0xffffffff`,
`addr=0x01000000`, `0xffffffd8`, `0x001fdfec`, or diagnostic exception stack.
The next fix should be real low-memory/VBR/A-trap/Memory Manager/resource-map/
address-map ownership, not more CritError video or FPU stubs.
