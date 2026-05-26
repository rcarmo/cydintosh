#ifndef MACHINE_LC_DISK_H
#define MACHINE_LC_DISK_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef LC_DISK_IMAGE_READ_ONLY
#define LC_DISK_IMAGE_READ_ONLY 1
#endif

#ifndef LC_DISK_SECTOR_SIZE
#define LC_DISK_SECTOR_SIZE 512u
#endif

typedef enum {
    LC_DISK_CMD_PROBE,
    LC_DISK_CMD_SWIM_READ_SECTOR,
    LC_DISK_CMD_SWIM_WRITE_SECTOR,
    LC_DISK_CMD_SCSI_READ,
    LC_DISK_CMD_SCSI_WRITE,
    LC_DISK_CMD_UNKNOWN,
} lc_disk_command_t;

typedef struct {
    bool partition_present;
    uint32_t partition_offset;
    uint32_t partition_size;
    uint32_t sector_size;
    uint32_t sector_count;
    bool read_only;
} lc_disk_info_t;

const char *lc_disk_command_name(lc_disk_command_t command);
esp_err_t lc_disk_probe(lc_disk_info_t *info);
void lc_disk_log_info(const lc_disk_info_t *info);
void lc_disk_log_policy(void);
bool lc_disk_write_allowed(void);
void lc_disk_trace_io(lc_disk_command_t command, uint64_t sector, uint32_t byte_count,
                      bool write, esp_err_t status);
void lc_disk_trace_sample_events(void);

#endif
