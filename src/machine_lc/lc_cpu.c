#include "lc_cpu.h"

#include "board_profiles.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lc_memory.h"
#include "lc_musashi_bus.h"
#include "lc_trace.h"
#include "m68k.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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
#define LC_CPU_VECTOR_PREVIEW_LIMIT 6u
#define LC_CPU_VECTOR_PREVIEW_WORDS 6u
#define LC_CPU_ENTRY_SCAN_LIMIT 0x0100u
#define LC_CPU_ENTRY_LOG_LIMIT 12u

#ifndef LC_CPU_ROM_ENTRY_PROBE
#define LC_CPU_ROM_ENTRY_PROBE 1
#endif

#ifndef LC_CPU_ROM_ENTRY_PROBE_CYCLES
#define LC_CPU_ROM_ENTRY_PROBE_CYCLES 20000000u
#endif

#ifndef LC_CPU_ROM_ENTRY_PROBE_BASE
#define LC_CPU_ROM_ENTRY_PROBE_BASE LC_ROM_WINDOW_24BIT_BASE_CANDIDATE
#endif

#ifndef LC_CPU_ROM_ENTRY_PROBE_OFFSET
#define LC_CPU_ROM_ENTRY_PROBE_OFFSET 0x0000008cu
#endif

#ifndef LC_CPU_ROM_ENTRY_PROBE_STACK
#define LC_CPU_ROM_ENTRY_PROBE_STACK (LC_GUEST_RAM_SIZE - 0x1000u)
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
    ESP_LOGI(TAG, "LC CPU scaffold: exec_quantum=%u cycles_per_quantum=%u rom_execution=disabled synthetic_probe=enabled",
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

static uint16_t lc_cpu_read_be16(const uint8_t bytes[2]) {
    return (uint16_t)(((uint16_t)bytes[0] << 8u) | (uint16_t)bytes[1]);
}

static const char *opcode_hint(uint16_t opcode) {
    if (opcode == 0x4e71u) {
        return "nop";
    }
    if (opcode == 0x4e75u) {
        return "rts";
    }
    if (opcode == 0x4e73u) {
        return "rte";
    }
    if (opcode == 0x4efau) {
        return "jmp-pc-relative";
    }
    if (opcode == 0x4ebau) {
        return "jsr-pc-relative";
    }
    if ((opcode & 0xff00u) == 0x6000u) {
        return "bra";
    }
    if ((opcode & 0xf000u) == 0x6000u) {
        return "bcc/bsr";
    }
    if ((opcode & 0xf100u) == 0x7000u) {
        return "moveq";
    }
    if ((opcode & 0xf1c0u) == 0x41c0u) {
        return "lea";
    }
    if ((opcode & 0xf000u) == 0x2000u) {
        return "move-long-family";
    }
    if ((opcode & 0xf000u) == 0x3000u) {
        return "move-word-family";
    }
    if ((opcode & 0xf000u) == 0x4000u) {
        return "misc/pea/jsr/jmp-family";
    }
    return "unknown-or-data";
}

static bool opcode_hint_is_code(const char *hint) {
    return hint != NULL && strcmp(hint, "unknown-or-data") != 0;
}

static uint32_t pc_relative_target(uint32_t instruction_address, uint16_t displacement) {
    return instruction_address + 2u + (uint32_t)(int32_t)(int16_t)displacement;
}

static const char *entry_transfer_hint(uint16_t opcode) {
    if (opcode == 0x4efau) {
        return "jmp-pc-relative";
    }
    if (opcode == 0x4ebau) {
        return "jsr-pc-relative";
    }
    if (opcode == 0x6000u) {
        return "bra-word";
    }
    if ((opcode & 0xff00u) == 0x6000u) {
        return "bra-byte";
    }
    if ((opcode & 0xf000u) == 0x6000u) {
        return "bcc/bsr";
    }
    return NULL;
}

static bool entry_transfer_has_word_displacement(uint16_t opcode) {
    if (opcode == 0x4efau || opcode == 0x4ebau) {
        return true;
    }
    if ((opcode & 0xf000u) != 0x6000u) {
        return false;
    }
    return (opcode & 0x00ffu) == 0;
}

static bool entry_transfer_has_byte_displacement(uint16_t opcode) {
    return (opcode & 0xf000u) == 0x6000u && (opcode & 0x00ffu) != 0 &&
           (opcode & 0x00ffu) != 0x00ffu;
}

static bool entry_target_in_rom(uint32_t target_offset, size_t rom_size) {
    return target_offset < rom_size;
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
    uint16_t best_first_opcode = 0;
    const char *best_sp_reason = "none";
    const char *best_opcode_hint = "none";

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
            uint16_t first_opcode = 0;
            const char *first_opcode_hint = "unavailable";
            if ((size_t)pc_rom_offset + 2u <= rom_map->size) {
                first_opcode = lc_cpu_read_be16(&rom_map->bytes[pc_rom_offset]);
                first_opcode_hint = opcode_hint(first_opcode);
                if (opcode_hint_is_code(first_opcode_hint)) {
                    score += 3;
                }
            }
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
                best_first_opcode = first_opcode;
                best_sp_reason = sp_reason;
                best_opcode_hint = first_opcode_hint;
            }
            if (logged < LC_CPU_VECTOR_LOG_LIMIT) {
                ESP_LOGI(TAG,
                         "LC vector candidate[%u]: file_offset=0x%05x sp=0x%08" PRIx32
                         " pc=0x%08" PRIx32 " rom_base=0x%08" PRIx32
                         " pc_rom_offset=0x%05" PRIx32 " score=%u sp=%s opcode=%04x hint=%s",
                         logged, (unsigned)offset, sp, pc, rom_base, pc_rom_offset, score,
                         sp_reason, first_opcode, first_opcode_hint);
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
             " pc_rom_offset=0x%05" PRIx32 " score=%u sp=%s opcode=%04x hint=%s",
             best_offset, best_sp, best_pc, best_rom_base, best_pc_rom_offset, best_score,
             best_sp_reason, best_first_opcode, best_opcode_hint);
    ESP_LOGW(TAG,
             "LC reset-vector scan is heuristic only; do not execute guest ROM until overlay and hardware stubs are verified");
}

void lc_cpu_scan_rom_entry_hints(const lc_rom_map_t *rom_map) {
    if (rom_map == NULL || !rom_map->mapped || rom_map->bytes == NULL || rom_map->size < 16) {
        ESP_LOGW(TAG, "LC ROM entry scan unavailable: ROM is not mapped");
        return;
    }

    const uint32_t rom_bases[] = {
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
    };
    const size_t scan_limit = rom_map->size < LC_CPU_ENTRY_SCAN_LIMIT ? rom_map->size : LC_CPU_ENTRY_SCAN_LIMIT;
    unsigned transfers = 0;
    unsigned reset_opcodes = 0;
    unsigned logged = 0;

    ESP_LOGI(TAG, "LC ROM entry scan: header_checksum=0x%08" PRIx32
                  " header_word1=0x%08" PRIx32 " limit=0x%zx",
             lc_read_be32(&rom_map->bytes[0]), lc_read_be32(&rom_map->bytes[4]), scan_limit);

    for (size_t offset = 0; offset + 2u <= scan_limit; offset += 2u) {
        const uint16_t opcode = lc_cpu_read_be16(&rom_map->bytes[offset]);
        if (opcode == 0x4e70u) {
            reset_opcodes++;
            if (logged < LC_CPU_ENTRY_LOG_LIMIT) {
                ESP_LOGI(TAG, "LC ROM entry hint[%u]: reset opcode at file_offset=0x%05x addr24=0x%08" PRIx32
                              " addr32=0x%08" PRIx32,
                         logged, (unsigned)offset,
                         LC_ROM_WINDOW_24BIT_BASE_CANDIDATE + (uint32_t)offset,
                         LC_ROM_WINDOW_32BIT_BASE_CANDIDATE + (uint32_t)offset);
                logged++;
            }
            continue;
        }

        const char *hint = entry_transfer_hint(opcode);
        if (hint == NULL) {
            continue;
        }

        uint32_t target_offset = 0;
        bool target_known = false;
        if (entry_transfer_has_word_displacement(opcode) && offset + 4u <= rom_map->size) {
            const uint16_t displacement = lc_cpu_read_be16(&rom_map->bytes[offset + 2u]);
            const uint32_t instruction_offset = (uint32_t)offset;
            target_offset = pc_relative_target(instruction_offset, displacement);
            target_known = entry_target_in_rom(target_offset, rom_map->size);
        } else if (entry_transfer_has_byte_displacement(opcode)) {
            const int8_t displacement = (int8_t)(opcode & 0xffu);
            target_offset = (uint32_t)offset + 2u + (uint32_t)(int32_t)displacement;
            target_known = entry_target_in_rom(target_offset, rom_map->size);
        }

        transfers++;
        if (logged < LC_CPU_ENTRY_LOG_LIMIT) {
            uint16_t target_opcode = 0;
            const char *target_opcode_hint = "unavailable";
            if (target_known && target_offset + 2u <= rom_map->size) {
                target_opcode = lc_cpu_read_be16(&rom_map->bytes[target_offset]);
                target_opcode_hint = opcode_hint(target_opcode);
            }
            ESP_LOGI(TAG,
                     "LC ROM entry hint[%u]: file_offset=0x%05x opcode=%04x hint=%s target=%s0x%05" PRIx32
                     " addr24=0x%08" PRIx32 " addr32=0x%08" PRIx32 " target_opcode=%04x target_hint=%s",
                     logged, (unsigned)offset, opcode, hint, target_known ? "" : "invalid:",
                     target_offset, rom_bases[0] + target_offset, rom_bases[1] + target_offset,
                     target_opcode, target_opcode_hint);
            logged++;
        }
    }

    ESP_LOGI(TAG, "LC ROM entry scan complete: transfers=%u reset_opcodes=%u logged=%u",
             transfers, reset_opcodes, logged);
    ESP_LOGW(TAG,
             "LC ROM entry scan suggests a ROM-header trampoline, not a normal SP/PC vector table; reset overlay mapping is still unverified");
}

void lc_cpu_preview_rom_vector_candidates(lc_memory_bus_t *bus) {
    if (bus == NULL || !bus->initialized || bus->rom == NULL || bus->rom_size < 8) {
        ESP_LOGW(TAG, "LC ROM vector opcode preview skipped: memory bus/ROM unavailable");
        return;
    }

    const uint32_t rom_bases[] = {
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
    };
    const size_t scan_limit = bus->rom_size < LC_CPU_VECTOR_SCAN_LIMIT ? bus->rom_size : LC_CPU_VECTOR_SCAN_LIMIT;
    unsigned previews = 0;

    ESP_LOGI(TAG, "LC ROM vector opcode preview: limit=0x%zx preview_words=%u", scan_limit,
             (unsigned)LC_CPU_VECTOR_PREVIEW_WORDS);
    for (size_t offset = 0; offset + 8u <= scan_limit; offset += 4u) {
        const uint32_t sp = lc_read_be32(&bus->rom[offset]);
        const uint32_t pc = lc_read_be32(&bus->rom[offset + 4u]);
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
            if (!pc_in_rom_window(pc, rom_base, bus->rom_size, &pc_rom_offset, &pc_score)) {
                continue;
            }

            const uint32_t fetch_pc = rom_base + pc_rom_offset;
            uint16_t words[LC_CPU_VECTOR_PREVIEW_WORDS] = {0};
            for (unsigned i = 0; i < LC_CPU_VECTOR_PREVIEW_WORDS; i++) {
                words[i] = lc_memory_bus_read16(bus, fetch_pc + (uint32_t)(i * 2u));
            }
            ESP_LOGI(TAG,
                     "LC ROM candidate preview[%u]: vector_offset=0x%05x sp=0x%08" PRIx32
                     " pc=0x%08" PRIx32 " fetch=0x%08" PRIx32
                     " words=%04x %04x %04x %04x %04x %04x hint=%s",
                     previews, (unsigned)offset, sp, pc, fetch_pc, words[0], words[1],
                     words[2], words[3], words[4], words[5], opcode_hint(words[0]));
            previews++;
            if (previews >= LC_CPU_VECTOR_PREVIEW_LIMIT) {
                ESP_LOGI(TAG, "LC ROM vector opcode preview stopped after %u candidates",
                         previews);
                return;
            }
        }
    }

    if (previews == 0) {
        ESP_LOGW(TAG, "LC ROM vector opcode preview found no plausible candidates");
    }
}

void lc_cpu_probe_rom_entry_execution(lc_memory_bus_t *bus) {
#if LC_CPU_ROM_ENTRY_PROBE
    if (bus == NULL || !bus->initialized || bus->ram == NULL || bus->rom == NULL) {
        ESP_LOGW(TAG, "LC ROM entry micro-probe skipped: memory bus unavailable");
        return;
    }
    if (LC_CPU_ROM_ENTRY_PROBE_OFFSET + 2u > bus->rom_size) {
        ESP_LOGW(TAG, "LC ROM entry micro-probe skipped: entry offset 0x%08x outside ROM size=0x%zx",
                 (unsigned)LC_CPU_ROM_ENTRY_PROBE_OFFSET, bus->rom_size);
        return;
    }

    const uint32_t entry_pc = LC_CPU_ROM_ENTRY_PROBE_BASE + LC_CPU_ROM_ENTRY_PROBE_OFFSET;
    ESP_LOGW(TAG,
             "LC ROM entry micro-probe enabled: bounded diagnostic only, not a boot claim; base=0x%08x entry_offset=0x%05x cycles=%u stack=0x%08x",
             (unsigned)LC_CPU_ROM_ENTRY_PROBE_BASE,
             (unsigned)LC_CPU_ROM_ENTRY_PROBE_OFFSET,
             (unsigned)LC_CPU_ROM_ENTRY_PROBE_CYCLES,
             (unsigned)LC_CPU_ROM_ENTRY_PROBE_STACK);

    memset(bus->ram, 0, bus->ram_size);
    lc_musashi_bus_reset_stats();
    lc_musashi_bus_attach(bus);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SP, LC_CPU_ROM_ENTRY_PROBE_STACK);
    m68k_set_reg(M68K_REG_SR, 0x2700u);
    m68k_set_reg(M68K_REG_PC, entry_pc);

    const uint16_t first_opcode = lc_memory_bus_read16(bus, entry_pc);
    ESP_LOGI(TAG,
             "LC ROM entry micro-probe: entry_pc=0x%08" PRIx32
             " first_opcode=0x%04x hint=%s sp=0x%08x",
             entry_pc, first_opcode, opcode_hint(first_opcode),
             (unsigned)LC_CPU_ROM_ENTRY_PROBE_STACK);
    const int cycles = m68k_execute((int)LC_CPU_ROM_ENTRY_PROBE_CYCLES);
    const unsigned int pc_after = m68k_get_reg(NULL, M68K_REG_PC);
    const unsigned int sp_after = m68k_get_reg(NULL, M68K_REG_SP);
    const unsigned int sr_after = m68k_get_reg(NULL, M68K_REG_SR);
    const unsigned int d0_after = m68k_get_reg(NULL, M68K_REG_D0);
    lc_trace_record(LC_TRACE_EVENT_CPU_CONFIG, pc_after, entry_pc, d0_after,
                    (uint16_t)cycles, false);
    ESP_LOGI(TAG,
             "LC ROM entry micro-probe result: cycles=%d pc_after=0x%08x sp_after=0x%08x sr=0x%04x d0=0x%08x reset_callbacks=%" PRIu32,
             cycles, pc_after, sp_after, sr_after, d0_after,
             lc_musashi_bus_reset_callback_count());
    lc_musashi_bus_log_stats();
    lc_memory_log_io_stub_summary();
    lc_trace_dump_recent(48);
    lc_musashi_bus_detach();
    ESP_LOGW(TAG,
             "LC ROM entry micro-probe completed; full LC ROM execution remains disabled until overlay and hardware stubs are verified");
#else
    (void)bus;
    ESP_LOGI(TAG, "LC ROM entry micro-probe disabled at compile time");
#endif
}

void lc_cpu_probe_synthetic_bus_execution(lc_memory_bus_t *bus) {
    if (bus == NULL || !bus->initialized) {
        ESP_LOGW(TAG, "LC synthetic CPU/bus probe skipped: memory bus unavailable");
        return;
    }

    const uint32_t synthetic_sp = 0x00002000u;
    const uint32_t synthetic_pc = 0x00000100u;
    esp_err_t err = lc_musashi_bus_write_synthetic_program(bus, synthetic_sp, synthetic_pc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LC synthetic CPU/bus probe program write failed: %s", esp_err_to_name(err));
        return;
    }

    lc_musashi_bus_reset_stats();
    lc_musashi_bus_attach(bus);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, 0x2700u);
    const unsigned int pc_after_reset = m68k_get_reg(NULL, M68K_REG_PC);
    const unsigned int sp_after_reset = m68k_get_reg(NULL, M68K_REG_SP);
    const int cycles = m68k_execute(64);
    const unsigned int pc_after_execute = m68k_get_reg(NULL, M68K_REG_PC);
    const unsigned int sr_after_execute = m68k_get_reg(NULL, M68K_REG_SR);
    const unsigned int cpu_type = m68k_get_reg(NULL, M68K_REG_CPU_TYPE);

    lc_trace_record(LC_TRACE_EVENT_CPU_CONFIG, pc_after_execute, synthetic_pc, cpu_type,
                    (uint16_t)cycles, false);
    ESP_LOGI(TAG,
             "LC synthetic 68EC020 bus probe: reset_pc=0x%08x reset_sp=0x%08x cycles=%d pc_after=0x%08x sr=0x%04x cpu_type=%u",
             pc_after_reset, sp_after_reset, cycles, pc_after_execute, sr_after_execute,
             cpu_type);
    lc_musashi_bus_log_stats();
    lc_musashi_bus_detach();
    ESP_LOGW(TAG, "LC synthetic CPU/bus probe executed RAM-only test code; LC ROM execution remains disabled");
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
