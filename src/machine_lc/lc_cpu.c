#include "lc_cpu.h"

#include "esp_log.h"
#include "lc_trace.h"
#include "m68k.h"

#include <inttypes.h>

static const char *TAG = "lc_cpu";

#if M68K_EMULATE_EC020 != OPT_ON
#error "Macintosh LC CPU scaffold requires M68K_EMULATE_EC020=OPT_ON"
#endif

#if M68K_EMULATE_020 != OPT_ON
#error "Macintosh LC CPU scaffold requires M68K_EMULATE_020=OPT_ON"
#endif

#ifdef M68K_FIXED_CPU_TYPE
#error "Macintosh LC CPU scaffold must not use M68K_FIXED_CPU_TYPE"
#endif

static const char *lc_cpu_type_name(unsigned int cpu_type) {
    switch (cpu_type) {
    case M68K_CPU_TYPE_68EC020:
        return "68EC020";
    case M68K_CPU_TYPE_68020:
        return "68020";
    default:
        return "unexpected";
    }
}

void lc_cpu_log_config(void) {
    const unsigned int initial_cpu_type = M68K_CPU_TYPE_68EC020;
    lc_trace_record(LC_TRACE_EVENT_CPU_CONFIG, 0, 0, initial_cpu_type, 0, false);
    ESP_LOGI(TAG, "LC CPU scaffold: initial_cpu=%s musashi_type=%u address_mode=24-bit-first",
             lc_cpu_type_name(initial_cpu_type), initial_cpu_type);
    ESP_LOGI(TAG, "LC CPU scaffold: emulate_010=%d emulate_ec020=%d emulate_020=%d emulate_030=%d emulate_040=%d",
             M68K_EMULATE_010, M68K_EMULATE_EC020, M68K_EMULATE_020,
#ifdef M68K_EMULATE_030
             M68K_EMULATE_030,
#else
             OPT_OFF,
#endif
             M68K_EMULATE_040);
    ESP_LOGI(TAG, "LC CPU scaffold: exec_quantum=%u cycles_per_quantum=%u runtime_execution=disabled",
             (unsigned)LC_CPU_EXEC_QUANTUM, (unsigned)LC_CPU_CYCLES_PER_QUANTUM);
}

void lc_cpu_log_reset_vector_candidates(const lc_rom_info_t *rom_info) {
    if (rom_info == NULL || rom_info->partition == NULL) {
        ESP_LOGW(TAG, "LC reset-vector candidates unavailable: ROM partition not mapped");
        return;
    }

    const uint32_t raw_first_long = lc_read_be32(&rom_info->first16[0]);
    const uint32_t raw_second_long = lc_read_be32(&rom_info->first16[4]);
    lc_trace_record(LC_TRACE_EVENT_ROM_VECTOR_CANDIDATE, raw_second_long, 0, raw_first_long, 4,
                    false);
    ESP_LOGI(TAG, "LC raw ROM vector candidates: long0=0x%08" PRIx32 " long1=0x%08" PRIx32,
             raw_first_long, raw_second_long);
    ESP_LOGW(TAG,
             "LC reset SP/PC mapping is not verified yet; first long currently doubles as ROM fingerprint, not a trusted stack pointer");
}

void lc_cpu_log_trace_hook_status(void) {
    ESP_LOGI(TAG,
             "LC trace hooks available: exception vectors, illegal instructions, bus/address errors, interrupt levels");
    ESP_LOGI(TAG, "LC trace hooks are scaffold-only until reset-vector execution is enabled");
}

void lc_cpu_trace_exception(uint8_t vector, uint32_t pc, uint16_t sr) {
    lc_trace_record(LC_TRACE_EVENT_EXCEPTION, pc, vector, sr, 0, false);
    ESP_LOGW(TAG, "LC exception vector=%u pc=0x%08" PRIx32 " sr=0x%04x", vector, pc,
             sr);
}

void lc_cpu_trace_illegal_instruction(uint32_t pc, uint16_t opcode) {
    lc_trace_record(LC_TRACE_EVENT_ILLEGAL_INSTRUCTION, pc, 0, opcode, 2, false);
    ESP_LOGW(TAG, "LC illegal/unimplemented instruction pc=0x%08" PRIx32 " opcode=0x%04x",
             pc, opcode);
}

void lc_cpu_trace_bus_error(uint32_t pc, uint32_t address, unsigned size, bool write) {
    lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, pc, address, 0, (uint16_t)size, write);
    ESP_LOGW(TAG, "LC bus error pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32, pc,
             write ? "write" : "read", size, address);
}

void lc_cpu_trace_address_error(uint32_t pc, uint32_t address, unsigned size, bool write) {
    lc_trace_record(LC_TRACE_EVENT_ADDRESS_ERROR, pc, address, 0, (uint16_t)size, write);
    ESP_LOGW(TAG, "LC address error pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32, pc,
             write ? "write" : "read", size, address);
}

void lc_cpu_trace_interrupt_level(uint32_t pc, unsigned level) {
    lc_trace_record(LC_TRACE_EVENT_INTERRUPT, pc, level, 0, 0, false);
    ESP_LOGI(TAG, "LC interrupt level=%u pc=0x%08" PRIx32, level, pc);
}
