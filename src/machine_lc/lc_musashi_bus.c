#include "lc_musashi_bus.h"

#include "esp_check.h"
#include "esp_log.h"
#include "lc_trace.h"
#include "m68k.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

static const char *TAG = "lc_musashi_bus";

static lc_memory_bus_t *active_bus;
static uint32_t current_function_code;
static uint32_t current_instruction_pc;
static uint32_t previous_instruction_pc;
static uint32_t reset_callback_count;
static uint32_t irq_ack_count;
static uint32_t instruction_callback_count;

#ifndef LC_MUSASHI_TRACE_ROM_WATCHPOINTS
#define LC_MUSASHI_TRACE_ROM_WATCHPOINTS 1
#endif

#if LC_MUSASHI_TRACE_ROM_WATCHPOINTS
typedef struct {
    uint32_t rom_offset;
    const char *label;
    bool seen;
} lc_rom_watchpoint_t;

static lc_rom_watchpoint_t rom_watchpoints[] = {
    {0x0000008cu, "reset-header-entry", false},
    {0x000000b4u, "post-hw-init-return", false},
    {0x000008e0u, "normal-reset-continuation", false},
    {0x00002e00u, "hw-init-entry", false},
    {0x00002f18u, "machine-id-dispatch", false},
    {0x00002f52u, "machine-dispatch-fallback", false},
    {0x00003048u, "machine-probe-next", false},
    {0x00003054u, "machine-probe-resume", false},
    {0x00003a96u, "hw-table-relocate", false},
    {0x00046674u, "slot-sense-pack-entry", false},
    {0x00046680u, "slot-sense-pack-flags", false},
    {0x000466cau, "slot-sense-pack-return", false},
    {0x0004641cu, "reset-dispatch-machine-class", false},
    {0x00046462u, "reset-dispatch-set-bit26", false},
    {0x00046494u, "reset-dispatch-skip-bit26", false},
    {0x000465e4u, "reset-subtest-table-loop", false},
    {0x00046620u, "reset-subtest-bit26-check", false},
    {0x00046628u, "reset-subtest-monitor-branch", false},
    {0x00046630u, "reset-final-bit26-check", false},
    {0x000467a6u, "machine-sense-dispatch", false},
    {0x000467b4u, "slot-mem-test-entry", false},
    {0x00046804u, "slot-mem-test-return", false},
    {0x00046850u, "ram-fill-forward-entry", false},
    {0x0004694cu, "ram-fill-forward-return", false},
    {0x00048cd0u, "diagnostic-preflight-entry", false},
    {0x00048cd2u, "diagnostic-preflight-entry-branch", false},
    {0x00048cdau, "diagnostic-preflight-sense", false},
    {0x00048ce8u, "diagnostic-preflight-bit26-test", false},
    {0x00048d04u, "diagnostic-preflight-monitor-branch", false},
    {0x00045c0cu, "f14000-slot-probe-start", false},
    {0x00045e3au, "f14000-slot-probe-outer-wait", false},
    {0x00045e44u, "f14000-slot-probe-wait-complete", false},
    {0x00045e38u, "f14000-slot-probe-return", false},
    {0x00049890u, "diagnostic-monitor-vector", false},
    {0x0004989cu, "diagnostic-monitor-entry", false},
    {0x000498a0u, "diagnostic-monitor-clear-flags", false},
    {0x000498a8u, "diagnostic-monitor-post-sense", false},
    {0x000498b0u, "diagnostic-monitor-bit26-test", false},
    {0x000498beu, "diagnostic-monitor-bit12-test", false},
    {0x000498ccu, "diagnostic-monitor-slot-call", false},
    {0x000498d6u, "diagnostic-monitor-slot-jump", false},
    {0x000498dau, "diagnostic-monitor-io-setup", false},
    {0x00049e68u, "monitor-select-io-base", false},
    {0x00049e96u, "monitor-init-scc-like", false},
    {0x00049fcau, "monitor-command-poll", false},
};

static void lc_musashi_bus_reset_rom_watchpoints(void) {
    for (size_t i = 0; i < sizeof(rom_watchpoints) / sizeof(rom_watchpoints[0]); i++) {
        rom_watchpoints[i].seen = false;
    }
}

static void lc_musashi_bus_log_rom_watchpoint(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    for (size_t i = 0; i < sizeof(rom_watchpoints) / sizeof(rom_watchpoints[0]); i++) {
        lc_rom_watchpoint_t *watch = &rom_watchpoints[i];
        if (watch->seen || watch->rom_offset != rom_offset) {
            continue;
        }
        watch->seen = true;
        ESP_LOGI(TAG,
                 "LC ROM watchpoint: label=%s pc=0x%08" PRIx32
                 " prev_pc=0x%08" PRIx32
                 " d0=0x%08x d1=0x%08x d2=0x%08x d6=0x%08x d7=0x%08x"
                 " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a6=0x%08x sp=0x%08x usp=0x%08x sr=0x%04x",
                 watch->label, pc, previous_instruction_pc,
                 m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                 m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D6),
                 m68k_get_reg(NULL, M68K_REG_D7), m68k_get_reg(NULL, M68K_REG_A0),
                 m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
                 m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_A6),
                 m68k_get_reg(NULL, M68K_REG_SP), m68k_get_reg(NULL, M68K_REG_USP),
                 m68k_get_reg(NULL, M68K_REG_SR));
        return;
    }
}
#endif

static unsigned int lc_musashi_read_instr_word(unsigned int address) {
    return lc_memory_bus_read16(active_bus, address);
}

unsigned int (*cpu_read_instr)(unsigned int address) = lc_musashi_read_instr_word;

void lc_musashi_bus_attach(lc_memory_bus_t *bus) {
    active_bus = bus;
    cpu_read_instr = lc_musashi_read_instr_word;
    ESP_LOGI(TAG, "attached LC memory bus to Musashi callbacks: bus=%p ram=%p rom=%p",
             (void *)bus, bus != NULL ? (void *)bus->ram : NULL,
             bus != NULL ? (const void *)bus->rom : NULL);
}

void lc_musashi_bus_detach(void) {
    active_bus = NULL;
}

lc_memory_bus_t *lc_musashi_bus_active(void) {
    return active_bus;
}

uint32_t lc_musashi_bus_function_code(void) {
    return current_function_code;
}

uint32_t lc_musashi_bus_current_pc(void) {
    return current_instruction_pc;
}

uint32_t lc_musashi_bus_reset_callback_count(void) {
    return reset_callback_count;
}

void lc_musashi_bus_reset_stats(void) {
    current_function_code = 0;
    current_instruction_pc = 0;
    previous_instruction_pc = 0;
    reset_callback_count = 0;
    irq_ack_count = 0;
    instruction_callback_count = 0;
#if LC_MUSASHI_TRACE_ROM_WATCHPOINTS
    lc_musashi_bus_reset_rom_watchpoints();
#endif
}

void lc_musashi_bus_log_stats(void) {
    ESP_LOGI(TAG,
             "Musashi callback stats: fc=%" PRIu32 " pc=0x%08" PRIx32
             " reset_callbacks=%" PRIu32 " irq_acks=%" PRIu32
             " instruction_callbacks=%" PRIu32,
             current_function_code, current_instruction_pc, reset_callback_count, irq_ack_count,
             instruction_callback_count);
}

esp_err_t lc_musashi_bus_write_synthetic_program(lc_memory_bus_t *bus, uint32_t sp,
                                                 uint32_t pc) {
    if (bus == NULL || !bus->initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    // Synthetic reset vector + tiny 68k program. This is deliberately RAM-only:
    //   0x0100: NOP
    //   0x0102: NOP
    //   0x0104: BRA.S -2   ; stable self-loop for bounded m68k_execute smoke
    ESP_RETURN_ON_ERROR(lc_memory_bus_write32(bus, 0x00000000u, sp), TAG,
                        "write synthetic SP failed");
    ESP_RETURN_ON_ERROR(lc_memory_bus_write32(bus, 0x00000004u, pc), TAG,
                        "write synthetic PC failed");
    ESP_RETURN_ON_ERROR(lc_memory_bus_write16(bus, pc + 0u, 0x4e71u), TAG,
                        "write synthetic nop0 failed");
    ESP_RETURN_ON_ERROR(lc_memory_bus_write16(bus, pc + 2u, 0x4e71u), TAG,
                        "write synthetic nop1 failed");
    ESP_RETURN_ON_ERROR(lc_memory_bus_write16(bus, pc + 4u, 0x60feu), TAG,
                        "write synthetic bra failed");
    ESP_LOGI(TAG, "wrote synthetic 68k smoke program: sp=0x%08" PRIx32 " pc=0x%08" PRIx32,
             sp, pc);
    return ESP_OK;
}

unsigned int cpu_read_byte(unsigned int address) {
    return lc_memory_bus_read8(active_bus, address);
}

unsigned int cpu_read_word(unsigned int address) {
    return lc_memory_bus_read16(active_bus, address);
}

unsigned int cpu_read_long(unsigned int address) {
    return lc_memory_bus_read32(active_bus, address);
}

void cpu_write_byte(unsigned int address, unsigned int value) {
    (void)lc_memory_bus_write8(active_bus, address, (uint8_t)value);
}

void cpu_write_word(unsigned int address, unsigned int value) {
    (void)lc_memory_bus_write16(active_bus, address, (uint16_t)value);
}

void cpu_write_long(unsigned int address, unsigned int value) {
    (void)lc_memory_bus_write32(active_bus, address, value);
}

void cpu_pulse_reset(void) {
    reset_callback_count++;
    lc_trace_record(LC_TRACE_EVENT_MARKER, 0, 0, 0x4c435253u, 0, false); // 'LCRS'
    ESP_LOGW(TAG, "Musashi RESET callback invoked by guest instruction (count=%" PRIu32 ")",
             reset_callback_count);
}

void cpu_set_fc(unsigned int fc) {
    current_function_code = fc;
}

int cpu_irq_ack(int level) {
    irq_ack_count++;
    lc_trace_record(LC_TRACE_EVENT_INTERRUPT, 0, (uint32_t)level, 0, 0, false);
    return (int)M68K_INT_ACK_AUTOVECTOR;
}

void cpu_instr_callback(int pc) {
    instruction_callback_count++;
    current_instruction_pc = (uint32_t)pc;
#if LC_MUSASHI_TRACE_ROM_WATCHPOINTS
    lc_musashi_bus_log_rom_watchpoint(current_instruction_pc);
#endif
    lc_trace_record(LC_TRACE_EVENT_MARKER, current_instruction_pc, 0, 0x4c434943u, 0,
                    false); // 'LCIC'
    previous_instruction_pc = current_instruction_pc;
}
