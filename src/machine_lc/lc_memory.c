#include "lc_memory.h"

#include "board_profiles.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_memory";

static void log_heap_caps(const char *label, uint32_t caps) {
    ESP_LOGI(TAG, "%s free=%u largest=%u", label,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}

void lc_memory_log_initial_map(void) {
    const uint32_t ram_base = LC_RAM_BASE_CANDIDATE;
    const uint32_t ram_size = LC_GUEST_RAM_SIZE;
    const uint32_t ram_end = ram_base + ram_size - 1u;
    const uint32_t rom_base = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
    const uint32_t rom_end = rom_base + LC_ROM_WINDOW_SIZE - 1u;

    ESP_LOGI(TAG, "LC initial memory scaffold: address_mode=24-bit-first");
    ESP_LOGI(TAG, "LC RAM candidate: base=0x%08" PRIx32 " size=0x%08" PRIx32 " end=0x%08" PRIx32,
             ram_base, ram_size, ram_end);
    ESP_LOGI(TAG, "LC ROM 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom_base, LC_ROM_WINDOW_SIZE, rom_end);
    ESP_LOGW(TAG, "LC ROM address window is a provisional 24-bit candidate; reset-vector execution must verify it");
    ESP_LOGI(TAG, "LC VRAM/framebuffer target: %dx%d@%dbpp size=0x%zx placement=undecided",
             DISP_WIDTH, DISP_HEIGHT, LC_GUEST_COLOR_DEPTH_BITS, LC_VRAM_SIZE);
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
