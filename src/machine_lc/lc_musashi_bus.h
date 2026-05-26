#ifndef MACHINE_LC_MUSASHI_BUS_H
#define MACHINE_LC_MUSASHI_BUS_H

#include "lc_memory.h"

#include "esp_err.h"

#include <stdint.h>

void lc_musashi_bus_attach(lc_memory_bus_t *bus);
void lc_musashi_bus_detach(void);
lc_memory_bus_t *lc_musashi_bus_active(void);
uint32_t lc_musashi_bus_function_code(void);
uint32_t lc_musashi_bus_current_pc(void);
uint32_t lc_musashi_bus_reset_callback_count(void);
void lc_musashi_bus_reset_stats(void);
void lc_musashi_bus_log_stats(void);
esp_err_t lc_musashi_bus_write_synthetic_program(lc_memory_bus_t *bus, uint32_t sp,
                                                 uint32_t pc);

#endif
