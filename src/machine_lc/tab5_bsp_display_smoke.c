#include "tab5_bsp_display_smoke.h"

#include "lc_video.h"

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "tab5_bsp_smoke";

#define TAB5_GT911_ADDR 0x14
#define TAB5_ST7123_ADDR 0x55
#define TAB5_STRIP_ROWS 32
#define TAB5_LC_SCALE_NUM 45u
#define TAB5_LC_SCALE_DEN 32u
#define TAB5_LC_SCALED_W ((LC_VIDEO_WIDTH * TAB5_LC_SCALE_NUM) / TAB5_LC_SCALE_DEN)
#define TAB5_LC_SCALED_H ((LC_VIDEO_HEIGHT * TAB5_LC_SCALE_NUM) / TAB5_LC_SCALE_DEN)
#define TAB5_LC_OFFSET_X ((BSP_LCD_H_RES - TAB5_LC_SCALED_W) / 2)
#define TAB5_LC_OFFSET_Y ((BSP_LCD_V_RES - TAB5_LC_SCALED_H) / 2)

typedef enum {
    TAB5_PANEL_UNKNOWN = 0,
    TAB5_PANEL_ILI9881C_GT911,
    TAB5_PANEL_ST7123,
} tab5_panel_kind_t;

static tab5_panel_kind_t detect_panel_kind(void) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "I2C handle is unavailable for panel detection");
        return TAB5_PANEL_UNKNOWN;
    }

    esp_err_t gt911 = i2c_master_probe(bus, TAB5_GT911_ADDR, 50);
    esp_err_t st7123 = i2c_master_probe(bus, TAB5_ST7123_ADDR, 50);
    ESP_LOGI(TAG, "panel probe: GT911@0x%02x=%s ST7123@0x%02x=%s", TAB5_GT911_ADDR,
             esp_err_to_name(gt911), TAB5_ST7123_ADDR, esp_err_to_name(st7123));

    if (st7123 == ESP_OK) {
        return TAB5_PANEL_ST7123;
    }
    if (gt911 == ESP_OK) {
        return TAB5_PANEL_ILI9881C_GT911;
    }
    return TAB5_PANEL_UNKNOWN;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)((r >> 3) << 11) | (uint16_t)((g >> 2) << 5) | (uint16_t)(b >> 3);
}

static uint32_t fnv1a_update16(uint32_t checksum, uint16_t value) {
    checksum ^= (uint8_t)(value & 0xffu);
    checksum *= 16777619u;
    checksum ^= (uint8_t)(value >> 8u);
    checksum *= 16777619u;
    return checksum;
}

static uint16_t smoke_background_pixel(int x, int y) {
    if (x < 10 || x >= BSP_LCD_H_RES - 10 || y < 10 || y >= BSP_LCD_V_RES - 10) {
        return rgb565(255, 255, 255);
    }
    if ((x % 120) < 2 || (y % 120) < 2) {
        return rgb565(42, 42, 58);
    }
    return rgb565(6, 10, 18);
}

static esp_err_t fill_screen_lc_pattern(esp_lcd_panel_handle_t panel) {
    ESP_LOGI(TAG,
             "drawing LC indexed test pattern: guest=%ux%u scale=%u/%u physical=%ux%u offset=(%u,%u)",
             LC_VIDEO_WIDTH, LC_VIDEO_HEIGHT, TAB5_LC_SCALE_NUM, TAB5_LC_SCALE_DEN,
             TAB5_LC_SCALED_W, TAB5_LC_SCALED_H, TAB5_LC_OFFSET_X, TAB5_LC_OFFSET_Y);

    uint8_t *indexed = heap_caps_malloc(LC_VIDEO_INDEXED_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (indexed == NULL) {
        indexed = heap_caps_malloc(LC_VIDEO_INDEXED_SIZE, MALLOC_CAP_8BIT);
    }
    if (indexed == NULL) {
        ESP_LOGE(TAG, "failed to allocate LC indexed pattern: %zu bytes", LC_VIDEO_INDEXED_SIZE);
        return ESP_ERR_NO_MEM;
    }

    uint16_t palette[LC_VIDEO_CLUT_ENTRIES] = {0};
    lc_video_init_debug_palette(palette);
    const uint32_t indexed_checksum = lc_video_fill_test_pattern(indexed, LC_VIDEO_INDEXED_SIZE);

    const size_t pixels = (size_t)BSP_LCD_H_RES * TAB5_STRIP_ROWS;
    uint16_t *strip = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (strip == NULL) {
        strip = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (strip == NULL) {
        heap_caps_free(indexed);
        ESP_LOGE(TAG, "failed to allocate %u-row BSP smoke strip", TAB5_STRIP_ROWS);
        return ESP_ERR_NO_MEM;
    }

    uint32_t rgb565_checksum = 2166136261u;
    for (int y = 0; y < BSP_LCD_V_RES; y += TAB5_STRIP_ROWS) {
        const int rows = (y + TAB5_STRIP_ROWS <= BSP_LCD_V_RES) ? TAB5_STRIP_ROWS : (BSP_LCD_V_RES - y);
        for (int row = 0; row < rows; row++) {
            const int py = y + row;
            for (int x = 0; x < BSP_LCD_H_RES; x++) {
                uint16_t px = smoke_background_pixel(x, py);
                if (x >= (int)TAB5_LC_OFFSET_X && x < (int)(TAB5_LC_OFFSET_X + TAB5_LC_SCALED_W) &&
                    py >= (int)TAB5_LC_OFFSET_Y && py < (int)(TAB5_LC_OFFSET_Y + TAB5_LC_SCALED_H)) {
                    const unsigned sx = (unsigned)(((uint32_t)(x - TAB5_LC_OFFSET_X) * LC_VIDEO_WIDTH) /
                                                   TAB5_LC_SCALED_W);
                    const unsigned sy = (unsigned)(((uint32_t)(py - TAB5_LC_OFFSET_Y) * LC_VIDEO_HEIGHT) /
                                                   TAB5_LC_SCALED_H);
                    px = palette[indexed[(size_t)sy * LC_VIDEO_ROWBYTES + sx]];

                    // Make the scaled LC viewport unmistakable on camera.
                    if (x - (int)TAB5_LC_OFFSET_X < 4 ||
                        (int)(TAB5_LC_OFFSET_X + TAB5_LC_SCALED_W) - x <= 4 ||
                        py - (int)TAB5_LC_OFFSET_Y < 4 ||
                        (int)(TAB5_LC_OFFSET_Y + TAB5_LC_SCALED_H) - py <= 4) {
                        px = rgb565(255, 255, 255);
                    }
                }
                strip[(size_t)row * BSP_LCD_H_RES + x] = px;
                rgb565_checksum = fnv1a_update16(rgb565_checksum, px);
            }
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, y, BSP_LCD_H_RES, y + rows, strip),
                            TAG, "draw LC pattern strip failed");
    }

    ESP_LOGI(TAG, "LC-on-Tab5 pattern drawn: indexed_checksum=0x%08" PRIx32
                  " physical_rgb565_checksum=0x%08" PRIx32,
             indexed_checksum, rgb565_checksum);
    heap_caps_free(strip);
    heap_caps_free(indexed);
    return ESP_OK;
}

esp_err_t tab5_bsp_display_smoke_run(void) {
    ESP_LOGI(TAG, "starting M5Stack BSP display smoke test");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    bsp_set_charge_qc_en(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_set_charge_en(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_reset_tp();
    vTaskDelay(pdMS_TO_TICKS(50));

    tab5_panel_kind_t panel_kind = detect_panel_kind();
    bsp_lcd_handles_t handles = {0};
    esp_err_t err;
    if (panel_kind == TAB5_PANEL_ST7123) {
        ESP_LOGI(TAG, "using ST7123 display path");
        err = bsp_display_new_with_handles_to_st7123(NULL, &handles);
    } else {
        ESP_LOGI(TAG, "using ILI9881C/ST7703-compatible display path (kind=%d)", panel_kind);
        err = bsp_display_new_with_handles(NULL, &handles);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "BSP display init failed");

    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(100), TAG, "backlight set failed");
    ESP_RETURN_ON_ERROR(fill_screen_lc_pattern(handles.panel), TAG, "LC pattern fill failed");
    ESP_LOGI(TAG, "LC display smoke pattern drawn; entering brightness heartbeat");

    while (true) {
        bsp_display_brightness_set(35);
        vTaskDelay(pdMS_TO_TICKS(700));
        bsp_display_brightness_set(100);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}
