#ifndef MACHINE_LC_MEMORY_H
#define MACHINE_LC_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#ifndef LC_RAM_BASE_CANDIDATE
#define LC_RAM_BASE_CANDIDATE 0x00000000u
#endif

#ifndef LC_GUEST_RAM_FALLBACK_SIZE
#define LC_GUEST_RAM_FALLBACK_SIZE (2u * 1024u * 1024u)
#endif

#ifndef LC_ROM_WINDOW_24BIT_BASE_CANDIDATE
#define LC_ROM_WINDOW_24BIT_BASE_CANDIDATE 0x00400000u
#endif

#define LC_ROM_WINDOW_SIZE 0x00080000u
#define LC_VRAM_SIZE ((size_t)DISP_WIDTH * (size_t)DISP_HEIGHT * (size_t)LC_GUEST_COLOR_DEPTH_BITS / 8u)

void lc_memory_log_initial_map(void);
void lc_memory_probe_guest_ram_allocation(void);

#endif
