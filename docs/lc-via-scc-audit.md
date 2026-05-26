# LC VIA/SCC/RTC/PRAM reuse audit

This note audits the existing Mac Plus `umac` VIA/SCC implementation before using
any of it for the Macintosh LC path.

## Files audited

- `external/umac/src/via.c`
- `external/umac/include/via.h`
- `external/umac/src/scc.c`
- `external/umac/include/scc.h`
- `external/umac/src/main.c`
- `external/umac/include/machw.h`

## Current Mac Plus VIA model

The generic part of `via.c` is a small 6522-ish register model with callbacks:

```c
struct via_cb {
    void (*ra_change)(uint8_t val);
    void (*rb_change)(uint8_t val);
    uint8_t (*ra_in)(void);
    uint8_t (*rb_in)(void);
    void (*sr_tx)(uint8_t val);
    void (*irq_set)(int status);
};
```

Implemented or partially implemented:

- 16 VIA register decode using A[12:9];
- port A/B read/write plus DDR masking on reads;
- shift-register transmit/receive behavior for Mac keyboard emulation;
- IFR/IER handling;
- CA1/CA2 event injection via `via_caX_event()`;
- coarse IRQ output via callback;
- placeholder `via_tick()` with no actual timer support.

The code has explicit Mac Plus assumptions:

- `via_regs[VIA_RA] = 0x10` at reset for ROM/RAM overlay;
- shift-register behavior is tailored to the original Mac keyboard protocol;
- comments map Mac Plus VIA IRQ bits as Timer1/Timer2/keyboard/VBL/1-second;
- no real VIA timers yet.

## Current Mac Plus machine callbacks

`external/umac/src/main.c` maps Mac Plus hardware semantics onto the generic VIA:

### Port A

```text
bit 7: SCC W/Req A/B input
bit 6: video page 2
bit 5: disk head select
bit 4: ROM/RAM overlay
bit 3: sound page 2
bits 2:0: sound volume
```

Only overlay is functionally significant today; it switches instruction fetch
between overlay and regular maps.

### Port B

```text
bit 7: sound reset/enable
bit 6: hblank
bit 5: mouse Y quadrature
bit 4: mouse X quadrature
bit 3: mouse button, active low
bits 2:0: RTC controls
```

Current code supplies mouse quadrature/button bits and ignores RTC controls.

### Shift register

The shift register implements a minimal pre-ADB Mac keyboard protocol:

- command `0x16`: keyboard model response;
- command `0x10`: inquiry/null or one queued scancode;
- pacing is delayed by `UMAC_EXECLOOP_QUANTUM` so the ROM sees a later response.

This is **not** an LC ADB model.

### IRQ routing

- VIA IRQ asserts 68k interrupt level 1.
- SCC IRQ asserts 68k interrupt level 2.

LC interrupt routing must be verified; do not assume these levels are sufficient.

## Current Mac Plus SCC model

`scc.c` is a minimal 85C30 model, mostly enough for mouse/DCD interrupt behavior:

- register pointer handling;
- WR2 vector;
- WR3 hunt mode tolerance;
- WR9 master interrupt enable/read-ack/status-high;
- WR15 external status interrupt enable;
- RR0/RR1/RR2/RR3/RR15 reads;
- DCD pin changes via `scc_set_dcd()`;
- IRQ callback on external/status interrupts.

The mouse path in `main.c` uses SCC DCD line changes plus VIA quadrature bits to
emulate Mac Plus mouse motion. This is not directly useful for LC ADB mouse input.

## Address-map assumptions that are Mac Plus-specific

`external/umac/include/machw.h` decodes:

```text
VIA:     (ADR24(addr) & 0xe80000) == 0xe80000
IWM:     0xdfe1ff..0xdfe1ff+0x2000
SCC RD:  top nibble 0x9xxxxx
SCC WR:  top nibble 0xbxxxxx
ROM:     0x400000 window plus overlay at reset
RAM:     0x000000/0x600000 windows depending on overlay
```

These addresses are for the Mac Plus path and must not be blindly reused for LC.
The LC memory scaffold now logs separate provisional 24-bit and 32-bit I/O windows
until reset-vector execution identifies the real access ranges.

## Reusable pieces for LC

Potentially reusable after isolation:

- the generic VIA register storage and IER/IFR mechanics;
- CA1/CA2 event injection concept for VBL/1-second tick;
- callback-based IRQ output pattern;
- SCC register skeleton for ROM serial probes if LC still expects an 85C30-like
  device at a compatible address.

These should be copied/adapted behind `src/machine_lc/` or a clean LC hardware
boundary, not linked directly into LC startup with Mac Plus callback semantics.

## Not directly reusable for LC

Do not reuse as-is:

- Mac Plus ROM/RAM overlay bit semantics;
- Mac Plus VIA address decoder;
- pre-ADB keyboard shift-register protocol as the primary input model;
- Mac Plus mouse quadrature through SCC DCD/VIA bits;
- IWM/Sony assumptions and patched Mac Plus `.Sony` driver paths;
- current absence of real VIA timer behavior.

## LC-specific gaps

Need to identify and/or implement:

- LC VIA1/VIA2 or equivalent address ranges;
- LC interrupt routing and autovector/vector behavior;
- VBL timing source and level;
- 1-second/RTC tick source;
- PRAM/RTC register protocol sufficient for boot;
- ADB controller behavior and whether the LC ROM expects Egret/CUDA-like glue;
- SCC serial probe behavior sufficient to avoid boot hangs;
- sound/ASC minimum stubs.

## Observed LC ROM-entry I/O probes

The first bounded ROM-entry micro-probe starts at `0x0040008c` in the 24-bit ROM
window. It reaches the guest `RESET` instruction and then the early dispatcher.
With 20k requested cycles, the first repeated I/O-candidate accesses are:

| PC range | Address pattern | Current stub behavior | Notes |
|---:|---:|---|---|
| `0x00403124`-`0x4080314a` | `0x00f01c00`, `0x00f21c00`, `0x00f41c00` | provisional VIA IER set/clear/readback | explicit `early-rom-probe-1c00-stride` stub; offset `0x1c00` matches VIA register 14/IER under A[12:9] decode; this advances the previous constant-`0xff` loop |
| `0x00403226` onward | `0x00f01e00`, `0x00f00600`, `0x00f00400`, `0x00f00000` plus mirrors | `early-lc-via-register` plus base-window harness | newly exposed VIA-like register accesses after the IER behavior; likely ORA/DDRB/DDRA/ORB style offsets, still not claimed as final LC VIA mapping |
| `0x40845c0c` onward | around `0x00f14800` plus offsets `0x0000`, `0x0400`, `0x0804` from the `0x00f14000`-class base | `early-f14000-device` | physical NuBus/slot video-probe family by macemu/BasiliskII reference; reports only observed `+0x0804` ready/complete bits `0x03` so the unpatched LC ROM can advance past the probe |
| `0x40849eaa` onward, in monitor-reaching captures | around `0x00f04000`/`0x50f04000`, notably offsets `+0`, `+2`, and `+6` | `early-f04000-device` | SCC-like status/data block from ROM address-map words at `0x33f0`-`0x3400`; offsets `+0`/`+4` return `0x04`, offset `+2` returns one-shot `0x05` only after a guest transmit-data write to `+6` and otherwise `0x04`, and offset `+6` returns `0x00`; this lets monitor transmit-complete paths advance without faking receive input |
| `0x4084a672` onward | `0x00effffc`, `0x00dffffc`, ... down toward configured RAM and `0x00fffffc` top-of-space probes | `ram-size-probe` | high-memory sizing probe; writes ignored and reads return absent-memory value above configured 4MB RAM |

A comparison probe using `0x4080008c` showed that the 68EC020 path masks that
address to `0x0080008c`. The decoder now maps `0x00800000`-`0x0087ffff` as a
masked alias of the same 512KB ROM so the guest can continue after it moves PCs
into the `0x408xxxxx` ROM window. Alternate ROM-header probes showed that a bare
`0x00402e00` entry is invalid because it jumps through `a4=0x40400000` to the ROM
header/fingerprint bytes, raises an A-line exception while low vectors are still
zero, and falls into zero-filled RAM. Seeding `a6=0x004000b4`, matching the reset
trampoline continuation at `0x004000ac`, lets the same `0x00402e00` body return
through `0x004000b4` and set `vbr=0x40846140`. Local macemu references are useful
for classifying the next range: `BasiliskII/docs/AARCH64_JIT_BRINGUP.md` records
the same `0x50f00000 / 0x50f14000` family as a later ROM wait/copy routine, and
`BasiliskII/src/rom_patches.cpp` calls it a physical NuBus slot/video probe that
BasiliskII skips because it supplies video through `slot_rom.cpp` and EMUL_OP
video callbacks. In Cydintosh the unpatched LC ROM first spun with `+0x0804=0x00`;
reporting bit 1 (`0x02`) advanced through the inner byte-output loop but stopped
at the outer `0x40845e3a` wait, while reporting bits 0 and 1 (`0x03`) advanced to
the ROM monitor/diagnostic dispatcher around `0x40849eae` after 49.5M cycles
(`serial-capture-20260526-212157.log`). That monitor stop records
`d7=0x01000304` (bits 24, 9, 8, and 2 set) and first `0x00f04000` status read
value `0x04`. After narrowing the guard to actual no-input polling sites and
adding the SCC-like transmit-ready one-shot, `serial-capture-20260526-213538.log`
advances through the monitor initialization path and stops at `0x40849fca`, the
serial command/read poll. That capture records `d7=0x01020304`, `d0=0x00008000`
(no input), 27 reads and 24 writes to `early-f04000-device`, and the last status
read from `+0x0002` as `0x04`. This confirms the new frontier is not a missing
transmit-ready bit; the seeded reset-body path is explicitly waiting in the ROM
monitor for serial input. The earlier `0x008039xx`/`0x00803428`/`0x00807428`
write stream was an artifact of continuing through zero RAM, not a normal
reset-overlay write sequence.

## Recommended next steps

1. Keep LC hardware stubs under `src/machine_lc/`.
2. Identify why the seeded reset-body path selects the ROM monitor/diagnostic
   dispatcher after the NuBus/slot-video probe; focus on reset flags and real
   VIA/SCC status bits before considering fake serial input or bypasses.
3. Continue bounded ROM execution and use the LC address decoder/trace ring to
   record the next accesses into I/O candidate windows.
4. Stub only the first missing device range needed to advance boot, preserving the
   panic-on-unexpected-write policy until ranges are understood.
5. Add RTC/PRAM responses before ADB if traces show time/PRAM probes happen first;
   otherwise start with ADB command logging.

## Current conclusion

The current Mac Plus VIA/SCC code is useful as a reference for style and small
register mechanics, but it is **not** an LC hardware implementation. LC should get
its own I/O boundary and only copy small generic pieces after the ROM's actual
access patterns are observed.
