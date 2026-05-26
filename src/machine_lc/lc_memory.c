#include "lc_memory.h"

#include "board_profiles.h"
#include "lc_trace.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_memory";

static bool address_in_window(uint32_t address, uint32_t base, uint32_t size) {
    return address >= base && address < (base + size);
}

const char *lc_memory_region_name(lc_addr_region_t region) {
    switch (region) {
    case LC_ADDR_REGION_RAM:
        return "ram";
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
        return "rom-24bit-candidate";
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        return "rom-32bit-candidate";
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
        return "io-24bit-candidate";
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return "io-32bit-candidate";
    case LC_ADDR_REGION_UNMAPPED:
    default:
        return "unmapped";
    }
}

lc_addr_decode_t lc_memory_decode_address(uint32_t address) {
    lc_addr_decode_t decoded = {
        .region = LC_ADDR_REGION_UNMAPPED,
        .base = 0,
        .size = 0,
        .offset = address,
        .name = lc_memory_region_name(LC_ADDR_REGION_UNMAPPED),
        .writable = false,
    };

    if (address < LC_GUEST_RAM_SIZE) {
        decoded.region = LC_ADDR_REGION_RAM;
        decoded.base = 0;
        decoded.size = LC_GUEST_RAM_SIZE;
        decoded.offset = address;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_24BIT_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_24BIT_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_32BIT_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_32BIT_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        return decoded;
    }

    if (address_in_window(address, LC_IO_24BIT_BASE_CANDIDATE, LC_IO_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_IO_24BIT_CANDIDATE;
        decoded.base = LC_IO_24BIT_BASE_CANDIDATE;
        decoded.size = LC_IO_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address_in_window(address, LC_IO_32BIT_BASE_CANDIDATE, LC_IO_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_IO_32BIT_CANDIDATE;
        decoded.base = LC_IO_32BIT_BASE_CANDIDATE;
        decoded.size = LC_IO_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    return decoded;
}

bool lc_memory_write_is_expected(const lc_addr_decode_t *decoded) {
    if (decoded == NULL) {
        return false;
    }
    switch (decoded->region) {
    case LC_ADDR_REGION_RAM:
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return decoded->writable;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
    case LC_ADDR_REGION_UNMAPPED:
    default:
        return false;
    }
}

bool lc_memory_should_panic_on_write(uint32_t address) {
#if LC_PANIC_ON_UNEXPECTED_WRITE
    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    return !lc_memory_write_is_expected(&decoded);
#else
    (void)address;
    return false;
#endif
}

void lc_memory_log_unmapped_access(uint32_t pc, uint32_t address, unsigned size, bool write) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 32;

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    if (decoded.region != LC_ADDR_REGION_UNMAPPED) {
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, decoded.offset, (uint16_t)size,
                        write);
        ESP_LOGI(TAG, "LC access pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32 " region=%s offset=0x%08" PRIx32,
                 pc, write ? "write" : "read", size, address, decoded.name, decoded.offset);
        return;
    }

    lc_trace_record(LC_TRACE_EVENT_UNMAPPED_ACCESS, pc, address, 0, (uint16_t)size, write);
    if (logged < log_limit) {
        ESP_LOGW(TAG, "LC unmapped access pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32 "%s",
                 pc, write ? "write" : "read", size, address,
                 (write && lc_memory_should_panic_on_write(address)) ? " panic_policy=would-panic" : "");
        logged++;
        if (logged == log_limit) {
            ESP_LOGW(TAG, "LC unmapped access logger reached %u entries; suppressing further unmapped logs", log_limit);
        }
    } else {
        suppressed++;
        if ((suppressed & 0xffu) == 0) {
            ESP_LOGW(TAG, "LC unmapped access logger suppressed %u additional entries", suppressed);
        }
    }
}

static void log_heap_caps(const char *label, uint32_t caps) {
    ESP_LOGI(TAG, "%s free=%u largest=%u", label,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}

void lc_memory_log_write_policy(void) {
    ESP_LOGI(TAG, "LC write policy: panic_on_unexpected_write=%d allowed=RAM,I/O-candidate denied=ROM,unmapped",
             LC_PANIC_ON_UNEXPECTED_WRITE);
    const uint32_t examples[] = {
        0x00000000u,
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_IO_24BIT_BASE_CANDIDATE,
        0x00E00000u,
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        lc_addr_decode_t decoded = lc_memory_decode_address(examples[i]);
        ESP_LOGI(TAG, "LC write policy example addr=0x%08" PRIx32 " region=%s expected=%s would_panic=%s",
                 examples[i], decoded.name,
                 lc_memory_write_is_expected(&decoded) ? "yes" : "no",
                 lc_memory_should_panic_on_write(examples[i]) ? "yes" : "no");
    }
}

void lc_memory_log_initial_map(void) {
    const uint32_t ram_base = LC_RAM_BASE_CANDIDATE;
    const uint32_t ram_size = LC_GUEST_RAM_SIZE;
    const uint32_t ram_end = ram_base + ram_size - 1u;
    const uint32_t rom24_base = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
    const uint32_t rom24_end = rom24_base + LC_ROM_WINDOW_SIZE - 1u;
    const uint32_t rom32_base = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
    const uint32_t rom32_end = rom32_base + LC_ROM_WINDOW_SIZE - 1u;
    const uint32_t io24_base = LC_IO_24BIT_BASE_CANDIDATE;
    const uint32_t io24_end = io24_base + LC_IO_WINDOW_SIZE - 1u;
    const uint32_t io32_base = LC_IO_32BIT_BASE_CANDIDATE;
    const uint32_t io32_end = io32_base + LC_IO_WINDOW_SIZE - 1u;

    ESP_LOGI(TAG, "LC initial memory scaffold: address_mode=24-bit-first, 32-bit candidates logged only");
    ESP_LOGI(TAG, "LC RAM candidate: base=0x%08" PRIx32 " size=0x%08" PRIx32 " end=0x%08" PRIx32,
             ram_base, ram_size, ram_end);
    ESP_LOGI(TAG, "LC ROM 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom24_base, LC_ROM_WINDOW_SIZE, rom24_end);
    ESP_LOGI(TAG, "LC ROM 32-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom32_base, LC_ROM_WINDOW_SIZE, rom32_end);
    ESP_LOGI(TAG, "LC I/O 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             io24_base, LC_IO_WINDOW_SIZE, io24_end);
    ESP_LOGI(TAG, "LC I/O 32-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             io32_base, LC_IO_WINDOW_SIZE, io32_end);
    ESP_LOGW(TAG, "LC ROM and I/O address windows are provisional; reset-vector execution must verify them");
    ESP_LOGI(TAG, "LC VRAM/framebuffer target: %dx%d@%dbpp size=0x%zx placement=undecided",
             DISP_WIDTH, DISP_HEIGHT, LC_GUEST_COLOR_DEPTH_BITS, LC_VRAM_SIZE);
}

void lc_memory_log_decoder_examples(void) {
    const uint32_t examples[] = {
        0x00000000u,
        LC_GUEST_RAM_SIZE - 1u,
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
        LC_IO_24BIT_BASE_CANDIDATE,
        LC_IO_32BIT_BASE_CANDIDATE,
        0x00E00000u,
    };

    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        lc_addr_decode_t decoded = lc_memory_decode_address(examples[i]);
        ESP_LOGI(TAG, "LC decode example addr=0x%08" PRIx32 " region=%s base=0x%08" PRIx32 " offset=0x%08" PRIx32 " writable=%s",
                 examples[i], decoded.name, decoded.base, decoded.offset,
                 decoded.writable ? "yes" : "no");
    }
}

void lc_memory_probe_guest_ram_allocation(void) {
    log_heap_caps("heap internal before LC RAM probe", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_caps("heap psram before LC RAM probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    size_t requested = LC_GUEST_RAM_SIZE;
    uint8_t *ram = (uint8_t *)heap_caps_malloc(requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ram == NULL) {
        ESP_LOGW(TAG, "LC guest RAM allocation failed for %zu bytes; trying fallback %u bytes",
                 requested, (unsigned)LC_GUEST_RAM_FALLBACK_SIZE);
        requested = LC_GUEST_RAM_FALLBACK_SIZE;
        ram = (uint8_t *)heap_caps_malloc(requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (ram == NULL) {
        ESP_LOGE(TAG, "LC guest RAM allocation probe failed for both primary and fallback sizes");
        log_heap_caps("heap psram after failed LC RAM probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return;
    }

    // Touch both ends so the diagnostic catches obvious allocation/backing issues.
    ram[0] = 0;
    ram[requested - 1u] = 0;
    ESP_LOGI(TAG, "LC guest RAM allocation probe succeeded: ptr=%p size=%zu", (void *)ram,
             requested);
    heap_caps_free(ram);
    log_heap_caps("heap psram after LC RAM probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lc_memory_probe_display_buffer_allocation(void) {
    ESP_LOGI(TAG, "LC display buffer targets: indexed_vram=%zu rgb565_full=%zu rgb565_strip_lines=%u rgb565_strip=%zu",
             LC_VRAM_SIZE, LC_RGB565_FRAMEBUFFER_SIZE, (unsigned)LC_DISPLAY_DMA_STRIP_LINES,
             LC_RGB565_DMA_STRIP_SIZE);
    log_heap_caps("heap dma/internal before display probe", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_caps("heap psram before display probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    uint8_t *vram = (uint8_t *)heap_caps_malloc(LC_VRAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (vram == NULL) {
        ESP_LOGW(TAG, "LC indexed VRAM PSRAM allocation probe failed: size=%zu", LC_VRAM_SIZE);
    } else {
        vram[0] = 0;
        vram[LC_VRAM_SIZE - 1u] = 0;
        ESP_LOGI(TAG, "LC indexed VRAM PSRAM allocation probe succeeded: ptr=%p size=%zu",
                 (void *)vram, LC_VRAM_SIZE);
        heap_caps_free(vram);
    }

    uint8_t *strip = (uint8_t *)heap_caps_malloc(
        LC_RGB565_DMA_STRIP_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (strip == NULL) {
        ESP_LOGW(TAG, "LC RGB565 DMA strip allocation probe failed: size=%zu", LC_RGB565_DMA_STRIP_SIZE);
    } else {
        strip[0] = 0;
        strip[LC_RGB565_DMA_STRIP_SIZE - 1u] = 0;
        ESP_LOGI(TAG, "LC RGB565 DMA strip allocation probe succeeded: ptr=%p size=%zu",
                 (void *)strip, LC_RGB565_DMA_STRIP_SIZE);
        heap_caps_free(strip);
    }

    ESP_LOGI(TAG, "LC full RGB565 framebuffer is diagnostic-only for now; dirty-row/strip rendering remains preferred");
    log_heap_caps("heap dma/internal after display probe", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_caps("heap psram after display probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static uint8_t lc_memory_io_stub_read8(uint32_t address, uint32_t offset) {
    (void)offset;
    lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, 0, address, 0xffu, 1, false);
    return 0xffu;
}

static esp_err_t lc_memory_io_stub_write8(uint32_t address, uint32_t offset, uint8_t value) {
    (void)offset;
    lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, 0, address, value, 1, true);
    return ESP_OK;
}

esp_err_t lc_memory_bus_init(lc_memory_bus_t *bus, const lc_rom_map_t *rom_map) {
    if (bus == NULL || rom_map == NULL || !rom_map->mapped || rom_map->bytes == NULL ||
        rom_map->size < LC_ROM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(bus, 0, sizeof(*bus));

    size_t requested = LC_GUEST_RAM_SIZE;
    bus->ram = (uint8_t *)heap_caps_calloc(1, requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bus->ram == NULL) {
        ESP_LOGW(TAG, "LC memory bus RAM allocation failed for %zu bytes; trying fallback %u bytes",
                 requested, (unsigned)LC_GUEST_RAM_FALLBACK_SIZE);
        requested = LC_GUEST_RAM_FALLBACK_SIZE;
        bus->ram = (uint8_t *)heap_caps_calloc(1, requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bus->using_fallback_ram = true;
    }
    if (bus->ram == NULL) {
        ESP_LOGE(TAG, "LC memory bus RAM allocation failed");
        return ESP_ERR_NO_MEM;
    }

    bus->ram_size = requested;
    bus->rom = rom_map->bytes;
    bus->rom_size = rom_map->size;
    bus->initialized = true;
    ESP_LOGI(TAG, "LC memory bus initialized: ram=%p size=0x%zx%s rom=%p size=0x%zx",
             (void *)bus->ram, bus->ram_size, bus->using_fallback_ram ? " fallback" : "",
             (const void *)bus->rom, bus->rom_size);
    return ESP_OK;
}

void lc_memory_bus_free(lc_memory_bus_t *bus) {
    if (bus == NULL) {
        return;
    }
    if (bus->ram != NULL) {
        heap_caps_free(bus->ram);
    }
    memset(bus, 0, sizeof(*bus));
}

uint8_t lc_memory_bus_read8(lc_memory_bus_t *bus, uint32_t address) {
    if (bus == NULL || !bus->initialized) {
        lc_memory_log_unmapped_access(0, address, 1, false);
        return 0xffu;
    }

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    switch (decoded.region) {
    case LC_ADDR_REGION_RAM:
        if (decoded.offset < bus->ram_size) {
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, 0, address, bus->ram[decoded.offset], 1,
                            false);
            return bus->ram[decoded.offset];
        }
        break;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        if (decoded.offset < bus->rom_size) {
            const uint8_t value = bus->rom[decoded.offset];
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, 0, address, value, 1, false);
            return value;
        }
        break;
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return lc_memory_io_stub_read8(address, decoded.offset);
    case LC_ADDR_REGION_UNMAPPED:
    default:
        break;
    }

    lc_memory_log_unmapped_access(0, address, 1, false);
    return 0xffu;
}

uint16_t lc_memory_bus_read16(lc_memory_bus_t *bus, uint32_t address) {
    const uint16_t hi = lc_memory_bus_read8(bus, address);
    const uint16_t lo = lc_memory_bus_read8(bus, address + 1u);
    return (uint16_t)((hi << 8) | lo);
}

uint32_t lc_memory_bus_read32(lc_memory_bus_t *bus, uint32_t address) {
    const uint32_t hi = lc_memory_bus_read16(bus, address);
    const uint32_t lo = lc_memory_bus_read16(bus, address + 2u);
    return (hi << 16) | lo;
}

esp_err_t lc_memory_bus_write8(lc_memory_bus_t *bus, uint32_t address, uint8_t value) {
    if (bus == NULL || !bus->initialized) {
        lc_memory_log_unmapped_access(0, address, 1, true);
        return ESP_ERR_INVALID_STATE;
    }

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    switch (decoded.region) {
    case LC_ADDR_REGION_RAM:
        if (decoded.offset < bus->ram_size) {
            bus->ram[decoded.offset] = value;
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, 0, address, value, 1, true);
            return ESP_OK;
        }
        break;
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return lc_memory_io_stub_write8(address, decoded.offset, value);
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, 0, address, value, 1, true);
        ESP_LOGW(TAG, "LC blocked ROM write addr=0x%08" PRIx32 " offset=0x%08" PRIx32
                      " value=0x%02x",
                 address, decoded.offset, value);
        return ESP_ERR_INVALID_STATE;
    case LC_ADDR_REGION_UNMAPPED:
    default:
        break;
    }

    lc_memory_log_unmapped_access(0, address, 1, true);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t lc_memory_bus_write16(lc_memory_bus_t *bus, uint32_t address, uint16_t value) {
    esp_err_t err = lc_memory_bus_write8(bus, address, (uint8_t)(value >> 8));
    if (err != ESP_OK) {
        return err;
    }
    return lc_memory_bus_write8(bus, address + 1u, (uint8_t)value);
}

esp_err_t lc_memory_bus_write32(lc_memory_bus_t *bus, uint32_t address, uint32_t value) {
    esp_err_t err = lc_memory_bus_write16(bus, address, (uint16_t)(value >> 16));
    if (err != ESP_OK) {
        return err;
    }
    return lc_memory_bus_write16(bus, address + 2u, (uint16_t)value);
}

void lc_memory_probe_bus_harness(const lc_rom_map_t *rom_map) {
    lc_memory_bus_t bus = {0};
    esp_err_t err = lc_memory_bus_init(&bus, rom_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LC memory bus harness init failed: %s", esp_err_to_name(err));
        return;
    }

    const uint32_t ram_addr = 0x00000000u;
    const uint32_t ram_tail_addr = (uint32_t)bus.ram_size - 4u;
    const uint32_t rom24_addr = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
    const uint32_t rom32_addr = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
    const uint32_t io_addr = LC_IO_24BIT_BASE_CANDIDATE;
    const uint32_t unmapped_addr = 0x00e00000u;

    const esp_err_t ram_write = lc_memory_bus_write32(&bus, ram_addr, 0x12345678u);
    const uint32_t ram_read = lc_memory_bus_read32(&bus, ram_addr);
    const esp_err_t ram_tail_write = lc_memory_bus_write32(&bus, ram_tail_addr, 0xa5a55a5au);
    const uint32_t ram_tail_read = lc_memory_bus_read32(&bus, ram_tail_addr);
    const uint32_t rom24_first = lc_memory_bus_read32(&bus, rom24_addr);
    const uint32_t rom24_second = lc_memory_bus_read32(&bus, rom24_addr + 4u);
    const uint32_t rom32_first = lc_memory_bus_read32(&bus, rom32_addr);
    const uint8_t io_read = lc_memory_bus_read8(&bus, io_addr);
    const esp_err_t io_write = lc_memory_bus_write8(&bus, io_addr, 0x5au);
    const esp_err_t rom_write = lc_memory_bus_write8(&bus, rom24_addr, 0x00u);
    const uint8_t unmapped_read = lc_memory_bus_read8(&bus, unmapped_addr);

    ESP_LOGI(TAG,
             "LC memory bus harness: ram_write=%s ram_read=0x%08" PRIx32
             " tail_write=%s tail_read=0x%08" PRIx32,
             esp_err_to_name(ram_write), ram_read, esp_err_to_name(ram_tail_write),
             ram_tail_read);
    ESP_LOGI(TAG,
             "LC memory bus harness: rom24[0]=0x%08" PRIx32 " rom24[4]=0x%08" PRIx32
             " rom32[0]=0x%08" PRIx32,
             rom24_first, rom24_second, rom32_first);
    ESP_LOGI(TAG,
             "LC memory bus harness: io_read=0x%02x io_write=%s rom_write_blocked=%s unmapped_read=0x%02x",
             io_read, esp_err_to_name(io_write), esp_err_to_name(rom_write), unmapped_read);

    lc_memory_bus_free(&bus);
    ESP_LOGI(TAG, "LC memory bus harness complete");
}
