#include "lc_rom.h"

#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_rom";

uint32_t lc_read_be32(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

esp_err_t lc_rom_probe(lc_rom_info_t *info) {
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    info->expected_size = LC_ROM_SIZE;

    const esp_partition_t *rom = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, LC_ROM_PARTITION_SUBTYPE, LC_ROM_PARTITION_LABEL);
    if (rom == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    info->partition = rom;
    info->partition_offset = rom->address;
    info->partition_size = rom->size;
    info->size_ok = rom->size >= LC_ROM_SIZE;

    esp_err_t err = esp_partition_read(rom, 0, info->first16, sizeof(info->first16));
    if (err != ESP_OK) {
        return err;
    }

    info->first_long = lc_read_be32(info->first16);
    info->first_long_ok = info->first_long == LC_ROM_EXPECTED_FIRST_LONG_VALUE;
    return ESP_OK;
}

esp_err_t lc_rom_map(lc_rom_map_t *map) {
    if (map == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(map, 0, sizeof(*map));

    esp_err_t err = lc_rom_probe(&map->info);
    if (err != ESP_OK) {
        return err;
    }
    if (!map->info.size_ok) {
        return ESP_ERR_INVALID_SIZE;
    }

    const void *mapped = NULL;
    err = esp_partition_mmap(map->info.partition, 0, LC_ROM_SIZE, ESP_PARTITION_MMAP_DATA,
                             &mapped, &map->handle);
    if (err != ESP_OK) {
        return err;
    }

    map->bytes = (const uint8_t *)mapped;
    map->size = LC_ROM_SIZE;
    map->mapped = true;
    return ESP_OK;
}

void lc_rom_unmap(lc_rom_map_t *map) {
    if (map == NULL || !map->mapped) {
        return;
    }
    esp_partition_munmap(map->handle);
    map->bytes = NULL;
    map->size = 0;
    map->mapped = false;
}

void lc_rom_log_info(const lc_rom_info_t *info) {
    if (info == NULL || info->partition == NULL) {
        ESP_LOGW(TAG, "LC ROM partition not found yet (expected data/0x40 label 'rom')");
        return;
    }

    ESP_LOGI(TAG, "LC ROM partition offset=0x%08" PRIx32 " size=0x%08" PRIx32,
             info->partition_offset, info->partition_size);
    ESP_LOGI(TAG,
             "LC ROM first16=%02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x %02x %02x %02x %02x",
             info->first16[0], info->first16[1], info->first16[2], info->first16[3],
             info->first16[4], info->first16[5], info->first16[6], info->first16[7],
             info->first16[8], info->first16[9], info->first16[10], info->first16[11],
             info->first16[12], info->first16[13], info->first16[14], info->first16[15]);
    ESP_LOGI(TAG, "LC ROM first_long=0x%08" PRIx32 " expected=0x%08x image_size_expected=0x%x",
             info->first_long, LC_ROM_EXPECTED_FIRST_LONG_VALUE, LC_ROM_SIZE);
    ESP_LOGI(TAG, "LC ROM validation: partition_size_ok=%s first_long_ok=%s",
             info->size_ok ? "yes" : "no", info->first_long_ok ? "yes" : "no");

    if (!info->size_ok || !info->first_long_ok) {
        ESP_LOGW(TAG, "LC ROM partition does not yet contain the expected local Macintosh LC ROM");
    }
}

void lc_rom_log_map_info(const lc_rom_map_t *map) {
    if (map == NULL || !map->mapped || map->bytes == NULL) {
        ESP_LOGW(TAG, "LC ROM is not mapped");
        return;
    }

    const uint32_t mapped_first_long = lc_read_be32(&map->bytes[0]);
    const uint32_t mapped_second_long = lc_read_be32(&map->bytes[4]);
    ESP_LOGI(TAG, "LC ROM mmap base=%p size=0x%zx handle=%u", (const void *)map->bytes,
             map->size, (unsigned)map->handle);
    ESP_LOGI(TAG, "LC ROM mmap validation: first_long=0x%08" PRIx32 " second_long=0x%08" PRIx32,
             mapped_first_long, mapped_second_long);
    if (mapped_first_long != LC_ROM_EXPECTED_FIRST_LONG_VALUE) {
        ESP_LOGW(TAG, "LC ROM mmap first long does not match expected local ROM metadata");
    }
}
