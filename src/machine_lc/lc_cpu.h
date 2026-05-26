#ifndef MACHINE_LC_CPU_H
#define MACHINE_LC_CPU_H

#include "lc_memory.h"
#include "lc_rom.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef LC_CPU_EXEC_QUANTUM
#define LC_CPU_EXEC_QUANTUM 1000u
#endif

#ifndef LC_CPU_CYCLES_PER_QUANTUM
#define LC_CPU_CYCLES_PER_QUANTUM (LC_CPU_EXEC_QUANTUM * 8u)
#endif

void lc_cpu_log_config(void);
void lc_cpu_log_reset_vector_candidates(const lc_rom_info_t *rom_info);
void lc_cpu_scan_reset_vector_candidates(const lc_rom_map_t *rom_map);
void lc_cpu_scan_rom_entry_hints(const lc_rom_map_t *rom_map);
void lc_cpu_preview_rom_vector_candidates(lc_memory_bus_t *bus);
void lc_cpu_probe_synthetic_bus_execution(lc_memory_bus_t *bus);
void lc_cpu_log_trace_hook_status(void);
void lc_cpu_trace_exception(uint8_t vector, uint32_t pc, uint16_t sr);
void lc_cpu_trace_illegal_instruction(uint32_t pc, uint16_t opcode);
void lc_cpu_trace_bus_error(uint32_t pc, uint32_t address, unsigned size, bool write);
void lc_cpu_trace_address_error(uint32_t pc, uint32_t address, unsigned size, bool write);
void lc_cpu_trace_interrupt_level(uint32_t pc, unsigned level);

#endif
