#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#ifndef LC_ROM_EXPECTED_SIZE
#define LC_ROM_EXPECTED_SIZE 0x80000u
#endif
#ifndef LC_ROM_EXPECTED_FIRST_LONG
#define LC_ROM_EXPECTED_FIRST_LONG 0x350EACF0u
#endif

static const char *TAG = "tab5_lc";

static uint32_t read_be32(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void log_chip_info(void) {
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Cydintosh LC color skeleton starting");
    ESP_LOGI(TAG, "machine=Macintosh LC experimental, target=M5Stack Tab5 ESP32-P4 only");
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
    const esp_partition_t *rom = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "rom");
    if (rom == NULL) {
        ESP_LOGW(TAG, "LC ROM partition not found yet (expected data/0x40 label 'rom')");
        return;
    }

    ESP_LOGI(TAG, "LC ROM partition offset=0x%08" PRIx32 " size=0x%08" PRIx32,
             rom->address, rom->size);

    uint8_t first[16] = {0};
    esp_err_t err = esp_partition_read(rom, 0, first, sizeof(first));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read LC ROM partition header: %s", esp_err_to_name(err));
        return;
    }

    uint32_t first_long = read_be32(first);
    ESP_LOGI(TAG, "LC ROM first16=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
             first[0], first[1], first[2], first[3], first[4], first[5], first[6], first[7],
             first[8], first[9], first[10], first[11], first[12], first[13], first[14], first[15]);
    ESP_LOGI(TAG, "LC ROM first_long=0x%08" PRIx32 " expected=0x%08x size_expected=0x%x",
             first_long, LC_ROM_EXPECTED_FIRST_LONG, LC_ROM_EXPECTED_SIZE);

    if (rom->size < LC_ROM_EXPECTED_SIZE || first_long != LC_ROM_EXPECTED_FIRST_LONG) {
        ESP_LOGW(TAG, "LC ROM partition does not yet contain the expected local Macintosh LC ROM");
    }
}

void app_main(void) {
    log_chip_info();
    log_lc_rom_partition();
    ESP_LOGI(TAG, "Milestone 0 skeleton is alive; display/touch and LC emulation are not enabled yet");
}
