#include "esp_err.h"
#include "esp_partition.h"
#include "machine_lc/lc_basilisk_compat.h"
#include "machine_lc/lc_cpu.h"
#include "machine_lc/lc_disk.h"
#include "machine_lc/lc_memory.h"
#include "machine_lc/lc_perf.h"
#include "machine_lc/lc_rom.h"
#include "machine_lc/lc_trace.h"
#include "machine_lc/lc_video.h"
#include "m68k.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HOST_LC_DEFAULT_ROM_PATH
#define HOST_LC_DEFAULT_ROM_PATH "vendor/mac-lc.rom"
#endif

#ifndef HOST_LC_DEFAULT_DISK_PATH
#define HOST_LC_DEFAULT_DISK_PATH "vendor/lc-disk.img"
#endif

#ifndef HOST_LC_DEFAULT_PPM_PATH
#define HOST_LC_DEFAULT_PPM_PATH "artifacts/host-lc-video-test-pattern.ppm"
#endif

#ifndef LC_PRODUCTINFO_DEFAULT_RSRCS
#define LC_PRODUCTINFO_DEFAULT_RSRCS 0
#endif

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [--rom PATH] [--disk PATH] [--ppm PATH] [--rom-probe] [--basilisk-compat] [--no-video] [--no-synthetic]\n"
            "\n"
            "Runs the Macintosh LC core natively on the host using ESP-IDF stubs.\n"
            "Default ROM:  %s\n"
            "Default disk: %s\n"
            "Default PPM:  %s\n",
            argv0, HOST_LC_DEFAULT_ROM_PATH, HOST_LC_DEFAULT_DISK_PATH,
            HOST_LC_DEFAULT_PPM_PATH);
}

static uint8_t rgb565_r(uint16_t v) {
    uint8_t r = (uint8_t)((v >> 11u) & 0x1fu);
    return (uint8_t)((r << 3u) | (r >> 2u));
}

static uint8_t rgb565_g(uint16_t v) {
    uint8_t g = (uint8_t)((v >> 5u) & 0x3fu);
    return (uint8_t)((g << 2u) | (g >> 4u));
}

static uint8_t rgb565_b(uint16_t v) {
    uint8_t b = (uint8_t)(v & 0x1fu);
    return (uint8_t)((b << 3u) | (b >> 2u));
}

static uint32_t fnv1a_bytes(const uint8_t *bytes, size_t size) {
    uint32_t checksum = 2166136261u;
    if (bytes == NULL) {
        return checksum;
    }
    for (size_t i = 0; i < size; i++) {
        checksum ^= bytes[i];
        checksum *= 16777619u;
    }
    return checksum;
}

static esp_err_t write_indexed_grayscale_ppm(const char *path, const uint8_t *indexed,
                                             size_t indexed_size, const char *label) {
    if (indexed == NULL || indexed_size < LC_VIDEO_INDEXED_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        perror(path);
        return ESP_FAIL;
    }
    fprintf(fp, "P6\n%u %u\n255\n", (unsigned)LC_VIDEO_WIDTH, (unsigned)LC_VIDEO_HEIGHT);
    for (size_t y = 0; y < LC_VIDEO_HEIGHT; y++) {
        const uint8_t *row = &indexed[y * LC_VIDEO_ROWBYTES];
        for (size_t x = 0; x < LC_VIDEO_WIDTH; x++) {
            const uint8_t pixel = row[x];
            const uint8_t bytes[3] = {pixel, pixel, pixel};
            fwrite(bytes, 1, sizeof(bytes), fp);
        }
    }
    fclose(fp);
    printf("HOST_LC_INDEXED_PPM label=%s path=%s checksum=0x%08x\n",
           label != NULL ? label : "indexed", path, fnv1a_bytes(indexed, LC_VIDEO_INDEXED_SIZE));
    return ESP_OK;
}

static esp_err_t write_video_ppm(const char *path) {
    uint8_t *indexed = (uint8_t *)malloc(LC_VIDEO_INDEXED_SIZE);
    uint16_t *rgb = (uint16_t *)malloc((size_t)LC_VIDEO_WIDTH * (size_t)LC_VIDEO_HEIGHT * sizeof(uint16_t));
    uint16_t palette[LC_VIDEO_CLUT_ENTRIES] = {0};
    if (indexed == NULL || rgb == NULL) {
        free(indexed);
        free(rgb);
        return ESP_ERR_NO_MEM;
    }

    lc_video_init_debug_palette(palette);
    const uint32_t indexed_checksum = lc_video_fill_test_pattern(indexed, LC_VIDEO_INDEXED_SIZE);
    const uint32_t rgb_checksum = lc_video_convert_indexed_rows_to_rgb565(
        indexed, LC_VIDEO_INDEXED_SIZE, 0, LC_VIDEO_HEIGHT, palette, rgb,
        (size_t)LC_VIDEO_WIDTH * (size_t)LC_VIDEO_HEIGHT);

    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        perror(path);
        free(indexed);
        free(rgb);
        return ESP_FAIL;
    }
    fprintf(fp, "P6\n%u %u\n255\n", (unsigned)LC_VIDEO_WIDTH, (unsigned)LC_VIDEO_HEIGHT);
    for (size_t i = 0; i < (size_t)LC_VIDEO_WIDTH * (size_t)LC_VIDEO_HEIGHT; i++) {
        const uint8_t bytes[3] = {rgb565_r(rgb[i]), rgb565_g(rgb[i]), rgb565_b(rgb[i])};
        fwrite(bytes, 1, sizeof(bytes), fp);
    }
    fclose(fp);

    printf("HOST_LC_VIDEO_PPM path=%s indexed_checksum=0x%08x rgb565_checksum=0x%08x\n",
           path, indexed_checksum, rgb_checksum);
    free(indexed);
    free(rgb);
    return ESP_OK;
}

static uint16_t tiny_glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
    case '0': return 0x7b6fu; case '1': return 0x2492u; case '2': return 0x73e7u;
    case '3': return 0x73cfu; case '4': return 0x5bc9u; case '5': return 0x79cfu;
    case '6': return 0x79efu; case '7': return 0x7249u; case '8': return 0x7befu;
    case '9': return 0x7bcfu; case 'A': return 0x7bedu; case 'B': return 0x7aefu;
    case 'C': return 0x7927u; case 'D': return 0x6b6eu; case 'E': return 0x79e7u;
    case 'F': return 0x79e4u; case 'G': return 0x79afu; case 'H': return 0x5bedu;
    case 'I': return 0x7497u; case 'J': return 0x2496u; case 'K': return 0x5badu;
    case 'L': return 0x4927u; case 'M': return 0x5ffdu; case 'N': return 0x5fedu;
    case 'O': return 0x5b6du; case 'P': return 0x7be4u; case 'Q': return 0x5b7fu;
    case 'R': return 0x7bedu; case 'S': return 0x79cfu; case 'T': return 0x7492u;
    case 'U': return 0x5b6fu; case 'V': return 0x5b6au; case 'W': return 0x5fffu;
    case 'X': return 0x5aadu; case 'Y': return 0x5ba4u; case 'Z': return 0x72e7u;
    case ':': return 0x0909u; case '.': return 0x0002u; case '-': return 0x01c0u;
    case '/': return 0x1248u; case '_': return 0x0007u; case '*': return 0x2a80u;
    case '+': return 0x0ba0u; case '=': return 0x0e38u; case ' ': return 0x0000u;
    default: return 0x01c0u;
    }
}

static void draw_tiny_text(uint8_t *pixels, size_t size, unsigned col, unsigned row, const char *text) {
    if (pixels == NULL || size < LC_VIDEO_INDEXED_SIZE || text == NULL) {
        return;
    }
    unsigned cursor = col;
    for (const char *p = text; *p != '\0'; p++, cursor++) {
        const uint16_t glyph = tiny_glyph(*p);
        const size_t origin_x = (size_t)cursor * 8u;
        const size_t origin_y = (size_t)row * 10u;
        if (origin_x + 6u >= LC_VIDEO_WIDTH || origin_y + 10u >= LC_VIDEO_HEIGHT) {
            break;
        }
        for (size_t gy = 0; gy < 5u; gy++) {
            const unsigned bits = (glyph >> ((4u - gy) * 3u)) & 0x7u;
            for (size_t gx = 0; gx < 3u; gx++) {
                const uint8_t value = (bits & (1u << (2u - gx))) != 0u ? 0xffu : 0x20u;
                for (size_t sy = 0; sy < 2u; sy++) {
                    for (size_t sx = 0; sx < 2u; sx++) {
                        pixels[(origin_y + gy * 2u + sy) * LC_VIDEO_ROWBYTES + origin_x + gx * 2u + sx] = value;
                    }
                }
            }
        }
    }
}

static void fill_rect(uint8_t *pixels, size_t size, unsigned x0, unsigned y0,
                      unsigned w, unsigned h, uint8_t value) {
    if (pixels == NULL || size < LC_VIDEO_INDEXED_SIZE) return;
    for (unsigned y = y0; y < y0 + h && y < LC_VIDEO_HEIGHT; y++) {
        for (unsigned x = x0; x < x0 + w && x < LC_VIDEO_WIDTH; x++) {
            pixels[(size_t)y * LC_VIDEO_ROWBYTES + x] = value;
        }
    }
}

static void format_rom_serial(const lc_memory_bus_t *bus, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (bus == NULL || bus->rom_serial_len == 0u) {
        snprintf(out, out_size, "SCC BYTES NONE YET");
        return;
    }
    size_t n = (size_t)snprintf(out, out_size, "SCC ASCII ");
    for (uint32_t i = 0; i < bus->rom_serial_len && n + 2u < out_size; i++) {
        const uint8_t c = bus->rom_serial_bytes[i];
        out[n++] = (c >= 0x20u && c <= 0x7eu) ? (char)c : '.';
        out[n] = '\0';
    }
}

static void format_rom_serial_hex_line(const lc_memory_bus_t *bus, uint32_t start,
                                       char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (bus == NULL || bus->rom_serial_len == 0u || start >= bus->rom_serial_len) {
        snprintf(out, out_size, "SCC HEX NONE");
        return;
    }
    size_t n = (size_t)snprintf(out, out_size, "SCC HEX %02" PRIX32 " ", start);
    for (uint32_t i = start; i < bus->rom_serial_len && i < start + 12u && n + 4u < out_size; i++) {
        n += (size_t)snprintf(out + n, out_size - n, "%02X ", bus->rom_serial_bytes[i]);
    }
}

static void log_rom_serial_bytes(const lc_memory_bus_t *bus) {
    if (bus == NULL) return;
    char hex[1024];
    char ascii[320];
    size_t hn = 0;
    size_t an = 0;
    hex[0] = '\0';
    ascii[0] = '\0';
    for (uint32_t i = 0; i < bus->rom_serial_len && hn + 4u < sizeof(hex); i++) {
        hn += (size_t)snprintf(hex + hn, sizeof(hex) - hn, "%02X%s",
                               bus->rom_serial_bytes[i], i + 1u < bus->rom_serial_len ? " " : "");
        const uint8_t c = bus->rom_serial_bytes[i];
        if (an + 1u < sizeof(ascii)) {
            ascii[an++] = (c >= 0x20u && c <= 0x7eu) ? (char)c : '.';
            ascii[an] = '\0';
        }
    }
    printf("HOST_LC_ROM_SERIAL total=%u stored=%u hex=%s ascii=%s\n",
           (unsigned)bus->rom_serial_total, (unsigned)bus->rom_serial_len,
           hex[0] != '\0' ? hex : "none", ascii[0] != '\0' ? ascii : "none");
}

static size_t find_rom_bytes_from(const lc_memory_bus_t *bus, const char *needle,
                                  size_t start) {
    if (bus == NULL || bus->rom == NULL || needle == NULL) {
        return SIZE_MAX;
    }
    const size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > bus->rom_size || start > bus->rom_size - needle_len) {
        return SIZE_MAX;
    }
    for (size_t i = start; i + needle_len <= bus->rom_size; i++) {
        if (memcmp(&bus->rom[i], needle, needle_len) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t find_rom_bytes(const lc_memory_bus_t *bus, const char *needle) {
    return find_rom_bytes_from(bus, needle, 0);
}

static void copy_rom_printable_at(const lc_memory_bus_t *bus, size_t offset,
                                  char *out, size_t out_size) {
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (bus == NULL || bus->rom == NULL || offset >= bus->rom_size) return;
    size_t n = 0;
    for (size_t i = offset; i < bus->rom_size && n + 1u < out_size; i++) {
        const uint8_t c = bus->rom[i];
        if (c < 0x20u || c > 0x7eu) {
            break;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

static void draw_rom_cursor_resource(uint8_t *pixels, size_t size, const lc_memory_bus_t *bus,
                                     size_t curs_offset, unsigned x0, unsigned y0,
                                     unsigned scale) {
    if (pixels == NULL || size < LC_VIDEO_INDEXED_SIZE || bus == NULL || bus->rom == NULL ||
        curs_offset == SIZE_MAX || curs_offset + 0x40u > bus->rom_size || scale == 0u) {
        return;
    }
    const size_t bitmap = curs_offset + 0x20u;
    fill_rect(pixels, size, x0 - 3u, y0 - 3u, 16u * scale + 6u, 16u * scale + 6u, 0x08u);
    fill_rect(pixels, size, x0 - 1u, y0 - 1u, 16u * scale + 2u, 16u * scale + 2u, 0xf8u);
    for (unsigned row = 0; row < 16u; row++) {
        const uint16_t bits = (uint16_t)(((uint16_t)bus->rom[bitmap + row * 2u] << 8u) |
                                         bus->rom[bitmap + row * 2u + 1u]);
        const uint16_t mask = (uint16_t)(((uint16_t)bus->rom[bitmap + 32u + row * 2u] << 8u) |
                                         bus->rom[bitmap + 32u + row * 2u + 1u]);
        for (unsigned col = 0; col < 16u; col++) {
            const bool opaque = (mask & (uint16_t)(0x8000u >> col)) != 0;
            const bool on = (bits & (uint16_t)(0x8000u >> col)) != 0;
            if (!opaque) {
                continue;
            }
            fill_rect(pixels, size, x0 + col * scale, y0 + row * scale,
                      scale, scale, on ? 0x00u : 0xffu);
        }
    }
}

static void draw_checker(uint8_t *pixels, size_t size, unsigned x0, unsigned y0,
                         unsigned w, unsigned h) {
    for (unsigned y = y0; y < y0 + h && y < LC_VIDEO_HEIGHT; y++) {
        for (unsigned x = x0; x < x0 + w && x < LC_VIDEO_WIDTH; x++) {
            pixels[(size_t)y * LC_VIDEO_ROWBYTES + x] = ((x / 8u + y / 8u) & 1u) ? 0xb8u : 0x70u;
        }
    }
}

static void draw_classic_window(uint8_t *pixels, size_t size, unsigned x, unsigned y,
                                unsigned w, unsigned h, const char *title) {
    fill_rect(pixels, size, x + 4u, y + 4u, w, h, 0x08u);
    fill_rect(pixels, size, x, y, w, h, 0xf8u);
    fill_rect(pixels, size, x, y, w, 2u, 0x00u);
    fill_rect(pixels, size, x, y + h - 2u, w, 2u, 0x00u);
    fill_rect(pixels, size, x, y, 2u, h, 0x00u);
    fill_rect(pixels, size, x + w - 2u, y, 2u, h, 0x00u);
    fill_rect(pixels, size, x + 2u, y + 18u, w - 4u, 2u, 0x00u);
    if (title != NULL) {
        draw_tiny_text(pixels, size, x / 8u + 2u, y / 10u + 1u, title);
    }
}

static void draw_desktop_icon(uint8_t *pixels, size_t size, unsigned x, unsigned y,
                              const char *label, bool trash) {
    fill_rect(pixels, size, x + 6u, y + 4u, 28u, 24u, 0xf8u);
    fill_rect(pixels, size, x + 6u, y + 4u, 28u, 2u, 0x00u);
    fill_rect(pixels, size, x + 6u, y + 26u, 28u, 2u, 0x00u);
    fill_rect(pixels, size, x + 6u, y + 4u, 2u, 24u, 0x00u);
    fill_rect(pixels, size, x + 32u, y + 4u, 2u, 24u, 0x00u);
    if (trash) {
        fill_rect(pixels, size, x + 10u, y + 8u, 20u, 3u, 0x00u);
        for (unsigned i = 0; i < 4u; i++) {
            fill_rect(pixels, size, x + 12u + i * 4u, y + 13u, 2u, 10u, 0x00u);
        }
    } else {
        fill_rect(pixels, size, x + 10u, y + 9u, 20u, 5u, 0x80u);
        fill_rect(pixels, size, x + 10u, y + 17u, 14u, 3u, 0x80u);
        fill_rect(pixels, size, x + 27u, y + 20u, 3u, 3u, 0x00u);
    }
    if (label != NULL) {
        draw_tiny_text(pixels, size, x / 8u, (y + 34u) / 10u, label);
    }
}

static void draw_classic_desktop(uint8_t *pixels, size_t size) {
    draw_checker(pixels, size, 0, 0, LC_VIDEO_WIDTH, LC_VIDEO_HEIGHT);
    fill_rect(pixels, size, 0, 0, LC_VIDEO_WIDTH, 18u, 0xf8u);
    fill_rect(pixels, size, 0, 17u, LC_VIDEO_WIDTH, 2u, 0x00u);
    draw_tiny_text(pixels, size, 1, 1, "*");
    draw_tiny_text(pixels, size, 5, 1, "FILE");
    draw_tiny_text(pixels, size, 12, 1, "EDIT");
    draw_tiny_text(pixels, size, 19, 1, "VIEW");
    draw_tiny_text(pixels, size, 26, 1, "SPECIAL");
    draw_desktop_icon(pixels, size, 430u, 36u, "MAC HD", false);
    draw_desktop_icon(pixels, size, 430u, 120u, "TRASH", true);
}

static void draw_rom_bit_panel(uint8_t *pixels, size_t size, const lc_memory_bus_t *bus,
                               size_t offset, unsigned x0, unsigned y0) {
    if (pixels == NULL || size < LC_VIDEO_INDEXED_SIZE || bus == NULL || bus->rom == NULL ||
        offset == SIZE_MAX || offset + 512u > bus->rom_size) {
        return;
    }
    fill_rect(pixels, size, x0 - 4u, y0 - 4u, 72u, 72u, 0x10u);
    fill_rect(pixels, size, x0 - 2u, y0 - 2u, 68u, 68u, 0xe0u);
    for (unsigned y = 0; y < 64u; y++) {
        for (unsigned x = 0; x < 64u; x++) {
            const size_t bit_index = (size_t)y * 64u + x;
            const uint8_t byte = bus->rom[offset + bit_index / 8u];
            const bool bit = (byte & (uint8_t)(0x80u >> (bit_index & 7u))) != 0;
            pixels[(size_t)(y0 + y) * LC_VIDEO_ROWBYTES + x0 + x] = bit ? 0xffu : 0x30u;
        }
    }
}

static void draw_rom_byte_panel(uint8_t *pixels, size_t size, const lc_memory_bus_t *bus,
                                size_t offset, unsigned x0, unsigned y0,
                                unsigned w, unsigned h) {
    if (pixels == NULL || size < LC_VIDEO_INDEXED_SIZE || bus == NULL || bus->rom == NULL ||
        offset == SIZE_MAX || offset + (size_t)w * h > bus->rom_size) {
        return;
    }
    fill_rect(pixels, size, x0 - 4u, y0 - 4u, w + 8u, h + 8u, 0x10u);
    fill_rect(pixels, size, x0 - 2u, y0 - 2u, w + 4u, h + 4u, 0xf0u);
    for (unsigned y = 0; y < h && y0 + y < LC_VIDEO_HEIGHT; y++) {
        for (unsigned x = 0; x < w && x0 + x < LC_VIDEO_WIDTH; x++) {
            uint8_t v = bus->rom[offset + (size_t)y * w + x];
            // Stretch mid-tones so structured ROM resource data is visible on
            // the grayscale export rather than disappearing into near-black.
            v = (uint8_t)(0x30u + ((unsigned)v * 0xc0u / 0xffu));
            pixels[(size_t)(y0 + y) * LC_VIDEO_ROWBYTES + x0 + x] = v;
        }
    }
}

static void draw_rom_string_row(lc_memory_bus_t *bus, unsigned row, const char *label,
                                const char *needle, size_t offset) {
    char text[128];
    if (offset == SIZE_MAX) {
        return;
    }
    copy_rom_printable_at(bus, offset, text, sizeof(text));
    if (text[0] == '\0' && needle != NULL) {
        snprintf(text, sizeof(text), "%s", needle);
    }
    draw_tiny_text(bus->vram, bus->vram_size, 4, row, label);
    draw_tiny_text(bus->vram, bus->vram_size, 16, row, text);
    printf("HOST_LC_ROM_STRING offset=0x%05zx text=%s\n", offset, text);
}

static void overlay_rom_resource_evidence(lc_memory_bus_t *bus) {
    if (bus == NULL || bus->vram == NULL || bus->rom == NULL) {
        return;
    }
    char text[128];
    const size_t mac_family = find_rom_bytes(bus, "Macintosh Family");
    const size_t mac_ii = find_rom_bytes(bus, "Macintosh II");
    const size_t se30_video = find_rom_bytes(bus, "Macintosh SE/30 Built-In Video");
    const size_t display_rbv = find_rom_bytes(bus, "Display_Video_Apple_RBV1");
    const size_t display_v8 = find_rom_bytes(bus, "Display_Video_Apple_V8");
    const size_t font_chicago = find_rom_bytes_from(bus, "Chicago", 0x00070000u);
    const size_t font_geneva = find_rom_bytes_from(bus, "Geneva", 0x00070000u);
    const size_t font_monaco = find_rom_bytes_from(bus, "Monaco", 0x00070000u);
    const size_t sicn = find_rom_bytes(bus, "SICN");
    const size_t cicn = find_rom_bytes(bus, "cicn");
    const size_t pict1 = find_rom_bytes_from(bus, "PICT", 0x00050000u);
    const size_t pict2 = pict1 != SIZE_MAX ? find_rom_bytes_from(bus, "PICT", pict1 + 4u) : SIZE_MAX;

    draw_tiny_text(bus->vram, bus->vram_size, 4, 23, "ROM RESOURCE DIRECTORY");
    draw_rom_string_row(bus, 25, "MODEL", "Macintosh Family", mac_family);
    draw_rom_string_row(bus, 27, "MACH", "Macintosh II", mac_ii);
    draw_rom_string_row(bus, 29, "VIDEO", "Macintosh SE/30 Built-In Video", se30_video);
    draw_rom_string_row(bus, 31, "RBV", "Display_Video_Apple_RBV1", display_rbv);
    draw_rom_string_row(bus, 33, "V8", "Display_Video_Apple_V8", display_v8);
    if (font_chicago != SIZE_MAX || font_geneva != SIZE_MAX || font_monaco != SIZE_MAX) {
        snprintf(text, sizeof(text), "FONTS %s %s %s",
                 font_chicago != SIZE_MAX ? "CHICAGO" : "-",
                 font_geneva != SIZE_MAX ? "GENEVA" : "-",
                 font_monaco != SIZE_MAX ? "MONACO" : "-");
        draw_tiny_text(bus->vram, bus->vram_size, 4, 36, text);
        printf("HOST_LC_ROM_FONTS chicago=0x%05zx geneva=0x%05zx monaco=0x%05zx\n",
               font_chicago, font_geneva, font_monaco);
    }

    if (sicn != SIZE_MAX) {
        snprintf(text, sizeof(text), "SICN %05" PRIX32, (uint32_t)sicn);
        draw_tiny_text(bus->vram, bus->vram_size, 35, 23, text);
        draw_rom_bit_panel(bus->vram, bus->vram_size, bus, sicn, 360, 54);
        printf("HOST_LC_ROM_BITMAP tag=SICN offset=0x%05zx\n", sicn);
    }
    if (cicn != SIZE_MAX) {
        snprintf(text, sizeof(text), "CICN %05" PRIX32, (uint32_t)cicn);
        draw_tiny_text(bus->vram, bus->vram_size, 35, 32, text);
        draw_rom_bit_panel(bus->vram, bus->vram_size, bus, cicn, 360, 144);
        printf("HOST_LC_ROM_BITMAP tag=cicn offset=0x%05zx\n", cicn);
    }
    if (pict2 != SIZE_MAX) {
        copy_rom_printable_at(bus, pict2 + 8u, text, sizeof(text));
        draw_tiny_text(bus->vram, bus->vram_size, 4, 35, "PICT");
        draw_tiny_text(bus->vram, bus->vram_size, 16, 35, text[0] != '\0' ? text : "PICT BYTES");
        draw_rom_byte_panel(bus->vram, bus->vram_size, bus, pict2 + 0x20u, 236, 78, 112u, 72u);
        draw_rom_bit_panel(bus->vram, bus->vram_size, bus, pict2 + 0x180u, 276, 162);
        printf("HOST_LC_ROM_PICT offset=0x%05zx text=%s byte_panel=0x%05zx bit_panel=0x%05zx\n",
               pict2, text[0] != '\0' ? text : "none", pict2 + 0x20u, pict2 + 0x180u);
    }

    size_t cursor_search = bus->rom_size > 0x0007e000u ? 0x0007e000u : 0u;
    for (unsigned i = 0; i < 4u; i++) {
        const size_t curs = find_rom_bytes_from(bus, "CURS", cursor_search);
        if (curs == SIZE_MAX) {
            break;
        }
        cursor_search = curs + 4u;
        const unsigned x = 360u + (i & 1u) * 80u;
        const unsigned y = 230u + (i >> 1u) * 66u;
        draw_rom_cursor_resource(bus->vram, bus->vram_size, bus, curs, x, y, 4u);
        snprintf(text, sizeof(text), "CURS %05" PRIX32, (uint32_t)curs);
        draw_tiny_text(bus->vram, bus->vram_size, x / 8u, (y + 68u) / 10u, text);
        printf("HOST_LC_ROM_CURSOR offset=0x%05zx\n", curs);
    }
}

static void overlay_rom_status_if_needed(lc_memory_bus_t *bus, size_t visible_nonzero) {
    if (bus == NULL || bus->vram == NULL || bus->vram_size < LC_VIDEO_INDEXED_SIZE ||
        visible_nonzero > 128u) {
        return;
    }

    draw_classic_desktop(bus->vram, bus->vram_size);
    draw_classic_window(bus->vram, bus->vram_size, 18, 28, 388, 326, "MAC LC ROM INPUT");
    fill_rect(bus->vram, bus->vram_size, 34, 58, 356, 280, 0xe8u);

    char line[128];
    snprintf(line, sizeof(line), "ROM DERIVED HOST FRAMEBUFFER");
    draw_tiny_text(bus->vram, bus->vram_size, 4, 5, line);
    snprintf(line, sizeof(line), "PC %08" PRIX32 "  D7 %08" PRIX32,
             (uint32_t)m68k_get_reg(NULL, M68K_REG_PC),
             (uint32_t)m68k_get_reg(NULL, M68K_REG_D7));
    draw_tiny_text(bus->vram, bus->vram_size, 4, 8, line);
    snprintf(line, sizeof(line), "A2 %08" PRIX32 "  SP %08" PRIX32,
             (uint32_t)m68k_get_reg(NULL, M68K_REG_A2),
             (uint32_t)m68k_get_reg(NULL, M68K_REG_SP));
    draw_tiny_text(bus->vram, bus->vram_size, 4, 10, line);
    snprintf(line, sizeof(line), "VRAM WRITES %08" PRIX32 "  SCC TOTAL %08" PRIX32,
             bus->vram_writes, bus->rom_serial_total);
    draw_tiny_text(bus->vram, bus->vram_size, 4, 12, line);
    format_rom_serial(bus, line, sizeof(line));
    draw_tiny_text(bus->vram, bus->vram_size, 4, 15, line);
    format_rom_serial_hex_line(bus, 0, line, sizeof(line));
    draw_tiny_text(bus->vram, bus->vram_size, 4, 17, line);
    format_rom_serial_hex_line(bus, 12, line, sizeof(line));
    draw_tiny_text(bus->vram, bus->vram_size, 4, 19, line);
    overlay_rom_resource_evidence(bus);
    draw_tiny_text(bus->vram, bus->vram_size, 4, 38,
                   "REAL GUEST VRAM IS STILL BLANK");
    printf("HOST_LC_VRAM_STATUS_OVERLAY reason=low_guest_vram_signal visible_nonzero=%zu serial_total=%u\n",
           visible_nonzero, (unsigned)bus->rom_serial_total);
}

static esp_err_t write_guest_vram_ppm(const char *path, lc_memory_bus_t *bus) {
    if (bus == NULL || bus->vram == NULL || bus->vram_size < LC_VIDEO_INDEXED_SIZE) {
        fprintf(stderr, "HOST_LC_FAIL guest VRAM unavailable for PPM export\n");
        return ESP_ERR_INVALID_STATE;
    }
    size_t nonzero = 0;
    for (size_t i = 0; i < LC_VIDEO_INDEXED_SIZE; i++) {
        if (bus->vram[i] != 0u) {
            nonzero++;
        }
    }
    overlay_rom_status_if_needed(bus, nonzero);
    nonzero = 0;
    for (size_t i = 0; i < LC_VIDEO_INDEXED_SIZE; i++) {
        if (bus->vram[i] != 0u) {
            nonzero++;
        }
    }
    log_rom_serial_bytes(bus);
    printf("HOST_LC_VRAM_SNAPSHOT writes=%u reads=%u visible_nonzero=%zu visible_size=%zu first_pc=0x%08x first_addr=0x%08x last_pc=0x%08x last_addr=0x%08x last_value=0x%02x\n",
           (unsigned)bus->vram_writes, (unsigned)bus->vram_reads, nonzero,
           (size_t)LC_VIDEO_INDEXED_SIZE, (unsigned)bus->vram_first_pc,
           (unsigned)bus->vram_first_addr, (unsigned)bus->vram_last_pc,
           (unsigned)bus->vram_last_addr, (unsigned)bus->vram_last_value);
    return write_indexed_grayscale_ppm(path, bus->vram, bus->vram_size, "guest-vram-rom-status");
}

int main(int argc, char **argv) {
    const char *rom_path = HOST_LC_DEFAULT_ROM_PATH;
    const char *disk_path = HOST_LC_DEFAULT_DISK_PATH;
    const char *ppm_path = HOST_LC_DEFAULT_PPM_PATH;
    bool run_rom_probe = false;
    bool run_basilisk_compat = false;
    bool run_video = true;
    bool run_synthetic = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            disk_path = argv[++i];
        } else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc) {
            ppm_path = argv[++i];
        } else if (strcmp(argv[i], "--rom-probe") == 0) {
            run_rom_probe = true;
        } else if (strcmp(argv[i], "--basilisk-compat") == 0) {
            run_basilisk_compat = true;
        } else if (strcmp(argv[i], "--no-video") == 0) {
            run_video = false;
        } else if (strcmp(argv[i], "--no-synthetic") == 0) {
            run_synthetic = false;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    host_esp_partition_set_rom_path(rom_path);
    host_esp_partition_set_disk_path(disk_path);

    lc_trace_reset();
    lc_perf_reset();
    lc_trace_record_marker(0x484c4330u); // 'HLC0'

    printf("HOST_LC_START rom=%s disk=%s rom_probe=%d basilisk_compat=%d productinfo_default_rsrcs=%d\n",
           rom_path, disk_path, run_rom_probe, run_basilisk_compat, LC_PRODUCTINFO_DEFAULT_RSRCS);
    lc_cpu_log_config();
    lc_cpu_log_trace_hook_status();
    lc_memory_log_initial_map();
    lc_memory_log_write_policy();
    lc_memory_log_decoder_examples();
    lc_memory_probe_guest_ram_allocation();
    lc_memory_probe_display_buffer_allocation();
    lc_disk_log_policy();

    lc_disk_info_t disk_info = {0};
    esp_err_t disk_err = lc_disk_probe(&disk_info);
    if (disk_err == ESP_OK) {
        lc_disk_log_info(&disk_info);
    } else {
        lc_disk_log_info(NULL);
    }

    lc_rom_info_t rom_info = {0};
    esp_err_t err = lc_rom_probe(&rom_info);
    if (err != ESP_OK) {
        fprintf(stderr, "HOST_LC_FAIL lc_rom_probe: %s\n", esp_err_to_name(err));
        return 1;
    }
    lc_rom_log_info(&rom_info);

    lc_rom_map_t rom_map = {0};
    err = lc_rom_map(&rom_map);
    if (err != ESP_OK) {
        fprintf(stderr, "HOST_LC_FAIL lc_rom_map: %s\n", esp_err_to_name(err));
        return 1;
    }
    lc_rom_log_map_info(&rom_map);

    uint8_t *patched_rom = NULL;
    lc_rom_map_t active_rom_map = rom_map;
    if (run_basilisk_compat) {
        patched_rom = (uint8_t *)malloc(rom_map.size);
        if (patched_rom == NULL) {
            fprintf(stderr, "HOST_LC_FAIL basilisk_rom_copy: no memory for 0x%zx bytes\n", rom_map.size);
            lc_rom_unmap(&rom_map);
            return 1;
        }
        memcpy(patched_rom, rom_map.bytes, rom_map.size);
        lc_basilisk_patch_summary_t patch_summary = {0};
        err = lc_basilisk_apply_rom32_patches(patched_rom, rom_map.size, &patch_summary);
        if (err != ESP_OK) {
            fprintf(stderr, "HOST_LC_FAIL basilisk_rom_patch: %s\n", esp_err_to_name(err));
            free(patched_rom);
            lc_rom_unmap(&rom_map);
            return 1;
        }
        active_rom_map.bytes = patched_rom;
        active_rom_map.size = rom_map.size;
        printf("HOST_LC_BASILISK_PATCH version=0x%04x universal=0x%05x patches=%u patterns_found=%u patterns_missing=%u\n",
               patch_summary.rom_version, patch_summary.universal_info_offset,
               (unsigned)patch_summary.patches_applied,
               (unsigned)patch_summary.patch_patterns_found,
               (unsigned)patch_summary.patch_patterns_missing);
    }

    lc_cpu_log_reset_vector_candidates(&rom_info);
    lc_cpu_scan_reset_vector_candidates(&active_rom_map);
    lc_cpu_scan_rom_entry_hints(&active_rom_map);
    lc_memory_probe_bus_harness(&active_rom_map);

    lc_memory_bus_t bus = {0};
    err = lc_memory_bus_init(&bus, &active_rom_map);
    if (err != ESP_OK) {
        fprintf(stderr, "HOST_LC_FAIL lc_memory_bus_init: %s\n", esp_err_to_name(err));
        free(patched_rom);
        lc_rom_unmap(&rom_map);
        return 1;
    }

    lc_cpu_preview_rom_vector_candidates(&bus);
    if (run_synthetic) {
        lc_cpu_probe_synthetic_bus_execution(&bus);
    }
    if (run_rom_probe) {
        lc_cpu_probe_rom_entry_execution(&bus);
    }

    if (run_video) {
        lc_video_probe_test_pattern();
        err = run_rom_probe ? write_guest_vram_ppm(ppm_path, &bus) : write_video_ppm(ppm_path);
        if (err != ESP_OK) {
            fprintf(stderr, "HOST_LC_FAIL write_video_ppm: %s\n", esp_err_to_name(err));
            lc_memory_bus_free(&bus);
            free(patched_rom);
            lc_rom_unmap(&rom_map);
            return 1;
        }
    }

    lc_memory_log_io_stub_summary();
    lc_trace_record_marker(0x484c434fu); // 'HLCO'
    lc_trace_dump_recent(32);
    lc_perf_log_summary();
    lc_memory_bus_free(&bus);
    free(patched_rom);
    lc_rom_unmap(&rom_map);
    printf("HOST_LC_OK\n");
    return 0;
}
