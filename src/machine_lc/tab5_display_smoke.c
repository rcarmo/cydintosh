#include "tab5_display_smoke.h"

#include "board_profiles.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lc_perf.h"

#include <inttypes.h>

static const char *TAG = "tab5_display_smoke";

#define TAB5_SMOKE_STRIP_PIXELS ((size_t)TAB5_DSI_PANEL_H_RES * (size_t)TAB5_DSI_SMOKE_STRIP_LINES)

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)((r & 0xF8u) << 8) | (uint16_t)((g & 0xFCu) << 3) |
           (uint16_t)(b >> 3);
}

static uint32_t checksum_rgb565(uint32_t checksum, uint16_t value) {
    checksum ^= value;
    return checksum * 16777619u;
}

const char *tab5_display_smoke_pattern_name(tab5_display_smoke_pattern_t pattern) {
    switch (pattern) {
    case TAB5_DISPLAY_SMOKE_SOLID_COLORS:
        return "solid-colors";
    case TAB5_DISPLAY_SMOKE_ORIENTATION_MARKERS:
        return "orientation-markers";
    case TAB5_DISPLAY_SMOKE_ONE_BIT_CHECKER:
        return "one-bit-checker";
    case TAB5_DISPLAY_SMOKE_INDEXED_RAMP:
        return "indexed-palette-ramp";
    default:
        return "unknown";
    }
}

void tab5_display_smoke_log_config(void) {
    ESP_LOGI(TAG,
             "Tab5 display smoke scaffold: dsi_panel=%ux%u strip_lines=%u strip_pixels=%zu bus=MIPI-DSI lanes=2 driver=not-initialized",
             TAB5_DSI_PANEL_H_RES, TAB5_DSI_PANEL_V_RES, TAB5_DSI_SMOKE_STRIP_LINES,
             TAB5_SMOKE_STRIP_PIXELS);
    ESP_LOGI(TAG, "Tab5 board constants: LCD_WIDTH=%d LCD_HEIGHT=%d backlight_gpio=%d",
             LCD_WIDTH, LCD_HEIGHT, TAB5_LCD_BACKLIGHT_GPIO);
}

static uint16_t smoke_pixel(tab5_display_smoke_pattern_t pattern, unsigned x, unsigned y) {
    switch (pattern) {
    case TAB5_DISPLAY_SMOKE_SOLID_COLORS: {
        const unsigned band = (y * 5u) / TAB5_DSI_PANEL_V_RES;
        static const uint16_t colors[] = {
            0xF800, // red
            0x07E0, // green
            0x001F, // blue
            0xFFFF, // white
            0x0000, // black
        };
        return colors[band < 5u ? band : 4u];
    }
    case TAB5_DISPLAY_SMOKE_ORIENTATION_MARKERS:
        if (x < 24u && y < 24u) {
            return rgb565(255, 0, 0); // top-left red
        }
        if (x >= TAB5_DSI_PANEL_H_RES - 24u && y < 24u) {
            return rgb565(0, 255, 0); // top-right green
        }
        if (x < 24u && y >= TAB5_DSI_PANEL_V_RES - 24u) {
            return rgb565(0, 0, 255); // bottom-left blue
        }
        if (x >= TAB5_DSI_PANEL_H_RES - 24u && y >= TAB5_DSI_PANEL_V_RES - 24u) {
            return rgb565(255, 255, 255); // bottom-right white
        }
        if (x == TAB5_DSI_PANEL_H_RES / 2u || y == TAB5_DSI_PANEL_V_RES / 2u) {
            return rgb565(255, 255, 0);
        }
        return rgb565((uint8_t)((x * 255u) / (TAB5_DSI_PANEL_H_RES - 1u)),
                      (uint8_t)((y * 255u) / (TAB5_DSI_PANEL_V_RES - 1u)), 0x30);
    case TAB5_DISPLAY_SMOKE_ONE_BIT_CHECKER:
        return (((x / 16u) ^ (y / 16u)) & 1u) ? 0xFFFF : 0x0000;
    case TAB5_DISPLAY_SMOKE_INDEXED_RAMP: {
        const uint8_t index = (uint8_t)((x * 255u) / (TAB5_DSI_PANEL_H_RES - 1u));
        const uint8_t stripe = (uint8_t)(((y / 32u) & 1u) ? 0x40u : 0x00u);
        const uint8_t value = (uint8_t)(index ^ stripe);
        return rgb565(value, (uint8_t)((value * 5u) & 0xffu), (uint8_t)(255u - value));
    }
    default:
        return 0;
    }
}

uint32_t tab5_display_smoke_render_pattern(tab5_display_smoke_pattern_t pattern) {
    uint16_t *strip = (uint16_t *)heap_caps_malloc(TAB5_SMOKE_STRIP_PIXELS * sizeof(uint16_t),
                                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                                                       MALLOC_CAP_8BIT);
    if (strip == NULL) {
        ESP_LOGW(TAG, "Tab5 display smoke strip allocation failed: pixels=%zu", TAB5_SMOKE_STRIP_PIXELS);
        return 0;
    }

    const uint64_t start = lc_perf_now_us();
    uint32_t checksum = 2166136261u;
    unsigned rendered_strips = 0;
    for (unsigned y0 = 0; y0 < TAB5_DSI_PANEL_V_RES; y0 += TAB5_DSI_SMOKE_STRIP_LINES) {
        const unsigned rows = (y0 + TAB5_DSI_SMOKE_STRIP_LINES <= TAB5_DSI_PANEL_V_RES)
                                  ? TAB5_DSI_SMOKE_STRIP_LINES
                                  : (TAB5_DSI_PANEL_V_RES - y0);
        for (unsigned y = 0; y < rows; y++) {
            for (unsigned x = 0; x < TAB5_DSI_PANEL_H_RES; x++) {
                const uint16_t pixel = smoke_pixel(pattern, x, y0 + y);
                strip[(size_t)y * TAB5_DSI_PANEL_H_RES + x] = pixel;
                checksum = checksum_rgb565(checksum, pixel);
            }
        }
        rendered_strips++;
    }
    lc_perf_record_us(LC_PERF_COUNTER_DISPLAY_FLUSH, lc_perf_now_us() - start);
    heap_caps_free(strip);

    ESP_LOGI(TAG, "Tab5 display smoke pattern=%s checksum=0x%08" PRIx32 " strips=%u pixels=%u",
             tab5_display_smoke_pattern_name(pattern), checksum, rendered_strips,
             TAB5_DSI_PANEL_H_RES * TAB5_DSI_PANEL_V_RES);
    return checksum;
}

void tab5_display_smoke_probe_patterns(void) {
    tab5_display_smoke_log_config();
    tab5_display_smoke_render_pattern(TAB5_DISPLAY_SMOKE_SOLID_COLORS);
    tab5_display_smoke_render_pattern(TAB5_DISPLAY_SMOKE_ORIENTATION_MARKERS);
    tab5_display_smoke_render_pattern(TAB5_DISPLAY_SMOKE_ONE_BIT_CHECKER);
    tab5_display_smoke_render_pattern(TAB5_DISPLAY_SMOKE_INDEXED_RAMP);
}
