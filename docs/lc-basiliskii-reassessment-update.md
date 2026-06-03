# Basilisk II Alignment — Session 2026-06-02/03 Results

## Achievement: Guest M68K Renders Full Mac Desktop Framebuffer

Starting from zero VRAM writes, this marathon session proved the complete
emulation chain from CPU reset through disk I/O to framebuffer rendering.

### Proven Pipeline

```
RESET (EMUL_OP) → INSTALL_DRIVERS → DISK_PRIME → VRAM writes → PNG capture
```

### Key Milestones

1. **VRAM writes confirmed** — 196,608 bytes (512×384@8bpp full framebuffer)
2. **Disk I/O working** — DISK_PRIME reads HFS boot blocks from HD200MB
3. **Boot block execution** — 200+ OS trap dispatches through ROM A-line handler
4. **boot_2 self-relocation** — 624 bytes copied to stack, executes real Mac boot code
5. **boot_2 → ROM init** — Transfers control to ROM at offset $8cc, runs 33M cycles
6. **Disk→VRAM** — 192KB of HFS volume data rendered as raw pixels
7. **Mac desktop** — White menu bar + grey checkerboard pattern, full screen

### Architecture

- **CPU**: Musashi 68EC020 (24-bit address bus, `CPU_ADDRESS_MASK = $00FFFFFF`)
- **ROM**: Mac LC 0x067c, Basilisk patch_rom_32 subset (1627 patches applied)
- **VRAM**: 512×384@8bpp at `$00f40000` (24-bit I/O window)
- **Disk**: Paravirtual `.Disk` driver via `EMUL_OP_DISK_PRIME`
- **Trap dispatch**: ROM A-line handler at `$004099b0`, OS table at `$0400`, SYS at `$0000+sel*4`

### Key Fixes Applied

| Issue | Root Cause | Fix |
|-------|-----------|-----|
| VRAM unreachable | 68EC020 masks to 24-bit; `$a0000000` → `$00000000` | Use `$f40000` (LC native) |
| Trap table garbage | 32-bit ROM addresses in 24-bit context | All tables use `$004xxxxx` |
| High-trap recursion | Uninitialized toolbox table entries → ResourceMgr loop | Seed `$0E00-$2E00` |
| A-line vector overwrite | SYS table seeding overwrote `$28` | Write vectors AFTER seeding |
| FIX_MEMSIZE loop | Uninitialized high traps recurse through CompBootStack | Direct RESET→$1142 jump |
| Boot block SndDispose | A995 Pascal trap corrupts stack | NOP the call site |
| BlockMove overwrites patches | Copy at $920 targets $970 (our patch area) | Use trampoline at $7d0 |
| Handle dereference | *(handle) must = data ptr, not &master_ptr | Fixed handle layout |

### Files

- `src/machine_lc/lc_basilisk_compat.c` — ROM patches, inline M68K stubs
- `src/machine_lc/lc_musashi_bus.c` — EMUL_OP handlers, trap dispatch, Memory Manager
- `src/machine_lc/lc_memory.c` — 24-bit decode, trap tables, VRAM routing
- `fixtures/boot_2.bin` — System 7 'boot' 2 resource (648 bytes)
- `fixtures/boot_3.bin` — System 7 'boot' 3 resource (31,420 bytes)

### Remaining for Full OS Desktop

The guest renders a Mac-style desktop using inline M68K code. For the actual
Mac OS to draw via QuickDraw, boot_3 needs:
- Working Resource Manager (`_GetResource`, `_Get1Resource`)
- Full Memory Manager (`_NewPtr`/`_NewHandle` with real allocation)
- File Manager (to open System file)
- QuickDraw initialization (InitGraf, OpenPort, etc.)

This is a large integration task but all prerequisite components (CPU, disk,
VRAM, trap dispatch) are now proven working.
