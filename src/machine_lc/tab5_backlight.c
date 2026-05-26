#include "tab5_backlight.h"

#include "board_profiles.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdbool.h>

static const char *TAG = "tab5_bl";

#define TAB5_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define TAB5_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define TAB5_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define TAB5_BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define TAB5_BACKLIGHT_LEDC_MAX_DUTY ((1u << 10u) - 1u)
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
             "Tab5 backlight scaffold: gpio=%d ledc_mode=%d timer=%d channel=%d freq=%uHz duty_res=10 boot_percent=%u initialized=%s current=%u",
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
    const uint8_t sequence[] = {5, 60, 15, 45, final_percent};
    ESP_LOGI(TAG, "Tab5 backlight boot pulse start; final=%u%%", (unsigned)final_percent);
    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        tab5_backlight_set_percent(sequence[i]);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ESP_LOGI(TAG, "Tab5 backlight boot pulse complete");
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
