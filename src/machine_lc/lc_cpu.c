#include "lc_cpu.h"

#include "esp_log.h"
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
    ESP_LOGI(TAG, "LC raw ROM vector candidates: long0=0x%08" PRIx32 " long1=0x%08" PRIx32,
             raw_first_long, raw_second_long);
    ESP_LOGW(TAG,
             "LC reset SP/PC mapping is not verified yet; first long currently doubles as ROM fingerprint, not a trusted stack pointer");
}
