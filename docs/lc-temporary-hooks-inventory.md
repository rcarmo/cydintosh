# LC temporary hook inventory

This inventory tracks the diagnostic scaffolding currently concentrated in
`src/machine_lc/lc_memory.c` and `src/machine_lc/lc_musashi_bus.c`. The goal is
not to delete all of it immediately; the goal is to stop adding narrow shims and
retire them behind coherent LC subsystems.

Current validated comparison point: `logs/serial-capture-20260527-222958.log`
runs the full 100M-cycle ROM-entry micro-probe without the previous diagnostic
monitor stop or low-RAM illegal callback. It expires at `pc_after=0x4081ab90` in
the Resource Manager / INIT-loading path, with no `0x00007fba`, no
`pc=0xffffffff`, no `addr=0x01000000`, no `0xffffffd8`, no `0x001fdfec`, and no
diagnostic exception stack.

## Classification key

- **Keep** — useful long-term diagnostics or a stable hardware-test harness.
- **Replace** — keep only until a named subsystem owns the behavior.
- **Delete** — remove once the replacing subsystem is in place; do not build new
  work on top of it.

## `lc_memory.c`

| Hook / area | Current purpose | Class | Retirement target |
|---|---|---:|---|
| `post_reset_atrap_table_overrides`, `lc_memory_reset_post_reset_atrap_table`, `lc_memory_set_post_reset_atrap_handler` | Records plausible `SetTrapAddress` writes and feeds synthetic trap-table reads. | Replace | RAM-owned low-memory A-trap table cells, updated by the real trap setter path. |
| `lc_memory_apply_rom_diagnostic_overrides` / no-FPU byte shadow | Presents BasiliskII-style no-FPU capability during ROM FPU diagnostics without ROM patching. | Replace | Machine capability initialization: no-FPU state should be represented in low memory / HW config state before reset code reads it. |
| `lc_memory_should_read_synthetic_ram_test_list` and `lc_memory_read_synthetic_ram_test_list` | Provides Basilisk-like BootGlobs/RAM-test list and MemTop around top-of-RAM while avoiding RAM-fill corruption. | Replace | Low-memory/BootGlobs initializer plus explicit RAM-test preservation policy. |
| `lc_memory_should_read_post_reset_no_mmu_flag` | Forces the post-reset InitMMU/no-MMU path expected by the direct EC020 probe. | Replace | Proper reset/BootGlobs no-MMU flag in RAM and correct reset call frame. |
| `lc_memory_should_read_post_reset_dispatch_kind`, `second_pass_locals`, `record_table`, `finalizer_descriptor`, `finalizer_rom_shape`, `mode_probe` | Per-PC data and ROM-shape substitutions for the reset memory-layout/address-map builder. | Delete | Coherent RAM-owned low-memory/address-map descriptor tables. |
| `lc_memory_should_read_post_reset_swap_mmu_dispatch_nop` and `lc_memory_read_post_reset_swap_mmu_dispatch_nop` | Temporarily NOPs the generic A-line dispatcher JSR for `A05D` after bus-side stack repair. | Delete | Correct A-line exception frame and trap-dispatch unwinding. |
| `lc_memory_should_read_basilisk_dispatch_magic` / `$0DB0` magic | Fallback only; `$0DB0` is now seeded into RAM-owned post-reset core low memory before the observed path reads it. | Replace | Low-memory initialization modeled after BasiliskII `M68K_EMUL_OP_FIX_DISPATCH_MAGIC`. |
| `lc_memory_should_read_post_reset_lowmem_callback` / `$0DBC` callback | Fallback only; `$0DBC` is now seeded into RAM-owned post-reset core low memory before the observed callback read. | Delete | Real low-memory callback/vector ownership. |
| `lc_memory_should_read_post_reset_atrap_table` / `lc_memory_read_post_reset_atrap_table` | Still routes low A-trap table reads to a default ROM `moveq #0,d0; rts` handler; high-table reads now prefer plausible RAM handlers. | Replace | RAM-owned A-trap dispatch tables and correct ROM trap-manager initialization. |
| `lc_memory_seed_post_reset_resource_lowmem_ram` and `lc_memory_seed_post_reset_resource_map_ram` | Seeds `TopMapHndl`/`SysMap`, Resource Manager vectors/globals, and the RAM-backed minimal resource map. Per-PC resource-map byte reads have been removed. | Replace | Coherent minimal Resource Manager map backed by Memory Manager handles. Preserve the map structure as a prototype, not as per-PC reads. |
| `lc_memory_should_read_fake_gdevice_chain` and `lc_memory_should_read_fake_video_globals` | Seeds minimal graphics-device/video globals for early video-default probes. | Replace | LC built-in video declaration model plus guest framebuffer/CLUT globals. |
| Early I/O classifier and generic I/O stubs (`early-lc-via-register`, `early-f04000-device`, `early-f10000-device`, `early-f14000-device`, `early-f16000-device`) | Carries reset diagnostics through VIA/SCC/slot/shift-register probes with summarized read/write logs. | Replace | Named LC VIA, RTC/PRAM, SCC, ADB, video, SWIM/SCSI devices. Keep summary logging. |
| Reset VIA/SCC synthetic IRQ status helpers | Feeds expected interrupt timing/IFR patterns for reset subtests. | Replace | Real VIA/SCC interrupt routing and timer model. |
| ROM masked/24-bit shadow write handling | Prevents diagnostic reset overlay writes from corrupting executing ROM bytes while allowing observed shadow writes. | Replace | Explicit reset overlay/address-map model. |
| Unmapped access, I/O stub, and summary loggers | Diagnostics for finding the next missing range or bad pointer. | Keep | Keep, but reduce noise once real devices exist. |
| RAM/VRAM/display allocation probes and synthetic RAM-only CPU smoke program support | Hardware and harness validation outside LC boot. | Keep | Keep as regression tests for Tab5/PSRAM/Musashi integration. |

## `lc_musashi_bus.c`

| Hook / area | Current purpose | Class | Retirement target |
|---|---|---:|---|
| ROM watchpoints, D-register transition logs, exception/illegal/invalid execution stack logs | Observability for current boot frontier and regressions. | Keep | Keep as gated diagnostics. |
| `lc_musashi_bus_maybe_pulse_reset_scc_timer_irq` and `lc_musashi_bus_maybe_pulse_reset_via_irq` | Injects synthetic level-4/level-1 interrupts at exact ROM wait loops. | Replace | SCC/VIA timer and interrupt-controller model. |
| `lc_musashi_bus_maybe_stub_get_video_default`, SlotManager/Control/DisposePtr/SwapMMU video-default stubs, fake video resource seeding | Skips early video-default traps and supplies tiny fake slot/video resource data. | Replace | Slot Manager/video declaration model, then real guest video globals. |
| `lc_musashi_bus_maybe_cap_post_reset_finalizer_loop` | Caps a DBRA count from provisional address-map descriptor state. | Delete | Real address-map descriptor tables. |
| `lc_musashi_bus_maybe_fix_post_reset_pack_empty_count` | Normalizes several bad pack/compress/finalizer values caused by synthetic descriptor state. | Delete | Real low-memory/address-map descriptor ownership. |
| `lc_musashi_bus_maybe_log_post_reset_resource_lookup` | Logs Resource Manager register state around map/type/ref scans. | Keep | Keep while Resource Manager is being replaced; later gate behind trace verbosity. |
| `lc_musashi_bus_maybe_cap_post_reset_resource_copy_loop` | Caps a Resource Manager DBRA count when map/ref-list state is malformed. | Delete | RAM-backed Resource Manager map with valid lengths and reference lists. |
| `lc_musashi_bus_maybe_log_post_reset_atrap_dispatch` and trap-name decoder | Logs observed A-traps through the ROM dispatcher. | Keep | Keep; expand decoder as needed. |
| `lc_musashi_bus_maybe_apply_post_reset_memory_trap` plus tiny handle allocator | Implements minimal NewPtr/NewHandle/HLock/SetHandleSize/GetHandleSize/StripAddress effects; heap is now kept below the top-of-RAM stack after the `0x00007fba` stack-collision diagnosis. | Replace | Coherent minimal Memory Manager service with real handle table and movable blocks. |
| `lc_musashi_bus_maybe_apply_post_reset_set_trap_address` | Applies plausible `SetTrapAddress` calls into synthetic trap-table overrides. | Replace | Real trap-table cells in RAM plus correct Trap Manager behavior. |
| `lc_musashi_bus_maybe_apply_post_reset_block_move` | Host-side bounded BlockMove for plausible RAM destinations/counts. | Replace | Real A-trap dispatch to Memory Manager/BlockMove semantics; keep overlap-safe copy logic as useful implementation detail. |
| `lc_musashi_bus_maybe_stub_post_reset_swap_mmu_dispatch` | NOPs `A05D` dispatcher JSR and seeds a return slot to avoid EC020 frame fall-through. | Delete | Correct 68020 A-line exception frame and trap return behavior. |
| `lc_musashi_bus_maybe_log_post_reset_high_trap_dispatch_entry` | Logs high A-trap dispatcher table index, stack, and registers. | Keep | Keep until high/low A-trap tables are RAM-owned and stable. |
| `lc_musashi_bus_maybe_fix_post_reset_high_trap_dispatch_return` | Rewrites an invalid high-trap RTS target to a ROM success helper. | Delete | Correct high A-trap table entries and trap-return stack shape. |
| `lc_musashi_bus_maybe_stub_post_reset_no_mmu_a001` | Skips an early `A001` reset-continuation trap. | Delete | Correct low A-trap table and no-MMU reset state. |
| `lc_musashi_bus_maybe_seed_post_reset_no_mmu_return` | Seeds a missing reset caller return slot for direct reset-body entry. | Replace | Real reset overlay/vector entry and call-frame construction. |
| `lc_musashi_bus_maybe_seed_post_reset_probe_tables` | Seeds `$0DB8`/`$0DD8` post-reset probe dispatch globals that the direct-entry path has not initialized naturally. | Replace | RAM-owned low-memory probe/dispatch table initialized by the normal ROM startup path or coherent low-memory subsystem. |
| `lc_musashi_bus_seed_post_reset_srt_table`, `lc_musashi_bus_maybe_seed_post_reset_srt_register`, SRT scan diagnostics, allocator entry/RTS diagnostics, and SRT I/O-fill guard | Seeds a RAM-owned synthetic empty Slot Manager sResource table at `$9800` when the direct host probe skips the ROM SRT builder; diagnostics show the current frontier still loops through the `0x40806984` scan with `A1` restored from the trap stack despite `$0D24/$0CBC` now being sane. The allocator entry/RTS trace shows A9C9 leaves `A0=0x40806d7c`, then the RTS stack has recursive `0x40806966` above `0x40806d7c/0x408060b2`. The bus-side I/O-fill guard catches the allocator loop when `A0` aliases the LC I/O aperture (`A0_24=0x00ffffff`), the memory-side SRT I/O write guard suppresses the follow-on writes attributed to `0x40806980/0x40806984`, and a one-shot outer-continuation escape removes the remaining false VRAM writes; together they reduce the bogus VRAM writes from 2,097,152 to 0. Repeating the escape or returning to the register-continuation stack shape regressed into diagnostic/zero-RAM paths, so the current accepted escape is one-shot only. Experiments that forced A9C9, empty lookup resolution, normal `$0DB8` table allocation, direct RTS chain repair, local `0x9a22` epilogue restore, or allocator A0 repair all regressed and were removed. | Replace | Real Slot Manager/SRT builder plus correct A9C9/callback/trap stack semantics, so `$0D24`, `$0CBC`, allocator saved `A0`, RTS targets, scan `A1`, and SRT table bounds are ROM-built rather than host-seeded/guarded. |
| `lc_musashi_bus_maybe_fix_post_reset_handoff_state` | Restores smashed top-of-RAM save registers before handoff. | Delete | Low-memory/stack layout that survives RAM-fill and descriptor generation naturally. |
| `lc_musashi_bus_maybe_log_post_reset_ram_execution` and invalid execution logger | Captures copied-table execution and bad PCs. | Keep | Keep as regression checks for `0x001fdfec`, `0xffffffff`, and low-RAM execution. |
| Synthetic 68k smoke program writer | Verifies Musashi callback bus execution independent of ROM boot. | Keep | Keep as a CPU/bus regression test. |

## Replacement order

1. **Low memory / VBR / trap tables first.** This retires `$0DBC`, the A-trap table synthetic reads, `A001`, `A05D`, and high-trap return repair.
2. **Minimal Memory Manager second.** This replaces tiny post-reset handle records and host-side memory-trap patches.
3. **Resource Manager third.** Move the current map prototype entirely into RAM-owned handles and remove per-PC resource-map reads/copy caps.
4. **Address-map/finalizer cleanup.** Replace descriptor and pack/compress normalizers with real descriptor tables.
5. **Hardware devices.** Replace reset VIA/SCC IRQ pulses and generic I/O stubs with named LC devices.
6. **Video/ADB/disk integration.** Retire video-default traps once Slot Manager/video globals exist; add boot media and ADB mouse after ROM reaches device probing.

## Regression checks while retiring hooks

Every A/B capture against `logs/serial-capture-20260527-222958.log` should check for:

- no `pc=0xffffffff`
- no `addr=0x01000000`
- no `0xffffffd8`
- no `0x001fdfec`
- no old negative Resource Manager copy counts (`0x00ffffxx` outside ordinary address constants)
- no return of `opcode=0x7562 ppc=0x00007fba pc=0x00007fbc`
- whether the new `pc_after=0x4081ab90` Resource Manager / INIT-loading loop moves toward boot-device probing
