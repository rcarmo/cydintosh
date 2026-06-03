#ifndef HOST_ESP_PARTITION_H
#define HOST_ESP_PARTITION_H

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_PARTITION_TYPE_APP = 0x00,
    ESP_PARTITION_TYPE_DATA = 0x01,
} esp_partition_type_t;

typedef uint8_t esp_partition_subtype_t;

#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS ((esp_partition_subtype_t)0x82)
#define ESP_PARTITION_MMAP_DATA 0

typedef struct {
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address;
    uint32_t size;
    char label[17];
} esp_partition_t;

typedef uintptr_t esp_partition_mmap_handle_t;

void host_esp_partition_set_rom_path(const char *path);
void host_esp_partition_set_disk_path(const char *path);

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset,
                             void *dst, size_t size);
esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset, size_t size,
                             int memory, const void **out_ptr,
                             esp_partition_mmap_handle_t *out_handle);
void esp_partition_munmap(esp_partition_mmap_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif
