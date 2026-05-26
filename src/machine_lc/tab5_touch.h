#ifndef MACHINE_LC_TAB5_TOUCH_H
#define MACHINE_LC_TAB5_TOUCH_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef TAB5_TOUCH_I2C_FREQ_HZ
#define TAB5_TOUCH_I2C_FREQ_HZ 400000u
#endif

typedef struct {
    bool bus_ready;
    bool gt911_present;
    bool st7123_present;
    uint8_t gt911_product_id[4];
} tab5_touch_probe_result_t;

esp_err_t tab5_touch_probe(tab5_touch_probe_result_t *result);
void tab5_touch_log_config(void);
void tab5_touch_log_probe_result(const tab5_touch_probe_result_t *result);

#endif
