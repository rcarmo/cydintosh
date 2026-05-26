#include "tab5_backlight.h"

#include "board_profiles.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "tab5_bl";

#define TAB5_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define TAB5_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
// Match the M5Stack Tab5 demo BSP: low-speed LEDC timer0/channel1, 12-bit duty.
#define TAB5_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_1
#define TAB5_BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_12_BIT
#define TAB5_BACKLIGHT_LEDC_MAX_DUTY ((1u << 12u) - 1u)
#define TAB5_BACKLIGHT_LEDC_FREQ_HZ 5000u

static uint8_t current_percent;
static bool initialized;

static uint32_t duty_from_percent(uint8_t percent) {
    if (percent > 100u) {
        percent = 100u;
    }
    return (TAB5_BACKLIGHT_LEDC_MAX_DUTY * (uint32_t)percent) / 100u;
}

void tab5_backlight_log_config(void) {
    ESP_LOGI(TAG,
             "Tab5 backlight scaffold: gpio=%d ledc_mode=%d timer=%d channel=%d freq=%uHz duty_res=12 boot_percent=%u initialized=%s current=%u",
             TAB5_LCD_BACKLIGHT_GPIO, TAB5_BACKLIGHT_LEDC_MODE, TAB5_BACKLIGHT_LEDC_TIMER,
             TAB5_BACKLIGHT_LEDC_CHANNEL, TAB5_BACKLIGHT_LEDC_FREQ_HZ,
             (unsigned)TAB5_BACKLIGHT_BOOT_PERCENT, initialized ? "yes" : "no",
             (unsigned)current_percent);
}

esp_err_t tab5_backlight_set_percent(uint8_t percent) {
    if (percent > 100u) {
        percent = 100u;
    }
    const uint32_t duty = duty_from_percent(percent);
    esp_err_t err = ledc_set_duty(TAB5_BACKLIGHT_LEDC_MODE, TAB5_BACKLIGHT_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set backlight duty: %s", esp_err_to_name(err));
        return err;
    }
    err = ledc_update_duty(TAB5_BACKLIGHT_LEDC_MODE, TAB5_BACKLIGHT_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update backlight duty: %s", esp_err_to_name(err));
        return err;
    }
    current_percent = percent;
    ESP_LOGI(TAG, "Tab5 backlight set to %u%% duty=%" PRIu32, (unsigned)percent, duty);
    return ESP_OK;
}

esp_err_t tab5_backlight_off(void) {
    return tab5_backlight_set_percent(0);
}

void tab5_backlight_boot_pulse(void) {
    if (!initialized) {
        return;
    }
    const uint8_t final_percent = current_percent;
    const uint8_t sequence[] = {0, 100, 0, 100, 10, 70, final_percent};
    ESP_LOGI(TAG, "Tab5 backlight boot pulse start; final=%u%%", (unsigned)final_percent);
    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        tab5_backlight_set_percent(sequence[i]);
        vTaskDelay(pdMS_TO_TICKS(180));
    }
    ESP_LOGI(TAG, "Tab5 backlight boot pulse complete");
}

_Noreturn void tab5_backlight_heartbeat_loop(void) {
    ESP_LOGI(TAG, "Tab5 backlight heartbeat loop active");
    while (true) {
        tab5_backlight_set_percent(12);
        vTaskDelay(pdMS_TO_TICKS(900));
        tab5_backlight_set_percent(65);
        vTaskDelay(pdMS_TO_TICKS(120));
        tab5_backlight_set_percent(20);
        vTaskDelay(pdMS_TO_TICKS(900));
        tab5_backlight_set_percent(45);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

static void boot_probe_write(const char *message) {
    esp_rom_printf("%s", message);
    (void)usb_serial_jtag_write_bytes(message, strlen(message), pdMS_TO_TICKS(20));
}

#define TAB5_PI4IOE1_ADDR 0x43u
#define TAB5_PI4IOE2_ADDR 0x44u
#define TAB5_PI4IO_REG_CHIP_RESET 0x01u
#define TAB5_PI4IO_REG_IO_DIR 0x03u
#define TAB5_PI4IO_REG_OUT_SET 0x05u
#define TAB5_PI4IO_REG_OUT_H_IM 0x07u
#define TAB5_PI4IO_REG_IN_DEF_STA 0x09u
#define TAB5_PI4IO_REG_PULL_EN 0x0Bu
#define TAB5_PI4IO_REG_PULL_SEL 0x0Du
#define TAB5_PI4IO_REG_INT_MASK 0x11u

static esp_err_t tab5_boot_probe_add_i2c_device(i2c_master_bus_handle_t bus, uint8_t addr,
                                                i2c_master_dev_handle_t *dev) {
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &dev_config, dev);
}

static esp_err_t tab5_boot_probe_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                                               uint8_t value) {
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(dev, data, sizeof(data), 50);
}

static void tab5_boot_probe_io_expanders(void) {
    boot_probe_write("TAB5_BOOT_DIAG: initializing Tab5 SYS I2C/PI4IOE expanders\n");

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TAB5_TOUCH_I2C_SDA,
        .scl_io_num = TAB5_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        boot_probe_write("TAB5_BOOT_DIAG: i2c_new_master_bus failed\n");
        return;
    }

    i2c_master_dev_handle_t pi4ioe1 = NULL;
    err = tab5_boot_probe_add_i2c_device(bus, TAB5_PI4IOE1_ADDR, &pi4ioe1);
    if (err == ESP_OK) {
        // Faithful subset of M5Stack BSP PI4IOE1 setup: release LCD_RST/TP_RST/CAM_RST,
        // enable speaker/ext-5V outputs, and disable high-Z on used pins.
        tab5_boot_probe_i2c_write_reg(pi4ioe1, TAB5_PI4IO_REG_CHIP_RESET, 0xff);
        tab5_boot_probe_i2c_write_reg(pi4ioe1, TAB5_PI4IO_REG_IO_DIR, 0b01111111);
        tab5_boot_probe_i2c_write_reg(pi4ioe1, TAB5_PI4IO_REG_OUT_H_IM, 0b00000000);
        tab5_boot_probe_i2c_write_reg(pi4ioe1, TAB5_PI4IO_REG_PULL_SEL, 0b01111111);
        tab5_boot_probe_i2c_write_reg(pi4ioe1, TAB5_PI4IO_REG_PULL_EN, 0b01111111);
        tab5_boot_probe_i2c_write_reg(pi4ioe1, TAB5_PI4IO_REG_OUT_SET, 0b01110110);
        boot_probe_write("TAB5_BOOT_DIAG: PI4IOE1 configured\n");
    } else {
        boot_probe_write("TAB5_BOOT_DIAG: PI4IOE1 add failed\n");
    }

    i2c_master_dev_handle_t pi4ioe2 = NULL;
    err = tab5_boot_probe_add_i2c_device(bus, TAB5_PI4IOE2_ADDR, &pi4ioe2);
    if (err == ESP_OK) {
        // Faithful subset of M5Stack BSP PI4IOE2 setup: WLAN/USB5V/charge defaults
        // and interrupt mask state. This should be harmless for a boot-visible probe.
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_CHIP_RESET, 0xff);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_IO_DIR, 0b10111001);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_OUT_H_IM, 0b00000110);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_PULL_SEL, 0b10111001);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_PULL_EN, 0b11111001);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_IN_DEF_STA, 0b01000000);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_INT_MASK, 0b10111111);
        tab5_boot_probe_i2c_write_reg(pi4ioe2, TAB5_PI4IO_REG_OUT_SET, 0b00001001);
        boot_probe_write("TAB5_BOOT_DIAG: PI4IOE2 configured\n");
    } else {
        boot_probe_write("TAB5_BOOT_DIAG: PI4IOE2 add failed\n");
    }
}

_Noreturn void tab5_backlight_raw_gpio_boot_probe_loop(void) {
    boot_probe_write("\nTAB5_BOOT_DIAG: raw GPIO22 probe loop starting\n");
    tab5_boot_probe_io_expanders();

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << TAB5_LCD_BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        boot_probe_write("TAB5_BOOT_DIAG: gpio_config failed\n");
    }

    while (true) {
        boot_probe_write("TAB5_BOOT_DIAG: GPIO22 high 1500ms\n");
        gpio_set_level(TAB5_LCD_BACKLIGHT_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(1500));

        boot_probe_write("TAB5_BOOT_DIAG: GPIO22 low 1500ms\n");
        gpio_set_level(TAB5_LCD_BACKLIGHT_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

esp_err_t tab5_backlight_init(uint8_t percent) {
    ledc_timer_config_t timer_config = {
        .speed_mode = TAB5_BACKLIGHT_LEDC_MODE,
        .timer_num = TAB5_BACKLIGHT_LEDC_TIMER,
        .duty_resolution = TAB5_BACKLIGHT_LEDC_DUTY_RES,
        .freq_hz = TAB5_BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure backlight LEDC timer: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = TAB5_LCD_BACKLIGHT_GPIO,
        .speed_mode = TAB5_BACKLIGHT_LEDC_MODE,
        .channel = TAB5_BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = TAB5_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = 0},
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure backlight LEDC channel: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "Tab5 backlight LEDC initialized on GPIO%d", TAB5_LCD_BACKLIGHT_GPIO);
    return tab5_backlight_set_percent(percent);
}
