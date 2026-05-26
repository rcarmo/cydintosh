#include "board_profiles.h"
#include "cyd_machine.h"
#include "machine_lc/lc_cpu.h"
#include "machine_lc/lc_memory.h"
#include "machine_lc/lc_rom.h"
#include "machine_lc/lc_trace.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"

#include <inttypes.h>

static const char *TAG = "tab5_lc";

static void log_chip_info(void) {
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Cydintosh LC color skeleton starting");
    ESP_LOGI(TAG, "machine=%s experimental, target=M5Stack Tab5 ESP32-P4 only", CYD_MACHINE_NAME);
    ESP_LOGI(TAG, "chip model=%d cores=%d revision=%d features=0x%08" PRIx32,
             chip.model, chip.cores, chip.revision, (uint32_t)chip.features);
    ESP_LOGI(TAG, "flash size=%" PRIu32 " bytes", flash_size);

#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "psram size=%zu bytes", esp_psram_get_size());
#else
    ESP_LOGW(TAG, "PSRAM support is not enabled in this skeleton build");
#endif

    ESP_LOGI(TAG, "heap internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "heap 8-bit free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void log_lc_rom_partition(void) {
    lc_rom_info_t info = {0};
    esp_err_t err = lc_rom_probe(&info);
    if (err == ESP_ERR_NOT_FOUND) {
        lc_rom_log_info(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to probe LC ROM partition: %s", esp_err_to_name(err));
        return;
    }
    lc_rom_log_info(&info);
    lc_cpu_log_reset_vector_candidates(&info);

    lc_rom_map_t map = {0};
    err = lc_rom_map(&map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap LC ROM partition: %s", esp_err_to_name(err));
        return;
    }
    lc_rom_log_map_info(&map);
    lc_rom_unmap(&map);
}

void app_main(void) {
    lc_trace_reset();
    lc_trace_record_marker(0x4c433030u); // 'LC00': skeleton start
    log_chip_info();
    ESP_LOGI(TAG, "boot diagnostics: machine=%s rom_expected_size=0x%x cpu_mode=68EC020-scaffold guest_ram=%d framebuffer=%dx%d@%dbpp",
             CYD_MACHINE_NAME, CYD_ROM_EXPECTED_SIZE, LC_GUEST_RAM_SIZE, DISP_WIDTH, DISP_HEIGHT,
             LC_GUEST_COLOR_DEPTH_BITS);
    lc_cpu_log_config();
    lc_memory_log_initial_map();
    lc_memory_log_decoder_examples();
    lc_memory_probe_guest_ram_allocation();
    lc_memory_probe_display_buffer_allocation();
    log_lc_rom_partition();
    lc_trace_record_marker(0x4c43304fu); // 'LC0O': skeleton diagnostics complete
    lc_trace_dump_recent(16);
    ESP_LOGI(TAG, "Milestone 0 skeleton is alive; display/touch and LC emulation are not enabled yet");
}
