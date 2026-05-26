#ifndef MACHINE_LC_MEMORY_H
#define MACHINE_LC_MEMORY_H

#include "esp_err.h"
#include "lc_rom.h"

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

#ifndef LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE
#define LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE 0x40400000u
#endif

#ifndef LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE
#define LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE 0x00800000u
#endif

#ifndef LC_IO_24BIT_BASE_CANDIDATE
#define LC_IO_24BIT_BASE_CANDIDATE 0x00F00000u
#endif

#ifndef LC_IO_32BIT_BASE_CANDIDATE
#define LC_IO_32BIT_BASE_CANDIDATE 0x50000000u
#endif

#define LC_ROM_WINDOW_SIZE 0x00080000u
#ifndef LC_RAM_SIZE_PROBE_LIMIT
#define LC_RAM_SIZE_PROBE_LIMIT LC_IO_24BIT_BASE_CANDIDATE
#endif
#ifndef LC_DISPLAY_DMA_STRIP_LINES
#define LC_DISPLAY_DMA_STRIP_LINES 16u
#endif

#ifndef LC_PANIC_ON_UNEXPECTED_WRITE
#define LC_PANIC_ON_UNEXPECTED_WRITE 1
#endif

#ifndef LC_ENABLE_ROM_MASKED_SHADOW
#define LC_ENABLE_ROM_MASKED_SHADOW 1
#endif
#ifndef LC_ENABLE_SYNTHETIC_RAM_TEST_LIST
#define LC_ENABLE_SYNTHETIC_RAM_TEST_LIST 1
#endif
#ifndef LC_SYNTHETIC_RAM_TEST_LIST_BASE
#define LC_SYNTHETIC_RAM_TEST_LIST_BASE (LC_GUEST_RAM_SIZE - 0x1cu)
#endif

#define LC_IO_WINDOW_SIZE 0x00100000u
#define LC_VRAM_SIZE ((size_t)DISP_WIDTH * (size_t)DISP_HEIGHT * (size_t)LC_GUEST_COLOR_DEPTH_BITS / 8u)
#define LC_RGB565_FRAMEBUFFER_SIZE ((size_t)DISP_WIDTH * (size_t)DISP_HEIGHT * 2u)
#define LC_RGB565_DMA_STRIP_SIZE ((size_t)DISP_WIDTH * (size_t)LC_DISPLAY_DMA_STRIP_LINES * 2u)

typedef enum {
    LC_ADDR_REGION_RAM,
    LC_ADDR_REGION_ROM_24BIT_CANDIDATE,
    LC_ADDR_REGION_ROM_32BIT_CANDIDATE,
    LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE,
    LC_ADDR_REGION_RAM_SIZE_PROBE,
    LC_ADDR_REGION_IO_24BIT_CANDIDATE,
    LC_ADDR_REGION_IO_32BIT_CANDIDATE,
    LC_ADDR_REGION_UNMAPPED,
} lc_addr_region_t;

typedef enum {
    LC_IO_STUB_NONE,
    LC_IO_STUB_WINDOW_BASE_HARNESS,
    LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE,
    LC_IO_STUB_EARLY_LC_VIA_REGISTER,
    LC_IO_STUB_EARLY_F04000_DEVICE,
    LC_IO_STUB_EARLY_F14000_DEVICE,
    LC_IO_STUB_GENERIC,
} lc_io_stub_kind_t;

typedef struct {
    lc_addr_region_t region;
    uint32_t base;
    uint32_t size;
    uint32_t offset;
    const char *name;
    bool writable;
    lc_io_stub_kind_t io_stub;
    const char *io_stub_name;
} lc_addr_decode_t;

typedef struct {
    uint8_t *ram;
    size_t ram_size;
    bool using_fallback_ram;
    const uint8_t *rom;
    size_t rom_size;
    uint8_t *rom_masked_shadow;
    size_t rom_masked_shadow_size;
    bool rom_masked_shadow_enabled;
    bool initialized;
} lc_memory_bus_t;

const char *lc_memory_region_name(lc_addr_region_t region);
const char *lc_memory_io_stub_kind_name(lc_io_stub_kind_t kind);
lc_addr_decode_t lc_memory_decode_address(uint32_t address);
bool lc_memory_write_is_expected(const lc_addr_decode_t *decoded);
bool lc_memory_should_panic_on_write(uint32_t address);
void lc_memory_log_unmapped_access(uint32_t pc, uint32_t address, unsigned size, bool write);
void lc_memory_log_initial_map(void);
void lc_memory_log_write_policy(void);
void lc_memory_log_decoder_examples(void);
void lc_memory_log_io_stub_summary(void);
void lc_memory_probe_guest_ram_allocation(void);
void lc_memory_probe_display_buffer_allocation(void);
esp_err_t lc_memory_bus_init(lc_memory_bus_t *bus, const lc_rom_map_t *rom_map);
void lc_memory_bus_free(lc_memory_bus_t *bus);
uint8_t lc_memory_bus_read8(lc_memory_bus_t *bus, uint32_t address);
uint16_t lc_memory_bus_read16(lc_memory_bus_t *bus, uint32_t address);
uint32_t lc_memory_bus_read32(lc_memory_bus_t *bus, uint32_t address);
esp_err_t lc_memory_bus_write8(lc_memory_bus_t *bus, uint32_t address, uint8_t value);
esp_err_t lc_memory_bus_write16(lc_memory_bus_t *bus, uint32_t address, uint16_t value);
esp_err_t lc_memory_bus_write32(lc_memory_bus_t *bus, uint32_t address, uint32_t value);
void lc_memory_probe_bus_harness(const lc_rom_map_t *rom_map);

#endif
