#include "tab5_touch.h"

#include "board_profiles.h"
#include "tab5_bsp_display.h"

#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "tab5_touch";

static i2c_master_bus_handle_t touch_bus;
static esp_lcd_panel_io_handle_t touch_io;
static esp_lcd_touch_handle_t touch_handle;
static tab5_touch_controller_t active_controller = TAB5_TOUCH_CONTROLLER_NONE;
static bool touch_bus_initialized;

const char *tab5_touch_controller_name(tab5_touch_controller_t controller) {
    switch (controller) {
    case TAB5_TOUCH_CONTROLLER_GT911:
        return "GT911";
    case TAB5_TOUCH_CONTROLLER_ST7123:
        return "ST7123";
    case TAB5_TOUCH_CONTROLLER_NONE:
    default:
        return "none";
    }
}

void tab5_touch_log_config(void) {
    ESP_LOGI(TAG,
             "Tab5 touch scaffold: i2c_port=0 sda=%d scl=%d freq=%u gt911=0x%02x st7123=0x%02x int_gpio=%d bus_ready=%s controller=%s",
             TAB5_TOUCH_I2C_SDA, TAB5_TOUCH_I2C_SCL, TAB5_TOUCH_I2C_FREQ_HZ,
             TAB5_TOUCH_GT911_ADDR, TAB5_TOUCH_ST7123_ADDR, TAB5_TOUCH_INT_GPIO,
             touch_bus_initialized ? "yes" : "no", tab5_touch_controller_name(active_controller));
}

static esp_err_t tab5_touch_i2c_init(void) {
    if (touch_bus_initialized) {
        return ESP_OK;
    }

    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Tab5 BSP I2C bus for touch: %s", esp_err_to_name(err));
        return err;
    }

    touch_bus = bsp_i2c_get_handle();
    if (touch_bus == NULL) {
        ESP_LOGE(TAG, "Tab5 BSP I2C bus handle is NULL after init");
        return ESP_ERR_INVALID_STATE;
    }

    touch_bus_initialized = true;
    ESP_LOGI(TAG, "Tab5 touch using BSP I2C bus on SDA=%d SCL=%d", TAB5_TOUCH_I2C_SDA,
             TAB5_TOUCH_I2C_SCL);
    return ESP_OK;
}

static bool tab5_touch_probe_addr(uint8_t addr) {
    if (!touch_bus_initialized) {
        return false;
    }
    esp_err_t err = i2c_master_probe(touch_bus, addr, 50);
    ESP_LOGI(TAG, "Tab5 touch I2C probe addr=0x%02x result=%s", addr, esp_err_to_name(err));
    return err == ESP_OK;
}

static esp_err_t tab5_touch_read_gt911_product_id(uint8_t id[4]) {
    if (id == NULL || !touch_bus_initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(id, 0, 4);

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TAB5_TOUCH_GT911_ADDR,
        .scl_speed_hz = TAB5_TOUCH_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(touch_bus, &dev_config, &dev);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t reg[] = {0x81, 0x40}; // GT911 product ID register, big-endian address
    err = i2c_master_transmit_receive(dev, reg, sizeof(reg), id, 4, 50);
    i2c_master_bus_rm_device(dev);
    return err;
}

esp_err_t tab5_touch_probe(tab5_touch_probe_result_t *result) {
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));

    esp_err_t err = tab5_touch_i2c_init();
    if (err != ESP_OK) {
        return err;
    }
    result->bus_ready = true;

    result->gt911_present = tab5_touch_probe_addr(TAB5_TOUCH_GT911_ADDR);
    if (result->gt911_present) {
        err = tab5_touch_read_gt911_product_id(result->gt911_product_id);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GT911 product ID read failed: %s", esp_err_to_name(err));
        }
    }

    result->st7123_present = tab5_touch_probe_addr(TAB5_TOUCH_ST7123_ADDR);
    return ESP_OK;
}

void tab5_touch_log_probe_result(const tab5_touch_probe_result_t *result) {
    if (result == NULL) {
        ESP_LOGW(TAG, "Tab5 touch probe result unavailable");
        return;
    }
    ESP_LOGI(TAG, "Tab5 touch probe: bus_ready=%s gt911=0x%02x:%s st7123=0x%02x:%s",
             result->bus_ready ? "yes" : "no", TAB5_TOUCH_GT911_ADDR,
             result->gt911_present ? "present" : "absent", TAB5_TOUCH_ST7123_ADDR,
             result->st7123_present ? "present" : "absent");
    if (result->gt911_present) {
        ESP_LOGI(TAG, "GT911 product id: %c%c%c%c (hex %02x %02x %02x %02x)",
                 result->gt911_product_id[0] ? result->gt911_product_id[0] : '.',
                 result->gt911_product_id[1] ? result->gt911_product_id[1] : '.',
                 result->gt911_product_id[2] ? result->gt911_product_id[2] : '.',
                 result->gt911_product_id[3] ? result->gt911_product_id[3] : '.',
                 result->gt911_product_id[0], result->gt911_product_id[1],
                 result->gt911_product_id[2], result->gt911_product_id[3]);
    }
    if (!result->gt911_present && !result->st7123_present) {
        ESP_LOGW(TAG, "No Tab5 touch controller detected yet; hardware may be absent/off or panel path may differ");
    }
}

static void tab5_touch_map_panel_to_lc(tab5_touch_point_t *point) {
    point->in_lc_viewport = false;
    point->lc_x = 0;
    point->lc_y = 0;

    if (point->x < TAB5_LC_OFFSET_X || point->x >= TAB5_LC_OFFSET_X + TAB5_LC_VIEWPORT_W ||
        point->y < TAB5_LC_OFFSET_Y || point->y >= TAB5_LC_OFFSET_Y + TAB5_LC_VIEWPORT_H) {
        return;
    }

    point->in_lc_viewport = true;
    point->lc_x = (uint16_t)(((uint32_t)(point->x - TAB5_LC_OFFSET_X) * LC_VIDEO_WIDTH) /
                             TAB5_LC_VIEWPORT_W);
    point->lc_y = (uint16_t)(((uint32_t)(point->y - TAB5_LC_OFFSET_Y) * LC_VIDEO_HEIGHT) /
                             TAB5_LC_VIEWPORT_H);
}

esp_err_t tab5_touch_init_reader(tab5_touch_controller_t *controller_out) {
    if (touch_handle != NULL) {
        if (controller_out != NULL) {
            *controller_out = active_controller;
        }
        return ESP_OK;
    }

    tab5_touch_probe_result_t probe = {0};
    esp_err_t err = tab5_touch_probe(&probe);
    if (err != ESP_OK) {
        return err;
    }
    tab5_touch_log_probe_result(&probe);

    if (probe.st7123_present) {
        active_controller = TAB5_TOUCH_CONTROLLER_ST7123;
    } else if (probe.gt911_present) {
        active_controller = TAB5_TOUCH_CONTROLLER_GT911;
    } else {
        active_controller = TAB5_TOUCH_CONTROLLER_NONE;
        return ESP_ERR_NOT_FOUND;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = TAB5_TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    if (active_controller == TAB5_TOUCH_CONTROLLER_ST7123) {
        esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
        io_config.scl_speed_hz = TAB5_TOUCH_I2C_FREQ_HZ;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(touch_bus, &io_config, &touch_io), TAG,
                            "failed to create ST7123 touch IO");
        err = esp_lcd_touch_new_i2c_st7123(touch_io, &tp_cfg, &touch_handle);
    } else {
        esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_config.dev_addr = TAB5_TOUCH_GT911_ADDR;
        io_config.scl_speed_hz = TAB5_TOUCH_I2C_FREQ_HZ;
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(touch_bus, &io_config, &touch_io), TAG,
                            "failed to create GT911 touch IO");
        err = esp_lcd_touch_new_i2c_gt911(touch_io, &tp_cfg, &touch_handle);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create %s touch reader: %s",
                 tab5_touch_controller_name(active_controller), esp_err_to_name(err));
        touch_handle = NULL;
        touch_io = NULL;
        active_controller = TAB5_TOUCH_CONTROLLER_NONE;
        return err;
    }

    ESP_LOGI(TAG, "Tab5 touch reader initialized: controller=%s max=%dx%d int_gpio=%d lc_viewport=%ux%u+%u+%u",
             tab5_touch_controller_name(active_controller), BSP_LCD_H_RES, BSP_LCD_V_RES,
             TAB5_TOUCH_INT_GPIO, TAB5_LC_VIEWPORT_W, TAB5_LC_VIEWPORT_H, TAB5_LC_OFFSET_X,
             TAB5_LC_OFFSET_Y);
    if (controller_out != NULL) {
        *controller_out = active_controller;
    }
    return ESP_OK;
}

esp_err_t tab5_touch_read_sample(tab5_touch_sample_t *sample) {
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(sample, 0, sizeof(*sample));

    esp_err_t err = tab5_touch_init_reader(&sample->controller);
    if (err != ESP_OK) {
        sample->read_status = err;
        return err;
    }
    sample->initialized = true;

    err = esp_lcd_touch_read_data(touch_handle);
    sample->read_status = err;
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_touch_point_data_t points[TAB5_TOUCH_MAX_POINTS] = {0};
    uint8_t count = 0;
    err = esp_lcd_touch_get_data(touch_handle, points, &count, TAB5_TOUCH_MAX_POINTS);
    sample->read_status = err;
    if (err != ESP_OK) {
        return err;
    }

    sample->pressed = count > 0;
    sample->point_count = count;
    for (uint8_t i = 0; i < count && i < TAB5_TOUCH_MAX_POINTS; i++) {
        sample->points[i].x = points[i].x;
        sample->points[i].y = points[i].y;
        sample->points[i].strength = points[i].strength;
        sample->points[i].track_id = points[i].track_id;
        tab5_touch_map_panel_to_lc(&sample->points[i]);
    }
    return ESP_OK;
}

void tab5_touch_log_sample(const tab5_touch_sample_t *sample) {
    if (sample == NULL) {
        ESP_LOGW(TAG, "Tab5 touch sample unavailable");
        return;
    }
    if (sample->read_status != ESP_OK) {
        ESP_LOGW(TAG, "Tab5 touch sample failed: controller=%s status=%s",
                 tab5_touch_controller_name(sample->controller), esp_err_to_name(sample->read_status));
        return;
    }
    if (!sample->pressed) {
        ESP_LOGI(TAG, "Tab5 touch sample: controller=%s no-touch",
                 tab5_touch_controller_name(sample->controller));
        return;
    }

    ESP_LOGI(TAG, "Tab5 touch sample: controller=%s points=%u", tab5_touch_controller_name(sample->controller),
             sample->point_count);
    for (uint8_t i = 0; i < sample->point_count && i < TAB5_TOUCH_MAX_POINTS; i++) {
        const tab5_touch_point_t *point = &sample->points[i];
        if (point->in_lc_viewport) {
            ESP_LOGI(TAG, "  point[%u]: panel=(%u,%u) lc=(%u,%u) strength=%u track=%u", i,
                     point->x, point->y, point->lc_x, point->lc_y, point->strength,
                     point->track_id);
        } else {
            ESP_LOGI(TAG, "  point[%u]: panel=(%u,%u) outside_lc_viewport strength=%u track=%u", i,
                     point->x, point->y, point->strength, point->track_id);
        }
    }
}
