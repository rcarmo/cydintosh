#ifndef MACHINE_LC_MEMORY_H
#define MACHINE_LC_MEMORY_H

#include <stdbool.h>
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

#ifndef LC_ROM_WINDOW_32BIT_BASE_CANDIDATE
#define LC_ROM_WINDOW_32BIT_BASE_CANDIDATE 0x40800000u
#endif

#ifndef LC_IO_24BIT_BASE_CANDIDATE
#define LC_IO_24BIT_BASE_CANDIDATE 0x00F00000u
#endif

#ifndef LC_IO_32BIT_BASE_CANDIDATE
#define LC_IO_32BIT_BASE_CANDIDATE 0x50000000u
#endif

#define LC_ROM_WINDOW_SIZE 0x00080000u
#define LC_IO_WINDOW_SIZE 0x00100000u
#define LC_VRAM_SIZE ((size_t)DISP_WIDTH * (size_t)DISP_HEIGHT * (size_t)LC_GUEST_COLOR_DEPTH_BITS / 8u)

typedef enum {
    LC_ADDR_REGION_RAM,
    LC_ADDR_REGION_ROM_24BIT_CANDIDATE,
    LC_ADDR_REGION_ROM_32BIT_CANDIDATE,
    LC_ADDR_REGION_IO_24BIT_CANDIDATE,
    LC_ADDR_REGION_IO_32BIT_CANDIDATE,
    LC_ADDR_REGION_UNMAPPED,
} lc_addr_region_t;

typedef struct {
    lc_addr_region_t region;
    uint32_t base;
    uint32_t size;
    uint32_t offset;
    const char *name;
    bool writable;
} lc_addr_decode_t;

const char *lc_memory_region_name(lc_addr_region_t region);
lc_addr_decode_t lc_memory_decode_address(uint32_t address);
void lc_memory_log_unmapped_access(uint32_t pc, uint32_t address, unsigned size, bool write);
void lc_memory_log_initial_map(void);
void lc_memory_log_decoder_examples(void);
void lc_memory_probe_guest_ram_allocation(void);

#endif
