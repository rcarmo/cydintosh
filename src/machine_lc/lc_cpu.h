#ifndef MACHINE_LC_CPU_H
#define MACHINE_LC_CPU_H

#include "lc_rom.h"

#include <stdint.h>

#ifndef LC_CPU_EXEC_QUANTUM
#define LC_CPU_EXEC_QUANTUM 1000u
#endif

#ifndef LC_CPU_CYCLES_PER_QUANTUM
#define LC_CPU_CYCLES_PER_QUANTUM (LC_CPU_EXEC_QUANTUM * 8u)
#endif

void lc_cpu_log_config(void);
void lc_cpu_log_reset_vector_candidates(const lc_rom_info_t *rom_info);

#endif
