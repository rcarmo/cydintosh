#include "tab5_bsp_display_smoke.h"

#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "tab5_bsp_smoke";

#define TAB5_GT911_ADDR 0x14
#define TAB5_ST7123_ADDR 0x55
#define TAB5_STRIP_ROWS 32

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

static esp_err_t fill_screen_stripes(esp_lcd_panel_handle_t panel) {
    const size_t pixels = (size_t)BSP_LCD_H_RES * TAB5_STRIP_ROWS;
    uint16_t *strip = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (strip == NULL) {
        strip = heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (strip == NULL) {
        ESP_LOGE(TAG, "failed to allocate %u-row smoke strip", TAB5_STRIP_ROWS);
        return ESP_ERR_NO_MEM;
    }

    const uint16_t colors[] = {
        rgb565(255, 0, 0),   rgb565(0, 255, 0),   rgb565(0, 0, 255),
        rgb565(255, 255, 0), rgb565(255, 0, 255), rgb565(0, 255, 255),
        rgb565(255, 255, 255), rgb565(32, 32, 32),
    };
    const unsigned color_count = sizeof(colors) / sizeof(colors[0]);

    for (int y = 0; y < BSP_LCD_V_RES; y += TAB5_STRIP_ROWS) {
        const int rows = (y + TAB5_STRIP_ROWS <= BSP_LCD_V_RES) ? TAB5_STRIP_ROWS : (BSP_LCD_V_RES - y);
        const uint16_t color = colors[((unsigned)y / TAB5_STRIP_ROWS) % color_count];
        for (int row = 0; row < rows; row++) {
            for (int x = 0; x < BSP_LCD_H_RES; x++) {
                uint16_t px = color;
                if (x < 12 || x >= BSP_LCD_H_RES - 12 || y + row < 12 || y + row >= BSP_LCD_V_RES - 12) {
                    px = rgb565(255, 255, 255);
                }
                if (((x / 48) + ((y + row) / 48)) % 2 == 0 && (y + row) < 160) {
                    px = rgb565(0, 0, 0);
                }
                strip[(size_t)row * BSP_LCD_H_RES + x] = px;
            }
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, y, BSP_LCD_H_RES, y + rows, strip),
                            TAG, "draw smoke strip failed");
    }

    free(strip);
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
    ESP_RETURN_ON_ERROR(fill_screen_stripes(handles.panel), TAG, "smoke fill failed");
    ESP_LOGI(TAG, "display smoke pattern drawn; entering brightness heartbeat");

    while (true) {
        bsp_display_brightness_set(35);
        vTaskDelay(pdMS_TO_TICKS(700));
        bsp_display_brightness_set(100);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}
