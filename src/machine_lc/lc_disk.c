#include "lc_disk.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "lc_trace.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_disk";

const char *lc_disk_command_name(lc_disk_command_t command) {
    switch (command) {
    case LC_DISK_CMD_PROBE:
        return "probe";
    case LC_DISK_CMD_SWIM_READ_SECTOR:
        return "swim-read-sector";
    case LC_DISK_CMD_SWIM_WRITE_SECTOR:
        return "swim-write-sector";
    case LC_DISK_CMD_SCSI_READ:
        return "scsi-read";
    case LC_DISK_CMD_SCSI_WRITE:
        return "scsi-write";
    case LC_DISK_CMD_UNKNOWN:
    default:
        return "unknown";
    }
}

bool lc_disk_write_allowed(void) {
    return LC_DISK_IMAGE_READ_ONLY == 0;
}

esp_err_t lc_disk_probe(lc_disk_info_t *info) {
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    info->sector_size = LC_DISK_SECTOR_SIZE;
    info->read_only = !lc_disk_write_allowed();

    const esp_partition_t *disk = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "disk");
    if (disk == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    info->partition_present = true;
    info->partition_offset = disk->address;
    info->partition_size = disk->size;
    info->sector_count = disk->size / LC_DISK_SECTOR_SIZE;
    return ESP_OK;
}

void lc_disk_log_info(const lc_disk_info_t *info) {
    if (info == NULL || !info->partition_present) {
        ESP_LOGW(TAG, "LC disk partition not found yet (expected data/littlefs label 'disk')");
        return;
    }

    ESP_LOGI(TAG,
             "LC disk partition: offset=0x%08" PRIx32 " size=0x%08" PRIx32
             " sector_size=%" PRIu32 " sectors=%" PRIu32 " read_only=%s",
             info->partition_offset, info->partition_size, info->sector_size, info->sector_count,
             info->read_only ? "yes" : "no");
}

void lc_disk_log_policy(void) {
    ESP_LOGI(TAG,
             "LC disk policy: image=vendor/lc-disk.img local-only firmware_partition=disk read_only=%s writes=%s",
             LC_DISK_IMAGE_READ_ONLY ? "yes" : "no",
             lc_disk_write_allowed() ? "allowed" : "blocked-and-traced");
    ESP_LOGI(TAG,
             "LC disk trace fields: command, sector/block, byte_count, read/write, status; no guest writes enabled yet");
}

void lc_disk_trace_io(lc_disk_command_t command, uint64_t sector, uint32_t byte_count,
                      bool write, esp_err_t status) {
    const bool blocked = write && !lc_disk_write_allowed();
    lc_trace_record(blocked ? LC_TRACE_EVENT_DISK_WRITE_BLOCKED : LC_TRACE_EVENT_DISK_IO, 0,
                    (uint32_t)(sector & 0xffffffffu), byte_count, 0, write);

    if (blocked) {
        ESP_LOGW(TAG,
                 "LC disk I/O blocked: command=%s sector=%" PRIu64
                 " byte_count=%" PRIu32 " status=%s",
                 lc_disk_command_name(command), sector, byte_count, esp_err_to_name(status));
        return;
    }

    ESP_LOGI(TAG,
             "LC disk I/O trace: command=%s sector=%" PRIu64 " byte_count=%" PRIu32
             " op=%s status=%s",
             lc_disk_command_name(command), sector, byte_count, write ? "write" : "read",
             esp_err_to_name(status));
}

void lc_disk_trace_sample_events(void) {
    lc_disk_trace_io(LC_DISK_CMD_PROBE, 0, 0, false, ESP_OK);
    lc_disk_trace_io(LC_DISK_CMD_SWIM_READ_SECTOR, 0, LC_DISK_SECTOR_SIZE, false, ESP_OK);
    lc_disk_trace_io(LC_DISK_CMD_SWIM_WRITE_SECTOR, 0, LC_DISK_SECTOR_SIZE, true,
                     ESP_ERR_NOT_ALLOWED);
}
