#include "lc_memory.h"

#include "board_profiles.h"

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

void lc_memory_log_unmapped_access(uint32_t pc, uint32_t address, unsigned size, bool write) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 32;

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    if (decoded.region != LC_ADDR_REGION_UNMAPPED) {
        ESP_LOGI(TAG, "LC access pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32 " region=%s offset=0x%08" PRIx32,
                 pc, write ? "write" : "read", size, address, decoded.name, decoded.offset);
        return;
    }

    if (logged < log_limit) {
        ESP_LOGW(TAG, "LC unmapped access pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32,
                 pc, write ? "write" : "read", size, address);
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
