#ifndef MACHINE_LC_ROM_H
#define MACHINE_LC_ROM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#define LC_ROM_PARTITION_SUBTYPE ((esp_partition_subtype_t)0x40)
#define LC_ROM_PARTITION_LABEL "rom"
#define LC_ROM_SIZE 0x80000u
#define LC_ROM_EXPECTED_FIRST_LONG_VALUE 0x350EACF0u

typedef struct {
    const esp_partition_t *partition;
    uint32_t partition_offset;
    uint32_t partition_size;
    uint32_t expected_size;
    uint32_t first_long;
    uint8_t first16[16];
    bool size_ok;
    bool first_long_ok;
} lc_rom_info_t;

typedef struct {
    lc_rom_info_t info;
    const uint8_t *bytes;
    size_t size;
    esp_partition_mmap_handle_t handle;
    bool mapped;
} lc_rom_map_t;

uint32_t lc_read_be32(const uint8_t bytes[4]);
esp_err_t lc_rom_probe(lc_rom_info_t *info);
esp_err_t lc_rom_map(lc_rom_map_t *map);
void lc_rom_unmap(lc_rom_map_t *map);
void lc_rom_log_info(const lc_rom_info_t *info);
void lc_rom_log_map_info(const lc_rom_map_t *map);

#endif
