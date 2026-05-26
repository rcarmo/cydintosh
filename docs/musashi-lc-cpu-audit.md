# Musashi CPU configuration audit for Macintosh LC bring-up

This note captures the CPU-core state for Macintosh LC bring-up. The first
checkpoint audited the Mac Plus-fixed Musashi configuration; the next checkpoint
adds an LC-only 68EC020/68020 configuration scaffold without claiming ROM
execution yet.

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
site directly.

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
compile-time Musashi configuration scaffold for 68EC020/68020, but LC ROM
execution is still pending the LC memory-map and CPU setup boundary.

`src/machine_lc/lc_cpu.c` now logs the selected initial CPU target
(`M68K_CPU_TYPE_68EC020`), compile-time emulation switches, conservative quantum
settings, and raw first/second ROM longwords as reset-vector candidates. These
events are also recorded in the LC trace ring buffer (`src/machine_lc/lc_trace.c`)
for later panic/hang dumps. It does not call `m68k_init()` or execute guest code
yet.

The next CPU-core step is to turn this scaffold into a runtime setup path that
selects `M68K_CPU_TYPE_68EC020`, verifies the actual LC reset SP/PC mapping, and
reports the first exception/unmapped-access failures.
