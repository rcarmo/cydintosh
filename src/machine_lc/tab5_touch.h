#ifndef MACHINE_LC_TAB5_TOUCH_H
#define MACHINE_LC_TAB5_TOUCH_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef TAB5_TOUCH_I2C_FREQ_HZ
#define TAB5_TOUCH_I2C_FREQ_HZ 400000u
#endif

#define TAB5_TOUCH_MAX_POINTS 5u

typedef enum {
    TAB5_TOUCH_CONTROLLER_NONE = 0,
    TAB5_TOUCH_CONTROLLER_GT911,
    TAB5_TOUCH_CONTROLLER_ST7123,
} tab5_touch_controller_t;

typedef struct {
    bool bus_ready;
    bool gt911_present;
    bool st7123_present;
    uint8_t gt911_product_id[4];
} tab5_touch_probe_result_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
    uint8_t track_id;
    bool in_lc_viewport;
    uint16_t lc_x;
    uint16_t lc_y;
} tab5_touch_point_t;

typedef struct {
    bool initialized;
    tab5_touch_controller_t controller;
    esp_err_t read_status;
    bool pressed;
    uint8_t point_count;
    tab5_touch_point_t points[TAB5_TOUCH_MAX_POINTS];
} tab5_touch_sample_t;

const char *tab5_touch_controller_name(tab5_touch_controller_t controller);
esp_err_t tab5_touch_probe(tab5_touch_probe_result_t *result);
void tab5_touch_log_config(void);
void tab5_touch_log_probe_result(const tab5_touch_probe_result_t *result);
esp_err_t tab5_touch_init_reader(tab5_touch_controller_t *controller_out);
esp_err_t tab5_touch_read_sample(tab5_touch_sample_t *sample);
void tab5_touch_log_sample(const tab5_touch_sample_t *sample);

#endif
