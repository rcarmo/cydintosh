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

// System resource fork is kept host-side and individual resources are copied
// into a guest scratch arena on demand.  Loading the entire 5MB fork into guest
// RAM used to collide with the ROM-created heap near $00c00000 and made boot
// code execute raw resource data (e.g. nift/nitt records).
#define SYSRSRC_COPY_BASE 0x00520000u
#define SYSRSRC_COPY_LIMIT 0x00780000u
static uint8_t *sysrsrc_buf = NULL;
static uint32_t sysrsrc_size = 0;
static uint32_t sysrsrc_copy_cursor = SYSRSRC_COPY_BASE;

typedef struct {
    uint32_t parent_id;
    uint32_t cnid;
    uint8_t is_dir;
    uint8_t finder_flags_hi;
    uint8_t finder_flags_lo;
    uint32_t type;
    uint32_t creator;
    uint32_t data_len;
    uint32_t rsrc_len;
    uint8_t name_len;
    char name[32];
} host_hfs_cat_entry_t;

static host_hfs_cat_entry_t *host_hfs_entries;
static size_t host_hfs_entry_count;
static bool host_hfs_catalog_loaded;

static uint16_t host_be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t host_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static bool host_hfs_add_entry(const host_hfs_cat_entry_t *entry) {
    if (host_hfs_entry_count >= 2048u) return false;
    host_hfs_cat_entry_t *next = (host_hfs_cat_entry_t *)realloc(host_hfs_entries, (host_hfs_entry_count + 1u) * sizeof(*host_hfs_entries));
    if (next == NULL) return false;
    host_hfs_entries = next;
    host_hfs_entries[host_hfs_entry_count++] = *entry;
    return true;
}

static void host_hfs_parse_catalog_once(void) {
    if (host_hfs_catalog_loaded) return;
    host_hfs_catalog_loaded = true;
    uint32_t disk_size = 0;
    if (!host_file_size(host_disk_path, &disk_size) || disk_size < 4096u) return;
    uint8_t *disk = (uint8_t *)malloc(disk_size);
    if (disk == NULL) return;
    FILE *fp = fopen(host_disk_path, "rb");
    if (fp == NULL) { free(disk); return; }
    size_t got = fread(disk, 1, disk_size, fp);
    fclose(fp);
    if (got != disk_size) { free(disk); return; }

    const uint8_t *mdb = disk + 1024u;
    if (host_be16(mdb + 0u) != 0x4244u) { free(disk); return; }
    const uint32_t alblk_size = host_be32(mdb + 0x14u);
    const uint16_t albl_start = host_be16(mdb + 0x1cu);
    const uint32_t cat_size = host_be32(mdb + 0x92u);
    if (alblk_size == 0u || cat_size == 0u || cat_size > disk_size) { free(disk); return; }
    uint8_t *cat = (uint8_t *)malloc(cat_size);
    if (cat == NULL) { free(disk); return; }
    uint32_t cat_pos = 0;
    for (uint32_t i = 0; i < 3u; i++) {
        const uint16_t start = host_be16(mdb + 0x96u + i * 4u);
        const uint16_t count = host_be16(mdb + 0x96u + i * 4u + 2u);
        if (count == 0u) continue;
        const uint64_t src = (uint64_t)albl_start * 512u + (uint64_t)start * alblk_size;
        uint64_t len = (uint64_t)count * alblk_size;
        if (src >= disk_size) continue;
        if (src + len > disk_size) len = disk_size - src;
        if (cat_pos + len > cat_size) len = cat_size - cat_pos;
        memcpy(cat + cat_pos, disk + src, (size_t)len);
        cat_pos += (uint32_t)len;
        if (cat_pos >= cat_size) break;
    }
    free(disk);
    if (cat_pos < 512u) { free(cat); return; }

    const uint8_t *node0 = cat;
    const uint16_t n0recs = host_be16(node0 + 10u);
    if (n0recs == 0u) { free(cat); return; }
    const uint16_t hdr_off = host_be16(node0 + 512u - 2u);
    if (hdr_off + 22u > 512u) { free(cat); return; }
    const uint8_t *hdr = node0 + hdr_off;
    const uint32_t first_leaf = host_be32(hdr + 10u);
    const uint16_t node_size = host_be16(hdr + 18u);
    if (node_size == 0u || node_size > 4096u) { free(cat); return; }

    uint32_t node_num = first_leaf;
    unsigned guard = 0;
    while (node_num != 0u && ++guard < 10000u) {
        uint64_t node_off = (uint64_t)node_num * node_size;
        if (node_off + node_size > cat_size) break;
        const uint8_t *node = cat + node_off;
        const uint32_t f_link = host_be32(node + 0u);
        const int8_t node_type = (int8_t)node[8];
        const uint16_t nrecs = host_be16(node + 10u);
        if (node_type != (int8_t)0xff) break;
        for (uint16_t r = 0; r < nrecs; r++) {
            const uint16_t rec_off = host_be16(node + node_size - 2u * (r + 1u));
            const uint16_t next_off = host_be16(node + node_size - 2u * (r + 2u));
            if (rec_off >= node_size || next_off > node_size || next_off <= rec_off) continue;
            const uint8_t *rec = node + rec_off;
            const uint8_t key_len = rec[0];
            if (key_len < 6u || rec_off + key_len + 4u >= next_off) continue;
            const uint32_t parent_id = host_be32(rec + 2u);
            const uint8_t name_len = rec[6] > 31u ? 31u : rec[6];
            const uint32_t data_off = (uint32_t)((1u + key_len + 1u) & ~1u);
            if (rec_off + data_off + 2u >= next_off) continue;
            const uint8_t *data = rec + data_off;
            host_hfs_cat_entry_t entry;
            memset(&entry, 0, sizeof(entry));
            entry.parent_id = parent_id;
            entry.name_len = name_len;
            memcpy(entry.name, rec + 7u, name_len);
            entry.name[name_len] = 0;
            if (data[0] == 1u && rec_off + data_off + 10u <= next_off) {
                entry.is_dir = 1u;
                entry.cnid = host_be32(data + 6u);
            } else if (data[0] == 2u && rec_off + data_off + 42u <= next_off) {
                entry.is_dir = 0u;
                entry.type = host_be32(data + 4u);
                entry.creator = host_be32(data + 8u);
                entry.finder_flags_hi = data[12u];
                entry.finder_flags_lo = data[13u];
                entry.cnid = host_be32(data + 20u);
                entry.data_len = host_be32(data + 26u);
                entry.rsrc_len = host_be32(data + 36u);
            } else {
                continue;
            }
            (void)host_hfs_add_entry(&entry);
        }
        node_num = f_link;
    }
    free(cat);
    fprintf(stderr, "HOST: parsed HFS catalog entries=%zu from %s\n", host_hfs_entry_count, host_disk_path);
}

int host_hfs_get_cat_info(uint32_t dir_id, uint16_t index, uint32_t *cnid, uint8_t *is_dir,
                          uint8_t *name_len, char *name, uint8_t *attr,
                          uint32_t *type, uint32_t *creator, uint16_t *fd_flags,
                          uint32_t *data_len, uint32_t *rsrc_len, uint32_t *parent_id) {
    host_hfs_parse_catalog_once();
    if (index == 0u || host_hfs_entry_count == 0u) return -43;
    uint16_t seen = 0;
    for (size_t i = 0; i < host_hfs_entry_count; i++) {
        const host_hfs_cat_entry_t *e = &host_hfs_entries[i];
        if (e->parent_id != dir_id) continue;
        seen++;
        if (seen != index) continue;
        if (cnid) *cnid = e->cnid;
        if (is_dir) *is_dir = e->is_dir;
        if (name_len) *name_len = e->name_len;
        if (name) memcpy(name, e->name, 32u);
        if (attr) *attr = e->is_dir ? 0x10u : 0u;
        if (type) *type = e->type;
        if (creator) *creator = e->creator;
        if (fd_flags) *fd_flags = ((uint16_t)e->finder_flags_hi << 8u) | e->finder_flags_lo;
        if (data_len) *data_len = e->data_len;
        if (rsrc_len) *rsrc_len = e->rsrc_len;
        if (parent_id) *parent_id = e->parent_id;
        return 0;
    }
    return -43;
}


void host_load_system_rsrc(uint8_t *ram, size_t ram_size) {
    (void)ram;
    (void)ram_size;
    FILE *f = fopen("fixtures/system_rsrc/System.rsrc", "rb");
    if (!f) { fprintf(stderr, "WARN: cannot open System.rsrc\n"); return; }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz > 0x600000u) sz = 0x600000u; // cap at 6MB
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(f); fprintf(stderr, "WARN: cannot allocate System.rsrc buffer\n"); return; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    free(sysrsrc_buf);
    sysrsrc_buf = buf;
    sysrsrc_size = (uint32_t)n;
    sysrsrc_copy_cursor = SYSRSRC_COPY_BASE;
    fprintf(stderr, "HOST: loaded System.rsrc (%zu bytes) host-side; guest copies start at $%06X\n",
            n, SYSRSRC_COPY_BASE);
}

uint16_t host_count_system_resources(uint32_t res_type) {
    if (sysrsrc_size == 0 || sysrsrc_buf == NULL) return 0;
    const uint8_t *rsrc = sysrsrc_buf;
    uint32_t map_offset = (uint32_t)rsrc[4]<<24 | rsrc[5]<<16 | rsrc[6]<<8 | rsrc[7];
    if (map_offset + 30 > sysrsrc_size) return 0;
    const uint8_t *map = rsrc + map_offset;
    uint16_t type_list_off = (uint16_t)(map[24]<<8 | map[25]);
    const uint8_t *tlist = map + type_list_off;
    int16_t num_types = (int16_t)(tlist[0]<<8 | tlist[1]) + 1;
    for (int i = 0; i < num_types; i++) {
        const uint8_t *te = tlist + 2 + i * 8;
        uint32_t t = (uint32_t)te[0]<<24 | te[1]<<16 | te[2]<<8 | te[3];
        if (t == res_type) return (uint16_t)((te[4]<<8 | te[5]) + 1u);
    }
    return 0;
}

// Find a resource in the loaded System.rsrc by type and ID.
// Returns the guest RAM address of a copied resource data block (after length
// prefix), and sets *out_size to the data length. Returns 0 if not found.
uint32_t host_find_system_resource(const uint8_t *ram, size_t ram_size,
                                   uint32_t res_type, int16_t res_id,
                                   uint32_t *out_size) {
    if (sysrsrc_size == 0 || sysrsrc_buf == NULL || ram == NULL) return 0;
    const uint8_t *rsrc = sysrsrc_buf;
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
            uint32_t guest_addr = (sysrsrc_copy_cursor + 3u) & ~3u;
            uint32_t next = guest_addr + dlen;
            if (next > SYSRSRC_COPY_LIMIT || next > ram_size) return 0;
            memcpy((uint8_t *)ram + guest_addr, rsrc + abs_off + 4u, dlen);
            sysrsrc_copy_cursor = next;
            return guest_addr;
        }
        return 0; // type found but ID not found
    }
    return 0; // type not found
}
