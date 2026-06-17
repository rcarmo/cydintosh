#include "lc_basilisk_compat.h"

#include "esp_log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "lc_basilisk";

#define LC_B2_SLOT_ROM_CAP 4096u

typedef struct {
    uint8_t bytes[LC_B2_SLOT_ROM_CAP];
    uint32_t p;
    bool overflow;
} lc_b2_slot_builder_t;

static uint16_t be16_at(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8u) | (uint16_t)p[1]);
}

static uint32_t be32_at(const uint8_t *p) {
    return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
           ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}

static void put16(uint8_t *rom, size_t rom_size, uint32_t offset, uint16_t value,
                  lc_basilisk_patch_summary_t *summary) {
    if (rom == NULL || offset + 1u >= rom_size) {
        return;
    }
    rom[offset + 0u] = (uint8_t)(value >> 8u);
    rom[offset + 1u] = (uint8_t)value;
    if (summary != NULL) {
        summary->patches_applied++;
    }
}

static void put32(uint8_t *rom, size_t rom_size, uint32_t offset, uint32_t value,
                  lc_basilisk_patch_summary_t *summary) {
    put16(rom, rom_size, offset + 0u, (uint16_t)(value >> 16u), summary);
    put16(rom, rom_size, offset + 2u, (uint16_t)value, summary);
}

static uint32_t find_data(const uint8_t *rom, size_t rom_size, uint32_t start,
                          uint32_t end, const uint8_t *data, size_t data_len,
                          lc_basilisk_patch_summary_t *summary) {
    if (rom == NULL || data == NULL || data_len == 0u || start >= rom_size) {
        if (summary != NULL) summary->patch_patterns_missing++;
        return 0;
    }
    if (end > rom_size) {
        end = (uint32_t)rom_size;
    }
    for (uint32_t off = start; off + data_len <= end; off++) {
        if (memcmp(&rom[off], data, data_len) == 0) {
            if (summary != NULL) summary->patch_patterns_found++;
            return off;
        }
    }
    if (summary != NULL) summary->patch_patterns_missing++;
    ESP_LOGW(TAG, "LC patch MISS find_data range=[0x%05" PRIx32 ",0x%05" PRIx32 "] len=%u bytes=%02x%02x%02x%02x%02x%02x",
             start, end, (unsigned)data_len,
             data_len > 0 ? data[0] : 0, data_len > 1 ? data[1] : 0,
             data_len > 2 ? data[2] : 0, data_len > 3 ? data[3] : 0,
             data_len > 4 ? data[4] : 0, data_len > 5 ? data[5] : 0);
    return 0;
}

#define LC_B2_FOURCC(a, b, c, d) (((uint32_t)(uint8_t)(a) << 24u) | ((uint32_t)(uint8_t)(b) << 16u) | ((uint32_t)(uint8_t)(c) << 8u) | (uint32_t)(uint8_t)(d))

static uint32_t find_rom_resource(const uint8_t *rom, size_t rom_size, uint32_t type,
                                  int16_t id, lc_basilisk_patch_summary_t *summary) {
    if (rom == NULL || rom_size < 0x20u) {
        if (summary != NULL) summary->patch_patterns_missing++;
        return 0;
    }
    uint32_t map = be32_at(&rom[0x1au]);
    if (map + 24u >= rom_size) {
        if (summary != NULL) summary->patch_patterns_missing++;
        return 0;
    }
    uint32_t rsrc = be32_at(&rom[map]);
    for (unsigned guard = 0; guard < 4096u && rsrc != 0u && rsrc + 24u < rom_size; guard++) {
        const uint32_t data = be32_at(&rom[rsrc + 12u]);
        const uint32_t rtype = be32_at(&rom[rsrc + 16u]);
        const int16_t rid = (int16_t)be16_at(&rom[rsrc + 20u]);
        if (rtype == type && rid == id && data < rom_size) {
            if (summary != NULL) summary->patch_patterns_found++;
            return data;
        }
        rsrc = be32_at(&rom[rsrc + 8u]);
    }
    if (summary != NULL) summary->patch_patterns_missing++;
    return 0;
}

static uint32_t find_rom_trap(const uint8_t *rom, size_t rom_size, uint16_t trap,
                              lc_basilisk_patch_summary_t *summary) {
    if (rom == NULL || rom_size < 0x26u) {
        if (summary != NULL) summary->patch_patterns_missing++;
        return 0;
    }
    const uint32_t trap_table = be32_at(&rom[0x22u]);
    if (trap_table >= rom_size) {
        if (summary != NULL) summary->patch_patterns_missing++;
        return 0;
    }
    const uint8_t *bp = &rom[trap_table];
    const uint8_t *end = &rom[rom_size];
    uint16_t rom_trap = 0xa800u;
    uint32_t ofs = 0;
    for (int table = 0; table < 2; table++) {
        for (int i = 0; i < 0x400 && bp < end; i++) {
            bool unimplemented = false;
            const uint8_t b = *bp++;
            if (b == 0x80u) {
                unimplemented = true;
            } else if (b == 0xffu) {
                if (bp + 4 > end) {
                    if (summary != NULL) summary->patch_patterns_missing++;
                    return 0;
                }
                ofs = be32_at(bp);
                bp += 4;
            } else if (b & 0x80u) {
                const int16_t add = (int16_t)((b & 0x7fu) << 1u);
                if (add == 0) {
                    if (summary != NULL) summary->patch_patterns_missing++;
                    return 0;
                }
                ofs = (uint32_t)(ofs + (uint32_t)(int32_t)add);
            } else {
                if (bp >= end) {
                    if (summary != NULL) summary->patch_patterns_missing++;
                    return 0;
                }
                // Branch-table offsets are signed (BasiliskII find_rom_trap):
                // ((b << 8) | next) << 1 interpreted as int16, so high-bit
                // offsets subtract.  Treating them as unsigned mis-resolves
                // every trap whose cumulative offset has the top bit set.
                const int16_t add = (int16_t)((((uint16_t)b << 8u) | (uint16_t)*bp++) << 1u);
                if (add == 0) {
                    if (summary != NULL) summary->patch_patterns_missing++;
                    return 0;
                }
                ofs = (uint32_t)(ofs + (uint32_t)(int32_t)add);
            }
            if (rom_trap == trap) {
                if (!unimplemented && ofs < rom_size) {
                    if (summary != NULL) summary->patch_patterns_found++;
                    return ofs;
                }
                if (summary != NULL) summary->patch_patterns_missing++;
                ESP_LOGW(TAG, "LC patch MISS find_rom_trap=0x%04x (unimplemented/oob ofs=0x%05" PRIx32 ")", trap, ofs);
                return 0;
            }
            rom_trap++;
        }
        rom_trap = 0xa000u;
    }
    if (summary != NULL) summary->patch_patterns_missing++;
    ESP_LOGW(TAG, "LC patch MISS find_rom_trap=0x%04x (not in table)", trap);
    return 0;
}

uint16_t lc_basilisk_rom_version(const uint8_t *rom, size_t rom_size) {
    if (rom == NULL || rom_size < 10u) {
        return 0;
    }
    return be16_at(&rom[8]);
}

uint32_t lc_basilisk_find_universal_info(const uint8_t *rom, size_t rom_size) {
    static const uint8_t universal_dat[] = {0xdc, 0x00, 0x05, 0x05, 0x3f, 0xff, 0x01, 0x00};
    const uint32_t base = find_data(rom, rom_size, 0x3400u, 0x3c00u,
                                    universal_dat, sizeof(universal_dat), NULL);
    return base != 0u ? base - 0x10u : 0u;
}

static void patch_nops(uint8_t *rom, size_t rom_size, uint32_t offset, unsigned words,
                       lc_basilisk_patch_summary_t *summary) {
    for (unsigned i = 0; i < words; i++) {
        put16(rom, rom_size, offset + i * 2u, LC_B2_M68K_NOP, summary);
    }
}

static void patch_abs_jump(uint8_t *rom, size_t rom_size, uint32_t offset, uint32_t target,
                           lc_basilisk_patch_summary_t *summary) {
    put16(rom, rom_size, offset + 0u, LC_B2_M68K_JMP, summary);
    put32(rom, rom_size, offset + 2u, target, summary);
}

static const uint8_t lc_b2_sony_driver[] = {
    0x6f,0x00,0,0,0,0,0,0, 0x00,0x18, 0x00,0x1c, 0x00,0x20, 0x00,0x2c, 0x00,0x52,
    0x05,'.','S','o','n','y',
    LC_B2_EMUL_OP_SONY_OPEN >> 8, LC_B2_EMUL_OP_SONY_OPEN & 0xff, 0x4e,0x75,
    LC_B2_EMUL_OP_SONY_PRIME >> 8, LC_B2_EMUL_OP_SONY_PRIME & 0xff, 0x60,0x0e,
    LC_B2_EMUL_OP_SONY_CONTROL >> 8, LC_B2_EMUL_OP_SONY_CONTROL & 0xff, 0x0c,0x68,0x00,0x01,0x00,0x1a,0x66,0x04,0x4e,0x75,
    LC_B2_EMUL_OP_SONY_STATUS >> 8, LC_B2_EMUL_OP_SONY_STATUS & 0xff,
    0x32,0x28,0x00,0x06,0x08,0x01,0x00,0x09,0x67,0x0c,0x4a,0x40,0x6f,0x02,0x42,0x40,0x31,0x40,0x00,0x10,0x4e,0x75,
    0x4a,0x40,0x6f,0x04,0x42,0x40,0x4e,0x75,0x2f,0x38,0x08,0xfc,0x4e,0x75,
    0x70,0xe8,0x4e,0x75,
};

static const uint8_t lc_b2_disk_driver[] = {
    0x6f,0x04,0,0,0,0,0,0, 0x00,0x18, 0x00,0x1c, 0x00,0x20, 0x00,0x2c, 0x00,0x52,
    0x05,'.','D','i','s','k',
    LC_B2_EMUL_OP_DISK_OPEN >> 8, LC_B2_EMUL_OP_DISK_OPEN & 0xff, 0x4e,0x75,
    LC_B2_EMUL_OP_DISK_PRIME >> 8, LC_B2_EMUL_OP_DISK_PRIME & 0xff, 0x60,0x0e,
    LC_B2_EMUL_OP_DISK_CONTROL >> 8, LC_B2_EMUL_OP_DISK_CONTROL & 0xff, 0x0c,0x68,0x00,0x01,0x00,0x1a,0x66,0x04,0x4e,0x75,
    LC_B2_EMUL_OP_DISK_STATUS >> 8, LC_B2_EMUL_OP_DISK_STATUS & 0xff,
    0x32,0x28,0x00,0x06,0x08,0x01,0x00,0x09,0x67,0x0c,0x4a,0x40,0x6f,0x02,0x42,0x40,0x31,0x40,0x00,0x10,0x4e,0x75,
    0x4a,0x40,0x6f,0x04,0x42,0x40,0x4e,0x75,0x2f,0x38,0x08,0xfc,0x4e,0x75,
    0x70,0xe8,0x4e,0x75,
};

static const uint8_t lc_b2_cdrom_driver[] = {
    0x6d,0x04,0,0,0,0,0,0, 0x00,0x1c, 0x00,0x20, 0x00,0x24, 0x00,0x30, 0x00,0x56,
    0x08,'.','A','p','p','l','e','C','D',0x00,
    LC_B2_EMUL_OP_CDROM_OPEN >> 8, LC_B2_EMUL_OP_CDROM_OPEN & 0xff, 0x4e,0x75,
    LC_B2_EMUL_OP_CDROM_PRIME >> 8, LC_B2_EMUL_OP_CDROM_PRIME & 0xff, 0x60,0x0e,
    LC_B2_EMUL_OP_CDROM_CONTROL >> 8, LC_B2_EMUL_OP_CDROM_CONTROL & 0xff, 0x0c,0x68,0x00,0x01,0x00,0x1a,0x66,0x04,0x4e,0x75,
    LC_B2_EMUL_OP_CDROM_STATUS >> 8, LC_B2_EMUL_OP_CDROM_STATUS & 0xff,
    0x32,0x28,0x00,0x06,0x08,0x01,0x00,0x09,0x67,0x0c,0x4a,0x40,0x6f,0x02,0x42,0x40,0x31,0x40,0x00,0x10,0x4e,0x75,
    0x4a,0x40,0x6f,0x04,0x42,0x40,0x4e,0x75,0x2f,0x38,0x08,0xfc,0x4e,0x75,
    0x70,0xe8,0x4e,0x75,
};

static const uint8_t lc_b2_adbop_patch[] = {
    0x40,0xe7,0x00,0x7c,0x07,0x00, LC_B2_EMUL_OP_ADBOP >> 8, LC_B2_EMUL_OP_ADBOP & 0xff,
    0x48,0xe7,0x70,0xf0,0x26,0x48,0x4a,0xab,0x00,0x04,0x67,0x00,0x00,0x18,
    0x20,0x53,0x22,0x6b,0x00,0x04,0x24,0x6b,0x00,0x08,0x26,0x78,0x0c,0xf8,
    0x4e,0x91,0x70,0x00,0x60,0x00,0x00,0x04,0x70,0x00,0x4c,0xdf,0x0f,0x0e,
    0x46,0xdf,0x4e,0x75,
};

static void sb_byte(lc_b2_slot_builder_t *b, uint8_t v) {
    if (b->p >= LC_B2_SLOT_ROM_CAP) { b->overflow = true; return; }
    b->bytes[b->p++] = v;
}

static void sb_word(lc_b2_slot_builder_t *b, uint16_t v) {
    sb_byte(b, (uint8_t)(v >> 8u)); sb_byte(b, (uint8_t)v);
}

static void sb_long(lc_b2_slot_builder_t *b, uint32_t v) {
    sb_word(b, (uint16_t)(v >> 16u)); sb_word(b, (uint16_t)v);
}

static void sb_offs(lc_b2_slot_builder_t *b, uint8_t type, uint32_t ptr) {
    const uint32_t offs = ptr - b->p;
    sb_byte(b, type); sb_byte(b, (uint8_t)(offs >> 16u)); sb_byte(b, (uint8_t)(offs >> 8u)); sb_byte(b, (uint8_t)offs);
}

static void sb_rsrc(lc_b2_slot_builder_t *b, uint8_t type, uint32_t data) {
    sb_byte(b, type); sb_byte(b, (uint8_t)(data >> 16u)); sb_byte(b, (uint8_t)(data >> 8u)); sb_byte(b, (uint8_t)data);
}

static void sb_end(lc_b2_slot_builder_t *b) { sb_long(b, 0xff000000u); }

static void sb_string(lc_b2_slot_builder_t *b, const char *s) {
    while (*s != '\0') sb_byte(b, (uint8_t)*s++);
    sb_byte(b, 0);
    if (b->p & 1u) sb_byte(b, 0);
}

static void sb_pstring(lc_b2_slot_builder_t *b, const char *s) {
    const size_t n = strlen(s);
    sb_byte(b, (uint8_t)n);
    for (size_t i = 0; i < n; i++) sb_byte(b, (uint8_t)s[i]);
    if (b->p & 1u) sb_byte(b, 0);
}

static uint32_t sb_vmode_params(lc_b2_slot_builder_t *b, uint16_t width, uint16_t height,
                                uint16_t rowbytes, uint16_t depth) {
    const uint32_t ret = b->p;
    sb_long(b, 50); sb_long(b, 0); sb_word(b, rowbytes);
    sb_word(b, 0); sb_word(b, 0); sb_word(b, height); sb_word(b, width);
    sb_word(b, 0); sb_word(b, 0); sb_long(b, 0);
    sb_long(b, 0x00480000u); sb_long(b, 0x00480000u);
    if (depth <= 8u) { sb_word(b, 0); sb_word(b, depth); sb_word(b, 1); sb_word(b, depth); }
    else if (depth == 16u) { sb_word(b, 16); sb_word(b, 16); sb_word(b, 3); sb_word(b, 5); }
    else { sb_word(b, 16); sb_word(b, 32); sb_word(b, 3); sb_word(b, 8); }
    sb_long(b, 0); sb_long(b, 0);
    return ret;
}

static uint32_t sb_vmode_desc(lc_b2_slot_builder_t *b, uint32_t params, bool direct) {
    const uint32_t ret = b->p;
    sb_offs(b, 0x01, params); sb_rsrc(b, 0x03, 1); sb_rsrc(b, 0x04, direct ? 2u : 0u); sb_end(b);
    return ret;
}

esp_err_t lc_basilisk_install_minimal_slot_rom(uint8_t *rom, size_t rom_size,
                                               uint32_t frame_base,
                                               uint16_t width,
                                               uint16_t height,
                                               uint16_t rowbytes,
                                               uint32_t *slot_rom_offset_out,
                                               uint32_t *slot_rom_size_out) {
    if (rom == NULL || rom_size < LC_B2_SLOT_ROM_CAP) return ESP_ERR_INVALID_ARG;
    lc_b2_slot_builder_t b = {0};

    const uint32_t boardType = b.p; sb_word(&b, 1); sb_word(&b, 0); sb_word(&b, 0); sb_word(&b, 0);
    const uint32_t boardName = b.p; sb_string(&b, "Basilisk II Slot ROM");
    const uint32_t vendorID = b.p; sb_string(&b, "Cydintosh/BasiliskII");
    const uint32_t revLevel = b.p; sb_string(&b, "V0.1");
    const uint32_t partNum = b.p; sb_string(&b, "CydintoshLC");
    const uint32_t date = b.p; sb_string(&b, "2026-06-01");
    const uint32_t vendorInfo = b.p;
    sb_offs(&b, 0x01, vendorID); sb_offs(&b, 0x03, revLevel); sb_offs(&b, 0x04, partNum); sb_offs(&b, 0x05, date); sb_end(&b);
    const uint32_t sRsrcBoard = b.p;
    sb_offs(&b, 0x01, boardType); sb_offs(&b, 0x02, boardName); sb_rsrc(&b, 0x20, 0x4232); sb_offs(&b, 0x24, vendorInfo); sb_end(&b);

    const uint32_t videoType = b.p; sb_word(&b, 3); sb_word(&b, 1); sb_word(&b, 1); sb_word(&b, 0x4232);
    const uint32_t videoName = b.p; sb_string(&b, "Display_Video_Apple_Basilisk");
    const uint32_t videoDrvr = b.p;
    sb_long(&b, 0x72); sb_word(&b, 0x4c00); sb_word(&b, 0); sb_word(&b, 0); sb_word(&b, 0);
    sb_word(&b, 0x32); sb_word(&b, 0x36); sb_word(&b, 0x3a); sb_word(&b, 0x46); sb_word(&b, 0x6c);
    sb_pstring(&b, ".Display_Video_Apple_Basilisk"); sb_word(&b, 1);
    sb_word(&b, LC_B2_EMUL_OP_VIDEO_OPEN); sb_word(&b, LC_B2_M68K_RTS);
    sb_word(&b, 0x70ff); sb_word(&b, 0x600e);
    sb_word(&b, LC_B2_EMUL_OP_VIDEO_CONTROL); sb_word(&b, 0x0c68); sb_word(&b, 0x0001); sb_word(&b, 0x001a); sb_word(&b, 0x6604); sb_word(&b, LC_B2_M68K_RTS);
    sb_word(&b, LC_B2_EMUL_OP_VIDEO_STATUS); sb_word(&b, 0x3228); sb_word(&b, 0x0006); sb_word(&b, 0x0801); sb_word(&b, 0x0009); sb_word(&b, 0x670c);
    sb_word(&b, 0x4a40); sb_word(&b, 0x6f02); sb_word(&b, 0x4240); sb_word(&b, 0x3140); sb_word(&b, 0x0010); sb_word(&b, LC_B2_M68K_RTS);
    sb_word(&b, 0x4a40); sb_word(&b, 0x6f04); sb_word(&b, 0x4240); sb_word(&b, LC_B2_M68K_RTS); sb_word(&b, 0x2f38); sb_word(&b, 0x08fc); sb_word(&b, LC_B2_M68K_RTS);
    sb_word(&b, 0x70e8); sb_word(&b, LC_B2_M68K_RTS);
    const uint32_t vidDrvrDir = b.p; sb_offs(&b, 0x02, videoDrvr); sb_end(&b);

    const uint32_t defaultGamma = b.p;
    sb_long(&b, 38 + 0x100); sb_word(&b, 0x2000); sb_string(&b, "Mac HiRes Std Gamma");
    sb_word(&b, 0); sb_word(&b, 0); sb_word(&b, 0); sb_word(&b, 1); sb_word(&b, 0x0100); sb_word(&b, 8);
    for (unsigned i = 0; i < 256u; i++) sb_byte(&b, (uint8_t)i);
    const uint32_t gammaDir = b.p; sb_offs(&b, 0x80, defaultGamma); sb_end(&b);

    const uint32_t minorBase = b.p; sb_long(&b, frame_base);
    const uint32_t minorLength = b.p; sb_long(&b, 0);
    const uint32_t p1 = sb_vmode_params(&b, width, height, (uint16_t)((width + 7u) / 8u), 1);
    const uint32_t p2 = sb_vmode_params(&b, width, height, (uint16_t)((width + 3u) / 4u), 2);
    const uint32_t p4 = sb_vmode_params(&b, width, height, (uint16_t)((width + 1u) / 2u), 4);
    const uint32_t p8 = sb_vmode_params(&b, width, height, rowbytes, 8);
    const uint32_t p16 = sb_vmode_params(&b, width, height, (uint16_t)(width * 2u), 16);
    const uint32_t p32 = sb_vmode_params(&b, width, height, (uint16_t)(width * 4u), 32);
    const uint32_t m1 = sb_vmode_desc(&b, p1, false), m2 = sb_vmode_desc(&b, p2, false), m4 = sb_vmode_desc(&b, p4, false), m8 = sb_vmode_desc(&b, p8, false), m16 = sb_vmode_desc(&b, p16, true), m32 = sb_vmode_desc(&b, p32, true);
    const uint32_t sRsrcVideo = b.p;
    sb_offs(&b, 0x01, videoType); sb_offs(&b, 0x02, videoName); sb_offs(&b, 0x04, vidDrvrDir); sb_rsrc(&b, 0x08, 0x4232);
    sb_offs(&b, 0x0a, minorBase); sb_offs(&b, 0x0b, minorLength); sb_offs(&b, 0x40, gammaDir); sb_rsrc(&b, 0x7d, 6);
    sb_offs(&b, 0x80, m1); sb_offs(&b, 0x81, m2); sb_offs(&b, 0x82, m4); sb_offs(&b, 0x83, m8); sb_offs(&b, 0x84, m16); sb_offs(&b, 0x85, m32); sb_end(&b);

    const uint32_t cpuType = b.p; sb_word(&b, 10); sb_word(&b, 3); sb_word(&b, 0); sb_word(&b, 24);
    const uint32_t cpuName = b.p; sb_string(&b, "CPU_68020");
    const uint32_t cpuMajor = b.p; sb_long(&b, 0); sb_long(&b, 0x7fffffff);
    const uint32_t cpuMinor = b.p; sb_long(&b, 0xf0800000); sb_long(&b, 0xf0ffffff);
    const uint32_t sRsrcCPU = b.p; sb_offs(&b, 0x01, cpuType); sb_offs(&b, 0x02, cpuName); sb_offs(&b, 0x81, cpuMajor); sb_offs(&b, 0x82, cpuMinor); sb_end(&b);

    const uint32_t sRsrcDir = b.p; sb_offs(&b, 0x01, sRsrcBoard); sb_offs(&b, 0x80, sRsrcVideo); sb_offs(&b, 0xf0, sRsrcCPU); sb_end(&b);
    sb_offs(&b, 0, sRsrcDir); sb_long(&b, b.p + 16u); sb_long(&b, 0); sb_word(&b, 0x0101); sb_long(&b, 0x5a932bc7); sb_word(&b, 0x000f);
    if (b.overflow || b.p > LC_B2_SLOT_ROM_CAP) return ESP_ERR_NO_MEM;

    const uint32_t slot_size = b.p;
    uint8_t *dst = rom + rom_size - slot_size;
    memcpy(dst, b.bytes, slot_size);
    dst[slot_size - 12u] = dst[slot_size - 11u] = dst[slot_size - 10u] = dst[slot_size - 9u] = 0;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < slot_size; i++) { crc = (crc << 1u) | (crc >> 31u); crc += dst[i]; }
    dst[slot_size - 12u] = (uint8_t)(crc >> 24u); dst[slot_size - 11u] = (uint8_t)(crc >> 16u); dst[slot_size - 10u] = (uint8_t)(crc >> 8u); dst[slot_size - 9u] = (uint8_t)crc;
    if (slot_rom_offset_out != NULL) *slot_rom_offset_out = (uint32_t)rom_size - slot_size;
    if (slot_rom_size_out != NULL) *slot_rom_size_out = slot_size;
    ESP_LOGW(TAG, "Basilisk minimal Slot ROM installed: offset=0x%05" PRIx32 " size=0x%04" PRIx32 " frame=0x%08" PRIx32 " %ux%u rowbytes=%u crc=0x%08" PRIx32,
             (uint32_t)rom_size - slot_size, slot_size, frame_base, width, height, rowbytes, crc);
    return ESP_OK;
}

esp_err_t lc_basilisk_apply_rom32_patches(uint8_t *rom, size_t rom_size,
                                          lc_basilisk_patch_summary_t *summary) {
    if (rom == NULL || rom_size < 0x80000u) {
        return ESP_ERR_INVALID_ARG;
    }
    lc_basilisk_patch_summary_t local = {0};
    if (summary == NULL) {
        summary = &local;
    }
    memset(summary, 0, sizeof(*summary));
    summary->rom_version = lc_basilisk_rom_version(rom, rom_size);
    if (summary->rom_version != LC_BASILISK_ROM_VERSION_32) {
        ESP_LOGW(TAG, "Basilisk ROM patch skipped: unsupported version=0x%04x", summary->rom_version);
        return ESP_ERR_INVALID_STATE;
    }

    summary->universal_info_offset = lc_basilisk_find_universal_info(rom, rom_size);
    if (summary->universal_info_offset != 0u && summary->universal_info_offset + 32u < rom_size) {
        const uint32_t nu_bus_info = be32_at(&rom[summary->universal_info_offset + 12u]);
        if (summary->universal_info_offset + nu_bus_info + 16u < rom_size) {
            uint8_t *bp = &rom[summary->universal_info_offset + nu_bus_info];
            bp[0] = 0x03u;
            for (int i = 1; i < 16; i++) bp[i] = 0x08u;
            summary->patches_applied += 16u;
        }
        rom[summary->universal_info_offset + 18u] = 14u; // Mac II-ish model ID, matches Basilisk prefs path.
        rom[summary->universal_info_offset + 22u] = 4u;  // FPU optional when no FPU is emulated.
        summary->patches_applied += 2u;

        // PatchHWBases: BasiliskII redirects every hardware base address in the
        // ROM decoderInfo to a scratch RAM region so the ROM's I/O probes hit
        // harmless backed memory instead of unmapped 0x50fxxxxx hardware.
        // Without this, post-CLKNOMEM init reads garbage from 0x50f00000 and
        // diverges into the high-ROM memory-layout dead path; interrupts are
        // delivered through EMUL_OP_IRQ instead.  The walk table at ROM 0x94a
        // holds (offset/4, lowMemGlobal) pairs terminated by 0xffff; the ASC
        // base (lmg 0xcc0) is intentionally left alone, matching patch_rom_32.
        {
            const uint32_t ui = summary->universal_info_offset;
            const int32_t dec_rel = (int32_t)be32_at(&rom[ui]);
            const uint32_t decoder = (uint32_t)((int32_t)ui + dec_rel);
            uint32_t t = 0x094au;
            unsigned hw_patched = 0u;
            while (t + 4u <= rom_size && be16_at(&rom[t]) != 0xffffu) {
                const int16_t ofs = (int16_t)be16_at(&rom[t]);
                const uint16_t lmg = be16_at(&rom[t + 2u]);
                const uint32_t slot = (uint32_t)((int32_t)decoder + (int32_t)ofs * 4);
                if (lmg != 0x0cc0u && slot + 3u < rom_size) {
                    put32(rom, rom_size, slot, LC_BASILISK_HW_SCRATCH_BASE, summary);
                    hw_patched++;
                }
                t += 4u;
            }
            ESP_LOGW(TAG, "LC PatchHWBases: decoderInfo=0x%05" PRIx32
                     " redirected %u hardware bases to scratch 0x%08x",
                     decoder, hw_patched, LC_BASILISK_HW_SCRATCH_BASE);
        }
    } else {
        summary->patch_patterns_missing++;
    }

    // BasiliskII patch_rom_32() core reset/hardware bypass subset.
    put16(rom, rom_size, 0x008cu, LC_B2_EMUL_OP_RESET, summary);
    // Jump directly to the INSTALL_DRIVERS site, bypassing all intermediate
    // init that triggers uninitialized high-trap recursion.  Basilisk's
    // EMUL_OP_RESET already sets up all required low-memory state.
    patch_abs_jump(rom, rom_size, 0x008eu, LC_BASILISK_ROM_BASE_32 + 0x00bau, summary);
    // Skip GetHardwareInfo + VIA init (ROM offset 0xc2..0xe3), exactly as
    // BasiliskII patch_rom_32 does.  Without this, the threaded-code init at
    // 0xc2 (jmp 0x2f18 = machine-id/hardware dispatch) walks into high-ROM
    // hardware-detection paths the emulator never satisfies, diverging from the
    // real boot rail (RESET -> CLKNOMEM -> PATCH_BOOT_GLOBS -> System).
    // 0xc2: NOP NOP (Don't GetHardwareInfo).  0xc6: 15x NOP (Don't init VIAs).
    if (0x00e4u <= rom_size && rom[0x00c2u] == 0x4eu && rom[0x00c3u] == 0xfau) {
        patch_nops(rom, rom_size, 0x00c2u, 2u, summary);   // GetHardwareInfo jmp (2 words)
        patch_nops(rom, rom_size, 0x00c6u, 15u, summary);  // VIA-init block (15 words)
    }
    // Basilisk runs ROM init with targeted hardware-loop patches instead of
    // skipping all init.  Do not jump $BA->$1142 here; let ROM init run so it
    // can establish A5/BootGlobs/low-memory invariants.  Port missing Basilisk
    // hardware patches as failures appear.
    // patch_abs_jump(rom, rom_size, 0x00bau, LC_BASILISK_ROM_BASE_32 + 0x1142u, summary);
    put16(rom, rom_size, 0x07c0u, 0x7e02u, summary);     // moveq #2,d7: 68020.
    // NOP the BNE.W at $C1A that skips the dispatch magic cookie write.
    // Without this, the A-line dispatcher never gets $5A932BC7 at $0DB0 and
    // falls into the NuBus scanner loop indefinitely.
    if (0x0c1cu < rom_size && rom[0x0c1au] == 0x66u && rom[0x0c1bu] == 0x00u) {
        patch_nops(rom, rom_size, 0x0c1au, 2u, summary);
    }
    put16(rom, rom_size, 0x07c2u, LC_B2_M68K_RTS, summary);

    static const uint8_t clear_globs_dat[] = {0x42, 0x9a, 0x36, 0x0a, 0x66, 0xfa};
    uint32_t base = find_data(rom, rom_size, 0x0a00u, 0x0b00u, clear_globs_dat,
                              sizeof(clear_globs_dat), summary);
    if (base != 0u) patch_nops(rom, rom_size, base + 2u, 2u, summary);

    static const uint8_t init_mmu_dat[] = {0x0c, 0x47, 0x00, 0x03, 0x62, 0x00, 0xfe};
    base = find_data(rom, rom_size, 0x4000u, 0x50000u, init_mmu_dat,
                     sizeof(init_mmu_dat), summary);
    if (base != 0u) {
        patch_nops(rom, rom_size, base, 4u, summary);
        put16(rom, rom_size, base + 10u, 0x7000u, summary);
        put16(rom, rom_size, base + 12u, LC_B2_M68K_NOP, summary);
    }

    static const uint8_t init_mmu2_dat[] = {0x08, 0x06, 0x00, 0x0d, 0x67};
    base = find_data(rom, rom_size, 0x4000u, 0x50000u, init_mmu2_dat,
                     sizeof(init_mmu2_dat), summary);
    if (base != 0u) rom[base + 4u] = 0x60u, summary->patches_applied++;

    static const uint8_t init_mmu3_dat[] = {0x0c, 0x2e, 0x00, 0x01, 0xff, 0xe6, 0x66, 0x0c, 0x4c, 0xed, 0x03, 0x87, 0xff, 0xe8};
    base = find_data(rom, rom_size, 0x4000u, 0x50000u, init_mmu3_dat,
                     sizeof(init_mmu3_dat), summary);
    if (base != 0u) put16(rom, rom_size, base + 6u, LC_B2_M68K_NOP, summary);

    static const uint8_t read_xpram_dat[] = {0x26, 0x4e, 0x41, 0xf9, 0x50, 0xf0,
                                             0x00, 0x00, 0x08, 0x90, 0x00, 0x02};
    base = find_data(rom, rom_size, 0x40000u, 0x50000u, read_xpram_dat,
                     sizeof(read_xpram_dat), summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 0u, LC_B2_EMUL_OP_READ_XPRAM, summary);
        put16(rom, rom_size, base + 2u, 0x4ed6u, summary); // jmp (a6)
    }

    static const uint8_t read_xpram2_dat[] = {0x26, 0x4e, 0x08, 0x92, 0x00, 0x02,
                                              0xea, 0x59, 0x02, 0x01, 0x00, 0x07,
                                              0x00, 0x01, 0x00, 0xb8};
    base = find_data(rom, rom_size, 0x40000u, 0x50000u, read_xpram2_dat,
                     sizeof(read_xpram2_dat), summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 0u, LC_B2_EMUL_OP_READ_XPRAM, summary);
        put16(rom, rom_size, base + 2u, 0x4ed6u, summary); // jmp (a6)
    }

    base = find_rom_trap(rom, rom_size, 0xa053u, summary);
    if (base != 0u) {
        // find_rom_trap returns the trap-table thunk.  On ROM23/26/27/32 that
        // thunk is just `jmp (a5)` (0x4ed5); the real ClkNoMem routine lives in
        // the 0xb0000..0xb8000 region and must be located by signature, exactly
        // as BasiliskII patch_rom_32 does.  Without this the native CLKNOMEM
        // EMUL_OP never replaces the routine, the real RTC/VIA clock code runs,
        // and boot later diverges into high-ROM memory-layout paths.
        if (base + 1u < rom_size && rom[base] == 0x4eu && rom[base + 1u] == 0xd5u) {
            static const uint8_t clk_no_mem_dat[] = {0x40, 0xc2, 0x00, 0x7c,
                                                     0x07, 0x00, 0x48, 0x42};
            uint32_t real = find_data(rom, rom_size, 0xb0000u, 0xb8000u,
                                      clk_no_mem_dat, sizeof(clk_no_mem_dat),
                                      summary);
            if (real != 0u) base = real;
        }
        put16(rom, rom_size, base + 0u, LC_B2_EMUL_OP_CLKNOMEM, summary);
        put16(rom, rom_size, base + 2u, 0x4ed5u, summary); // jmp (a5)
    }

    put16(rom, rom_size, 0x010eu, LC_B2_EMUL_OP_PATCH_BOOT_GLOBS, summary);
    put16(rom, rom_size, 0x0110u, LC_B2_M68K_NOP, summary);

    static const uint8_t init_scc_dat[] = {0x08, 0x38, 0x00, 0x01, 0x0d, 0xd1, 0x67, 0x04};
    base = find_data(rom, rom_size, 0x0a00u, 0x0a80u, init_scc_dat,
                     sizeof(init_scc_dat), summary);
    if (base != 0u) put16(rom, rom_size, base, LC_B2_M68K_RTS, summary);

    put16(rom, rom_size, 0x09c0u, LC_B2_M68K_RTS, summary); // Don't init IWM.
    put16(rom, rom_size, 0x09a0u, LC_B2_M68K_RTS, summary); // Don't init SCSI.
    put16(rom, rom_size, 0x0a30u, LC_B2_M68K_RTS, summary); // Don't init SCC.
    if (getenv("LC_FAITHFUL_DISK_BOOT") == NULL) {
        patch_nops(rom, rom_size, 0x014cu, 2u, summary);   // Don't clear trap table ($1292 clears $100-$2000).
    }
    // The $160-$1d3 block is the dispatch/SCC/VIA init.  Our default scaffold
    // NOPs the whole thing, which also skips the trap-dispatch build (InitOS at
    // 0x999e / 0x9a9a) — so the ROM never builds its real trap table.  Under
    // faithful disk boot, let this block run (the surgical hardware RTS's above
    // — IWM/SCSI/SCC/VIA — keep the hardware bits inert) so InitOS builds the
    // real dispatch table, the way BasiliskII does.
    if (getenv("LC_FAITHFUL_DISK_BOOT") == NULL) {
        patch_nops(rom, rom_size, 0x0160u, 58u, summary);  // Skip all dispatch/SCC/VIA init ($160-$1d3).
    }
    // NOP $11e (memory test bit): 2 words.
    patch_nops(rom, rom_size, 0x011eu, 2u, summary);
    put16(rom, rom_size, 0x9f4cu, LC_B2_M68K_RTS, summary); // Don't DisableIntSources.

    // SetupTimeK fake CPU timing.
    put16(rom, rom_size, 0x0800u, 0x31fcu, summary); put16(rom, rom_size, 0x0802u, 10000u, summary); put16(rom, rom_size, 0x0804u, 0x0d00u, summary);
    put16(rom, rom_size, 0x0806u, 0x31fcu, summary); put16(rom, rom_size, 0x0808u, 10000u, summary); put16(rom, rom_size, 0x080au, 0x0d02u, summary);
    put16(rom, rom_size, 0x080cu, 0x31fcu, summary); put16(rom, rom_size, 0x080eu, 10000u, summary); put16(rom, rom_size, 0x0810u, 0x0b24u, summary);
    put16(rom, rom_size, 0x0812u, 0x31fcu, summary); put16(rom, rom_size, 0x0814u, 10000u, summary); put16(rom, rom_size, 0x0816u, 0x0ceau, summary);
    put16(rom, rom_size, 0x0818u, LC_B2_M68K_RTS, summary);

    // InitTimeMgr VIA write removal.
    put16(rom, rom_size, 0xb0e2u, 0x4cdfu, summary);
    put16(rom, rom_size, 0xb0e4u, 0x1f3fu, summary);
    put16(rom, rom_size, 0xb0e6u, LC_B2_M68K_RTS, summary);

    // Don't EnableOneSecInts / EnableParityPatch / Enable60HzInts / EnableSlotInts.
    static const uint8_t lea_dat[] = {0x41, 0xf9};
    base = find_data(rom, rom_size, 0x0226u, 0x022au, lea_dat, sizeof(lea_dat), summary);
    if (base != 0u) patch_nops(rom, rom_size, base, 5u, summary);
    base = find_data(rom, rom_size, 0x0230u, 0x0234u, lea_dat, sizeof(lea_dat), summary);
    if (base != 0u) patch_nops(rom, rom_size, base, 5u, summary);
    base = find_data(rom, rom_size, 0x02eeu, 0x02f2u, lea_dat, sizeof(lea_dat), summary);
    if (base != 0u) patch_nops(rom, rom_size, base, 5u, summary);

    // Don't mangle frame buffer base (Basilisk keeps MacFrameBaseMac stable).
    patch_nops(rom, rom_size, 0x5b78u, 2u, summary);
    put16(rom, rom_size, 0x5b7cu, 0x2401u, summary);
    put16(rom, rom_size, 0x5b7eu, 0x605eu, summary);

    // Don't access 0x50f1a101 (ROM32 hardware scratch path).
    if (0x423cu < rom_size && be16_at(&rom[0x4234u]) == 0x50f1u &&
        be16_at(&rom[0x4236u]) == 0xa101u) {
        patch_nops(rom, rom_size, 0x4232u, 5u, summary);
    }

    // Don't init ASC.  Basilisk patches this to jmp (a6), returning via the ROM
    // continuation register instead of touching absent ASC hardware.
    static const uint8_t init_asc_dat[] = {0x26, 0x68, 0x00, 0x30, 0x12, 0x00, 0xeb, 0x01};
    base = find_data(rom, rom_size, 0x4000u, 0x5000u, init_asc_dat,
                     sizeof(init_asc_dat), summary);
    if (base != 0u) put16(rom, rom_size, base, 0x4ed6u, summary); // jmp (a6)

    static const uint8_t model_id2_dat[] = {0x45, 0xf9, 0x5f, 0xff, 0xff, 0xfc, 0x20, 0x12};
    base = find_data(rom, rom_size, 0x4000u, 0x5000u, model_id2_dat,
                     sizeof(model_id2_dat), summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 6u, 0x7000u, summary); // moveq #0,d0
        put16(rom, rom_size, base + 8u, 0xb040u, summary); // cmp.w d0,d0
        put16(rom, rom_size, base + 10u, 0x4ed6u, summary); // jmp (a6)
    }

    static const uint8_t via2_dat[] = {0x20, 0x78, 0x0c, 0xec, 0x11, 0x7c, 0x00, 0x90};
    base = find_data(rom, rom_size, 0xa000u, 0xa400u, via2_dat, sizeof(via2_dat), summary);
    if (base != 0u) put16(rom, rom_size, base + 4u, LC_B2_M68K_RTS, summary);

    static const uint8_t via2b_dat[] = {0x20, 0x78, 0x0c, 0xec, 0x11, 0x7c, 0x00, 0x90,
                                        0x00, 0x13, 0x4e, 0x75};
    base = find_data(rom, rom_size, 0x40000u, 0x44000u, via2b_dat,
                     sizeof(via2b_dat), summary);
    if (base != 0u) put16(rom, rom_size, base + 4u, LC_B2_M68K_RTS, summary);

    // Compute boot stack and fix memory size, matching Basilisk's CompBootStack patch.
    const uint16_t comp_boot_stack[] = {
        0x2038, 0x010c, 0xd0b8, 0x02a6, 0xe288, 0x0880, 0x0000, 0x0440, 0x0400,
        0x2040, LC_B2_EMUL_OP_FIX_MEMSIZE, LC_B2_M68K_RTS,
    };
    for (size_t i = 0; i < sizeof(comp_boot_stack) / sizeof(comp_boot_stack[0]); i++) {
        put16(rom, rom_size, 0x0490u + (uint32_t)i * 2u, comp_boot_stack[i], summary);
    }

    static const uint8_t fix_memsize2_dat[] = {0x22, 0x30, 0x81, 0xe2, 0x0d, 0xdc,
                                               0xff, 0xba, 0xd2, 0xb0, 0x81, 0xe2,
                                               0x0d, 0xdc, 0xff, 0xec, 0x21, 0xc1,
                                               0x1e, 0xf8};
    base = find_data(rom, rom_size, 0x4c000u, 0x4c080u, fix_memsize2_dat,
                     sizeof(fix_memsize2_dat), summary);
    if (base != 0u) patch_nops(rom, rom_size, base + 16u, 2u, summary);

    // The direct host path installs Basilisk's replacement .Sony/.Disk drivers
    // below, so do not enter the ROM's original early .Sony Open path first.
    // That path depends on a fully rebuilt Resource Manager map and otherwise
    // prevents reaching Basilisk's patched .Sound/InstallDrivers site.
    patch_nops(rom, rom_size, 0x1134u, 2u, summary);
    put16(rom, rom_size, 0x1142u, LC_B2_EMUL_OP_INSTALL_DRIVERS, summary);
    // NOP everything from $1144 to $1181 (31 words = 62 bytes).
    patch_nops(rom, rom_size, 0x1144u, 31u, summary);
    // After INSTALL_DRIVERS, load and execute boot blocks from disk.
    // Minimal stub: set up ioParam block, fire DISK_PRIME, jump to boot blocks.
    static const uint8_t boot_loader[] = {
        // sub.w #80,sp (allocate 80-byte param block)
        0x9e, 0xfc, 0x00, 0x50,
        // move.l sp,a0 (A0 = param block)
        0x20, 0x4f,
        // Clear param block: move.l #0,d0 / move.w #19,d1 / .clr: move.l d0,(a0)+ / dbf d1,.clr
        0x70, 0x00,  // moveq #0,d0
        0x32, 0x3c, 0x00, 0x13,  // move.w #19,d1
        0x20, 0xc0,  // .clr: move.l d0,(a0)+
        0x51, 0xc9, 0xff, 0xfc,  // dbf d1,.clr
        // Restore a0 to param block base
        0x20, 0x4f,  // move.l sp,a0
        // ioTrap = $0002 (read command) at offset 6
        0x31, 0x7c, 0x00, 0x02, 0x00, 0x06,  // move.w #2,6(a0)
        // ioBuffer = $800 (param block offset 32)
        0x21, 0x7c, 0x00, 0x00, 0x08, 0x00, 0x00, 0x20,  // move.l #$800,32(a0)
        // ioReqCount = 1024 (param block offset 36)
        0x21, 0x7c, 0x00, 0x00, 0x04, 0x00, 0x00, 0x24,  // move.l #1024,36(a0)
        // ioPosOffset = 0 (already zero from clear)
        // ioRefNum = -63 (disk driver, at offset 24)
        0x31, 0x7c, 0xff, 0xc1, 0x00, 0x18,  // move.w #-63,24(a0)
        // ioVRefNum = 1 at offset 22
        0x31, 0x7c, 0x00, 0x01, 0x00, 0x16,  // move.w #1,22(a0)
        // Set up A1 = dce (use a separate area at $8800 for dce)
        0x22, 0x7c, 0x00, 0x00, 0x88, 0x00,  // move.l #$8800,a1
        // Fire DISK_PRIME EMUL_OP (A0=pb, A1=dce)
        LC_B2_EMUL_OP_DISK_PRIME >> 8, LC_B2_EMUL_OP_DISK_PRIME & 0xff,
        // Check boot block signature at $800: should be $4C4B ('LK')
        0x0c, 0x78, 0x4c, 0x4b, 0x08, 0x00,  // cmpi.w #$4C4B,$800
        // If not LK, spin: bne.s *
        0x66, 0xfe,
        // Jump to boot block entry at $802
        0x4e, 0xf8, 0x08, 0x02,  // jmp $802.w
    };
    if (0x1182u + sizeof(boot_loader) <= rom_size) {
        memcpy(&rom[0x1182u], boot_loader, sizeof(boot_loader));
        summary->patches_applied += (uint32_t)sizeof(boot_loader);
    }

    // NOP CompBootStack at ROM offset 0x490.  In Basilisk, this only runs
    // once from the 'boot 3' resource.  Without disk resources loaded, the
    // ROM re-enters it via uninitialized high-trap recursion, consuming stack.
    // Replace with moveq #0,d0; rts so it returns immediately.
    put16(rom, rom_size, 0x0490u, 0x7000u, summary); // moveq #0,d0
    put16(rom, rom_size, 0x0492u, LC_B2_M68K_RTS, summary);

    // NOP _SndDisposeChannel (A995) at ROM offset 0x11e4.  The high-trap table
    // isn't populated for this trap, so dispatch falls into ResourceMgr and
    // creates a reentry loop.  No sound hardware exists.
    put16(rom, rom_size, 0x11e4u, LC_B2_M68K_NOP, summary);

    // The _ShutDown handler for A05B isn't in this ROM's trap table.
    // NOP the problematic A8B4/A895 call sites that corrupt the stack.
    put16(rom, rom_size, 0x057au, LC_B2_M68K_NOP, summary); // was A8B4 _Control
    put16(rom, rom_size, 0x0580u, LC_B2_M68K_NOP, summary); // was clr.w -(sp)
    put16(rom, rom_size, 0x0582u, LC_B2_M68K_NOP, summary); // was A895
    // Skip directly to the INSTALL_DRIVERS site at 0x1142.
    // Patch 0x586 with JMP 0x40801142 (6-byte absolute JMP).
    put16(rom, rom_size, 0x0586u, 0x4ef9u, summary); // JMP abs.l
    put16(rom, rom_size, 0x0588u, 0x4080u, summary); // high word
    put16(rom, rom_size, 0x058au, 0x1142u, summary); // low word

    // Don't write to VIA in InitADB.  This mirrors Basilisk's ROM32 patch for
    // ROMs with the late InitADB code at 0xb2c6a/0xb2d2e.
    if (0xa8a9u < rom_size && be16_at(&rom[0xa8a8u]) == 0u) {
        patch_nops(rom, rom_size, 0xb2c6au, 3u, summary);
        patch_nops(rom, rom_size, 0xb2d2eu, 12u, summary);
        patch_nops(rom, rom_size, 0xb2d4au, 2u, summary);
    } else {
        patch_nops(rom, rom_size, 0xa8a8u, 3u, summary);
        patch_nops(rom, rom_size, 0xa662u, 5u, summary);
        patch_nops(rom, rom_size, 0xa672u, 2u, summary);
    }

    // Time Manager traps, DebugUtil, Microseconds and SCSI dispatch through the
    // same EMUL_OP boundary Basilisk uses.  RmvTime/PrimeTime run with
    // interrupts masked in Basilisk, preserving the ROM's SR dance here too.
    base = find_rom_trap(rom, rom_size, 0xa058u, summary);
    if (base != 0u) { put16(rom, rom_size, base, LC_B2_EMUL_OP_INSTIME, summary); put16(rom, rom_size, base + 2u, LC_B2_M68K_RTS, summary); }
    base = find_rom_trap(rom, rom_size, 0xa059u, summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 0u, 0x40e7u, summary); // move sr,-(sp)
        put16(rom, rom_size, base + 2u, 0x007cu, summary); put16(rom, rom_size, base + 4u, 0x0700u, summary);
        put16(rom, rom_size, base + 6u, LC_B2_EMUL_OP_RMVTIME, summary);
        put16(rom, rom_size, base + 8u, 0x46dfu, summary); // move (sp)+,sr
        put16(rom, rom_size, base + 10u, LC_B2_M68K_RTS, summary);
    }
    base = find_rom_trap(rom, rom_size, 0xa05au, summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 0u, 0x40e7u, summary); // move sr,-(sp)
        put16(rom, rom_size, base + 2u, 0x007cu, summary); put16(rom, rom_size, base + 4u, 0x0700u, summary);
        put16(rom, rom_size, base + 6u, LC_B2_EMUL_OP_PRIMETIME, summary);
        put16(rom, rom_size, base + 8u, 0x46dfu, summary); // move (sp)+,sr
        put16(rom, rom_size, base + 10u, LC_B2_M68K_RTS, summary);
    }
    base = find_rom_trap(rom, rom_size, 0xa093u, summary);
    if (base != 0u) { put16(rom, rom_size, base, LC_B2_EMUL_OP_MICROSECONDS, summary); put16(rom, rom_size, base + 2u, LC_B2_M68K_RTS, summary); }
    base = find_rom_trap(rom, rom_size, 0xa05bu, summary);
    if (base != 0u) {
        put16(rom, rom_size, base, LC_B2_EMUL_OP_SHUTDOWN, summary);
        // NOP the A895 call and RTS that follow the handler entry so
        // fallthrough from the preceding _Control (A8B4) path doesn't
        // re-enter an uninitialized high-trap dispatch.
        put16(rom, rom_size, base + 2u, LC_B2_M68K_NOP, summary);
        put16(rom, rom_size, base + 4u, LC_B2_M68K_NOP, summary);
    }
    base = find_rom_trap(rom, rom_size, 0xa815u, summary);
    if (base != 0u) { put16(rom, rom_size, base, LC_B2_EMUL_OP_SCSI_DISPATCH, summary); put16(rom, rom_size, base + 2u, 0x2e49u, summary); put16(rom, rom_size, base + 4u, LC_B2_M68K_JMP_A0, summary); }
    base = find_rom_trap(rom, rom_size, 0xa07cu, summary);
    if (base != 0u && base + sizeof(lc_b2_adbop_patch) < rom_size) {
        memcpy(&rom[base], lc_b2_adbop_patch, sizeof(lc_b2_adbop_patch));
        summary->patches_applied += (uint32_t)sizeof(lc_b2_adbop_patch);
    }

    // VIA interrupt handler subset from Basilisk patch_rom_32().
    put16(rom, rom_size, 0x9bc4u, 0x7002u, summary);
    patch_nops(rom, rom_size, 0x9bc6u, 4u, summary);
    patch_nops(rom, rom_size, 0xa296u, 2u, summary);
    put16(rom, rom_size, 0xa29au, LC_B2_EMUL_OP_IRQ, summary);
    put16(rom, rom_size, 0xa29cu, 0x4a80u, summary);
    put16(rom, rom_size, 0xa29eu, 0x67f4u, summary);

    static const uint8_t nubus_dat[] = {0x45, 0xfa, 0x00, 0x0a, 0x42, 0xa7, 0x10, 0x11};
    base = find_data(rom, rom_size, 0x5000u, 0x6000u, nubus_dat, sizeof(nubus_dat), summary);
    if (base != 0u) patch_nops(rom, rom_size, base + 6u, 3u, summary);

    // Really don't mangle the framebuffer base on larger ROMs.
    static const uint8_t frame_base_dat[] = {0x22, 0x78, 0x0d, 0xd8, 0xd3, 0xe9, 0x00, 0x08};
    base = find_data(rom, rom_size, 0x8c000u, 0x8d000u, frame_base_dat,
                     sizeof(frame_base_dat), summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 0u, 0x2401u, summary); // move.l d1,d2
        put16(rom, rom_size, base + 2u, LC_B2_M68K_RTS, summary);
    }

    static const uint8_t block_move_dat[] = {0x20, 0x5f, 0x22, 0x5f, 0x0c, 0x38,
                                             0x00, 0x04, 0x01, 0x2f};
    base = find_data(rom, rom_size, 0x87000u, 0x87800u, block_move_dat,
                     sizeof(block_move_dat), summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 4u, LC_B2_EMUL_OP_BLOCK_MOVE, summary);
        put16(rom, rom_size, base + 6u, 0x7000u, summary); // moveq #0,d0
        put16(rom, rom_size, base + 8u, LC_B2_M68K_RTS, summary);
    }

    static const uint8_t ptest2_dat[] = {0x0c, 0x38, 0x00, 0x04, 0x01, 0x2f,
                                         0x6d, 0x54, 0x48, 0xe7, 0xf8, 0x60};
    base = find_data(rom, rom_size, 0u, (uint32_t)rom_size, ptest2_dat,
                     sizeof(ptest2_dat), summary);
    if (base != 0u) {
        put16(rom, rom_size, base + 8u, LC_B2_M68K_NOP, summary);
        put16(rom, rom_size, base + 10u, 0xf4f8u, summary); // cpusha dc/ic
        put16(rom, rom_size, base + 12u, LC_B2_M68K_NOP, summary);
        put16(rom, rom_size, base + 14u, 0x7000u, summary); // moveq #0,d0
        put16(rom, rom_size, base + 16u, LC_B2_M68K_RTS, summary);
    }

    // Keep MemoryDispatch implemented; otherwise the ROM can install an
    // unimplemented trap while Resource/Memory Manager is still starting up.
    static const uint8_t memdisp_dat[] = {0x30, 0x3c, 0xa8, 0x9f, 0xa7, 0x46,
                                          0x30, 0x3c, 0xa0, 0x5c, 0xa2, 0x47};
    base = find_data(rom, rom_size, 0x4f100u, 0x4f180u, memdisp_dat,
                     sizeof(memdisp_dat), summary);
    if (base != 0u) put16(rom, rom_size, base + 10u, LC_B2_M68K_NOP, summary);

    // Patch .EDisk so it doesn't scan the ROMBase..0xe00000 region as RAM.
    const uint32_t edisk_offset = find_rom_resource(rom, rom_size, LC_B2_FOURCC('D','R','V','R'), 51, summary);
    if (edisk_offset != 0u) {
        static const uint8_t edisk_dat[] = {0xd5, 0xfc, 0x00, 0x01, 0x00, 0x00,
                                            0xb5, 0xfc, 0x00, 0xe0, 0x00, 0x00};
        base = find_data(rom, rom_size, edisk_offset, edisk_offset + 0x10000u,
                         edisk_dat, sizeof(edisk_dat), summary);
        if (base != 0u) {
            put16(rom, rom_size, base + 8u, 0u, summary);
            put16(rom, rom_size, base + 10u, 0u, summary);
        }
    }

    const uint32_t sony_offset = find_rom_resource(rom, rom_size, LC_B2_FOURCC('D','R','V','R'), 4, summary);
    if (sony_offset != 0u && sony_offset + 0x400u + sizeof(lc_b2_cdrom_driver) < rom_size) {
        memcpy(&rom[sony_offset], lc_b2_sony_driver, sizeof(lc_b2_sony_driver));
        memcpy(&rom[sony_offset + 0x100u], lc_b2_disk_driver, sizeof(lc_b2_disk_driver));
        memcpy(&rom[sony_offset + 0x200u], lc_b2_cdrom_driver, sizeof(lc_b2_cdrom_driver));
        summary->patches_applied += (uint32_t)(sizeof(lc_b2_sony_driver) +
                                               sizeof(lc_b2_disk_driver) +
                                               sizeof(lc_b2_cdrom_driver));

        // Basilisk hooks vCheckLoad so resource loads can be observed/patched
        // after the ROM's normal loader has done its work.
        patch_abs_jump(rom, rom_size, 0x1b8f4u, LC_BASILISK_ROM_BASE_32 + sony_offset + 0x300u, summary);
        uint32_t p = sony_offset + 0x300u;
        put16(rom, rom_size, p + 0u, 0x2f03u, summary); // move.l d3,-(sp) save type
        put16(rom, rom_size, p + 2u, 0x2078u, summary); put16(rom, rom_size, p + 4u, 0x07f0u, summary);
        put16(rom, rom_size, p + 6u, LC_B2_M68K_JSR_A0, summary);
        put16(rom, rom_size, p + 8u, 0x221fu, summary); // move.l (sp)+,d1 restore type
        put16(rom, rom_size, p + 10u, LC_B2_EMUL_OP_CHECKLOAD, summary);
        put16(rom, rom_size, p + 12u, LC_B2_M68K_RTS, summary);

        ESP_LOGW(TAG, "Basilisk replacement .Sony/.Disk/.AppleCD drivers installed: sony=0x%05" PRIx32 " disk=0x%05" PRIx32 " cdrom=0x%05" PRIx32 " checkload=0x%05" PRIx32,
                 sony_offset, sony_offset + 0x100u, sony_offset + 0x200u, sony_offset + 0x300u);
    } else {
        summary->patch_patterns_missing++;
    }

    static const uint8_t poll_0172[] = {0x4a, 0x38, 0x01, 0x72, 0x66, 0xfa};
    base = find_data(rom, rom_size, 0x2a00u, 0x2b00u, poll_0172, sizeof(poll_0172), summary);
    if (base != 0u) patch_nops(rom, rom_size, base, 3u, summary);

    static const uint8_t poll_016a[] = {0xb0, 0xb8, 0x01, 0x6a, 0x6e, 0xfa};
    base = find_data(rom, rom_size, 0xc000u, 0xc200u, poll_016a, sizeof(poll_016a), summary);
    if (base != 0u) patch_nops(rom, rom_size, base, 3u, summary);

    uint32_t slot_off = 0, slot_size = 0;
    if (lc_basilisk_install_minimal_slot_rom(rom, rom_size, LC_BASILISK_FRAME_BASE_MAC,
                                             512u, 384u, 512u, &slot_off, &slot_size) == ESP_OK) {
        summary->patches_applied += slot_size;
    } else {
        summary->patch_patterns_missing++;
    }

    ESP_LOGW(TAG,
             "Basilisk ROM32 compatibility patches applied: version=0x%04x universal=0x%05" PRIx32
             " patches=%" PRIu32 " patterns_found=%" PRIu32 " patterns_missing=%" PRIu32,
             summary->rom_version, summary->universal_info_offset, summary->patches_applied,
             summary->patch_patterns_found, summary->patch_patterns_missing);
    return ESP_OK;
}

// Public wrapper: resolve the ROM handler offset for an A-trap via the ROM's
// trap dispatch table (signed branch-table offsets).  Returns 0 if the trap is
// unimplemented or out of range.  Used by the faithful-disk-boot path to seed
// the OS trap table with real ROM handlers instead of the no-op scaffold.
uint32_t lc_basilisk_find_rom_trap(const uint8_t *rom, size_t rom_size, uint16_t trap) {
    return find_rom_trap(rom, rom_size, trap, NULL);
}
