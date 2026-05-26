#include "lc_musashi_bus.h"

#include "esp_check.h"
#include "esp_log.h"
#include "lc_trace.h"
#include "m68k.h"

#include <inttypes.h>

static const char *TAG = "lc_musashi_bus";

static lc_memory_bus_t *active_bus;
static uint32_t current_function_code;
static uint32_t reset_callback_count;
static uint32_t irq_ack_count;
static uint32_t instruction_callback_count;

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

uint32_t lc_musashi_bus_reset_callback_count(void) {
    return reset_callback_count;
}

void lc_musashi_bus_reset_stats(void) {
    current_function_code = 0;
    reset_callback_count = 0;
    irq_ack_count = 0;
    instruction_callback_count = 0;
}

void lc_musashi_bus_log_stats(void) {
    ESP_LOGI(TAG,
             "Musashi callback stats: fc=%" PRIu32 " reset_callbacks=%" PRIu32
             " irq_acks=%" PRIu32 " instruction_callbacks=%" PRIu32,
             current_function_code, reset_callback_count, irq_ack_count,
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
    lc_trace_record(LC_TRACE_EVENT_MARKER, (uint32_t)pc, 0, 0x4c434943u, 0,
                    false); // 'LCIC'
}
