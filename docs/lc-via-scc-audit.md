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

## Recommended next steps

1. Keep LC hardware stubs under `src/machine_lc/`.
2. During first ROM execution, use the LC address decoder and trace ring to record
   the first accesses into I/O candidate windows.
3. Add an LC I/O decoder table from observed accesses before implementing devices.
4. Stub only the first missing device range needed to advance boot, preserving the
   panic-on-unexpected-write policy until ranges are understood.
5. Add RTC/PRAM responses before ADB if traces show time/PRAM probes happen first;
   otherwise start with ADB command logging.

## Current conclusion

The current Mac Plus VIA/SCC code is useful as a reference for style and small
register mechanics, but it is **not** an LC hardware implementation. LC should get
its own I/O boundary and only copy small generic pieces after the ROM's actual
access patterns are observed.
