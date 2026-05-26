#include "lc_memory.h"

#include "board_profiles.h"
#include "lc_musashi_bus.h"
#include "lc_trace.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_memory";

#define LC_IO_STUB_KIND_COUNT 7u
#define LC_EARLY_VIA_REGISTER_COUNT 16u
#define LC_EARLY_F04000_REGISTER_COUNT 256u
#define LC_EARLY_F14000_REGISTER_COUNT 4096u
#define LC_EARLY_F14000_POLL_STATUS_OFFSET 0x0804u
#define LC_EARLY_F14000_POLL_READY_BITS 0x03u
#define LC_IO_OFFSET_STATS_SLOTS 24u
#define LC_RAM_SIZE_TOP_PROBE_BYTES 0x10u
#define LC_EARLY_VIA_IER_REGISTER 14u
#define LC_EARLY_VIA_IFR_REGISTER 13u
#define LC_EARLY_VIA_ORA_NO_HANDSHAKE_REGISTER 15u
#define LC_EARLY_VIA_ORA_EXTERNAL_LOW_MASK 0x01u

typedef struct {
    uint32_t reads;
    uint32_t writes;
    uint32_t first_pc;
    uint32_t first_address;
    uint32_t last_pc;
    uint32_t last_address;
    uint8_t last_value;
    bool seen;
} lc_io_stub_stats_t;

typedef struct {
    uint32_t reads;
    uint32_t writes;
    uint32_t first_pc;
    uint32_t last_pc;
    uint16_t offset;
    uint8_t last_value;
    bool seen;
} lc_io_offset_stats_t;

static lc_io_stub_stats_t io_stub_stats[LC_IO_STUB_KIND_COUNT];
static lc_io_offset_stats_t early_f04000_offset_stats[LC_IO_OFFSET_STATS_SLOTS];
static lc_io_offset_stats_t early_f14000_offset_stats[LC_IO_OFFSET_STATS_SLOTS];
static uint8_t early_probe_via_ier;
static uint8_t early_lc_via_registers[LC_EARLY_VIA_REGISTER_COUNT];
static uint8_t early_f04000_registers[LC_EARLY_F04000_REGISTER_COUNT];
static uint8_t early_f14000_registers[LC_EARLY_F14000_REGISTER_COUNT];
static bool early_f04000_tx_ready_once;
static uint32_t masked_rom_shadow_writes;
static uint32_t masked_rom_shadow_first_pc;
static uint32_t masked_rom_shadow_first_addr;
static uint32_t masked_rom_shadow_last_pc;
static uint32_t masked_rom_shadow_last_addr;
static uint8_t masked_rom_shadow_last_value;

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
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        return "rom-32bit-masked-candidate";
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
        return "ram-size-probe";
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
        return "io-24bit-candidate";
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return "io-32bit-candidate";
    case LC_ADDR_REGION_UNMAPPED:
    default:
        return "unmapped";
    }
}

const char *lc_memory_io_stub_kind_name(lc_io_stub_kind_t kind) {
    switch (kind) {
    case LC_IO_STUB_NONE:
        return "none";
    case LC_IO_STUB_WINDOW_BASE_HARNESS:
        return "io-window-base/harness";
    case LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE:
        return "early-rom-probe-1c00-stride";
    case LC_IO_STUB_EARLY_LC_VIA_REGISTER:
        return "early-lc-via-register";
    case LC_IO_STUB_EARLY_F04000_DEVICE:
        return "early-f04000-device";
    case LC_IO_STUB_EARLY_F14000_DEVICE:
        return "early-f14000-device";
    case LC_IO_STUB_GENERIC:
    default:
        return "generic-io-stub";
    }
}

static bool lc_memory_is_early_via_register_offset(uint32_t offset) {
    if ((offset & 0x000001ffu) != 0) {
        return false;
    }
    const uint32_t slot_base = offset & ~0x00001fffu;
    return slot_base == 0 || slot_base == 0x00002000u || (slot_base & 0x0001ffffu) == 0;
}

static unsigned lc_memory_via_register_index(uint32_t offset) {
    return (unsigned)((offset >> 9u) & 0x0fu);
}

static lc_io_stub_kind_t lc_memory_classify_io_stub(uint32_t offset) {
    if (offset >= 0x00004000u && offset < 0x00006000u) {
        return LC_IO_STUB_EARLY_F04000_DEVICE;
    }
    if (offset >= 0x00014000u && offset < 0x00016000u) {
        return LC_IO_STUB_EARLY_F14000_DEVICE;
    }
    if ((offset & 0x0001ffffu) == 0x00001c00u) {
        return LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE;
    }
    if (offset == 0) {
        return LC_IO_STUB_WINDOW_BASE_HARNESS;
    }
    if (lc_memory_is_early_via_register_offset(offset)) {
        return LC_IO_STUB_EARLY_LC_VIA_REGISTER;
    }
    return LC_IO_STUB_GENERIC;
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

    if (address_in_window(address, LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_32BIT_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = LC_ENABLE_ROM_MASKED_SHADOW != 0;
        return decoded;
    }

    if (address >= LC_GUEST_RAM_SIZE && address < LC_RAM_SIZE_PROBE_LIMIT) {
        decoded.region = LC_ADDR_REGION_RAM_SIZE_PROBE;
        decoded.base = LC_GUEST_RAM_SIZE;
        decoded.size = LC_RAM_SIZE_PROBE_LIMIT - LC_GUEST_RAM_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address >= LC_IO_24BIT_BASE_CANDIDATE + LC_IO_WINDOW_SIZE - LC_RAM_SIZE_TOP_PROBE_BYTES &&
        address < LC_IO_24BIT_BASE_CANDIDATE + LC_IO_WINDOW_SIZE) {
        decoded.region = LC_ADDR_REGION_RAM_SIZE_PROBE;
        decoded.base = LC_IO_24BIT_BASE_CANDIDATE + LC_IO_WINDOW_SIZE - LC_RAM_SIZE_TOP_PROBE_BYTES;
        decoded.size = LC_RAM_SIZE_TOP_PROBE_BYTES;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address_in_window(address, LC_IO_24BIT_BASE_CANDIDATE, LC_IO_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_IO_24BIT_CANDIDATE;
        decoded.base = LC_IO_24BIT_BASE_CANDIDATE;
        decoded.size = LC_IO_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        decoded.io_stub = lc_memory_classify_io_stub(decoded.offset);
        decoded.io_stub_name = lc_memory_io_stub_kind_name(decoded.io_stub);
        return decoded;
    }

    if (address_in_window(address, LC_IO_32BIT_BASE_CANDIDATE, LC_IO_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_IO_32BIT_CANDIDATE;
        decoded.base = LC_IO_32BIT_BASE_CANDIDATE;
        decoded.size = LC_IO_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        decoded.io_stub = lc_memory_classify_io_stub(decoded.offset);
        decoded.io_stub_name = lc_memory_io_stub_kind_name(decoded.io_stub);
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
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
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
    ESP_LOGI(TAG, "LC write policy: panic_on_unexpected_write=%d masked_rom_shadow=%d allowed=RAM,I/O-candidate%s denied=ROM,unmapped",
             LC_PANIC_ON_UNEXPECTED_WRITE, LC_ENABLE_ROM_MASKED_SHADOW,
             LC_ENABLE_ROM_MASKED_SHADOW ? ",masked-ROM-alias-shadow" : "");
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
    const uint32_t rom32_reset_base = LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE;
    const uint32_t rom32_reset_end = rom32_reset_base + LC_ROM_WINDOW_SIZE - 1u;
    const uint32_t io24_base = LC_IO_24BIT_BASE_CANDIDATE;
    const uint32_t io24_end = io24_base + LC_IO_WINDOW_SIZE - 1u;
    const uint32_t io32_base = LC_IO_32BIT_BASE_CANDIDATE;
    const uint32_t io32_end = io32_base + LC_IO_WINDOW_SIZE - 1u;

    ESP_LOGI(TAG, "LC initial memory scaffold: address_mode=24-bit-first, 32-bit candidates logged only, masked_rom_shadow=%d",
             LC_ENABLE_ROM_MASKED_SHADOW);
    ESP_LOGI(TAG, "LC RAM candidate: base=0x%08" PRIx32 " size=0x%08" PRIx32 " end=0x%08" PRIx32,
             ram_base, ram_size, ram_end);
    ESP_LOGI(TAG, "LC ROM 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom24_base, LC_ROM_WINDOW_SIZE, rom24_end);
    ESP_LOGI(TAG, "LC ROM 32-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom32_base, LC_ROM_WINDOW_SIZE, rom32_end);
    ESP_LOGI(TAG, "LC ROM 32-bit reset/header candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom32_reset_base, LC_ROM_WINDOW_SIZE, rom32_reset_end);
    ESP_LOGI(TAG, "LC I/O 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             io24_base, LC_IO_WINDOW_SIZE, io24_end);
    ESP_LOGI(TAG, "LC I/O 32-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             io32_base, LC_IO_WINDOW_SIZE, io32_end);
    ESP_LOGW(TAG, "LC ROM and I/O address windows are provisional; reset-vector execution must verify them");
    ESP_LOGW(TAG, "LC masked ROM alias shadow is diagnostic-only reset-overlay scaffolding; it is not a boot claim");
    ESP_LOGI(TAG, "LC VRAM/framebuffer target: %dx%d@%dbpp size=0x%zx placement=undecided",
             DISP_WIDTH, DISP_HEIGHT, LC_GUEST_COLOR_DEPTH_BITS, LC_VRAM_SIZE);
}

void lc_memory_log_decoder_examples(void) {
    const uint32_t examples[] = {
        0x00000000u,
        LC_GUEST_RAM_SIZE - 1u,
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE,
        LC_GUEST_RAM_SIZE,
        LC_RAM_SIZE_PROBE_LIMIT - 4u,
        LC_IO_24BIT_BASE_CANDIDATE,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00001c00u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00021c00u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00041c00u,
        LC_IO_32BIT_BASE_CANDIDATE,
        0x00E00000u,
    };

    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        lc_addr_decode_t decoded = lc_memory_decode_address(examples[i]);
        ESP_LOGI(TAG,
                 "LC decode example addr=0x%08" PRIx32 " region=%s base=0x%08" PRIx32
                 " offset=0x%08" PRIx32 " writable=%s io_stub=%s",
                 examples[i], decoded.name, decoded.base, decoded.offset,
                 decoded.writable ? "yes" : "no", decoded.io_stub_name != NULL ? decoded.io_stub_name : "none");
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

static void lc_memory_update_io_stub_stats(lc_io_stub_kind_t kind, uint32_t pc,
                                           uint32_t address, uint8_t value, bool write) {
    if ((unsigned)kind >= LC_IO_STUB_KIND_COUNT) {
        kind = LC_IO_STUB_GENERIC;
    }
    lc_io_stub_stats_t *stats = &io_stub_stats[kind];
    if (!stats->seen) {
        stats->first_pc = pc;
        stats->first_address = address;
        stats->seen = true;
    }
    stats->last_pc = pc;
    stats->last_address = address;
    stats->last_value = value;
    if (write) {
        stats->writes++;
    } else {
        stats->reads++;
    }
}

static void lc_memory_update_offset_stats(lc_io_offset_stats_t *stats, uint16_t offset,
                                          uint32_t pc, uint8_t value, bool write) {
    lc_io_offset_stats_t *slot = NULL;
    for (unsigned i = 0; i < LC_IO_OFFSET_STATS_SLOTS; i++) {
        if (stats[i].seen && stats[i].offset == offset) {
            slot = &stats[i];
            break;
        }
        if (!stats[i].seen && slot == NULL) {
            slot = &stats[i];
        }
    }
    if (slot == NULL) {
        slot = &stats[LC_IO_OFFSET_STATS_SLOTS - 1u];
    }
    if (!slot->seen) {
        slot->seen = true;
        slot->offset = offset;
        slot->first_pc = pc;
    }
    slot->last_pc = pc;
    slot->last_value = value;
    if (write) {
        slot->writes++;
    } else {
        slot->reads++;
    }
}

static void lc_memory_log_offset_stats(const char *name, const lc_io_offset_stats_t *stats) {
    for (unsigned i = 0; i < LC_IO_OFFSET_STATS_SLOTS; i++) {
        if (!stats[i].seen) {
            continue;
        }
        ESP_LOGI(TAG,
                 "LC I/O offset summary: name=%s offset=0x%04x reads=%" PRIu32
                 " writes=%" PRIu32 " first_pc=0x%08" PRIx32 " last_pc=0x%08" PRIx32
                 " last_value=0x%02x",
                 name, stats[i].offset, stats[i].reads, stats[i].writes,
                 stats[i].first_pc, stats[i].last_pc, stats[i].last_value);
    }
}

static void lc_memory_log_io_stub_access(const lc_addr_decode_t *decoded, uint32_t pc,
                                         uint32_t address, uint8_t value, bool write) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 64;

    if (logged < log_limit) {
        ESP_LOGI(TAG,
                 "LC I/O stub %s pc=0x%08" PRIx32 " addr=0x%08" PRIx32
                 " offset=0x%08" PRIx32 " value=0x%02x name=%s",
                 write ? "write" : "read", pc, address, decoded->offset, value,
                 decoded->io_stub_name);
        logged++;
        if (logged == log_limit) {
            ESP_LOGI(TAG, "LC I/O stub logger reached %u entries; suppressing further I/O logs",
                     log_limit);
        }
    } else {
        suppressed++;
        if ((suppressed & 0xffu) == 0) {
            ESP_LOGI(TAG, "LC I/O stub logger suppressed %u additional entries", suppressed);
        }
    }
}

void lc_memory_log_io_stub_summary(void) {
    for (unsigned i = 0; i < LC_IO_STUB_KIND_COUNT; i++) {
        const lc_io_stub_stats_t *stats = &io_stub_stats[i];
        if (!stats->seen) {
            continue;
        }
        ESP_LOGI(TAG,
                 "LC I/O stub summary: name=%s reads=%" PRIu32 " writes=%" PRIu32
                 " first_pc=0x%08" PRIx32 " first_addr=0x%08" PRIx32
                 " last_pc=0x%08" PRIx32 " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                 lc_memory_io_stub_kind_name((lc_io_stub_kind_t)i), stats->reads, stats->writes,
                 stats->first_pc, stats->first_address, stats->last_pc, stats->last_address,
                 stats->last_value);
    }
    lc_memory_log_offset_stats("early-f04000-device", early_f04000_offset_stats);
    lc_memory_log_offset_stats("early-f14000-device", early_f14000_offset_stats);
    if (masked_rom_shadow_writes != 0) {
        ESP_LOGI(TAG,
                 "LC masked ROM shadow summary: writes=%" PRIu32
                 " first_pc=0x%08" PRIx32 " first_addr=0x%08" PRIx32
                 " last_pc=0x%08" PRIx32 " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                 masked_rom_shadow_writes, masked_rom_shadow_first_pc,
                 masked_rom_shadow_first_addr, masked_rom_shadow_last_pc,
                 masked_rom_shadow_last_addr, masked_rom_shadow_last_value);
    }
}

static void lc_memory_log_masked_rom_shadow_write(uint32_t pc, uint32_t address, uint8_t value) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 48;

    if (masked_rom_shadow_writes == 0) {
        masked_rom_shadow_first_pc = pc;
        masked_rom_shadow_first_addr = address;
    }
    masked_rom_shadow_writes++;
    masked_rom_shadow_last_pc = pc;
    masked_rom_shadow_last_addr = address;
    masked_rom_shadow_last_value = value;

    if (logged < log_limit) {
        ESP_LOGW(TAG,
                 "LC masked ROM shadow write pc=0x%08" PRIx32 " addr=0x%08" PRIx32
                 " value=0x%02x diagnostic_reset_overlay=yes",
                 pc, address, value);
        logged++;
        if (logged == log_limit) {
            ESP_LOGW(TAG,
                     "LC masked ROM shadow write logger reached %u entries; suppressing further shadow logs",
                     log_limit);
        }
    } else {
        suppressed++;
        if ((suppressed & 0xffu) == 0) {
            ESP_LOGW(TAG, "LC masked ROM shadow write logger suppressed %u additional entries",
                     suppressed);
        }
    }
}

static uint8_t lc_memory_io_stub_read8(const lc_addr_decode_t *decoded, uint32_t address) {
    const uint32_t pc = lc_musashi_bus_current_pc();
    uint8_t value = 0xffu;
    if (decoded->io_stub == LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE) {
        // Offset 0x1c00 is the 6522 VIA IER register when VIA registers are
        // decoded with A[12:9]. The early ROM probe writes 0xff, then clears
        // one enable bit at a time and expects reads to return bit7 set plus
        // the current enable mask. Treat the observed 0x00f?1c00 mirrors as
        // one provisional VIA-style IER alias until the real LC VIA windows are
        // fully decoded.
        value = (uint8_t)(0x80u | early_probe_via_ier);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_LC_VIA_REGISTER) {
        const unsigned reg = lc_memory_via_register_index(decoded->offset);
        if (reg == LC_EARLY_VIA_IER_REGISTER) {
            value = (uint8_t)(0x80u | early_probe_via_ier);
        } else if (reg == LC_EARLY_VIA_IFR_REGISTER) {
            value = 0;
        } else if (reg == LC_EARLY_VIA_ORA_NO_HANDSHAKE_REGISTER) {
            // Register 15 is the 6522 ORA/no-handshake alias under the
            // provisional A[12:9] decode. Reads reflect external pin state,
            // not just the output latch. The reset dispatcher clears bit 1
            // and then tests bit 0 at 0x50f01e00; leaving bit 0 latched high
            // from the earlier init table sends the ROM into its diagnostic
            // monitor path. Model that external bit as low until the real LC
            // VIA/ADB/PRAM lines are decoded.
            value = (uint8_t)(early_lc_via_registers[reg] &
                              (uint8_t)~LC_EARLY_VIA_ORA_EXTERNAL_LOW_MASK);
        } else {
            value = early_lc_via_registers[reg];
        }
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F04000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F04000_REGISTER_COUNT - 1u);
        if (reg_offset == 0u || reg_offset == 4u) {
            // Provisional SCC-like status aliases: transmitter empty, no
            // receive character available.  The ROM diagnostic monitor also
            // polls +2 after writing a byte to +6; that path gets a single
            // bit0-ready pulse below so transmit can complete without faking
            // serial input for the later command/read loop.
            value = 0x04u;
        } else if (reg_offset == 2u) {
            value = early_f04000_tx_ready_once ? 0x05u : 0x04u;
            early_f04000_tx_ready_once = false;
        } else if (reg_offset == 6u) {
            value = 0x00u;
        } else {
            value = early_f04000_registers[reg_offset];
        }
        lc_memory_update_offset_stats(early_f04000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, false);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F14000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F14000_REGISTER_COUNT - 1u);
        value = early_f14000_registers[reg_offset];
        if (reg_offset == LC_EARLY_F14000_POLL_STATUS_OFFSET) {
            // The seeded LC reset-body probe reaches a ROM routine that matches
            // BasiliskII/macemu's documented physical NuBus/slot video probe
            // family (`0x50f00000 / 0x50f14000`).  The routine writes bytes to
            // offsets +0x0000/+0x0400 and polls +0x0804 bit 1 in its inner
            // loop, then bit 0 in an outer wait before advancing.  Report only
            // those two ready/complete bits for now so the bounded probe can
            // discover the next real hardware dependency without patching ROM
            // code or faking serial input.
            value = (uint8_t)(value | LC_EARLY_F14000_POLL_READY_BITS);
        }
        lc_memory_update_offset_stats(early_f14000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, false);
    }
    lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
    lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, false);
    lc_memory_log_io_stub_access(decoded, pc, address, value, false);
    return value;
}

static void lc_memory_write_early_via_register(uint32_t offset, uint8_t value) {
    const unsigned reg = lc_memory_via_register_index(offset);
    if (reg == LC_EARLY_VIA_IER_REGISTER) {
        const uint8_t mask = (uint8_t)(value & 0x7fu);
        if ((value & 0x80u) != 0) {
            early_probe_via_ier = (uint8_t)(early_probe_via_ier | mask);
        } else {
            early_probe_via_ier = (uint8_t)(early_probe_via_ier & (uint8_t)~mask);
        }
        early_lc_via_registers[LC_EARLY_VIA_IER_REGISTER] = early_probe_via_ier;
        return;
    }
    if (reg == LC_EARLY_VIA_IFR_REGISTER) {
        early_lc_via_registers[LC_EARLY_VIA_IFR_REGISTER] &= (uint8_t)~value;
        return;
    }
    early_lc_via_registers[reg] = value;
}

static esp_err_t lc_memory_io_stub_write8(const lc_addr_decode_t *decoded, uint32_t address,
                                          uint8_t value) {
    const uint32_t pc = lc_musashi_bus_current_pc();
    if (decoded->io_stub == LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE ||
        decoded->io_stub == LC_IO_STUB_EARLY_LC_VIA_REGISTER) {
        lc_memory_write_early_via_register(decoded->offset, value);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F04000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F04000_REGISTER_COUNT - 1u);
        early_f04000_registers[reg_offset] = value;
        if (reg_offset == 6u) {
            early_f04000_tx_ready_once = true;
        }
        lc_memory_update_offset_stats(early_f04000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, true);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F14000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F14000_REGISTER_COUNT - 1u);
        early_f14000_registers[reg_offset] = value;
        lc_memory_update_offset_stats(early_f14000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, true);
    }
    lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, true);
    lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, true);
    lc_memory_log_io_stub_access(decoded, pc, address, value, true);
    return ESP_OK;
}

esp_err_t lc_memory_bus_init(lc_memory_bus_t *bus, const lc_rom_map_t *rom_map) {
    if (bus == NULL || rom_map == NULL || !rom_map->mapped || rom_map->bytes == NULL ||
        rom_map->size < LC_ROM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(bus, 0, sizeof(*bus));
    memset(io_stub_stats, 0, sizeof(io_stub_stats));
    memset(early_f04000_offset_stats, 0, sizeof(early_f04000_offset_stats));
    memset(early_f14000_offset_stats, 0, sizeof(early_f14000_offset_stats));
    memset(early_lc_via_registers, 0xff, sizeof(early_lc_via_registers));
    memset(early_f04000_registers, 0xff, sizeof(early_f04000_registers));
    memset(early_f14000_registers, 0xff, sizeof(early_f14000_registers));
    early_f04000_tx_ready_once = false;
    masked_rom_shadow_writes = 0;
    masked_rom_shadow_first_pc = 0;
    masked_rom_shadow_first_addr = 0;
    masked_rom_shadow_last_pc = 0;
    masked_rom_shadow_last_addr = 0;
    masked_rom_shadow_last_value = 0;
    early_lc_via_registers[LC_EARLY_VIA_IFR_REGISTER] = 0;
    early_lc_via_registers[LC_EARLY_VIA_IER_REGISTER] = 0;
    early_probe_via_ier = 0;

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
#if LC_ENABLE_ROM_MASKED_SHADOW
    bus->rom_masked_shadow_size = bus->rom_size < LC_ROM_WINDOW_SIZE ? bus->rom_size : LC_ROM_WINDOW_SIZE;
    bus->rom_masked_shadow = (uint8_t *)heap_caps_malloc(bus->rom_masked_shadow_size,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bus->rom_masked_shadow == NULL) {
        ESP_LOGW(TAG,
                 "LC masked ROM alias shadow allocation failed: size=0x%zx; masked alias remains read-only ROM",
                 bus->rom_masked_shadow_size);
        bus->rom_masked_shadow_size = 0;
    } else {
        memcpy(bus->rom_masked_shadow, bus->rom, bus->rom_masked_shadow_size);
        bus->rom_masked_shadow_enabled = true;
        ESP_LOGW(TAG,
                 "LC masked ROM alias shadow initialized: base=0x%08x size=0x%zx diagnostic_reset_overlay=yes",
                 LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE, bus->rom_masked_shadow_size);
    }
#endif
    bus->initialized = true;
    ESP_LOGI(TAG, "LC memory bus initialized: ram=%p size=0x%zx%s rom=%p size=0x%zx masked_shadow=%p size=0x%zx enabled=%d",
             (void *)bus->ram, bus->ram_size, bus->using_fallback_ram ? " fallback" : "",
             (const void *)bus->rom, bus->rom_size, (void *)bus->rom_masked_shadow,
             bus->rom_masked_shadow_size, bus->rom_masked_shadow_enabled);
    return ESP_OK;
}

void lc_memory_bus_free(lc_memory_bus_t *bus) {
    if (bus == NULL) {
        return;
    }
    if (bus->ram != NULL) {
        heap_caps_free(bus->ram);
    }
    if (bus->rom_masked_shadow != NULL) {
        heap_caps_free(bus->rom_masked_shadow);
    }
    memset(bus, 0, sizeof(*bus));
}

uint8_t lc_memory_bus_read8(lc_memory_bus_t *bus, uint32_t address) {
    if (bus == NULL || !bus->initialized) {
        lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, false);
        return 0xffu;
    }

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    switch (decoded.region) {
    case LC_ADDR_REGION_RAM:
        if (decoded.offset < bus->ram_size) {
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, bus->ram[decoded.offset], 1,
                            false);
            return bus->ram[decoded.offset];
        }
        break;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        if (decoded.offset < bus->rom_size) {
            const uint8_t value = bus->rom[decoded.offset];
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, false);
            return value;
        }
        break;
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        if (bus->rom_masked_shadow_enabled && decoded.offset < bus->rom_masked_shadow_size) {
            const uint8_t value = bus->rom_masked_shadow[decoded.offset];
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, false);
            return value;
        }
        if (decoded.offset < bus->rom_size) {
            const uint8_t value = bus->rom[decoded.offset];
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, false);
            return value;
        }
        break;
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, 0xffu, 1, false);
        return 0xffu;
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return lc_memory_io_stub_read8(&decoded, address);
    case LC_ADDR_REGION_UNMAPPED:
    default:
        break;
    }

    lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, false);
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
        lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, true);
        return ESP_ERR_INVALID_STATE;
    }

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    switch (decoded.region) {
    case LC_ADDR_REGION_RAM:
        if (decoded.offset < bus->ram_size) {
            bus->ram[decoded.offset] = value;
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, true);
            return ESP_OK;
        }
        break;
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, true);
        return ESP_OK;
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return lc_memory_io_stub_write8(&decoded, address, value);
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        if (bus->rom_masked_shadow_enabled && decoded.offset < bus->rom_masked_shadow_size) {
            bus->rom_masked_shadow[decoded.offset] = value;
            const uint32_t pc = lc_musashi_bus_current_pc();
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, true);
            lc_memory_log_masked_rom_shadow_write(pc, address, value);
            return ESP_OK;
        }
        lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, lc_musashi_bus_current_pc(), address, value, 1, true);
        ESP_LOGW(TAG, "LC blocked masked ROM alias write addr=0x%08" PRIx32
                      " offset=0x%08" PRIx32 " value=0x%02x shadow_enabled=%d",
                 address, decoded.offset, value, bus->rom_masked_shadow_enabled);
        return ESP_ERR_INVALID_STATE;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, lc_musashi_bus_current_pc(), address, value, 1, true);
        ESP_LOGW(TAG, "LC blocked ROM write addr=0x%08" PRIx32 " offset=0x%08" PRIx32
                      " value=0x%02x",
                 address, decoded.offset, value);
        return ESP_ERR_INVALID_STATE;
    case LC_ADDR_REGION_UNMAPPED:
    default:
        break;
    }

    lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, true);
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
