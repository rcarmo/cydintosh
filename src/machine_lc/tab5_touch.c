#include "tab5_touch.h"

#include "board_profiles.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "tab5_touch";

static i2c_master_bus_handle_t touch_bus;
static bool touch_bus_initialized;

void tab5_touch_log_config(void) {
    ESP_LOGI(TAG,
             "Tab5 touch scaffold: i2c_port=0 sda=%d scl=%d freq=%u gt911=0x%02x st7123=0x%02x int_gpio=%d bus_ready=%s",
             TAB5_TOUCH_I2C_SDA, TAB5_TOUCH_I2C_SCL, TAB5_TOUCH_I2C_FREQ_HZ,
             TAB5_TOUCH_GT911_ADDR, TAB5_TOUCH_ST7123_ADDR, TAB5_TOUCH_INT_GPIO,
             touch_bus_initialized ? "yes" : "no");
}

static esp_err_t tab5_touch_i2c_init(void) {
    if (touch_bus_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TAB5_TOUCH_I2C_SDA,
        .scl_io_num = TAB5_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &touch_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init Tab5 touch I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    touch_bus_initialized = true;
    ESP_LOGI(TAG, "Tab5 touch I2C bus initialized on SDA=%d SCL=%d", TAB5_TOUCH_I2C_SDA,
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
