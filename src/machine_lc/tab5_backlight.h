#ifndef MACHINE_LC_TAB5_BACKLIGHT_H
#define MACHINE_LC_TAB5_BACKLIGHT_H

#include "esp_err.h"

#include <stdint.h>

#ifndef TAB5_BACKLIGHT_BOOT_PERCENT
#define TAB5_BACKLIGHT_BOOT_PERCENT 20u
#endif

esp_err_t tab5_backlight_init(uint8_t percent);
esp_err_t tab5_backlight_set_percent(uint8_t percent);
esp_err_t tab5_backlight_off(void);
void tab5_backlight_boot_pulse(void);
_Noreturn void tab5_backlight_heartbeat_loop(void);
void tab5_backlight_log_config(void);

#endif
