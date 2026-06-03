#ifndef MACHINE_LC_BASILISK_COMPAT_H
#define MACHINE_LC_BASILISK_COMPAT_H

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#define LC_BASILISK_ROM_VERSION_32 0x067cu
#define LC_BASILISK_ROM_BASE_32 0x40800000u
#define LC_BASILISK_FRAME_BASE_MAC 0xa0000000u

#define LC_B2_M68K_NOP 0x4e71u
#define LC_B2_M68K_RTS 0x4e75u
#define LC_B2_M68K_JMP 0x4ef9u
#define LC_B2_M68K_JMP_A0 0x4ed0u
#define LC_B2_M68K_JSR_A0 0x4e90u

#define LC_B2_M68K_EXEC_RETURN 0x7100u
#define LC_B2_EMUL_BREAK 0x7101u
#define LC_B2_EMUL_OP_SHUTDOWN 0x7102u
#define LC_B2_EMUL_OP_RESET 0x7103u
#define LC_B2_EMUL_OP_CLKNOMEM 0x7104u
#define LC_B2_EMUL_OP_READ_XPRAM 0x7105u
#define LC_B2_EMUL_OP_READ_XPRAM2 0x7106u
#define LC_B2_EMUL_OP_PATCH_BOOT_GLOBS 0x7107u
#define LC_B2_EMUL_OP_FIX_BOOTSTACK 0x7108u
#define LC_B2_EMUL_OP_FIX_MEMSIZE 0x7109u
#define LC_B2_EMUL_OP_INSTALL_DRIVERS 0x710au
#define LC_B2_EMUL_OP_SONY_OPEN 0x710bu
#define LC_B2_EMUL_OP_SONY_PRIME 0x710cu
#define LC_B2_EMUL_OP_SONY_CONTROL 0x710du
#define LC_B2_EMUL_OP_SONY_STATUS 0x710eu
#define LC_B2_EMUL_OP_DISK_OPEN 0x710fu
#define LC_B2_EMUL_OP_DISK_PRIME 0x7110u
#define LC_B2_EMUL_OP_DISK_CONTROL 0x7111u
#define LC_B2_EMUL_OP_DISK_STATUS 0x7112u
#define LC_B2_EMUL_OP_CDROM_OPEN 0x7113u
#define LC_B2_EMUL_OP_CDROM_PRIME 0x7114u
#define LC_B2_EMUL_OP_CDROM_CONTROL 0x7115u
#define LC_B2_EMUL_OP_CDROM_STATUS 0x7116u
#define LC_B2_EMUL_OP_ADBOP 0x7123u
#define LC_B2_EMUL_OP_INSTIME 0x7124u
#define LC_B2_EMUL_OP_RMVTIME 0x7125u
#define LC_B2_EMUL_OP_PRIMETIME 0x7126u
#define LC_B2_EMUL_OP_MICROSECONDS 0x7127u
#define LC_B2_EMUL_OP_SCSI_DISPATCH 0x7128u
#define LC_B2_EMUL_OP_IRQ 0x7129u
#define LC_B2_EMUL_OP_VIDEO_OPEN 0x7118u
#define LC_B2_EMUL_OP_VIDEO_CONTROL 0x7119u
#define LC_B2_EMUL_OP_VIDEO_STATUS 0x711au
#define LC_B2_EMUL_OP_CHECKLOAD 0x712cu
#define LC_B2_EMUL_OP_BLOCK_MOVE 0x7130u
#define LC_B2_EMUL_OP_DEBUGUTIL 0x7136u

typedef struct {
    uint16_t rom_version;
    uint32_t universal_info_offset;
    uint32_t patches_applied;
    uint32_t patch_patterns_found;
    uint32_t patch_patterns_missing;
} lc_basilisk_patch_summary_t;

uint16_t lc_basilisk_rom_version(const uint8_t *rom, size_t rom_size);
uint32_t lc_basilisk_find_universal_info(const uint8_t *rom, size_t rom_size);
esp_err_t lc_basilisk_apply_rom32_patches(uint8_t *rom, size_t rom_size,
                                          lc_basilisk_patch_summary_t *summary);
esp_err_t lc_basilisk_install_minimal_slot_rom(uint8_t *rom, size_t rom_size,
                                               uint32_t frame_base,
                                               uint16_t width,
                                               uint16_t height,
                                               uint16_t rowbytes,
                                               uint32_t *slot_rom_offset_out,
                                               uint32_t *slot_rom_size_out);

#endif
