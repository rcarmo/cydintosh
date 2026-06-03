#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static bool host_time_initialized;
static struct timespec host_time_zero;
static char host_rom_path[1024] = "vendor/mac-lc.rom";
static char host_disk_path[1024] = "vendor/lc-disk.img";
static esp_partition_t host_rom_partition;
static esp_partition_t host_disk_partition;

typedef struct {
    void *ptr;
    size_t size;
} host_mmap_entry_t;

#define HOST_MMAP_ENTRY_COUNT 16
static host_mmap_entry_t host_mmap_entries[HOST_MMAP_ENTRY_COUNT];

static int64_t host_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!host_time_initialized) {
        host_time_zero = now;
        host_time_initialized = true;
    }
    const int64_t sec = (int64_t)(now.tv_sec - host_time_zero.tv_sec);
    const int64_t nsec = (int64_t)(now.tv_nsec - host_time_zero.tv_nsec);
    return sec * 1000000ll + nsec / 1000ll;
}

const char *esp_err_to_name(esp_err_t err) {
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_ALLOWED:
        return "ESP_ERR_NOT_ALLOWED";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

uint32_t esp_log_timestamp(void) {
    return (uint32_t)(host_now_us() / 1000ll);
}

int64_t esp_timer_get_time(void) {
    return host_now_us();
}

void host_esp_log_write(const char *level, const char *tag, const char *fmt, ...) {
    fprintf(stdout, "%s (%u) %s: ", level, esp_log_timestamp(), tag != NULL ? tag : "host");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

void heap_caps_free(void *ptr) {
    free(ptr);
}

size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return (size_t)512u * 1024u * 1024u;
}

size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return (size_t)256u * 1024u * 1024u;
}

void host_esp_partition_set_rom_path(const char *path) {
    if (path != NULL && path[0] != '\0') {
        snprintf(host_rom_path, sizeof(host_rom_path), "%s", path);
    }
}

void host_esp_partition_set_disk_path(const char *path) {
    if (path != NULL && path[0] != '\0') {
        snprintf(host_disk_path, sizeof(host_disk_path), "%s", path);
    }
}

static bool host_file_size(const char *path, uint32_t *size_out) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) {
        return false;
    }
    if (size_out != NULL) {
        *size_out = (uint32_t)st.st_size;
    }
    return true;
}

static void host_fill_partition(esp_partition_t *partition, esp_partition_type_t type,
                                esp_partition_subtype_t subtype, const char *label,
                                uint32_t address, uint32_t size) {
    memset(partition, 0, sizeof(*partition));
    partition->type = type;
    partition->subtype = subtype;
    partition->address = address;
    partition->size = size;
    snprintf(partition->label, sizeof(partition->label), "%s", label);
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype,
                                                const char *label) {
    if (type != ESP_PARTITION_TYPE_DATA || label == NULL) {
        return NULL;
    }

    uint32_t size = 0;
    if (strcmp(label, "rom") == 0 && host_file_size(host_rom_path, &size)) {
        host_fill_partition(&host_rom_partition, type, subtype, label, 0x00410000u, size);
        return &host_rom_partition;
    }
    if (strcmp(label, "disk") == 0 && host_file_size(host_disk_path, &size)) {
        host_fill_partition(&host_disk_partition, type, subtype, label, 0x00430000u, size);
        return &host_disk_partition;
    }
    return NULL;
}

static const char *host_partition_path(const esp_partition_t *partition) {
    if (partition == &host_rom_partition || strcmp(partition->label, "rom") == 0) {
        return host_rom_path;
    }
    if (partition == &host_disk_partition || strcmp(partition->label, "disk") == 0) {
        return host_disk_path;
    }
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset,
                             void *dst, size_t size) {
    if (partition == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *path = host_partition_path(partition);
    if (path == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, (long)src_offset, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    const size_t got = fread(dst, 1, size, fp);
    fclose(fp);
    return got == size ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset, size_t size,
                             int memory, const void **out_ptr,
                             esp_partition_mmap_handle_t *out_handle) {
    (void)memory;
    if (partition == NULL || out_ptr == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    void *buf = malloc(size);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_partition_read(partition, offset, buf, size);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    for (size_t i = 0; i < HOST_MMAP_ENTRY_COUNT; i++) {
        if (host_mmap_entries[i].ptr == NULL) {
            host_mmap_entries[i].ptr = buf;
            host_mmap_entries[i].size = size;
            *out_ptr = buf;
            *out_handle = (esp_partition_mmap_handle_t)(i + 1u);
            return ESP_OK;
        }
    }
    free(buf);
    return ESP_ERR_NO_MEM;
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle) {
    if (handle == 0 || handle > HOST_MMAP_ENTRY_COUNT) {
        return;
    }
    host_mmap_entry_t *entry = &host_mmap_entries[handle - 1u];
    free(entry->ptr);
    entry->ptr = NULL;
    entry->size = 0;
}

// Load boot 2 and boot 3 resources from fixture files into guest RAM.
void host_load_boot_resources(uint8_t *ram, size_t ram_size) {
    if (ram == NULL || ram_size < 0x910000u) return;
    const char *paths[] = {"fixtures/boot_2.bin", "fixtures/boot_3.bin"};
    const uint32_t addrs[] = {0x900000u, 0x902000u};
    const uint32_t handles[] = {0x4ff00u, 0x4ff08u};
    const uint32_t sizes[] = {648, 31420};
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) { fprintf(stderr, "WARN: cannot open %s\n", paths[i]); continue; }
        size_t n = fread(&ram[addrs[i]], 1, sizes[i], f);
        fclose(f);
        // Set up Mac handle: handle_addr points to master_ptr, master_ptr points to data
        // handle[0] = pointer to master pointer
        // master_ptr = address of data
        uint32_t master_ptr_addr = handles[i] + 4u;
        uint32_t data_addr = addrs[i];
        // Write data pointer directly at handle address (Mac handle convention:
        // handle = address containing the master pointer = data address)
        ram[handles[i] + 0] = (data_addr >> 24) & 0xff;
        ram[handles[i] + 1] = (data_addr >> 16) & 0xff;
        ram[handles[i] + 2] = (data_addr >> 8) & 0xff;
        ram[handles[i] + 3] = data_addr & 0xff;
        fprintf(stderr, "HOST: loaded %s (%zu bytes) to RAM $%05x handle=$%05x\n",
                paths[i], n, addrs[i], handles[i]);
    }
}

// System resource fork loaded into guest RAM at this base address:
#define SYSRSRC_RAM_BASE 0x00A00000u
static uint32_t sysrsrc_size = 0;

void host_load_system_rsrc(uint8_t *ram, size_t ram_size) {
    if (ram == NULL || ram_size < SYSRSRC_RAM_BASE + 0x600000u) return;
    FILE *f = fopen("fixtures/system_rsrc/System.rsrc", "rb");
    if (!f) { fprintf(stderr, "WARN: cannot open System.rsrc\n"); return; }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0x600000u) sz = 0x600000u; // cap at 6MB
    size_t n = fread(&ram[SYSRSRC_RAM_BASE], 1, sz, f);
    fclose(f);
    sysrsrc_size = (uint32_t)n;
    fprintf(stderr, "HOST: loaded System.rsrc (%zu bytes) at RAM $%06X\n",
            n, SYSRSRC_RAM_BASE);
}

// Find a resource in the loaded System.rsrc by type and ID.
// Returns the guest RAM address of the resource data (after length prefix),
// and sets *out_size to the data length. Returns 0 if not found.
uint32_t host_find_system_resource(const uint8_t *ram, size_t ram_size,
                                   uint32_t res_type, int16_t res_id,
                                   uint32_t *out_size) {
    if (sysrsrc_size == 0 || ram == NULL) return 0;
    const uint8_t *rsrc = &ram[SYSRSRC_RAM_BASE];
    uint32_t data_offset = (uint32_t)rsrc[0]<<24 | rsrc[1]<<16 | rsrc[2]<<8 | rsrc[3];
    uint32_t map_offset = (uint32_t)rsrc[4]<<24 | rsrc[5]<<16 | rsrc[6]<<8 | rsrc[7];
    if (map_offset + 30 > sysrsrc_size) return 0;
    
    const uint8_t *map = rsrc + map_offset;
    uint16_t type_list_off = (uint16_t)(map[24]<<8 | map[25]);
    const uint8_t *tlist = map + type_list_off;
    int16_t num_types = (int16_t)(tlist[0]<<8 | tlist[1]) + 1;
    
    for (int i = 0; i < num_types; i++) {
        const uint8_t *te = tlist + 2 + i * 8;
        uint32_t t = (uint32_t)te[0]<<24 | te[1]<<16 | te[2]<<8 | te[3];
        if (t != res_type) continue;
        uint16_t count = (uint16_t)(te[4]<<8 | te[5]) + 1;
        uint16_t ref_off = (uint16_t)(te[6]<<8 | te[7]);
        const uint8_t *refs = tlist + ref_off;
        for (int j = 0; j < count; j++) {
            const uint8_t *re = refs + j * 12;
            int16_t rid = (int16_t)(re[0]<<8 | re[1]);
            if (rid != res_id) continue;
            // Found! Get data offset (3 bytes at re[5..7])
            uint32_t doff = (uint32_t)re[5]<<16 | re[6]<<8 | re[7];
            uint32_t abs_off = data_offset + doff;
            if (abs_off + 4 > sysrsrc_size) return 0;
            uint32_t dlen = (uint32_t)rsrc[abs_off]<<24 | rsrc[abs_off+1]<<16 |
                            rsrc[abs_off+2]<<8 | rsrc[abs_off+3];
            if (out_size) *out_size = dlen;
            // Return guest RAM address of the data (after length prefix)
            return SYSRSRC_RAM_BASE + abs_off + 4;
        }
        return 0; // type found but ID not found
    }
    return 0; // type not found
}
