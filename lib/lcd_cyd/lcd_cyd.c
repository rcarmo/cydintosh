#include "lcd_cyd.h"

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "hw.h"

#include "driver/gpio.h"

#if defined(LCD_PANEL_ILI9341_SPI)
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#endif

#if defined(LCD_PANEL_RGB)
#include "esp_lcd_panel_rgb.h"
#endif

static const char *TAG = "lcd_cyd";

static esp_lcd_panel_handle_t panel_handle = NULL;

#if defined(LCD_PANEL_ILI9341_SPI)
static esp_lcd_panel_io_handle_t io_handle = NULL;

static const ili9341_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xCF, (uint8_t[]){0x00, 0xAA, 0xE0},             3, 0},
    {0xED, (uint8_t[]){0x67, 0x03, 0x12, 0x81},       4, 0},
    {0xE8, (uint8_t[]){0x8A, 0x01, 0x78},             3, 0},
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t[]){0x20},                         1, 0},
    {0xEA, (uint8_t[]){0x00, 0x00},                   2, 0},
    {0xC0, (uint8_t[]){0x23},                         1, 0},
    {0xC1, (uint8_t[]){0x11},                         1, 0},
    {0xC5, (uint8_t[]){0x43, 0x4C},                   2, 0},
    {0xC7, (uint8_t[]){0xA0},                         1, 0},
    {0xB1, (uint8_t[]){0x00, 0x18},                   2, 0},
    {0xF2, (uint8_t[]){0x00},                         1, 0},
    {0x26, (uint8_t[]){0x01},                         1, 0},
    {0xE0,
     (uint8_t[]){0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09,
                 0x00},
     15,                                                 0},
    {0xE1,
     (uint8_t[]){0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36,
                 0x0F},
     15,                                                 0},
    {0xB6, (uint8_t[]){0x08, 0x82, 0x27},             3, 0},
    {0x36, (uint8_t[]){0x08}, 1, 0}, // MADCTL: BGR (portrait, no mirror)
    {0x21, NULL,                                      0, 0},
};
#endif

static void backlight_on(void) {
#if TFT_BL >= 0
    gpio_reset_pin(TFT_BL);
    gpio_set_direction(TFT_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(TFT_BL, 1);
#endif
}

void lcd_cyd_init(void) {
    static bool initialized = false;
    if (initialized) {
        ESP_LOGI(TAG, "LCD already initialized, skipping");
        return;
    }

    ESP_LOGI(TAG, "Initialize LCD for %s", BOARD_NAME);

#if defined(LCD_PANEL_ILI9341_SPI)
    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t bus_config =
        ILI9341_PANEL_BUS_SPI_CONFIG(TFT_SPI_CLK, TFT_SPI_MOSI, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config =
        ILI9341_PANEL_IO_SPI_CONFIG(TFT_SPI_CS, TFT_DC, NULL, NULL);
    io_config.pclk_hz = 80 * 1000 * 1000; // 80MHz
    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ILI9341 panel driver");
    ili9341_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(ili9341_lcd_init_cmd_t),
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RESET,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
#elif defined(LCD_PANEL_RGB)
    ESP_LOGI(TAG, "Install RGB panel driver (%dx%d)", LCD_WIDTH, LCD_HEIGHT);
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =
            {
                .pclk_hz = LCD_RGB_PCLK_HZ,
                .h_res = LCD_WIDTH,
                .v_res = LCD_HEIGHT,
                .hsync_pulse_width = LCD_RGB_HSYNC_PULSE_WIDTH,
                .hsync_back_porch = LCD_RGB_HSYNC_BACK_PORCH,
                .hsync_front_porch = LCD_RGB_HSYNC_FRONT_PORCH,
                .vsync_pulse_width = LCD_RGB_VSYNC_PULSE_WIDTH,
                .vsync_back_porch = LCD_RGB_VSYNC_BACK_PORCH,
                .vsync_front_porch = LCD_RGB_VSYNC_FRONT_PORCH,
                .flags =
                    {
                        .pclk_active_neg = LCD_RGB_PCLK_ACTIVE_NEG,
                    },
            },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_WIDTH * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = LCD_RGB_HSYNC_GPIO,
        .vsync_gpio_num = LCD_RGB_VSYNC_GPIO,
        .de_gpio_num = LCD_RGB_DE_GPIO,
        .pclk_gpio_num = LCD_RGB_PCLK_GPIO,
        .disp_gpio_num = GPIO_UNUSED,
        .data_gpio_nums =
            {
                LCD_RGB_DATA0_GPIO,  LCD_RGB_DATA1_GPIO,  LCD_RGB_DATA2_GPIO,  LCD_RGB_DATA3_GPIO,
                LCD_RGB_DATA4_GPIO,  LCD_RGB_DATA5_GPIO,  LCD_RGB_DATA6_GPIO,  LCD_RGB_DATA7_GPIO,
                LCD_RGB_DATA8_GPIO,  LCD_RGB_DATA9_GPIO,  LCD_RGB_DATA10_GPIO, LCD_RGB_DATA11_GPIO,
                LCD_RGB_DATA12_GPIO, LCD_RGB_DATA13_GPIO, LCD_RGB_DATA14_GPIO, LCD_RGB_DATA15_GPIO,
            },
        .flags =
            {
                .fb_in_psram = 1,
            },
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
#else
#error "No LCD panel type selected"
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    esp_err_t disp_err = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (disp_err != ESP_OK && disp_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(disp_err);
    }

    backlight_on();

    initialized = true;
    ESP_LOGI(TAG, "LCD initialized");
}

void lcd_draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *data) {
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, data);
}

void lcd_wait_trans_complete(void) {
#if defined(LCD_PANEL_ILI9341_SPI)
    // Wait for all queued SPI transactions to complete.
    esp_lcd_panel_io_tx_param(io_handle, -1, NULL, 0);
#endif
}

void lcd_get_rgb_framebuffer(void **fb_out) {
#if defined(LCD_PANEL_RGB)
    esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, fb_out);
#else
    *fb_out = NULL;
#endif
}
