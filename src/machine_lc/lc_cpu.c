#include "lc_cpu.h"

#include "board_profiles.h"
#include "esp_log.h"
#include "lc_memory.h"
#include "lc_trace.h"
#include "m68k.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

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

#define LC_CPU_VECTOR_SCAN_LIMIT 0x4000u
#define LC_CPU_VECTOR_LOG_LIMIT 8u

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

static bool plausible_initial_sp(uint32_t sp, const char **reason_out, unsigned *score_out) {
    const char *reason = "sp-ok";
    unsigned score = 1;
    if (sp == 0) {
        reason = "sp-zero";
        goto reject;
    }
    if ((sp & 1u) != 0) {
        reason = "sp-odd";
        goto reject;
    }
    if (sp > LC_GUEST_RAM_SIZE) {
        reason = "sp-outside-initial-ram";
        goto reject;
    }
    if (sp >= LC_GUEST_RAM_SIZE - 0x10000u) {
        reason = "sp-near-ram-top";
        score += 2;
    } else {
        reason = "sp-in-ram";
    }
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    if (score_out != NULL) {
        *score_out = score;
    }
    return true;

reject:
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    if (score_out != NULL) {
        *score_out = 0;
    }
    return false;
}

static bool pc_in_rom_window(uint32_t pc, uint32_t rom_base, size_t rom_size,
                             uint32_t *rom_offset_out, unsigned *score_out) {
    unsigned score = 1;
    if ((pc & 1u) != 0) {
        return false;
    }

    uint32_t rom_offset = 0;
    if (pc >= rom_base && pc < rom_base + rom_size) {
        rom_offset = pc - rom_base;
        score += 2;
    } else {
        const uint32_t pc24 = pc & 0x00ffffffu;
        const uint32_t base24 = rom_base & 0x00ffffffu;
        if (pc24 < base24 || pc24 >= base24 + rom_size) {
            return false;
        }
        rom_offset = pc24 - base24;
        score += 1;
    }

    if (rom_offset < 0x1000u) {
        score += 2;
    }
    if ((pc & 0xff000000u) == (rom_base & 0xff000000u)) {
        score += 1;
    }
    if (rom_offset_out != NULL) {
        *rom_offset_out = rom_offset;
    }
    if (score_out != NULL) {
        *score_out = score;
    }
    return true;
}

void lc_cpu_scan_reset_vector_candidates(const lc_rom_map_t *rom_map) {
    if (rom_map == NULL || !rom_map->mapped || rom_map->bytes == NULL || rom_map->size < 8) {
        ESP_LOGW(TAG, "LC reset-vector scan unavailable: ROM is not mapped");
        return;
    }

    const uint32_t rom_bases[] = {
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
    };
    const size_t scan_limit = rom_map->size < LC_CPU_VECTOR_SCAN_LIMIT ? rom_map->size : LC_CPU_VECTOR_SCAN_LIMIT;
    unsigned candidates = 0;
    unsigned logged = 0;
    unsigned best_score = 0;
    uint32_t best_offset = 0;
    uint32_t best_sp = 0;
    uint32_t best_pc = 0;
    uint32_t best_rom_base = 0;
    uint32_t best_pc_rom_offset = 0;
    const char *best_sp_reason = "none";

    ESP_LOGI(TAG, "LC reset-vector scan: limit=0x%zx initial_ram=0x%08x rom_bases=0x%08x,0x%08x",
             scan_limit, LC_GUEST_RAM_SIZE, LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
             LC_ROM_WINDOW_32BIT_BASE_CANDIDATE);

    for (size_t offset = 0; offset + 8u <= scan_limit; offset += 4u) {
        const uint32_t sp = lc_read_be32(&rom_map->bytes[offset]);
        const uint32_t pc = lc_read_be32(&rom_map->bytes[offset + 4u]);
        const char *sp_reason = NULL;
        unsigned sp_score = 0;
        if (!plausible_initial_sp(sp, &sp_reason, &sp_score)) {
            continue;
        }

        for (size_t base_index = 0; base_index < sizeof(rom_bases) / sizeof(rom_bases[0]);
             base_index++) {
            uint32_t pc_rom_offset = 0;
            unsigned pc_score = 0;
            const uint32_t rom_base = rom_bases[base_index];
            if (!pc_in_rom_window(pc, rom_base, rom_map->size, &pc_rom_offset, &pc_score)) {
                continue;
            }

            unsigned score = sp_score + pc_score;
            if (offset == 0) {
                score += 3;
            }
            candidates++;
            lc_trace_record(LC_TRACE_EVENT_ROM_VECTOR_CANDIDATE, pc, rom_base, sp,
                            (uint16_t)offset, false);
            if (score > best_score) {
                best_score = score;
                best_offset = (uint32_t)offset;
                best_sp = sp;
                best_pc = pc;
                best_rom_base = rom_base;
                best_pc_rom_offset = pc_rom_offset;
                best_sp_reason = sp_reason;
            }
            if (logged < LC_CPU_VECTOR_LOG_LIMIT) {
                ESP_LOGI(TAG,
                         "LC vector candidate[%u]: file_offset=0x%05x sp=0x%08" PRIx32
                         " pc=0x%08" PRIx32 " rom_base=0x%08" PRIx32
                         " pc_rom_offset=0x%05" PRIx32 " score=%u sp=%s",
                         logged, (unsigned)offset, sp, pc, rom_base, pc_rom_offset, score,
                         sp_reason);
                logged++;
            }
        }
    }

    ESP_LOGI(TAG, "LC reset-vector scan complete: candidates=%u logged=%u", candidates, logged);
    if (candidates == 0) {
        ESP_LOGW(TAG,
                 "LC reset-vector scan found no plausible SP/PC pairs in first 0x%zx bytes; ROM overlay/reset mapping remains unknown",
                 scan_limit);
        return;
    }
    ESP_LOGI(TAG,
             "LC reset-vector best candidate: file_offset=0x%05" PRIx32 " sp=0x%08" PRIx32
             " pc=0x%08" PRIx32 " rom_base=0x%08" PRIx32
             " pc_rom_offset=0x%05" PRIx32 " score=%u sp=%s",
             best_offset, best_sp, best_pc, best_rom_base, best_pc_rom_offset, best_score,
             best_sp_reason);
    ESP_LOGW(TAG,
             "LC reset-vector scan is heuristic only; do not execute guest ROM until overlay and hardware stubs are verified");
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
