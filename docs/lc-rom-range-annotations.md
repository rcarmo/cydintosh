# LC ROM range annotations for current frontier

This note annotates the ROM ranges currently tied to the `191451` frontier:
ROM diagnostic monitor guard at `0x40849ff8`, and pre-monitor low-RAM illegal
callback `opcode=0x7562 ppc=0x00007fba pc=0x00007fbc format_vector=0x0010`.

Raw disassemblies were generated with:

```bash
/usr/bin/m68k-linux-gnu-objdump -b binary -m m68k \
  --adjust-vma=0x40800000 -D vendor/mac-lc.rom \
  --start-address=<start> --stop-address=<stop>
```

Temporary copies live under `/workspace/tmp/lc_disasm/`.

## `0x408099b0..0x40809a32` — A-line/high-trap dispatcher

Important instructions:

```asm
408099b0  movel %a2,%sp@-
408099b2  movel %d2,%sp@-
408099b4  moveal %sp@(10),%a2
408099b8  movew %a2@+,%d2        ; read A-trap word from exception PC
408099ba  cmpiw #0xa800,%d2
408099be  bcs   0x408099f0       ; low OS/tool trap path
408099c0  subiw #0xac00,%d2
408099c4  bcc   0x408099e0       ; alternate high table path
408099c6  movel @(0x1e00,%d2:w:4),%sp@(8)
408099ce  movel %a2,%sp@(12)
408099d2  movel %sp@+,%d2
408099d4  moveal %sp@+,%a2
408099d6  rts                    ; returns to table-selected handler
408099e0  movel @(0x0e00,%d2:w:4),%sp@(8)
408099ec  rtd #4                 ; alternate high-trap return
408099f0  movel %d1,%sp@-
408099f2  movel %a1,%sp@-
408099f4  movew %d2,%d1
408099f6  movel %a2,%sp@(20)     ; write post-trap PC into exception frame
408099fa  andiw #0x0100,%d2
408099fe  bne   0x40809a20
40809a00  moveb %d1,%d2
40809a04  jsr @(0x0400,%d2:w:4)@(0)
40809a0a  restore a0/a1/d1/d2/a2
40809a18  rts
40809a20  moveb %d1,%d2
40809a22  jsr @(0x0000,%d2:w:4)@(0)
40809a32  rts
```

Interpretation:

- The dispatcher expects the exception frame to contain the A-trap word PC at
  `sp@(10)` after saving `a2/d2`.
- High traps are converted into direct return targets by writing the selected
  table long over `sp@(8)`, then `rts`/`rtd` unwinds into that handler.
- Low traps dispatch indirectly through low-memory tables at `$0400` or `$0000`.
- Current synthetic low-memory trap table reads and bus-side return repairs are
  compensating for trap tables/exception frames that are not yet owned by RAM.
- Observed high-table reads include `$1054` for `A895`/ShutDown. A direct bypass
  of that frame advanced but regressed to `pc=0xffffffff`/`addr=0x01000000`, so
  the correct fix is trap-table/frame ownership rather than per-trap bypass.

Retirement target: a RAM-owned low/high A-trap table plus correct 68020 A-line
exception-frame construction and unwinding.

## `0x408001ac` — low-memory callback `$0DBC`

```asm
408001aa  moveq #1,%d0
408001ac  moveal 0x0dbc,%a0
408001b0  jsr %a0@
408001b2  jsr 0x40800600
408001b6  lea 0x9d96,%a0
408001bc  jsr %pc@(0x408001b6,%a0:l)
```

Interpretation:

- The ROM calls a long stored at low-memory `$0DBC` immediately after a no-MMU /
  reset-continuation section.
- Current code synthesizes `$0DBC = 0x40800338` (a ROM `RTS`) only for reads at
  this PC.
- This is likely adjacent to the remaining `0x00007fba` low-RAM callback
  symptom: a real low-memory callback/vector table should explain where `$0DBC`
  should point and what stack/register contract it expects.

Retirement target: seed `$0DBC` and related low-memory callbacks in RAM through a
single low-memory initializer, or let the ROM initialize them naturally.

## `0x40800218` — rejected continuation candidate

```asm
40800214  bsrw 0x40800580
40800218  lea 0x0ff8,%a0
4080021e  jsr %pc@(0x40800218,%a0:l)
40800222  bsrw 0x408011f0
40800226  lea 0x1529e,%a0
```

Interpretation:

- The malformed high-trap stack showed `frame_pc=0x40800218`, so it is a plausible
  continuation after earlier reset initialization.
- A direct resume experiment to this address regressed to
  `pc=0xffffffff`/`addr=0x01000000`.
- Therefore this is evidence about the expected exception frame, not a safe
  bypass target.

Retirement target: make the dispatcher return through this continuation only if
the real ROM frame/table state naturally requests it.

## `0x40800584` and `0x40800d88` — tiny ROM helpers currently used as stubs

```asm
4080057a  .short 0xa8b4          ; FillRoundRect
4080057c  rts
40800582  .short 0xa895          ; ShutDown
40800584  rts
```

```asm
40800d84  moveml %sp@+,%d1-%d2/%a0-%a1
40800d88  moveq #0,%d0
40800d8a  rts
```

Interpretation:

- `0x40800584` is an `RTS` immediately after a `ShutDown` trap word and appeared
  as an old next-stack value during high-trap return repair.
- In the `191451`/`194755` frontier, the high-trap table read for `A895` uses
  table address `$1054` and sets post-trap `a2=0x40800584`; routing that trap
  through the generic success helper can therefore perform a second `RTS` from a
  stack not shaped for it. Later instrumentation confirmed the real RAM table
  already contains a plausible handler (`$1054 -> 0x4080ed7e` in
  `serial-capture-20260527-200034.log`), so the old synthetic high-table read was
  masking ROM-owned table state.
- After high-table reads were allowed to use plausible RAM handlers, the probe
  advanced through more Resource Manager growth work but still hit the same
  low-RAM illegal path. `serial-capture-20260527-200914.log` and
  `serial-capture-20260527-201224.log` show the actual bad transfer begins at
  `prev_pc=0x40800584`: the helper `RTS` consumes `0x00004080` from the stack,
  executes RAM-fill pattern words, and eventually reaches the illegal callback at
  `0x00007fba`.
- `serial-capture-20260527-215107.log` eliminates this path without directly
  bypassing `0x40800584`: the Resource Manager/GDevice scaffolds no longer put
  fill-pattern code at the popped address, `$0DB8`/`$0DD8` are seeded before the
  ROM's post-reset probe table uses them, and the synthetic Memory Manager heap
  starts below the top-of-RAM stack. The 100M-cycle run now ends at
  `pc_after=0x4081abec` with no `0x00007fba`, no diagnostic exception stack, and
  no monitor stop.
- `0x40800d88` is a convenient ROM `moveq #0,d0; rts` helper and is currently the
  default synthetic A-trap handler target.
- These are useful diagnostic landing pads, but using them as universal trap
  handlers hides missing trap implementations and can create bad return-stack
  contracts for terminal or helper-shaped traps such as `A895`.

Retirement target: replace default-success trap routing with trap-specific RAM
entries and minimal implementations.

## `0x4081b65e..0x4081bfe0` — Resource Manager map/type/ref/data path

Key entry/lookup logic:

```asm
4081b65e  moveal 0x0a50,%a4      ; TopMapHndl
4081b670  moveal %a4@,%a3        ; map = *handle
4081b672  cmpw %a3@(20),%d6      ; map id/fileRefNum
4081b678  moveal %a3,%a2
4081b698  moveal %a4@,%a3
4081b69a  addaw %a3@(24),%a3     ; type list offset
4081b6a0  movew %a2@+,%d5        ; type count
4081b6d0  bsr 0x4081b698
4081b6d6  cmpl %a2@,%d3          ; type match, e.g. 'CURS'/'FONT'
4081b6e2  movew %a2@(4),%d4      ; ref count
4081b6ea  movew %a2@(6),%d0      ; ref-list offset
4081b6f2  addal %a3,%a2          ; ref list pointer
```

Resource Manager trampoline vectors:

```asm
4081b704  clrb 0x0b9a
4081b708  moveal 0x0698,%a0
4081b70c  jmp %a0@               ; current synthetic vector -> 0x4081b70e
4081b7c4  moveal 0x069c,%a1
4081b7c8  jmp %a1@               ; current synthetic error/epilogue vector
4081b8f4  moveal 0x07f0,%a0
4081b8f8  jmp %a0@               ; resource load/realize vector
```

Resource data and map-growth logic:

```asm
4081b8c2  movel %a2@(4),%d0
4081b8c6  andl 0x031a,%d0        ; resource offset mask
4081b8ca  addl 0x02ae,%d0        ; ROMBase
4081bf8a  ... grow map/data area
4081bfca  .short 0xa024          ; SetHandleSize
4081bfd0  ... compute overlap/copy
4081bfde  .short 0xa02e          ; BlockMove
4081bfe0  restore and rts
```

Interpretation:

- The current synthetic map is structurally close enough to keep as the prototype:
  `TopMapHndl`, type-list offset, name-list offset, type/ref records, and resource
  data offsets now produce bounded copies.
- The remaining problem is ownership: some globals (`$02AE`, `$031A`, `$0698`,
  `$069C`, `$07F0`, `$0A50`, `$0A58`) are still provided as per-PC synthetic
  reads, while map growth is half RAM-backed and half host-side trap behavior.

Retirement target: initialize these low-memory globals and map handles in RAM,
then let a minimal Memory Manager own `SetHandleSize`/`BlockMove` semantics.

## `0x40849fca..0x40849ff8` — diagnostic/serial monitor guard

```asm
40849fca  movew #0x8000,%d0
40849fce  btst #17,%d7
40849fd2  beq 0x40849ff8
40849fd4  btst #0,%a3@(2)
40849fda  beq 0x40849ff8
40849fdc  moveq #1,%d0
40849fde  moveb %d0,%a3@(2,%d3:l)
40849fe2  moveb %a3@(2),%d0
40849fe6  andib #0x70,%d0
40849fea  beq 0x40849ff2
40849fec  moveb #0x30,%a3@(2,%d3:l)
40849ff2  lsll #8,%d0
40849ff4  moveb %a3@(6),%d0
40849ff8  jmp %fp@
```

Interpretation:

- The guard is not the first fault; it is where the ROM diagnostic/serial monitor
  path settles after the earlier low-RAM illegal callback.
- `d7=0x010a7b6e` indicates multiple diagnostic flags are still set. The active
  plan should focus on preventing the pre-monitor trap/callback failure rather
  than making the monitor path prettier.

Retirement target: reach boot-device probing before this monitor guard by fixing
low-memory/VBR/A-trap/resource ownership and then replacing provisional I/O stubs
with named devices.

## Immediate implications

1. `$0DBC`, `$0400/$0000`, `$0E00/$1E00`, and Resource Manager globals must be
   seeded through one RAM-owned low-memory initializer, not PC-gated read hooks.
2. The `0x00007fba` callback is probably not solved by continuing to patch return
   targets; it should be traced to the low-memory callback/trap table that placed
   that address on the stack or in a vector.
3. The current Resource Manager prototype is useful, but it should move behind
   Memory Manager handle ownership before more resource types are added.
