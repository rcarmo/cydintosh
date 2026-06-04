#include "lc_musashi_bus.h"

#include "lc_basilisk_compat.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "lc_disk.h"
#include "lc_trace.h"
#include "m68k.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "lc_musashi_bus";

#ifndef LC_PRODUCTINFO_DEFAULT_RSRCS
#define LC_PRODUCTINFO_DEFAULT_RSRCS 0
#endif

static lc_memory_bus_t *active_bus;
static uint32_t current_function_code;
static uint32_t current_instruction_pc;
static uint32_t previous_instruction_pc;
static uint32_t reset_callback_count;
static uint32_t irq_ack_count;
static uint32_t instruction_callback_count;
static uint32_t reset_scc_timer_wait0_hits;
static uint32_t reset_scc_timer_wait1_hits;
static uint32_t reset_scc_timer_irq_pulses;
static bool reset_scc_timer_irq_asserted;
static uint32_t reset_via_irq_wait0_hits;
static uint32_t reset_via_irq_wait1_hits;
static uint32_t reset_via_irq_pulses;
static bool reset_via_irq_asserted;
static bool get_video_default_stub_logged;
static bool slot_manager_video_default_stub_logged;
static bool control_video_default_stub_logged;
static bool disposeptr_video_default_stub_logged;
static bool swap_mmu_video_default_stub_logged;
static bool post_reset_no_mmu_a001_stub_logged;
static bool post_reset_swap_mmu_dispatch_stub_logged;
static bool post_reset_swap_mmu_dispatch_nop_active;
static bool post_reset_resource_copy_cap_logged;
static bool post_reset_resource_scan_cap_logged;
static unsigned post_reset_resource_lookup_logs;
static unsigned post_reset_resource_low_stack_logs;
static unsigned post_reset_resource_map_loop_hits;
static bool post_reset_resource_map_loop_escape_logged;
static unsigned post_reset_count_combos_logs;
static unsigned post_reset_slot_scan_loop_hits;
static unsigned post_reset_slot_first_scan_loop_hits;
static unsigned post_reset_slot_init_scan_calls;
static unsigned post_reset_atrap_dispatch_logs;
static unsigned post_reset_block_move_logs;
static unsigned post_reset_set_trap_address_logs;
static unsigned post_reset_memory_trap_logs;
static unsigned post_reset_ram_exec_logs;
static unsigned post_reset_invalid_exec_logs;
static unsigned post_reset_trap_return_canonicalize_logs;
static unsigned post_reset_high_dispatch_logs;
static unsigned post_reset_attr_low_dispatch_return_logs;
static bool post_reset_shutdown_rts_logged;
static bool post_reset_probe_tables_seeded;
static bool post_reset_main_zone_seeded;
static bool post_reset_resource_map_handle_registered;
static bool post_reset_rom_map_handle_repair_logged;
static bool post_reset_event_wait_complete_logged;
static bool post_reset_event_wait_capture_logged;
static bool post_reset_vbl_queue_repair_logged;
static bool post_reset_vbl_loop_escape_logged;
static bool post_reset_basilisk_unit_table_newptr_logged;
static bool post_reset_slot_init_success_logged;
static bool post_reset_slot_dispatch_rebuild_skip_logged;
static bool post_reset_getstring_startup_logged;
static bool post_reset_srt_entry_logged;
static bool post_reset_srt_table_seed_logged;
static unsigned post_reset_srt_scan_logs;
static unsigned post_reset_srt_alloc_rts_logs;
static unsigned post_reset_srt_alloc_entry_logs;
static bool post_reset_srt_io_fill_guard_logged;
static bool post_reset_srt_escape_logged;
static bool post_reset_slot_first_pass_escape_logged;
static bool post_reset_device_base_restore_logged;
static bool post_reset_high_trap_handler_sr_logged;
static uint32_t post_reset_event_wait_hits;
static uint32_t post_reset_event_wait_loop_calls;
static uint32_t post_reset_vbl_init_trace_hits;
static uint32_t post_reset_event_wait_saved_a0;
static uint32_t post_reset_event_wait_saved_sp;
static uint32_t post_reset_event_newhandle;
static uint32_t post_reset_event_newhandle_ptr;
static uint32_t post_reset_rom_map_handle_dynamic;
static uint32_t post_reset_univ_info_observed;
static uint32_t post_reset_heap_bump;
static uint32_t post_reset_handle_bump;
static uint32_t post_reset_emergency_bump;
static uint32_t post_reset_low_emergency_bump;
static unsigned basilisk_emul_op_logs;

#define LC_POST_RESET_HANDLE_RECORD_COUNT 1024u

static uint32_t post_reset_handle_size_handle[LC_POST_RESET_HANDLE_RECORD_COUNT];
static uint32_t post_reset_handle_data_ptr[LC_POST_RESET_HANDLE_RECORD_COUNT];
static uint32_t post_reset_handle_size_value[LC_POST_RESET_HANDLE_RECORD_COUNT];
static bool post_reset_handle_locked[LC_POST_RESET_HANDLE_RECORD_COUNT];

typedef struct {
    uint32_t header;
    uint32_t total;
    uint32_t header_size;
} lc_post_reset_free_block_t;

static lc_post_reset_free_block_t post_reset_free_blocks[128];

typedef struct {
    uint32_t pc;
    uint16_t trap_word;
    uint32_t sp;
    uint32_t d0;
    uint32_t d1;
    uint32_t d2;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t stack0;
    uint32_t stack4;
    uint32_t stack8;
    uint32_t stack12;
    uint32_t stack16;
    uint32_t stack20;
    uint32_t stack24;
    uint32_t stack28;
} lc_post_reset_low_trap_snapshot_t;

static lc_post_reset_low_trap_snapshot_t post_reset_low_trap_ring[16];
static unsigned post_reset_low_trap_ring_index;

#define LC_FAKE_VIDEO_RESOURCE_ADDR 0x00001000u

#define LC_LOWMEM_LINE_A_VECTOR 0x00000028u
#define LC_LOWMEM_LOW_TRAP_TABLE 0x00000400u
#define LC_LOWMEM_MEM_TOP 0x00000108u
#define LC_LOWMEM_BUF_PTR 0x0000010cu
#define LC_LOWMEM_HEAP_END 0x00000114u
#define LC_LOWMEM_THE_ZONE 0x00000118u
#define LC_LOWMEM_APPL_LIMIT 0x00000130u
#define LC_LOWMEM_MEM_ERR 0x00000220u
#define LC_LOWMEM_SYS_ZONE 0x000002a6u
#define LC_LOWMEM_APPL_ZONE 0x000002aau
#define LC_LOWMEM_TOP_MAP_HNDL 0x00000a50u
#define LC_LOWMEM_SYS_MAP_HNDL 0x00000a54u
#define LC_LOWMEM_ROM_MAP_HNDL 0x00000b06u

#define LC_RESOURCE_ROM_MASTER_PTR_BASE 0x00008400u
#define LC_RESOURCE_ROM_MASTER_PTR_LIMIT 0x00008700u

#define LC_POST_RESET_MASTER_PTR_BASE 0x00002800u
#define LC_POST_RESET_MASTER_PTR_LIMIT 0x00003800u

#define LC_MEMORY_ZONE_DEFAULT_START 0x00003800u
#define LC_MEMORY_ZONE_HEADER_SIZE 0x00000034u
#define LC_MEMORY_ZONE_MIN_START 0x00001000u
#define LC_MEMORY_BLOCK_SIZE_MASK 0x00ffffffu
#define LC_POST_RESET_HEAP_TOP_RESERVE 0x00040000u
#define LC_POST_RESET_EMERGENCY_HEAP_BASE 0x00090000u
#define LC_POST_RESET_EMERGENCY_HEAP_LIMIT 0x000f0000u
#define LC_POST_RESET_LOW_EMERGENCY_HEAP_BASE 0x0000a000u
#define LC_POST_RESET_LOW_EMERGENCY_HEAP_LIMIT 0x0001a000u
#define LC_POST_RESET_RESOURCE_MAP_PROTECT_SIZE 0x00004000u

#define LC_SLOT_SP_RESULT 0x00u
#define LC_SLOT_SP_MISC 0x1cu
#define LC_SLOT_SP_SLOT 0x31u
#define LC_SLOT_RECORD_BASE 0x00009200u
#define LC_SLOT_RECORD_STRIDE 0x00000020u
#define LC_SLOT_RECORD_STATUS 0x0004u
#define LC_POST_RESET_SRT_BASE 0x00009400u
#define LC_POST_RESET_SRT_RECORD_STRIDE 0x00000018u
#define LC_POST_RESET_SRT_RECORD_COUNT 8u

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
    {0x0000010eu, "basilisk-bootglobs-equivalent-load", false},
    {0x00001120u, "basilisk-unit-table-newptr", false},
    {0x00001134u, "basilisk-early-sony-open-call", false},
    {0x00001140u, "basilisk-install-drivers-branch", false},
    {0x00001142u, "basilisk-install-drivers-emul-op", false},
    {0x00001254u, "basilisk-open-helper-trap", false},
    {0x00001274u, "basilisk-open-helper-fallback-trap", false},
    {0x000008e0u, "normal-reset-continuation", false},
    {0x00002e00u, "hw-init-entry", false},
    {0x00002f18u, "machine-id-dispatch", false},
    {0x00002f52u, "machine-dispatch-fallback", false},
    {0x00003048u, "machine-probe-next", false},
    {0x00003054u, "machine-probe-resume", false},
    {0x00003a96u, "hw-table-relocate", false},
    {0x0004638cu, "diagnostic-stub-set-bit24", false},
    {0x00046396u, "diagnostic-stub-jump-preflight", false},
    {0x00046674u, "slot-sense-pack-entry", false},
    {0x00046680u, "slot-sense-pack-flags", false},
    {0x000466cau, "slot-sense-pack-return", false},
    {0x0004641cu, "reset-dispatch-machine-class", false},
    {0x00046462u, "reset-dispatch-set-bit26", false},
    {0x00046494u, "reset-dispatch-skip-bit26", false},
    {0x00046576u, "reset-region-list-entry", false},
    {0x00046580u, "reset-region-list-start", false},
    {0x00046582u, "reset-region-list-load", false},
    {0x000465a4u, "reset-region-fill-return", false},
    {0x000465a8u, "reset-region-fill-error-test", false},
    {0x000465acu, "reset-region-next", false},
    {0x000465b0u, "reset-region-copy-entry", false},
    {0x000465c0u, "reset-region-copy-error-test", false},
    {0x000465d2u, "reset-vector-relocate-entry", false},
    {0x000465e0u, "reset-subtest-table-start", false},
    {0x000465e4u, "reset-subtest-table-loop", false},
    {0x00046f5au, "reset-scc-register-test-entry", false},
    {0x00046f74u, "reset-scc-register-test-one", false},
    {0x00046f90u, "reset-scc-register-test-restore", false},
    {0x0004703eu, "reset-scc-timer-test-entry", false},
    {0x00047080u, "reset-scc-timer-wait-first", false},
    {0x0004708au, "reset-scc-timer-wait-second", false},
    {0x000470d0u, "reset-scc-timer-test-return", false},
    {0x000470d2u, "reset-scc-timer-autovector-handler", false},
    {0x0004713eu, "reset-via-irq-test-entry", false},
    {0x000471bcu, "reset-via-irq-wait-first", false},
    {0x00047270u, "reset-via-irq-wait-second", false},
    {0x000472b0u, "reset-via-irq-test-restore", false},
    {0x000472cau, "reset-via-irq-test-return", false},
    {0x000472ccu, "reset-via-irq-autovector-handler", false},
    {0x0004730cu, "reset-f10000-register-test-entry", false},
    {0x000473eeu, "reset-f10000-register-test-return", false},
    {0x000473f4u, "reset-f14000-register-test-entry", false},
    {0x00047560u, "reset-f14000-register-test-restore", false},
    {0x000477d2u, "reset-next-subtest-8b-entry", false},
    {0x00047942u, "reset-f16000-shift-test-entry", false},
    {0x00047b48u, "reset-f16000-shift-test-error-100", false},
    {0x00047b5au, "reset-f16000-shift-test-error-200", false},
    {0x00047b70u, "reset-f16000-shift-test-error-300", false},
    {0x00047b82u, "reset-f16000-shift-test-error-400", false},
    {0x00047b94u, "reset-f16000-shift-test-error-500", false},
    {0x00047ba6u, "reset-f16000-shift-test-error-600", false},
    {0x00047bb6u, "reset-f16000-shift-test-error-700", false},
    {0x00047bc4u, "reset-f16000-shift-test-error-800", false},
    {0x00047bfeu, "reset-f16000-shift-test-return", false},
    {0x00047c30u, "reset-next-subtest-8d-entry", false},
    {0x00046604u, "reset-subtest-return", false},
    {0x00046610u, "reset-subtest-error-class", false},
    {0x00046618u, "reset-subtest-error-report", false},
    {0x00046620u, "reset-subtest-bit26-check", false},
    {0x00046624u, "reset-subtest-restore", false},
    {0x00046628u, "reset-subtest-monitor-branch", false},
    {0x0004662eu, "reset-subtest-done", false},
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
    {0x00048d08u, "diagnostic-preflight-bit26-path", false},
    {0x00048d2cu, "diagnostic-preflight-set-monitor-flags", false},
    {0x00048d44u, "diagnostic-preflight-cache-flags", false},
    {0x00048d5cu, "diagnostic-preflight-monitor-branch2", false},
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
    {0x00002310u, "crit-error-entry", false},
    {0x00002316u, "crit-error-magic-check", false},
    {0x0004167eu, "post-reset-memory-layout-error-branch", false},
    {0x0004168eu, "post-reset-memory-layout-entry", false},
    {0x0004169au, "post-reset-memory-layout-no-mmu-check", false},
    {0x000416a2u, "post-reset-memory-layout-early-return", false},
    {0x000416aeu, "post-reset-memory-layout-expanded-path", false},
    {0x000416b8u, "post-reset-memory-layout-finalizer-call", false},
    {0x000416bcu, "post-reset-memory-layout-after-finalizer", false},
    {0x000416c0u, "post-reset-memory-layout-copy-after-finalizer", false},
    {0x000416d4u, "post-reset-memory-layout-second-record-copy", false},
    {0x000416dcu, "post-reset-memory-layout-stack-adjust", false},
    {0x000416e0u, "post-reset-memory-layout-fpu-frontier", false},
    {0x0004172cu, "post-reset-memory-layout-header", false},
    {0x00041740u, "post-reset-memory-layout-mode-byte", false},
    {0x00041770u, "post-reset-memory-layout-pram-byte", false},
    {0x0004178cu, "post-reset-memory-layout-flags-save", false},
    {0x000418e4u, "post-reset-memory-layout-second-pass", false},
    {0x00041900u, "post-reset-memory-layout-dispatch", false},
    {0x00041914u, "post-reset-memory-layout-dispatch-error", false},
    {0x00041922u, "post-reset-memory-layout-dispatch-check", false},
    {0x00041a56u, "post-reset-memory-layout-second-pass-return", false},
    {0x00041b1eu, "post-reset-memory-layout-record-copy", false},
    {0x00041b34u, "post-reset-memory-layout-record-word", false},
    {0x00041b3eu, "post-reset-memory-layout-record-next", false},
    {0x00041b8eu, "post-reset-memory-layout-finalize-entry", false},
    {0x00041bd0u, "post-reset-memory-layout-finalize-restore", false},
    {0x00041bd4u, "post-reset-memory-layout-finalize-unlink", false},
    {0x00041c46u, "post-reset-memory-layout-case-entry", false},
    {0x00041c72u, "post-reset-memory-layout-case-dispatch-source", false},
    {0x00041c7eu, "post-reset-memory-layout-case-table", false},
    {0x00041cbcu, "post-reset-memory-layout-case-f", false},
    {0x00041cbeu, "post-reset-memory-layout-case-f-misaligned", false},
    {0x00041cdau, "post-reset-memory-layout-return", false},
    {0x00041d5eu, "post-reset-memory-layout-push-entry", false},
    {0x00041d72u, "post-reset-memory-layout-pack-entry", false},
    {0x00041d92u, "post-reset-memory-layout-empty-sentinel", false},
    {0x00041d98u, "post-reset-memory-layout-after-sentinel", false},
    {0x00041db8u, "post-reset-memory-layout-count-entry", false},
    {0x00041e1eu, "post-reset-memory-layout-compress-entry", false},
    {0x00041e34u, "post-reset-memory-layout-compress-status", false},
    {0x00041e4eu, "post-reset-memory-layout-compress-return", false},
    {0x00041e52u, "post-reset-memory-layout-compress-pass-entry", false},
    {0x00041e66u, "post-reset-memory-layout-compress-loop", false},
    {0x00041e86u, "post-reset-memory-layout-compress-loop-next", false},
    {0x00041e94u, "post-reset-memory-layout-compress-trap", false},
    {0x00041e96u, "post-reset-memory-layout-compress-scan", false},
    {0x00041ea4u, "post-reset-memory-layout-compress-scan-read", false},
    {0x00041eceu, "post-reset-memory-layout-compress-alloc", false},
    {0x00041ef0u, "post-reset-memory-layout-compress-emit-a", false},
    {0x00041f1cu, "post-reset-memory-layout-compress-emit-b", false},
    {0x00041f46u, "post-reset-memory-layout-compress-extra", false},
    {0x00049e96u, "monitor-init-scc-like", false},
    {0x00049fcau, "monitor-command-poll", false},
};

static void lc_musashi_bus_reset_rom_watchpoints(void) {
    for (size_t i = 0; i < sizeof(rom_watchpoints) / sizeof(rom_watchpoints[0]); i++) {
        rom_watchpoints[i].seen = false;
    }
}

static uint16_t lc_musashi_bus_peek_ram16(uint32_t address) {
    if (active_bus == NULL || !active_bus->initialized || active_bus->ram == NULL ||
        active_bus->ram_size < 2u || address > active_bus->ram_size - 2u) {
        return 0xffffu;
    }
    return (uint16_t)(((uint16_t)active_bus->ram[address] << 8u) |
                      (uint16_t)active_bus->ram[address + 1u]);
}

static uint32_t lc_musashi_bus_peek_ram32(uint32_t address) {
    const uint32_t hi = lc_musashi_bus_peek_ram16(address);
    const uint32_t lo = lc_musashi_bus_peek_ram16(address + 2u);
    return (hi << 16u) | lo;
}

static bool lc_musashi_bus_rom_offset_for_address(uint32_t address, uint32_t *offset) {
    const uint32_t bases[] = {
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE,
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        if (address >= bases[i] && address < bases[i] + LC_ROM_WINDOW_SIZE) {
            if (offset != NULL) {
                *offset = address - bases[i];
            }
            return true;
        }
    }
    return false;
}

static uint16_t lc_musashi_bus_peek_rom16(uint32_t address) {
    uint32_t offset = 0;
    if (active_bus == NULL || active_bus->rom == NULL ||
        !lc_musashi_bus_rom_offset_for_address(address, &offset) ||
        offset + 1u >= active_bus->rom_size) {
        return 0xffffu;
    }
    return (uint16_t)(((uint16_t)active_bus->rom[offset] << 8u) |
                      (uint16_t)active_bus->rom[offset + 1u]);
}

static uint32_t lc_musashi_bus_peek_guest32(uint32_t address) {
    if (active_bus != NULL && active_bus->initialized && active_bus->ram != NULL &&
        active_bus->ram_size >= 4u && address <= active_bus->ram_size - 4u) {
        return lc_musashi_bus_peek_ram32(address);
    }
    uint32_t offset = 0;
    if (active_bus != NULL && active_bus->rom != NULL &&
        lc_musashi_bus_rom_offset_for_address(address, &offset) &&
        offset + 3u < active_bus->rom_size) {
        return ((uint32_t)active_bus->rom[offset] << 24u) |
               ((uint32_t)active_bus->rom[offset + 1u] << 16u) |
               ((uint32_t)active_bus->rom[offset + 2u] << 8u) |
               (uint32_t)active_bus->rom[offset + 3u];
    }
    return 0xffffffffu;
}

static bool lc_musashi_bus_basilisk_slot_rom_active(void) {
    if (active_bus == NULL || active_bus->rom == NULL || active_bus->rom_size < 8u) {
        return false;
    }
    const size_t off = active_bus->rom_size - 6u;
    return active_bus->rom[off + 0u] == 0x5au && active_bus->rom[off + 1u] == 0x93u &&
           active_bus->rom[off + 2u] == 0x2bu && active_bus->rom[off + 3u] == 0xc7u;
}

static void lc_musashi_bus_log_exception_stack(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0004638cu && rom_offset != 0x00002310u &&
        rom_offset != 0x00002316u && rom_offset != 0x000416e0u) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint16_t w0 = lc_musashi_bus_peek_ram16(sp + 0u);
    const uint16_t w1 = lc_musashi_bus_peek_ram16(sp + 2u);
    const uint16_t w2 = lc_musashi_bus_peek_ram16(sp + 4u);
    const uint16_t w3 = lc_musashi_bus_peek_ram16(sp + 6u);
    const uint16_t w4 = lc_musashi_bus_peek_ram16(sp + 8u);
    const uint16_t w5 = lc_musashi_bus_peek_ram16(sp + 10u);
    const uint16_t w6 = lc_musashi_bus_peek_ram16(sp + 12u);
    const uint16_t w7 = lc_musashi_bus_peek_ram16(sp + 14u);
    ESP_LOGW(TAG,
             "LC diagnostic exception stack: sp=0x%08" PRIx32
             " words=%04x %04x %04x %04x %04x %04x %04x %04x"
             " frame_pc=0x%04x%04x format_vector=0x%04x",
             sp, w0, w1, w2, w3, w4, w5, w6, w7, w1, w2, w3);
}

static void lc_musashi_bus_log_rom_watchpoint(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    for (size_t i = 0; i < sizeof(rom_watchpoints) / sizeof(rom_watchpoints[0]); i++) {
        lc_rom_watchpoint_t *watch = &rom_watchpoints[i];
        if (watch->seen || watch->rom_offset != rom_offset) {
            continue;
        }
        watch->seen = true;
        lc_musashi_bus_log_exception_stack(pc);
        ESP_LOGI(TAG,
                 "LC ROM watchpoint: label=%s pc=0x%08" PRIx32
                 " prev_pc=0x%08" PRIx32 " ir=0x%04x"
                 " d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x d4=0x%08x d5=0x%08x d6=0x%08x d7=0x%08x"
                 " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x a5=0x%08x a6=0x%08x sp=0x%08x usp=0x%08x sr=0x%04x",
                 watch->label, pc, previous_instruction_pc,
                 m68k_get_reg(NULL, M68K_REG_IR), m68k_get_reg(NULL, M68K_REG_D0),
                 m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                 m68k_get_reg(NULL, M68K_REG_D3), m68k_get_reg(NULL, M68K_REG_D4),
                 m68k_get_reg(NULL, M68K_REG_D5), m68k_get_reg(NULL, M68K_REG_D6),
                 m68k_get_reg(NULL, M68K_REG_D7), m68k_get_reg(NULL, M68K_REG_A0),
                 m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
                 m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_A4),
                 m68k_get_reg(NULL, M68K_REG_A5), m68k_get_reg(NULL, M68K_REG_A6),
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

bool lc_musashi_bus_should_nop_post_reset_swap_mmu_dispatch(void) {
    return post_reset_swap_mmu_dispatch_nop_active;
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
    reset_scc_timer_wait0_hits = 0;
    reset_scc_timer_wait1_hits = 0;
    reset_scc_timer_irq_pulses = 0;
    reset_scc_timer_irq_asserted = false;
    reset_via_irq_wait0_hits = 0;
    reset_via_irq_wait1_hits = 0;
    reset_via_irq_pulses = 0;
    reset_via_irq_asserted = false;
    get_video_default_stub_logged = false;
    slot_manager_video_default_stub_logged = false;
    control_video_default_stub_logged = false;
    disposeptr_video_default_stub_logged = false;
    swap_mmu_video_default_stub_logged = false;
    post_reset_no_mmu_a001_stub_logged = false;
    post_reset_swap_mmu_dispatch_stub_logged = false;
    post_reset_swap_mmu_dispatch_nop_active = false;
    post_reset_resource_copy_cap_logged = false;
    post_reset_resource_scan_cap_logged = false;
    post_reset_resource_lookup_logs = 0;
    post_reset_resource_low_stack_logs = 0;
    post_reset_resource_map_loop_hits = 0;
    post_reset_resource_map_loop_escape_logged = false;
    post_reset_count_combos_logs = 0;
    post_reset_slot_scan_loop_hits = 0;
    post_reset_slot_first_scan_loop_hits = 0;
    post_reset_slot_init_scan_calls = 0;
    post_reset_atrap_dispatch_logs = 0;
    post_reset_block_move_logs = 0;
    post_reset_set_trap_address_logs = 0;
    post_reset_memory_trap_logs = 0;
    post_reset_ram_exec_logs = 0;
    post_reset_invalid_exec_logs = 0;
    post_reset_trap_return_canonicalize_logs = 0;
    post_reset_high_dispatch_logs = 0;
    post_reset_attr_low_dispatch_return_logs = 0;
    post_reset_shutdown_rts_logged = false;
    post_reset_probe_tables_seeded = false;
    post_reset_main_zone_seeded = false;
    post_reset_resource_map_handle_registered = false;
    post_reset_rom_map_handle_repair_logged = false;
    post_reset_slot_first_pass_escape_logged = false;
    post_reset_high_trap_handler_sr_logged = false;
    post_reset_event_wait_complete_logged = false;
    post_reset_vbl_queue_repair_logged = false;
    post_reset_vbl_loop_escape_logged = false;
    post_reset_basilisk_unit_table_newptr_logged = false;
    post_reset_slot_init_success_logged = false;
    post_reset_slot_dispatch_rebuild_skip_logged = false;
    post_reset_getstring_startup_logged = false;
    post_reset_event_wait_hits = 0;
    post_reset_event_wait_loop_calls = 0;
    post_reset_vbl_init_trace_hits = 0;
    post_reset_event_newhandle = 0;
    post_reset_event_newhandle_ptr = 0;
    post_reset_rom_map_handle_dynamic = 0;
    post_reset_univ_info_observed = 0;
    // Keep the tiny synthetic Memory Manager heap below both the direct probe's
    // top-of-RAM stack and the later ROM reset stack descent.  Starting at
    // 0x00100000 let the real stack overwrite the rebuilt resource map near
    // 0x00102508 during post-reset ROM execution; 0x00018000 leaves a larger
    // high-stack reserve while staying above low globals/master-pointer slabs.
    post_reset_heap_bump = 0x00018000u;
    post_reset_emergency_bump = 0;
    post_reset_low_emergency_bump = 0;
    basilisk_emul_op_logs = 0;
    // Keep synthetic master-pointer cells out of the system zone heap.  The
    // ProductInfo.DefaultRSRCs=1 ROM map path creates hundreds of handles while
    // rebuilding the map; starting at 0x3000 eventually crossed the 0x3800
    // zone start and let map handles alias the RAM-fill/free-block pattern.
    post_reset_handle_bump = LC_POST_RESET_MASTER_PTR_BASE;
    memset(post_reset_handle_size_handle, 0, sizeof(post_reset_handle_size_handle));
    memset(post_reset_handle_data_ptr, 0, sizeof(post_reset_handle_data_ptr));
    memset(post_reset_handle_size_value, 0, sizeof(post_reset_handle_size_value));
    memset(post_reset_handle_locked, 0, sizeof(post_reset_handle_locked));
    memset(post_reset_free_blocks, 0, sizeof(post_reset_free_blocks));
    memset(post_reset_low_trap_ring, 0, sizeof(post_reset_low_trap_ring));
    post_reset_low_trap_ring_index = 0;
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
    if (reset_scc_timer_irq_asserted && level == M68K_IRQ_4) {
        reset_scc_timer_irq_asserted = false;
        m68k_set_irq(M68K_IRQ_NONE);
        ESP_LOGI(TAG,
                 "LC reset SCC timer synthetic IRQ acknowledged: level=%d pulses=%" PRIu32
                 " wait0_hits=%" PRIu32 " wait1_hits=%" PRIu32,
                 level, reset_scc_timer_irq_pulses, reset_scc_timer_wait0_hits,
                 reset_scc_timer_wait1_hits);
    } else if (reset_via_irq_asserted && level == M68K_IRQ_1) {
        reset_via_irq_asserted = false;
        m68k_set_irq(M68K_IRQ_NONE);
        if (reset_via_irq_pulses <= 3u || reset_via_irq_pulses == 128u ||
            reset_via_irq_pulses == 138u || reset_via_irq_pulses == 148u) {
            ESP_LOGI(TAG,
                     "LC reset VIA synthetic IRQ acknowledged: level=%d pulses=%" PRIu32
                     " wait0_hits=%" PRIu32 " wait1_hits=%" PRIu32,
                     level, reset_via_irq_pulses, reset_via_irq_wait0_hits,
                     reset_via_irq_wait1_hits);
        }
    }
    return (int)M68K_INT_ACK_AUTOVECTOR;
}

static bool lc_musashi_bus_handle_basilisk_emul_op(int opcode);

int cpu_illg_callback(int opcode) {
    if (lc_musashi_bus_handle_basilisk_emul_op(opcode)) {
        return 1;
    }
    const uint32_t ppc = m68k_get_reg(NULL, M68K_REG_PPC);
    const uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    lc_trace_record(LC_TRACE_EVENT_ILLEGAL_INSTRUCTION, ppc, pc, (uint32_t)opcode, 2, false);
    ESP_LOGW(TAG,
             "LC illegal instruction callback: opcode=0x%04x ppc=0x%08" PRIx32
             " pc=0x%08" PRIx32 " current_pc=0x%08" PRIx32
             " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x d7=0x%08x sr=0x%04x",
             opcode & 0xffff, ppc, pc, current_instruction_pc, previous_instruction_pc, sp,
             m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D3),
             m68k_get_reg(NULL, M68K_REG_D7), m68k_get_reg(NULL, M68K_REG_SR));
    return 0;
}

static void lc_musashi_bus_maybe_pulse_reset_via_irq(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    // Diagnostic-only model for reset subtest d7=0x87.  The ROM installs a
    // level-1 autovector handler at VBR+0x64 and then checks two VIA timer
    // phases.  Pulse only in those two wait loops; lc_memory supplies the IFR
    // bit pattern consumed by the handler.
    if (reset_via_irq_pulses < 138u && rom_offset == 0x000471bcu) {
        reset_via_irq_wait0_hits++;
        if ((reset_via_irq_wait0_hits % 16u) == 0 && !reset_via_irq_asserted) {
            reset_via_irq_asserted = true;
            reset_via_irq_pulses++;
            if (reset_via_irq_pulses <= 3u || reset_via_irq_pulses == 128u ||
                reset_via_irq_pulses == 138u) {
                ESP_LOGI(TAG,
                         "LC reset VIA synthetic IRQ pulse: phase=first level=1 pc=0x%08" PRIx32
                         " pulse=%" PRIu32 " hits=%" PRIu32,
                         pc, reset_via_irq_pulses, reset_via_irq_wait0_hits);
            }
            m68k_set_irq(M68K_IRQ_1);
        }
    } else if (reset_via_irq_pulses >= 138u && reset_via_irq_pulses < 148u &&
               rom_offset == 0x00047270u) {
        reset_via_irq_wait1_hits++;
        if ((reset_via_irq_wait1_hits % 16u) == 0 && !reset_via_irq_asserted) {
            reset_via_irq_asserted = true;
            reset_via_irq_pulses++;
            if (reset_via_irq_pulses <= 141u || reset_via_irq_pulses == 148u) {
                ESP_LOGI(TAG,
                         "LC reset VIA synthetic IRQ pulse: phase=second level=1 pc=0x%08" PRIx32
                         " pulse=%" PRIu32 " hits=%" PRIu32,
                         pc, reset_via_irq_pulses, reset_via_irq_wait1_hits);
            }
            m68k_set_irq(M68K_IRQ_1);
        }
    }
}

static void lc_musashi_bus_maybe_stub_get_video_default(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00002326u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }

    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    if (a0 < active_bus->ram_size) {
        // Trap A080 is GetVideoDefault.  During this bounded bring-up, the
        // A-line vector still points to diagnostics; provide a minimal default
        // byte in the caller's buffer and skip just this trap instruction.
        active_bus->ram[a0] = 0x00u;
    }
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    if (!get_video_default_stub_logged) {
        get_video_default_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed ROM GetVideoDefault trap: pc=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " wrote=%s next_pc=0x%08" PRIx32,
                 pc, a0, a0 < active_bus->ram_size ? "yes" : "no", pc + 2u);
    }
}

static void lc_musashi_bus_ram_write8(uint32_t address, uint8_t value) {
    if (active_bus == NULL || active_bus->ram == NULL || address >= active_bus->ram_size) {
        return;
    }
    static unsigned resource_map_header_watch_logs = 0;
    const uint8_t old_value = active_bus->ram[address];
    if (address >= 0x0001a518u && address <= 0x0001a527u &&
        old_value != value && resource_map_header_watch_logs < 200u) {
        ESP_LOGW(TAG,
                 "LC watch synthetic dynamic resource-map header write: pc=0x%08" PRIx32
                 " addr=0x%08" PRIx32 " old=0x%02x new=0x%02x"
                 " d0=0x%08x d1=0x%08x d2=0x%08x a0=0x%08x a2=0x%08x",
                 current_instruction_pc, address, old_value, value,
                 m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                 m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_A0),
                 m68k_get_reg(NULL, M68K_REG_A2));
        resource_map_header_watch_logs++;
    }
    active_bus->ram[address] = value;
}

static void lc_musashi_bus_ram_write16(uint32_t address, uint16_t value) {
    lc_musashi_bus_ram_write8(address + 0u, (uint8_t)(value >> 8u));
    lc_musashi_bus_ram_write8(address + 1u, (uint8_t)value);
}

static void lc_musashi_bus_ram_write32(uint32_t address, uint32_t value) {
    lc_musashi_bus_ram_write16(address + 0u, (uint16_t)(value >> 16u));
    lc_musashi_bus_ram_write16(address + 2u, (uint16_t)value);
}

static uint32_t lc_musashi_bus_basilisk_find_driver_by_pstring(const char *name, size_t name_len) {
    if (active_bus == NULL || active_bus->rom == NULL || name == NULL || name_len == 0u ||
        name_len > 255u || active_bus->rom_size < name_len + 20u) {
        return 0;
    }
    for (uint32_t off = 18u; off + name_len < active_bus->rom_size; off++) {
        if (active_bus->rom[off] == (uint8_t)name_len &&
            memcmp(&active_bus->rom[off + 1u], name, name_len) == 0) {
            return LC_BASILISK_ROM_BASE_32 + off - 18u;
        }
    }
    return 0;
}

static void lc_musashi_bus_basilisk_install_unit_driver(uint32_t unit_table,
                                                        int16_t ref_num,
                                                        uint32_t driver_addr,
                                                        uint16_t flags,
                                                        uint32_t dce_handle,
                                                        uint32_t dce_ptr) {
    if (active_bus == NULL || active_bus->ram == NULL || driver_addr == 0u ||
        unit_table + 256u >= active_bus->ram_size || dce_handle + 3u >= active_bus->ram_size ||
        dce_ptr + 63u >= active_bus->ram_size) {
        return;
    }
    const uint32_t unit_index = (~(uint32_t)(int32_t)ref_num) & 0xffu;
    lc_musashi_bus_ram_write32(dce_handle, dce_ptr);
    lc_musashi_bus_ram_write32(unit_table + unit_index * 4u, dce_handle);
    lc_musashi_bus_ram_write32(dce_ptr + 0u, driver_addr); // dCtlDriver
    lc_musashi_bus_ram_write16(dce_ptr + 4u, flags);       // dCtlFlags
    lc_musashi_bus_ram_write16(dce_ptr + 6u, 3u);          // qFlags: driver version >= 3.
    lc_musashi_bus_ram_write32(dce_ptr + 16u, 0u);         // dCtlPosition
    lc_musashi_bus_ram_write16(dce_ptr + 24u, (uint16_t)ref_num); // dCtlRefNum-like slot.
}

static void lc_musashi_bus_basilisk_install_drivers(uint32_t pb) {
    if (active_bus == NULL || active_bus->ram == NULL) {
        return;
    }

    uint32_t unit_table = lc_musashi_bus_peek_ram32(0x0000011cu);
    if (unit_table < 0x00001000u || unit_table + 256u >= active_bus->ram_size) {
        unit_table = 0x00008a00u;
        lc_musashi_bus_ram_write32(0x0000011cu, unit_table);
    }
    for (uint32_t i = 0; i < 256u && unit_table + i < active_bus->ram_size; i++) {
        lc_musashi_bus_ram_write8(unit_table + i, 0);
    }

    const uint32_t disk_driver = lc_musashi_bus_basilisk_find_driver_by_pstring(".Disk", 5u);
    const uint32_t sony_driver = lc_musashi_bus_basilisk_find_driver_by_pstring(".Sony", 5u);
    if (sony_driver != 0u) {
        lc_musashi_bus_basilisk_install_unit_driver(unit_table, -5, sony_driver, 0x6f00u,
                                                    0x00008b00u, 0x00008b20u);
    }
    if (disk_driver != 0u) {
        lc_musashi_bus_basilisk_install_unit_driver(unit_table, -63, disk_driver, 0x6f04u,
                                                    0x00008b04u, 0x00008b80u);
        if (pb + 21u < active_bus->ram_size) {
            lc_musashi_bus_ram_write32(pb + 18u, disk_driver + 0x12u); // ioNamePtr -> .Disk
        }
    }

    const uint32_t asc_regs = 0x0008d000u;
    if (asc_regs + 0x1000u <= active_bus->ram_size) {
        for (uint32_t i = 0; i < 0x1000u; i++) {
            lc_musashi_bus_ram_write8(asc_regs + i, 0);
        }
        lc_musashi_bus_ram_write8(asc_regs + 0x800u, 0x0fu);
        lc_musashi_bus_ram_write32(0x00000cc0u, asc_regs);
    }

    ESP_LOGW(TAG,
             "LC Basilisk InstallDrivers modeled: pb=0x%08" PRIx32
             " unit_table=0x%08" PRIx32 " sony=0x%08" PRIx32
             " disk=0x%08" PRIx32 " asc=0x%08x",
             pb, unit_table, sony_driver, disk_driver, asc_regs);
}

static int16_t lc_musashi_bus_basilisk_disk_prime(bool is_sony, uint32_t pb, uint32_t dce) {
    if (active_bus == NULL || active_bus->ram == NULL || pb + 49u >= active_bus->ram_size ||
        dce + 19u >= active_bus->ram_size) {
        return -50; // paramErr
    }
    const uint32_t buffer = lc_musashi_bus_peek_ram32(pb + 32u);   // ioBuffer
    const uint32_t length = lc_musashi_bus_peek_ram32(pb + 36u);   // ioReqCount
    uint64_t position = lc_musashi_bus_peek_ram32(dce + 16u);      // dCtlPosition
    const uint16_t pos_mode = lc_musashi_bus_peek_ram16(pb + 44u); // ioPosMode
    if ((pos_mode & 0x0100u) != 0u && pb + 53u < active_bus->ram_size) {
        position = ((uint64_t)lc_musashi_bus_peek_ram32(pb + 46u) << 32u) |
                   (uint64_t)lc_musashi_bus_peek_ram32(pb + 50u);
    }
    if ((length & 0x1ffu) != 0u || (position & 0x1ffu) != 0u ||
        length > 0x00100000u || buffer + length < buffer) {
        lc_musashi_bus_ram_write32(pb + 40u, 0u); // ioActCount
        return -50; // paramErr
    }

    const bool read_op = (lc_musashi_bus_peek_ram16(pb + 6u) & 0xffu) == 2u; // aRdCmd
    if (!read_op && !lc_disk_write_allowed()) {
        lc_musashi_bus_ram_write32(pb + 40u, 0u);
        lc_disk_trace_io(is_sony ? LC_DISK_CMD_SWIM_WRITE_SECTOR : LC_DISK_CMD_SCSI_WRITE,
                         position / 512u, length, true, ESP_ERR_NOT_ALLOWED);
        return -44; // wPrErr
    }

    const esp_partition_t *disk = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "disk");
    if (disk == NULL || position + length > disk->size) {
        lc_musashi_bus_ram_write32(pb + 40u, 0u);
        return -65; // offLinErr
    }

    uint8_t scratch[512];
    uint32_t done = 0;
    while (done < length) {
        const uint32_t chunk = (length - done) > sizeof(scratch) ? sizeof(scratch) : (length - done);
        esp_err_t err = ESP_OK;
        if (read_op) {
            err = esp_partition_read(disk, (size_t)(position + done), scratch, chunk);
            if (err != ESP_OK) {
                lc_musashi_bus_ram_write32(pb + 40u, done);
                lc_disk_trace_io(is_sony ? LC_DISK_CMD_SWIM_READ_SECTOR : LC_DISK_CMD_SCSI_READ,
                                 (position + done) / 512u, chunk, false, err);
                return -19; // readErr
            }
            for (uint32_t i = 0; i < chunk; i++) {
                (void)lc_memory_bus_write8(active_bus, buffer + done + i, scratch[i]);
            }
        } else {
            for (uint32_t i = 0; i < chunk; i++) {
                scratch[i] = (uint8_t)lc_memory_bus_read8(active_bus, buffer + done + i);
            }
            // Host/firmware disk writes remain intentionally blocked unless the
            // project is built with LC_DISK_IMAGE_READ_ONLY=0 and esp_partition
            // write support is added for the active platform.
            err = ESP_FAIL;
            lc_musashi_bus_ram_write32(pb + 40u, done);
            lc_disk_trace_io(is_sony ? LC_DISK_CMD_SWIM_WRITE_SECTOR : LC_DISK_CMD_SCSI_WRITE,
                             (position + done) / 512u, chunk, true, err);
            return -20; // writErr
        }
        done += chunk;
    }

    lc_musashi_bus_ram_write32(pb + 40u, done);             // ioActCount
    lc_musashi_bus_ram_write32(dce + 16u, (uint32_t)(position + done));
    lc_disk_trace_io(is_sony ? LC_DISK_CMD_SWIM_READ_SECTOR : LC_DISK_CMD_SCSI_READ,
                     position / 512u, done, !read_op, ESP_OK);
    return 0;
}

static int16_t lc_musashi_bus_basilisk_disk_control_status(uint16_t op, uint32_t pb,
                                                           uint32_t dce) {
    (void)dce;
    if (active_bus == NULL || active_bus->ram == NULL || pb + 31u >= active_bus->ram_size) {
        return -50;
    }
    const uint16_t code = lc_musashi_bus_peek_ram16(pb + 26u); // csCode
    const uint32_t param = lc_musashi_bus_peek_ram32(pb + 28u); // csParam
    lc_disk_info_t info = {0};
    const bool have_disk = lc_disk_probe(&info) == ESP_OK;
    switch (code) {
    case 1:  // KillIO
    case 5:  // Verify disk
    case 6:  // Format (read-only policy is enforced by Prime writes)
    case 65: // accRun periodic action
        return have_disk ? 0 : -65;
    case 24: // Get partition size
        if (param + 3u < active_bus->ram_size) {
            lc_musashi_bus_ram_write32(param, have_disk ? info.sector_count : 0u);
        }
        return have_disk ? 0 : -65;
    case 43: { // DriverGestalt
        const uint32_t selector = param + 7u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(param) : 0;
        uint32_t value = 0;
        switch (selector) {
        case 0x76657273u: value = 0x01008000u; break; // 'vers'
        case 0x64657674u: value = 0x6469736bu; break; // 'devt' -> 'disk'
        case 0x696e7466u: value = 0x42322020u; break; // 'intf' -> 'B2  '
        case 0x73796e63u: value = 0x01000000u; break; // 'sync'
        case 0x70757267u: value = 0u; break;          // 'purg'
        case 0x656a6563u: value = 0x00030003u; break; // 'ejec'
        case 0x766d6f70u: value = 0u; break;          // 'vmop'
        case 0x626f6f74u: // 'boot'
            if (param + 7u < active_bus->ram_size) {
                lc_musashi_bus_ram_write16(param + 4u, 0u);
                lc_musashi_bus_ram_write16(param + 6u, (uint16_t)-63);
            }
            return 0;
        default:
            return -18; // statusErr
        }
        if (param + 7u < active_bus->ram_size) {
            lc_musashi_bus_ram_write32(param + 4u, value);
        }
        return 0;
    }
    case 8: // Get drive status
        if (param + 21u < active_bus->ram_size) {
            for (uint32_t i = 0; i < 22u; i++) lc_musashi_bus_ram_write8(param + i, 0);
            lc_musashi_bus_ram_write8(param + 2u, LC_DISK_IMAGE_READ_ONLY ? 0xffu : 0x00u);
            lc_musashi_bus_ram_write8(param + 3u, have_disk ? 8u : 0u); // fixed disk in place
            lc_musashi_bus_ram_write8(param + 4u, have_disk ? 1u : 0u);
            lc_musashi_bus_ram_write16(param + 18u, have_disk ? (uint16_t)info.sector_count : 0u);
            lc_musashi_bus_ram_write16(param + 20u, have_disk ? (uint16_t)(info.sector_count >> 16u) : 0u);
        }
        return have_disk ? 0 : -65;
    default:
        return 0;
    }
}

static void lc_musashi_bus_post_reset_set_handle_record(uint32_t handle, uint32_t data_ptr,
                                                         uint32_t size);

static bool lc_musashi_bus_guest_pstring_equals(uint32_t addr, const char *s) {
    if (s == NULL || active_bus == NULL) {
        return false;
    }
    const size_t len = strlen(s);
    if (len > 255u) {
        return false;
    }
    uint8_t n = 0;
    if (active_bus->ram != NULL && addr < active_bus->ram_size) {
        n = active_bus->ram[addr];
        if (n == len && addr + 1u + len <= active_bus->ram_size) {
            return memcmp(&active_bus->ram[addr + 1u], s, len) == 0;
        }
    }
    uint32_t off = 0;
    if (active_bus->rom != NULL && lc_musashi_bus_rom_offset_for_address(addr, &off) &&
        off + 1u + len <= active_bus->rom_size) {
        n = active_bus->rom[off];
        return n == len && memcmp(&active_bus->rom[off + 1u], s, len) == 0;
    }
    return false;
}

static void lc_musashi_bus_maybe_basilisk_open_driver_trap(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (!lc_musashi_bus_basilisk_slot_rom_active() || cpu_read_word(pc) != 0xa000u ||
        (rom_offset != 0x00001254u && rom_offset != 0x00001274u)) {
        return;
    }
    const uint32_t pb = m68k_get_reg(NULL, M68K_REG_A0);
    const uint32_t name_ptr = active_bus != NULL && active_bus->ram != NULL &&
        pb + 21u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(pb + 18u) : 0u;
    const bool known_name = lc_musashi_bus_guest_pstring_equals(name_ptr, ".Sony") ||
                            lc_musashi_bus_guest_pstring_equals(name_ptr, ".Sound") ||
                            lc_musashi_bus_guest_pstring_equals(name_ptr, "netBOOT");
    if (!known_name) {
        return;
    }
    if (pb + 17u < active_bus->ram_size) {
        lc_musashi_bus_ram_write16(pb + 16u, 0u); // ioResult
    }
    m68k_set_reg(M68K_REG_D0, 0);
    const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    m68k_set_reg(M68K_REG_SR, (sr & 0xfff0u) | 0x0004u);
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    static unsigned logged = 0;
    if (logged < 8u) {
        ESP_LOGW(TAG,
                 "LC Basilisk modeled early Open trap: pc=0x%08" PRIx32
                 " pb=0x%08" PRIx32 " name=0x%08" PRIx32 " next=0x%08" PRIx32,
                 pc, pb, name_ptr, pc + 2u);
        logged++;
    }
}

static void lc_musashi_bus_maybe_force_basilisk_install_drivers_branch(uint32_t pc) {
    if (!lc_musashi_bus_basilisk_slot_rom_active() || (pc & 0x000fffffu) != 0x00001140u) {
        return;
    }
    // Basilisk's ROM32 patch executes M68K_EMUL_OP_INSTALL_DRIVERS at 0x1142.
    // The host Resource Manager shim can leave A0 non-zero at the preceding BNE;
    // normalize that result so this path follows Basilisk's patched startup
    // flow instead of skipping directly into later Resource Manager startup.
    if (m68k_get_reg(NULL, M68K_REG_A0) != 0u) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGW(TAG,
                     "LC normalized pre-InstallDrivers A0 to follow Basilisk ROM32 patch: pc=0x%08" PRIx32
                     " old_a0=0x%08x",
                     pc, m68k_get_reg(NULL, M68K_REG_A0));
        }
        m68k_set_reg(M68K_REG_A0, 0);
    }
}

static void lc_musashi_bus_maybe_stub_basilisk_unit_table_newptr(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (!lc_musashi_bus_basilisk_slot_rom_active() || rom_offset != 0x00001120u ||
        cpu_read_word(pc) != 0xa71eu || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }

    const uint32_t unit_table = 0x00008a00u;
    const uint32_t bytes = 512u;
    if (unit_table + bytes > active_bus->ram_size) {
        return;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        lc_musashi_bus_ram_write8(unit_table + i, 0);
    }
    m68k_set_reg(M68K_REG_A0, unit_table);
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    if (!post_reset_basilisk_unit_table_newptr_logged) {
        post_reset_basilisk_unit_table_newptr_logged = true;
        ESP_LOGW(TAG,
                 "LC modeled Basilisk UnitNtryTbl NewPtr before InstallDrivers: pc=0x%08" PRIx32
                 " ptr=0x%08" PRIx32 " bytes=%u next=0x%08" PRIx32,
                 pc, unit_table, (unsigned)bytes, pc + 2u);
    }
}

static uint32_t lc_musashi_bus_basilisk_rom32(uint32_t rom_offset) {
    if (active_bus == NULL || active_bus->rom == NULL || rom_offset + 3u >= active_bus->rom_size) {
        return 0;
    }
    return ((uint32_t)active_bus->rom[rom_offset + 0u] << 24u) |
           ((uint32_t)active_bus->rom[rom_offset + 1u] << 16u) |
           ((uint32_t)active_bus->rom[rom_offset + 2u] << 8u) |
           (uint32_t)active_bus->rom[rom_offset + 3u];
}

static void lc_musashi_bus_basilisk_log_emul_op(uint16_t opcode, const char *name) {
    if (basilisk_emul_op_logs < 80u) {
        ESP_LOGW(TAG,
                 "LC Basilisk EMUL_OP %s opcode=0x%04x ppc=0x%08x pc=0x%08x"
                 " d0=0x%08x d1=0x%08x d2=0x%08x a0=0x%08x a1=0x%08x a6=0x%08x sp=0x%08x",
                 name != NULL ? name : "unknown", opcode, m68k_get_reg(NULL, M68K_REG_PPC),
                 m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_D0),
                 m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                 m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                 m68k_get_reg(NULL, M68K_REG_A6), m68k_get_reg(NULL, M68K_REG_SP));
        basilisk_emul_op_logs++;
    }
}

static bool lc_musashi_bus_handle_basilisk_emul_op(int opcode) {
    const uint16_t op = (uint16_t)(opcode & 0xffffu);
    if (op < LC_B2_M68K_EXEC_RETURN || op >= 0x7140u) {
        return false;
    }
    if (active_bus == NULL || active_bus->ram == NULL || active_bus->rom == NULL) {
        return false;
    }

    switch (op) {
    case LC_B2_M68K_EXEC_RETURN:
        lc_musashi_bus_basilisk_log_emul_op(op, "EXEC_RETURN");
        return true;

    case LC_B2_EMUL_OP_RESET: {
        lc_musashi_bus_basilisk_log_emul_op(op, "RESET");
        if (active_bus->ram_size >= 0x10000u) {
            const uint32_t clear_base = active_bus->ram_size >= 4096u ? (uint32_t)active_bus->ram_size - 4096u : 0u;
            for (uint32_t i = 0; i < 4096u && clear_base + i < active_bus->ram_size; i++) {
                lc_musashi_bus_ram_write8(clear_base + i, 0);
            }
            const uint32_t boot_globs = (uint32_t)active_bus->ram_size - 0x1cu;
            lc_musashi_bus_ram_write32(boot_globs + 0x00u, 0u);
            lc_musashi_bus_ram_write32(boot_globs + 0x04u, (uint32_t)active_bus->ram_size);
            lc_musashi_bus_ram_write32(boot_globs + 0x08u, 0xffffffffu);
            lc_musashi_bus_ram_write32(boot_globs + 0x0cu, 0u);

            // Let the Basilisk slot-ROM path use the ROM's real Slot Manager
            // handler instead of the old no-PDS synthetic low-trap default.
            lc_memory_set_post_reset_atrap_handler(0xa06eu, LC_BASILISK_ROM_BASE_32 + 0x00006e16u);

            const uint32_t univ = lc_basilisk_find_universal_info(active_bus->rom, active_bus->rom_size);
            if (univ != 0u) {
                const uint32_t addr_map_off = lc_musashi_bus_basilisk_rom32(univ + 0x00u);
                uint32_t hwcfg = lc_musashi_bus_basilisk_rom32(univ + 0x10u);
                hwcfg &= 0xefffffffu; // No FPU.
                m68k_set_reg(M68K_REG_D0, lc_musashi_bus_basilisk_rom32(univ + 0x18u));
                m68k_set_reg(M68K_REG_D1, lc_musashi_bus_basilisk_rom32(univ + 0x1cu));
                m68k_set_reg(M68K_REG_D2, hwcfg);
                m68k_set_reg(M68K_REG_A0, LC_BASILISK_ROM_BASE_32 + univ + addr_map_off);
                m68k_set_reg(M68K_REG_A1, LC_BASILISK_ROM_BASE_32 + univ);
            }
            // Basilisk writes MMU32Bit=1 during RESET so StripAddress
            // returns addresses unchanged in 32-bit mode.
            lc_musashi_bus_ram_write8(0x00000cb2u, 1u); // MMU32Bit = true
            // Dispatch magic cookie — signals that trap tables are initialized.
            lc_musashi_bus_ram_write32(0x00000db0u, 0x5a932bc7u);

            // SYS trap table at $0000 + selector*4 (for A2xx traps with SYS bit).
            // Skip entries 0-1 (RESET vectors at $00-$07) and seed $08-$3FC.
            // The A-line vector ($28), bus-error ($08), address-error ($0C),
            // illegal ($10) are overwritten below.
            for (uint32_t i = 0x02u; i < 0x100u; i++) {
                lc_musashi_bus_ram_write32(i * 4u, LC_BASILISK_ROM_BASE_32 + 0x0d88u);
            }
            // Now write exception vectors OVER the SYS table entries:
            lc_musashi_bus_ram_write32(LC_LOWMEM_LINE_A_VECTOR, 0x408099b0u); // $28

            // Seed essential low-memory globals for boot block execution.
            lc_musashi_bus_ram_write32(0x000002aeu, LC_BASILISK_ROM_BASE_32); // ROMBase (32-bit)
            // Write a spin loop at $7f0 as return target after boot code exits.
            lc_musashi_bus_ram_write16(0x000007f0u, 0x60feu); // BRA.S * (infinite loop)
            // Set illegal-instruction vector ($10) to spin at $7f2 for diagnostics
            lc_musashi_bus_ram_write32(0x00000010u, 0x000007f2u);
            lc_musashi_bus_ram_write16(0x000007f2u, 0x60feu); // BRA.S *
            // Set address-error vector ($0c) to spin at $7f4
            lc_musashi_bus_ram_write32(0x0000000cu, 0x000007f4u);
            lc_musashi_bus_ram_write16(0x000007f4u, 0x60feu);
            // Set bus-error vector ($08) to spin at $7f6
            lc_musashi_bus_ram_write32(0x00000008u, 0x000007f6u);
            lc_musashi_bus_ram_write16(0x000007f6u, 0x60feu);
            lc_musashi_bus_ram_write32(0x0000010cu, (uint32_t)active_bus->ram_size); // MemTop
            lc_musashi_bus_ram_write32(0x00000108u, (uint32_t)active_bus->ram_size); // RAMSize (BufPtr)
            lc_musashi_bus_ram_write32(0x00000130u, (uint32_t)active_bus->ram_size); // HighMem
            lc_musashi_bus_ram_write32(0x00000900u, 0x00002800u); // SysZone start
            lc_musashi_bus_ram_write32(0x00000904u, 0x00380000u); // SysZone end (ApplZone)
            lc_musashi_bus_ram_write32(0x000002a6u, 0x00380000u); // CurStackBase
            lc_musashi_bus_ram_write32(0x00000118u, 0x00380000u); // ApplZone
            // Drive Queue Header (empty)
            lc_musashi_bus_ram_write16(0x00000308u, 0); // DrvQHdr.qFlags
            // Set up a drive queue entry at $8A40 for boot disk (drive 1, refNum -63)
            const uint32_t dqe = 0x00008a40u;
            lc_musashi_bus_ram_write32(dqe + 0u, 0); // qLink (no next)
            lc_musashi_bus_ram_write16(dqe + 4u, 1u); // qType = drive
            lc_musashi_bus_ram_write16(dqe + 6u, 1u); // dQDrive = 1
            lc_musashi_bus_ram_write16(dqe + 8u, (uint16_t)(int16_t)-63); // dQRefNum = -63 (disk)
            lc_musashi_bus_ram_write16(dqe + 10u, 0); // dQFSID = HFS
            lc_musashi_bus_ram_write16(dqe + 12u, (uint16_t)(409600u >> 16)); // dQDrvSz high
            lc_musashi_bus_ram_write16(dqe + 14u, (uint16_t)(409600u & 0xFFFF)); // dQDrvSz low
            lc_musashi_bus_ram_write32(0x0000030au, dqe); // DrvQHdr.qHead
            lc_musashi_bus_ram_write32(0x0000030eu, dqe); // DrvQHdr.qTail
            // BootDrive = 1 (matches drive queue entry dQDrive=1)
            lc_musashi_bus_ram_write16(0x00000210u, 1u);
            lc_musashi_bus_ram_write32(0x00000130u, (uint32_t)active_bus->ram_size - 0x8000u); // DefltStack
            lc_musashi_bus_ram_write32(0x0000031au, 0x00ffffffu); // Lo3Bytes (strip mask)
            lc_musashi_bus_ram_write16(0x00000d00u, 10000u); // TimeDBRA
            lc_musashi_bus_ram_write16(0x00000d02u, 10000u); // TimeSCCDBRA
            lc_musashi_bus_ram_write16(0x00000d04u, 10000u); // TimeSCSIDBRA

            // Pre-populate OS trap table ($0400-$0800) with the ROM's generic
            // NOP handler (moveq #0,d0; rts at ROM+0x0d88) using 32-bit addresses.
            for (uint32_t i = 0; i < 0x100u; i++) {
                lc_musashi_bus_ram_write32(0x00000400u + i * 4u, LC_BASILISK_ROM_BASE_32 + 0x0d88u);
            }
            // Pre-populate toolbox trap table ($0E00-$2E00)
            for (uint32_t addr = 0x00000e00u; addr < 0x00002e00u; addr += 4u) {
                lc_musashi_bus_ram_write32(addr, LC_BASILISK_ROM_BASE_32 + 0x0d88u);
            }

            m68k_set_reg(M68K_REG_A6, boot_globs);
            m68k_set_reg(M68K_REG_A4, (uint32_t)active_bus->ram_size); // BootGlobs A4 for PATCH_BOOT_GLOBS
            // Do PATCH_BOOT_GLOBS work here since we skip $10e entirely:
            {
                const uint32_t a4 = (uint32_t)active_bus->ram_size;
                if (a4 >= 26u) {
                    lc_musashi_bus_ram_write32(a4 - 20u, (uint32_t)active_bus->ram_size); // MemTop
                    lc_musashi_bus_ram_write8(a4 - 26u, 0); // No MMU
                    lc_musashi_bus_ram_write8(a4 - 25u, 1u); // No MMU flag
                }
            }
            m68k_set_reg(M68K_REG_SP, 0x00010000u);
        }
        return true;
    }

    case LC_B2_EMUL_OP_CLKNOMEM: {
        lc_musashi_bus_basilisk_log_emul_op(op, "CLKNOMEM");
        uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
        const bool is_read = (d1 & 0x80u) != 0u;
        if ((d1 & 0x78u) == 0x38u) {
            const uint8_t reg = (uint8_t)(((d1 << 5u) & 0xe0u) | ((d1 >> 10u) & 0x1fu));
            if (is_read) {
                d2 = 0;
                if (reg == 0x08u) {
                    d2 = 0x00u;
                } else if (reg == 0x8au) {
                    d2 = 0x05u; // 32-bit addressing enabled, matching Basilisk.
                } else if (reg == 0xe0u || reg == 0xe2u) {
                    d2 = 0x00u;
                } else if (reg == 0xe1u) {
                    d2 = 0xf1u;
                } else if (reg == 0xe3u) {
                    d2 = 0x0au;
                }
            }
        } else {
            const uint8_t reg = (uint8_t)((d1 >> 2u) & 0x1fu);
            if (is_read) {
                d2 = reg < 0x08u ? 0u : 0u;
            }
        }
        m68k_set_reg(M68K_REG_D0, 0);
        m68k_set_reg(M68K_REG_D1, d2 & 0xffu);
        m68k_set_reg(M68K_REG_D2, d2 & 0xffu);
        return true;
    }

    case LC_B2_EMUL_OP_READ_XPRAM:
        lc_musashi_bus_basilisk_log_emul_op(op, "READ_XPRAM");
        m68k_set_reg(M68K_REG_D1, 0);
        return true;

    case LC_B2_EMUL_OP_READ_XPRAM2: {
        lc_musashi_bus_basilisk_log_emul_op(op, "READ_XPRAM2");
        const uint32_t reg = m68k_get_reg(NULL, M68K_REG_D0) & 0xffu;
        m68k_set_reg(M68K_REG_D0, reg == 0x8au ? 0x05u : 0u);
        return true;
    }

    case LC_B2_EMUL_OP_PATCH_BOOT_GLOBS: {
        lc_musashi_bus_basilisk_log_emul_op(op, "PATCH_BOOT_GLOBS");
        const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
        if (a4 >= 26u && a4 < active_bus->ram_size) {
            lc_musashi_bus_ram_write32(a4 - 20u, (uint32_t)active_bus->ram_size);
            lc_musashi_bus_ram_write8(a4 - 26u, 0);
            lc_musashi_bus_ram_write8(a4 - 25u, (uint8_t)(active_bus->ram[a4 - 25u] | 1u));
        }
        m68k_set_reg(M68K_REG_A6, (uint32_t)active_bus->ram_size);
        return true;
    }

    case LC_B2_EMUL_OP_FIX_BOOTSTACK:
        lc_musashi_bus_basilisk_log_emul_op(op, "FIX_BOOTSTACK");
        m68k_set_reg(M68K_REG_A1, (uint32_t)(active_bus->ram_size * 3u / 4u));
        return true;

    case LC_B2_EMUL_OP_FIX_MEMSIZE: {
        lc_musashi_bus_basilisk_log_emul_op(op, "FIX_MEMSIZE");
        static unsigned fix_memsize_calls = 0;
        fix_memsize_calls++;
        if (fix_memsize_calls <= 2u) {
            uint32_t diff = 0;
            if (active_bus->ram_size > 0x1efcu) {
                diff = lc_musashi_bus_peek_ram32(0x1ef8u) - lc_musashi_bus_peek_ram32(0x1ef4u);
                lc_musashi_bus_ram_write32(0x1ef8u, (uint32_t)active_bus->ram_size);
                lc_musashi_bus_ram_write32(0x1ef4u, (uint32_t)active_bus->ram_size - diff);
            }
        }
        // After the first meaningful call, CompBootStack gets re-entered
        // via bogus high-trap dispatch (uninitialized table entries point
        // back into ResourceMgr which recurses here).  Return without
        // modifying memory to break the loop.
        return true;
    }

    case LC_B2_EMUL_OP_INSTALL_DRIVERS:
        lc_musashi_bus_basilisk_log_emul_op(op, "INSTALL_DRIVERS");
        lc_musashi_bus_basilisk_install_drivers(m68k_get_reg(NULL, M68K_REG_A0));
        // Load boot resources from fixture files into RAM and set up handles.
        // boot_2 at $50000, boot_3 at $52000. Handles at $4ff00/$4ff08.
        {
            extern void host_load_boot_resources(uint8_t *ram, size_t ram_size);
            extern void host_load_system_rsrc(uint8_t *ram, size_t ram_size);
            host_load_boot_resources(active_bus->ram, active_bus->ram_size);
            host_load_system_rsrc(active_bus->ram, active_bus->ram_size);
            // Register handle sizes so _GetHandleSize returns correct values.
            lc_musashi_bus_post_reset_set_handle_record(0x0004ff00u, 0x00900000u, 648u);
            lc_musashi_bus_post_reset_set_handle_record(0x0004ff08u, 0x00902000u, 31420u);
            // Boot continuation trampoline: ROM calls through $DBC to start boot_2.
            // Must be set HERE (after lc_memory seed which overwrites $DBC).
            // Place trampoline at $7F800 (safe area above heap, below stack).
            const uint32_t tramp = 0x00e00000u;
            lc_musashi_bus_ram_write32(0x00000dbcu, tramp); // StartBoot = trampoline
            lc_musashi_bus_ram_write16(tramp + 0u, 0x267cu); // MOVEA.L #imm,A3
            lc_musashi_bus_ram_write32(tramp + 2u, 0x0004ff00u); // boot_2 handle
            lc_musashi_bus_ram_write16(tramp + 6u, 0x4ef9u); // JMP abs.L
            lc_musashi_bus_ram_write32(tramp + 8u, 0x00900000u); // boot_2 code addr
        }
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_SONY_OPEN:
        lc_musashi_bus_basilisk_log_emul_op(op, "SONY_OPEN");
        lc_musashi_bus_ram_write32(m68k_get_reg(NULL, M68K_REG_A1) + 16u, 0u);
        m68k_set_reg(M68K_REG_D0, 0);
        return true;
    case LC_B2_EMUL_OP_SONY_PRIME:
        lc_musashi_bus_basilisk_log_emul_op(op, "SONY_PRIME");
        m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)lc_musashi_bus_basilisk_disk_prime(true, m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1)));
        return true;
    case LC_B2_EMUL_OP_SONY_CONTROL:
    case LC_B2_EMUL_OP_SONY_STATUS:
        lc_musashi_bus_basilisk_log_emul_op(op, op == LC_B2_EMUL_OP_SONY_CONTROL ? "SONY_CONTROL" : "SONY_STATUS");
        m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)lc_musashi_bus_basilisk_disk_control_status(op, m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1)));
        return true;

    case LC_B2_EMUL_OP_DISK_OPEN:
        lc_musashi_bus_basilisk_log_emul_op(op, "DISK_OPEN");
        lc_musashi_bus_ram_write32(m68k_get_reg(NULL, M68K_REG_A1) + 16u, 0u);
        m68k_set_reg(M68K_REG_D0, 0);
        return true;
    case LC_B2_EMUL_OP_DISK_PRIME:
        lc_musashi_bus_basilisk_log_emul_op(op, "DISK_PRIME");
        {
            uint32_t dpb = m68k_get_reg(NULL, M68K_REG_A0);
            ESP_LOGI(TAG, "LC DISK_PRIME params: pb=0x%08" PRIx32
                     " buf=0x%08" PRIx32 " len=0x%08" PRIx32
                     " trap=0x%04x refnum=%d pos_mode=0x%04x",
                     dpb,
                     lc_musashi_bus_peek_ram32(dpb + 32u),
                     lc_musashi_bus_peek_ram32(dpb + 36u),
                     (unsigned)lc_musashi_bus_peek_ram16(dpb + 6u),
                     (int)(int16_t)lc_musashi_bus_peek_ram16(dpb + 24u),
                     (unsigned)lc_musashi_bus_peek_ram16(dpb + 44u));
        }
        ESP_LOGI(TAG, "LC A-line vector at DISK_PRIME time: $28=0x%08" PRIx32 " $800=0x%04x",
                 lc_musashi_bus_peek_ram32(0x28u),
                 (unsigned)lc_musashi_bus_peek_ram16(0x800u));
        m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)lc_musashi_bus_basilisk_disk_prime(false, m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1)));
        ESP_LOGI(TAG, "LC DISK_PRIME result: D0=0x%08x $800=0x%04x $802=0x%04x",
                 m68k_get_reg(NULL, M68K_REG_D0),
                 (unsigned)lc_musashi_bus_peek_ram16(0x800u),
                 (unsigned)lc_musashi_bus_peek_ram16(0x802u));
        return true;
    case LC_B2_EMUL_OP_DISK_CONTROL:
    case LC_B2_EMUL_OP_DISK_STATUS:
        lc_musashi_bus_basilisk_log_emul_op(op, op == LC_B2_EMUL_OP_DISK_CONTROL ? "DISK_CONTROL" : "DISK_STATUS");
        m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)lc_musashi_bus_basilisk_disk_control_status(op, m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1)));
        return true;

    case LC_B2_EMUL_OP_CDROM_OPEN:
    case LC_B2_EMUL_OP_CDROM_PRIME:
    case LC_B2_EMUL_OP_CDROM_CONTROL:
    case LC_B2_EMUL_OP_CDROM_STATUS:
        lc_musashi_bus_basilisk_log_emul_op(op, "CDROM_NO_DEVICE");
        m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)-65); // offLinErr
        return true;

    case LC_B2_EMUL_OP_ADBOP:
        lc_musashi_bus_basilisk_log_emul_op(op, "ADBOP_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_INSTIME:
    case LC_B2_EMUL_OP_RMVTIME:
    case LC_B2_EMUL_OP_PRIMETIME:
    case LC_B2_EMUL_OP_MICROSECONDS:
        lc_musashi_bus_basilisk_log_emul_op(op, "TIME_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_SCSI_DISPATCH:
        lc_musashi_bus_basilisk_log_emul_op(op, "SCSI_DISPATCH_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_VIDEO_OPEN:
        lc_musashi_bus_basilisk_log_emul_op(op, "VIDEO_OPEN_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_VIDEO_CONTROL:
        lc_musashi_bus_basilisk_log_emul_op(op, "VIDEO_CONTROL_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_VIDEO_STATUS:
        lc_musashi_bus_basilisk_log_emul_op(op, "VIDEO_STATUS_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_IRQ:
        lc_musashi_bus_basilisk_log_emul_op(op, "IRQ_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;

    case LC_B2_EMUL_OP_BLOCK_MOVE: {
        lc_musashi_bus_basilisk_log_emul_op(op, "BLOCK_MOVE_STUB");
        const uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
        const uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
        uint32_t len = m68k_get_reg(NULL, M68K_REG_D0);
        if (len > 0x00100000u) len = 0x00100000u;
        if (src < active_bus->ram_size && dst < active_bus->ram_size &&
            len <= active_bus->ram_size - src && len <= active_bus->ram_size - dst) {
            memmove(&active_bus->ram[dst], &active_bus->ram[src], len);
        }
        m68k_set_reg(M68K_REG_D0, 0);
        return true;
    }

    case LC_B2_EMUL_OP_SHUTDOWN:
    case LC_B2_EMUL_OP_DEBUGUTIL:
    case LC_B2_EMUL_OP_CHECKLOAD:
    default:
        lc_musashi_bus_basilisk_log_emul_op(op, "NOOP_STUB");
        m68k_set_reg(M68K_REG_D0, 0);
        return true;
    }
}

static void lc_musashi_bus_seed_fake_video_resource(void) {
    // Minimal fields consumed immediately by the ROM code after SFindStruct:
    // +0x04 rowBytes, +0x0a/+0x0c geometry-like words, +0x20 resource type.
    lc_musashi_bus_ram_write16(LC_FAKE_VIDEO_RESOURCE_ADDR + 0x04u, 64u);
    lc_musashi_bus_ram_write16(LC_FAKE_VIDEO_RESOURCE_ADDR + 0x0au, 384u);
    lc_musashi_bus_ram_write16(LC_FAKE_VIDEO_RESOURCE_ADDR + 0x0cu, 512u);
    lc_musashi_bus_ram_write16(LC_FAKE_VIDEO_RESOURCE_ADDR + 0x20u, 1u);
}

static void lc_musashi_bus_set_trap_success_and_skip(uint32_t pc) {
    m68k_set_reg(M68K_REG_D0, 0);
    const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    m68k_set_reg(M68K_REG_SR, (sr & 0xfff0u) | 0x0004u); // Z=1, noErr
    m68k_set_reg(M68K_REG_PC, pc + 2u);
}

static void lc_musashi_bus_maybe_stub_video_default_bad_indirect(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000027a4u) {
        return;
    }
    const uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
    if (a1 != 0xb6db6db6u && a1 != 0x6db6db6du && a1 < 0xff000000u) {
        return;
    }
    lc_musashi_bus_set_trap_success_and_skip(pc);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed video-default indirect callback: pc=0x%08" PRIx32
                 " bad_a1=0x%08" PRIx32 " next_pc=0x%08" PRIx32,
                 pc, a1, pc + 2u);
    }
}

static void lc_musashi_bus_maybe_stub_slot_manager_video_default(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00002352u && rom_offset != 0x00002364u &&
        rom_offset != 0x00002370u) {
        return;
    }

    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    if (rom_offset == 0x00002352u) {
        // First SlotManager call in this block.  Return noErr so the ROM scans
        // for the default video resource rather than branching to diagnostics.
        lc_musashi_bus_ram_write32(a0 + 4u, 0);
    } else if (rom_offset == 0x00002364u) {
        // SFindStruct-like slot lookup for d3=0x80.
    } else {
        // Return a tiny fake sResource structure for the immediately following
        // `movea.l (a0),a1` / `cmpi.w #1,$20(a1)` sequence.
        lc_musashi_bus_seed_fake_video_resource();
        lc_musashi_bus_ram_write32(a0, LC_FAKE_VIDEO_RESOURCE_ADDR);
    }

    lc_musashi_bus_set_trap_success_and_skip(pc);
    if (!slot_manager_video_default_stub_logged) {
        slot_manager_video_default_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed ROM SlotManager video-default traps: first_pc=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " fake_resource=0x%08x next_pc=0x%08" PRIx32,
                 pc, a0, LC_FAKE_VIDEO_RESOURCE_ADDR, pc + 2u);
    }
}

static void lc_musashi_bus_maybe_stub_control_video_default(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000023beu && rom_offset != 0x000023dau) {
        return;
    }
    lc_musashi_bus_set_trap_success_and_skip(pc);
    if (!control_video_default_stub_logged) {
        control_video_default_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed ROM Control trap in video-default path: pc=0x%08" PRIx32
                 " next_pc=0x%08" PRIx32,
                 pc, pc + 2u);
    }
}

static void lc_musashi_bus_maybe_stub_disposeptr_video_default(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000023eeu) {
        return;
    }
    lc_musashi_bus_set_trap_success_and_skip(pc);
    if (!disposeptr_video_default_stub_logged) {
        disposeptr_video_default_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed ROM DisposePtr trap in video-default path: pc=0x%08" PRIx32
                 " next_pc=0x%08" PRIx32,
                 pc, pc + 2u);
    }
}

static void lc_musashi_bus_maybe_stub_swap_mmu_video_default(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0000242au && rom_offset != 0x00002438u &&
        rom_offset != 0x0000248cu && rom_offset != 0x000024cau &&
        rom_offset != 0x00002508u && rom_offset != 0x00002520u &&
        rom_offset != 0x00002774u && rom_offset != 0x0000277eu) {
        return;
    }
    lc_musashi_bus_set_trap_success_and_skip(pc);
    if (!swap_mmu_video_default_stub_logged) {
        swap_mmu_video_default_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed ROM SwapMMUMode trap in video-default path: pc=0x%08" PRIx32
                 " next_pc=0x%08" PRIx32,
                 pc, pc + 2u);
    }
}

static void lc_musashi_bus_maybe_cap_post_reset_finalizer_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00041c06u) {
        return;
    }
    const uint32_t d4 = m68k_get_reg(NULL, M68K_REG_D4);
    if ((d4 & 0xffffu) <= 0x000fu) {
        return;
    }
    const uint32_t capped = (d4 & 0xffff0000u) | 0x00000003u;
    m68k_set_reg(M68K_REG_D4, capped);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC capped post-reset memory-layout finalizer DBRA count: pc=0x%08" PRIx32
                 " old_d4=0x%08" PRIx32 " new_d4=0x%08" PRIx32,
                 pc, d4, capped);
    }
}

static void lc_musashi_bus_maybe_fix_post_reset_pack_empty_count(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset == 0x00041d92u) {
        // The provisional post-reset descriptor state can make the pack helper
        // see an empty generated-entry stack, which falls back to sentinel base
        // 0x7fff0000 and later creates the bad 0x7ffffc02 entry.  Until the real
        // low-memory address-map tables are modeled, keep the empty case bounded
        // to base zero so the next frontier reflects descriptor contents rather
        // than the sentinel fallback.
        m68k_set_reg(M68K_REG_A0, 0);
        m68k_set_reg(M68K_REG_PC, pc + 6u);
        static bool logged_empty = false;
        if (!logged_empty) {
            logged_empty = true;
            ESP_LOGW(TAG,
                     "LC patched post-reset pack empty-count sentinel: pc=0x%08" PRIx32
                     " forced_a0=0 next_pc=0x%08" PRIx32,
                     pc, pc + 6u);
        }
        return;
    }
    if (rom_offset == 0x00041d5eu && m68k_get_reg(NULL, M68K_REG_D0) == 0x0000fc02u) {
        // Diagnostic normalization for the same provisional empty descriptor:
        // strip the synthetic high flag bits so the compressor can expose the
        // next dependency instead of immediately rejecting 0x0000fc02.
        m68k_set_reg(M68K_REG_D0, 0x00000002u);
        static bool logged_entry = false;
        if (!logged_entry) {
            logged_entry = true;
            ESP_LOGW(TAG,
                     "LC normalized post-reset generated entry: pc=0x%08" PRIx32
                     " old_d0=0x0000fc02 new_d0=0x00000002",
                     pc);
        }
        return;
    }
    if (rom_offset == 0x00041e4eu && m68k_get_reg(NULL, M68K_REG_D1) == 0x00000201u) {
        // The current synthetic descriptor stack still returns a packed-layout
        // status with bit 9 and bit 0 set. Normalize only this observed value so
        // later boot code can reveal the next real dependency.
        m68k_set_reg(M68K_REG_D1, 0);
        static bool logged_status = false;
        if (!logged_status) {
            logged_status = true;
            ESP_LOGW(TAG,
                     "LC normalized post-reset compress status: pc=0x%08" PRIx32
                     " old_d1=0x00000201 new_d1=0",
                     pc);
        }
        return;
    }
    if (rom_offset == 0x00041bd4u) {
        const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
        if (a4 + 7u < (uint32_t)(active_bus != NULL ? active_bus->ram_size : 0u)) {
            lc_musashi_bus_ram_write32(a4 + 4u, 0x408416bcu);
            static bool logged_return = false;
            if (!logged_return) {
                logged_return = true;
                ESP_LOGW(TAG,
                         "LC repaired post-reset finalizer return address: pc=0x%08" PRIx32
                         " frame=0x%08" PRIx32 " return_slot=0x%08" PRIx32
                         " value=0x408416bc",
                         pc, a4, a4 + 4u);
            }
        }
    }
}

static bool lc_musashi_bus_post_reset_plausible_rom_pc(uint32_t value);

static void lc_musashi_bus_maybe_add_dynamic_str_resource(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if ((rom_offset != 0x0001b5b4u && rom_offset != 0x0001b6d0u &&
         rom_offset != 0x0001b698u) ||
        active_bus == NULL || active_bus->ram == NULL ||
        m68k_get_reg(NULL, M68K_REG_SP) >= 0x00030000u ||
        m68k_get_reg(NULL, M68K_REG_D3) != 0x53545220u) {
        return;
    }
    const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
    if (a4 + 3u >= active_bus->ram_size) {
        return;
    }
    const uint32_t map = lc_musashi_bus_peek_ram32(a4);
    if (map == 0u || map + 0x200u >= active_bus->ram_size) {
        return;
    }
    const uint16_t type_off = lc_musashi_bus_peek_ram16(map + 24u);
    const uint16_t name_off = lc_musashi_bus_peek_ram16(map + 26u);
    if (type_off < 0x001cu || name_off <= type_off + 0x20u ||
        map + name_off >= active_bus->ram_size) {
        return;
    }
    const uint32_t type_list = map + type_off;
    const uint16_t count_minus_one = lc_musashi_bus_peek_ram16(type_list);
    const uint32_t type_count = (uint32_t)count_minus_one + 1u;
    for (uint32_t i = 0; i < type_count && i < 64u; i++) {
        if (lc_musashi_bus_peek_ram32(type_list + 2u + i * 8u) == 0x53545220u) {
            return;
        }
    }
    const uint32_t entry = type_list + 2u + type_count * 8u;
    const uint32_t ref_list = entry + 8u;
    if (ref_list + 11u >= map + name_off || ref_list + 11u >= active_bus->ram_size) {
        return;
    }
    lc_musashi_bus_ram_write16(type_list, (uint16_t)(count_minus_one + 1u));
    lc_musashi_bus_ram_write32(entry + 0u, 0x53545220u); // 'STR '
    lc_musashi_bus_ram_write16(entry + 4u, 0x0000u);     // one reference minus one.
    lc_musashi_bus_ram_write16(entry + 6u, (uint16_t)(ref_list - type_list));
    lc_musashi_bus_ram_write16(ref_list + 0u, 0xe000u);  // startup STR id -8192.
    lc_musashi_bus_ram_write16(ref_list + 2u, 0xffffu);  // no name.
    lc_musashi_bus_ram_write32(ref_list + 4u, 0x00000100u); // attr + resource data offset.
    lc_musashi_bus_ram_write32(ref_list + 8u, 0x00008320u); // seeded STR master pointer.
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC appended STR resource type to dynamic Resource Manager map: pc=0x%08" PRIx32
                 " map=0x%08" PRIx32 " type_list=0x%08" PRIx32
                 " old_count=0x%04x entry=0x%08" PRIx32 " ref_list=0x%08" PRIx32,
                 pc, map, type_list, count_minus_one, entry, ref_list);
    }
}

static void lc_musashi_bus_maybe_cap_post_reset_resource_type_scan(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0001b6d6u && rom_offset != 0x0001b6d8u &&
        rom_offset != 0x0001b6dau) {
        return;
    }
    const uint32_t d5 = m68k_get_reg(NULL, M68K_REG_D5) & 0xffffu;
    if (d5 <= 0x0200u) {
        return;
    }
    m68k_set_reg(M68K_REG_PC, 0x4081b6fcu);
    if (!post_reset_resource_scan_cap_logged) {
        post_reset_resource_scan_cap_logged = true;
        ESP_LOGW(TAG,
                 "LC capped runaway ResourceMgr type scan: pc=0x%08" PRIx32
                 " d3=0x%08x d5=0x%08" PRIx32 " a2=0x%08x a3=0x%08x a4=0x%08x"
                 " target=0x4081b6fc",
                 pc, m68k_get_reg(NULL, M68K_REG_D3), d5,
                 m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3),
                 m68k_get_reg(NULL, M68K_REG_A4));
    }
}

static void lc_musashi_bus_maybe_log_post_reset_resource_lookup(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const bool low_stack_frontier = sp < 0x00030000u;
    if (!low_stack_frontier && post_reset_resource_lookup_logs >= 192u) {
        return;
    }
    if (low_stack_frontier && post_reset_resource_low_stack_logs >= 384u) {
        return;
    }
    if (rom_offset != 0x0001b5b4u && rom_offset != 0x0001b5c4u &&
        rom_offset != 0x0001b65eu && rom_offset != 0x0001b670u &&
        rom_offset != 0x0001b67cu && rom_offset != 0x0001b686u &&
        rom_offset != 0x0001b696u && rom_offset != 0x0001b698u &&
        rom_offset != 0x0001b6d0u && rom_offset != 0x0001b6d6u &&
        rom_offset != 0x0001b6fcu && rom_offset != 0x0001b704u &&
        rom_offset != 0x0001b7bau && rom_offset != 0x0001b7c4u) {
        return;
    }
    if (low_stack_frontier) {
        post_reset_resource_low_stack_logs++;
    } else {
        post_reset_resource_lookup_logs++;
    }
    const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
    const uint32_t a3 = m68k_get_reg(NULL, M68K_REG_A3);
    const uint32_t map_from_a4 = lc_musashi_bus_peek_guest32(a4);
    const uint32_t next_from_map = lc_musashi_bus_peek_guest32(map_from_a4 + 16u);
    const uint32_t type_offs = lc_musashi_bus_peek_guest32(map_from_a4 + 24u);
    const uint32_t top_map_h = lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL);
    const uint32_t sys_map_h = lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL);
    const uint32_t rom_map_h = lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL);
    ESP_LOGI(TAG,
             "LC post-reset ResourceMgr trace: pc=0x%08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x d4=0x%08x d5=0x%08x d6=0x%08x d7=0x%08x"
             " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x sp=0x%08x"
             " low_stack=%d map_a4=0x%08" PRIx32 " map_a3=0x%08" PRIx32 " next=0x%08" PRIx32
             " type_offs=0x%08" PRIx32 " low_top=0x%08" PRIx32 " low_sys=0x%08" PRIx32 " low_rom=0x%08" PRIx32,
             pc, m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D3),
             m68k_get_reg(NULL, M68K_REG_D4), m68k_get_reg(NULL, M68K_REG_D5),
             m68k_get_reg(NULL, M68K_REG_D6), m68k_get_reg(NULL, M68K_REG_D7),
             m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
             m68k_get_reg(NULL, M68K_REG_A2), a3, a4, sp,
             low_stack_frontier ? 1 : 0,
             map_from_a4, lc_musashi_bus_peek_guest32(a3), next_from_map, type_offs,
             top_map_h, sys_map_h, rom_map_h);
}

static void lc_musashi_bus_maybe_escape_post_reset_resource_map_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0001b5c8u) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp >= 0x00100000u) {
        // ReDoMap legitimately walks many ROM resources while rebuilding the
        // map on the high bootstrap stack.  Do not treat that as a terminal
        // lookup cycle.
        return;
    }
    post_reset_resource_map_loop_hits++;
    if (post_reset_resource_map_loop_hits <= 1u ||
        post_reset_resource_map_loop_escape_logged) {
        return;
    }

    const uint32_t d3 = m68k_get_reg(NULL, M68K_REG_D3);
    const bool str_lookup = d3 == 0x53545220u; // 'STR '
    if (str_lookup) {
        // The low-stack loop is a corrupted rebuilt-map traversal while looking
        // for a startup STR resource.  Re-enter the ROM's normal found-resource
        // realization path using the stable RAM-backed seed map instead of
        // returning a made-up result or rewriting the damaged dynamic map.
        m68k_set_reg(M68K_REG_A4, 0x00008000u);
        m68k_set_reg(M68K_REG_A3, 0x00008040u);
        m68k_set_reg(M68K_REG_A2, 0x00008060u);
        m68k_set_reg(M68K_REG_D0, 0u);
        m68k_set_reg(M68K_REG_PC, 0x4081b5ccu);
    }
    post_reset_resource_map_loop_escape_logged = true;
    ESP_LOGW(TAG,
             "%s post-reset ResourceMgr map-chain loop: pc=0x%08" PRIx32
             " hits=%u d3=0x%08x d6=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x sp=0x%08x"
             " map_a4=0x%08" PRIx32 " next=0x%08" PRIx32,
             str_lookup ? "LC routed STR lookup through seeded map for" : "LC observed",
             pc, post_reset_resource_map_loop_hits,
             d3, m68k_get_reg(NULL, M68K_REG_D6),
             m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3),
             m68k_get_reg(NULL, M68K_REG_A4), sp,
             lc_musashi_bus_peek_guest32(m68k_get_reg(NULL, M68K_REG_A4)),
             lc_musashi_bus_peek_guest32(lc_musashi_bus_peek_guest32(m68k_get_reg(NULL, M68K_REG_A4)) + 16u));
}

static void lc_musashi_bus_maybe_log_post_reset_count_combos(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_count_combos_logs >= 96u) {
        return;
    }
    if (rom_offset != 0x0001abceu && rom_offset != 0x0001abecu &&
        rom_offset != 0x0001ac00u && rom_offset != 0x0001ac12u) {
        return;
    }
    post_reset_count_combos_logs++;
    ESP_LOGI(TAG,
             "LC post-reset CountCombos trace: pc=0x%08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x d4=0x%08x"
             " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x sp=0x%08x",
             pc, m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D3),
             m68k_get_reg(NULL, M68K_REG_D4), m68k_get_reg(NULL, M68K_REG_A0),
             m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
             m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_SP));
}

static void lc_musashi_bus_maybe_cap_post_reset_resource_copy_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0001beb4u) {
        return;
    }
    const uint32_t d5 = m68k_get_reg(NULL, M68K_REG_D5);
    if ((d5 & 0xffffu) <= 0x003fu) {
        return;
    }
    const uint32_t capped = (d5 & 0xffff0000u) | 0x00000000u;
    m68k_set_reg(M68K_REG_D5, capped);
    if (!post_reset_resource_copy_cap_logged) {
        post_reset_resource_copy_cap_logged = true;
        const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
        const uint32_t a3 = m68k_get_reg(NULL, M68K_REG_A3);
        const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
        const uint32_t map = lc_musashi_bus_peek_ram32(a4);
        const uint32_t top_map = lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL);
        const uint32_t sys_map_h = lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL);
        const uint32_t rom_map_h = lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL);
        ESP_LOGW(TAG,
                 "LC capped post-reset resource copy DBRA count: pc=0x%08" PRIx32
                 " old_d5=0x%08" PRIx32 " new_d5=0x%08" PRIx32
                 " d0=0x%08x a0=0x%08x a3=0x%08x a4=0x%08x map=0x%08" PRIx32
                 " top=0x%08" PRIx32 " sys=0x%08" PRIx32 " rom=0x%08" PRIx32
                 " cur=0x%04x sysref=0x%04x"
                 " top_ptr=0x%08" PRIx32 " rom_ptr=0x%08" PRIx32
                 " map_hdr=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " map_offs=%08" PRIx32 " %08" PRIx32
                 " a0_m8=%08" PRIx32 " a0_m4=%08" PRIx32 " a0=%08" PRIx32 " a0_p4=%08" PRIx32,
                 pc, d5, capped, m68k_get_reg(NULL, M68K_REG_D0),
                 a0, a3, a4, map, top_map, sys_map_h, rom_map_h,
                 lc_musashi_bus_peek_ram16(0x00000a5au), lc_musashi_bus_peek_ram16(0x00000a58u),
                 lc_musashi_bus_peek_ram32(top_map), lc_musashi_bus_peek_ram32(rom_map_h),
                 lc_musashi_bus_peek_ram32(map + 0u), lc_musashi_bus_peek_ram32(map + 4u),
                 lc_musashi_bus_peek_ram32(map + 8u), lc_musashi_bus_peek_ram32(map + 12u),
                 lc_musashi_bus_peek_ram32(map + 24u), lc_musashi_bus_peek_ram32(map + 28u),
                 lc_musashi_bus_peek_ram32(a0 - 8u), lc_musashi_bus_peek_ram32(a0 - 4u),
                 lc_musashi_bus_peek_ram32(a0 + 0u), lc_musashi_bus_peek_ram32(a0 + 4u));
    }
}

static bool lc_musashi_bus_post_reset_dispatch_matches(uint32_t pc,
                                                        uint16_t trap_word,
                                                        uint16_t trap_low) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009a04u && rom_offset != 0x00009a20u && rom_offset != 0x00009a22u) {
        return false;
    }
    const uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    const uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    return (d1 & 0xffffu) == trap_word || (d2 & 0xffffu) == trap_low;
}

static const char *lc_musashi_bus_post_reset_trap_name(uint16_t trap_word) {
    switch (trap_word & 0x01ffu) {
    case 0x019u: return "InitZone";
    case 0x01fu: return "DisposePtr";
    case 0x023u: return "DisposeHandle";
    case 0x024u: return "SetHandleSize";
    case 0x025u: return "GetHandleSize";
    case 0x029u: return "HLock";
    case 0x02au: return "HUnlock";
    case 0x02bu: return "EmptyHandle";
    case 0x02du: return "SetApplLimit";
    case 0x02eu: return "BlockMove";
    case 0x03fu: return "InitUtil";
    case 0x047u: return "SetTrapAddress";
    case 0x051u: return "ReadXPRam";
    case 0x055u: return "StripAddress";
    case 0x05du: return "SwapMMUMode";
    case 0x061u: return "MaxBlock";
    case 0x06eu: return "SlotManager";
    case 0x11eu: return "NewPtr";
    case 0x122u: return "NewHandle";
    case 0x126u: return "HandleZone";
    default: return "unknown";
    }
}

static void lc_musashi_bus_record_post_reset_low_trap(uint32_t pc,
                                                       uint16_t trap_word,
                                                       uint32_t sp,
                                                       uint32_t d0,
                                                       uint32_t d1,
                                                       uint32_t d2,
                                                       uint32_t a0,
                                                       uint32_t a1,
                                                       uint32_t a2) {
    lc_post_reset_low_trap_snapshot_t *slot =
        &post_reset_low_trap_ring[post_reset_low_trap_ring_index %
                                  (sizeof(post_reset_low_trap_ring) /
                                   sizeof(post_reset_low_trap_ring[0]))];
    slot->pc = pc;
    slot->trap_word = trap_word;
    slot->sp = sp;
    slot->d0 = d0;
    slot->d1 = d1;
    slot->d2 = d2;
    slot->a0 = a0;
    slot->a1 = a1;
    slot->a2 = a2;
    slot->stack0 = lc_musashi_bus_peek_ram32(sp + 0u);
    slot->stack4 = lc_musashi_bus_peek_ram32(sp + 4u);
    slot->stack8 = lc_musashi_bus_peek_ram32(sp + 8u);
    slot->stack12 = lc_musashi_bus_peek_ram32(sp + 12u);
    slot->stack16 = lc_musashi_bus_peek_ram32(sp + 16u);
    slot->stack20 = lc_musashi_bus_peek_ram32(sp + 20u);
    slot->stack24 = lc_musashi_bus_peek_ram32(sp + 24u);
    slot->stack28 = lc_musashi_bus_peek_ram32(sp + 28u);
    post_reset_low_trap_ring_index++;
}

static void lc_musashi_bus_dump_post_reset_low_trap_ring(void) {
    const unsigned ring_len = sizeof(post_reset_low_trap_ring) /
                              sizeof(post_reset_low_trap_ring[0]);
    const unsigned total = post_reset_low_trap_ring_index < ring_len ?
                               post_reset_low_trap_ring_index :
                               ring_len;
    const unsigned start = post_reset_low_trap_ring_index >= total ?
                               post_reset_low_trap_ring_index - total :
                               0;
    for (unsigned i = 0; i < total; i++) {
        const lc_post_reset_low_trap_snapshot_t *slot =
            &post_reset_low_trap_ring[(start + i) % ring_len];
        ESP_LOGW(TAG,
                 "LC recent low A-trap before attr return: slot=%u pc=0x%08" PRIx32
                 " trap=0x%04x name=%s sp=0x%08" PRIx32
                 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " +16=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " d0=0x%08" PRIx32 " d1=0x%08" PRIx32 " d2=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " a1=0x%08" PRIx32 " a2=0x%08" PRIx32,
                 i, slot->pc, slot->trap_word,
                 lc_musashi_bus_post_reset_trap_name(slot->trap_word), slot->sp,
                 slot->stack0, slot->stack4, slot->stack8, slot->stack12,
                 slot->stack16, slot->stack20, slot->stack24, slot->stack28,
                 slot->d0, slot->d1, slot->d2, slot->a0, slot->a1, slot->a2);
    }
}

static void lc_musashi_bus_maybe_canonicalize_post_reset_trap_return(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009a32u || active_bus == NULL || active_bus->ram == NULL ||
        !active_bus->initialized) {
        return;
    }

    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 3u >= active_bus->ram_size) {
        return;
    }
    const uint32_t target = lc_musashi_bus_peek_ram32(sp);
    const uint32_t rom_limit = active_bus->rom_size < LC_ROM_WINDOW_SIZE
                                   ? (uint32_t)active_bus->rom_size
                                   : LC_ROM_WINDOW_SIZE;
    if ((target & 1u) != 0 || target < 0x000000aau || target >= rom_limit) {
        return;
    }

    // Some ROM Memory Manager paths return through the low A-trap dispatcher
    // with a 24-bit-stripped ROM return address on the stack.  On real LC ROM
    // execution that address still resolves into the ROM execution context; in
    // this direct host probe, low memory is RAM and may contain diagnostic fill
    // data.  Canonicalize only the dispatcher's RTS target, preserving the
    // guest stack shape while avoiding execution of RAM fill as code.
    const uint32_t canonical = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE + target;
    lc_musashi_bus_ram_write32(sp, canonical);
    if (target == 0x00006d7cu && (m68k_get_reg(NULL, M68K_REG_D0) & 0x80000000u) != 0u) {
        // The return is from a small NewPtr rebuild path.  The real Memory
        // Manager may have returned memFullErr after our synthetic allocator
        // supplied/reused A0; preserve the synthetic success result so the ROM
        // continues the table rebuild instead of branching into diagnostics.
        m68k_set_reg(M68K_REG_D0, 0);
    }
    if (post_reset_trap_return_canonicalize_logs < 16u) {
        const uint16_t rom_word = active_bus->rom != NULL && target + 1u < active_bus->rom_size
                                      ? (uint16_t)(((uint16_t)active_bus->rom[target] << 8u) |
                                                   active_bus->rom[target + 1u])
                                      : 0xffffu;
        ESP_LOGW(TAG,
                 "LC canonicalized stripped ROM trap return: pc=0x%08" PRIx32
                 " sp=0x%08" PRIx32 " target=0x%08" PRIx32
                 " canonical=0x%08" PRIx32 " rom_word=0x%04x"
                 " d0=0x%08x d1=0x%08x d2=0x%08x a0=0x%08x a2=0x%08x",
                 pc, sp, target, canonical, rom_word, m68k_get_reg(NULL, M68K_REG_D0),
                 m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                 m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A2));
        post_reset_trap_return_canonicalize_logs++;
    }
}

static void lc_musashi_bus_maybe_log_post_reset_attr_low_dispatch_return(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009a18u || post_reset_attr_low_dispatch_return_logs >= 16u) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t target = lc_musashi_bus_peek_ram32(sp);
    const bool attr_tagged_target = (target & 0xff000000u) == 0x58000000u;
    const bool zero_target = target == 0u;
    if (!attr_tagged_target && !zero_target) {
        return;
    }
    const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
    ESP_LOGW(TAG,
             "LC %s low A-trap dispatcher return: pc=0x%08" PRIx32
             " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
             " target=0x%08" PRIx32
             " stack_m16=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d3=0x%08x d7=0x%08x"
             " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x a4p=0x%08" PRIx32
             " a5=0x%08x a6=0x%08x sr=0x%04x",
             attr_tagged_target ? "attr-tagged" : "zero-target",
             pc, previous_instruction_pc, sp, target,
             lc_musashi_bus_peek_ram32(sp - 16u), lc_musashi_bus_peek_ram32(sp - 12u),
             lc_musashi_bus_peek_ram32(sp - 8u), lc_musashi_bus_peek_ram32(sp - 4u),
             lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
             lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
             m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D3),
             m68k_get_reg(NULL, M68K_REG_D7), m68k_get_reg(NULL, M68K_REG_A0),
             m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
             m68k_get_reg(NULL, M68K_REG_A3), a4, lc_musashi_bus_peek_ram32(a4),
             m68k_get_reg(NULL, M68K_REG_A5), m68k_get_reg(NULL, M68K_REG_A6),
             m68k_get_reg(NULL, M68K_REG_SR));
    lc_musashi_bus_dump_post_reset_low_trap_ring();
    post_reset_attr_low_dispatch_return_logs++;
}

static bool lc_musashi_bus_post_reset_plausible_stripped_rom_return(uint32_t target) {
    return target >= 0x000000aau && active_bus != NULL && active_bus->rom != NULL &&
           target < active_bus->rom_size && (target & 1u) == 0u;
}

static void lc_musashi_bus_maybe_fix_post_reset_low_dispatch_return(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009a16u && rom_offset != 0x00009a32u) {
        return;
    }
    if (active_bus == NULL || active_bus->ram == NULL || !active_bus->initialized) {
        return;
    }

    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t candidate_slots[] = {sp, sp + 4u};
    for (size_t i = 0; i < sizeof(candidate_slots) / sizeof(candidate_slots[0]); i++) {
        const uint32_t slot = candidate_slots[i];
        if (slot + 7u >= active_bus->ram_size) {
            continue;
        }
        const uint32_t target = lc_musashi_bus_peek_ram32(slot);
        const uint32_t next = lc_musashi_bus_peek_ram32(slot + 4u);
        if (target != 0u || !lc_musashi_bus_post_reset_plausible_stripped_rom_return(next)) {
            continue;
        }
        const uint32_t canonical = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE + next;
        lc_musashi_bus_ram_write32(slot, canonical);
        if (post_reset_trap_return_canonicalize_logs < 16u) {
            ESP_LOGW(TAG,
                     "LC repaired zero low A-trap return slot: pc=0x%08" PRIx32
                     " sp=0x%08" PRIx32 " slot=0x%08" PRIx32
                     " stripped_next=0x%08" PRIx32 " canonical=0x%08" PRIx32
                     " d0=0x%08x d1=0x%08x d2=0x%08x a2=0x%08x",
                     pc, sp, slot, next, canonical, m68k_get_reg(NULL, M68K_REG_D0),
                     m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                     m68k_get_reg(NULL, M68K_REG_A2));
            post_reset_trap_return_canonicalize_logs++;
        }
    }
}

static void lc_musashi_bus_maybe_log_post_reset_atrap_dispatch(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009a04u && rom_offset != 0x00009a20u && rom_offset != 0x00009a22u) {
        return;
    }
    const uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    const uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    uint16_t trap_word = (uint16_t)(d1 & 0xffffu);
    if ((trap_word & 0xf000u) != 0xa000u) {
        trap_word = (uint16_t)(0xa000u | (d2 & 0x0fffu));
    }
    lc_musashi_bus_record_post_reset_low_trap(pc, trap_word,
                                              m68k_get_reg(NULL, M68K_REG_SP),
                                              m68k_get_reg(NULL, M68K_REG_D0), d1, d2,
                                              m68k_get_reg(NULL, M68K_REG_A0),
                                              m68k_get_reg(NULL, M68K_REG_A1),
                                              m68k_get_reg(NULL, M68K_REG_A2));
    if (post_reset_atrap_dispatch_logs < 200u) {
        ESP_LOGI(TAG,
                 "LC post-reset A-trap dispatch trace: pc=0x%08" PRIx32
                 " trap=0x%04x name=%s d0=0x%08x d1=0x%08x d2=0x%08x"
                 " a0=0x%08x a1=0x%08x a2=0x%08x sp=0x%08x",
                 pc, trap_word, lc_musashi_bus_post_reset_trap_name(trap_word),
                 m68k_get_reg(NULL, M68K_REG_D0), d1, d2,
                 m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                 m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_SP));
        post_reset_atrap_dispatch_logs++;
    }
}

static uint32_t lc_musashi_bus_post_reset_heap_stack_floor(void) {
    if (active_bus == NULL || active_bus->ram == NULL || active_bus->ram_size == 0u) {
        return 0;
    }
    uint32_t floor = active_bus->ram_size > LC_POST_RESET_HEAP_TOP_RESERVE
                         ? (uint32_t)active_bus->ram_size - LC_POST_RESET_HEAP_TOP_RESERVE
                         : (uint32_t)active_bus->ram_size;
    const uint32_t appl_limit = lc_musashi_bus_peek_ram32(LC_LOWMEM_APPL_LIMIT) & ~1u;
    if (appl_limit >= 0x00100000u && appl_limit < floor) {
        floor = appl_limit;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & ~1u;
    if (sp >= 0x00100000u && sp > 0x00001000u && sp - 0x00001000u < floor) {
        floor = sp - 0x00001000u;
    }
    return floor;
}

static void lc_musashi_bus_post_reset_write_alloc_header(uint32_t header,
                                                          uint32_t total,
                                                          uint32_t header_size) {
    const uint32_t allocated_header = 0x40000000u | (total & LC_MEMORY_BLOCK_SIZE_MASK);
    if (header_size == 8u) {
        lc_musashi_bus_ram_write32(header + 0u, allocated_header);
        lc_musashi_bus_ram_write32(header + 4u, lc_musashi_bus_peek_ram32(LC_LOWMEM_THE_ZONE));
    } else {
        lc_musashi_bus_ram_write32(header + 0u, allocated_header);
        lc_musashi_bus_ram_write32(header + 4u, total & LC_MEMORY_BLOCK_SIZE_MASK);
        lc_musashi_bus_ram_write32(header + 8u, lc_musashi_bus_peek_ram32(LC_LOWMEM_THE_ZONE));
    }
}

static bool lc_musashi_bus_post_reset_remember_free_block(uint32_t header,
                                                          uint32_t total,
                                                          uint32_t header_size) {
    if (total < header_size || header + total > (active_bus != NULL ? active_bus->ram_size : 0u)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(post_reset_free_blocks) / sizeof(post_reset_free_blocks[0]); i++) {
        if (post_reset_free_blocks[i].total == 0u) {
            post_reset_free_blocks[i].header = header;
            post_reset_free_blocks[i].total = total;
            post_reset_free_blocks[i].header_size = header_size;
            return true;
        }
    }
    return false;
}

static bool lc_musashi_bus_post_reset_range_overlaps_active_stack(uint32_t start,
                                                                   uint32_t length) {
    if (length == 0u || active_bus == NULL || active_bus->ram == NULL ||
        start >= active_bus->ram_size || length > active_bus->ram_size - start) {
        return false;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & ~1u;
    const uint32_t guard_low = sp > 0x100u ? sp - 0x100u : 0u;
    uint32_t guard_high = sp + 0x400u;
    if (guard_high < sp || guard_high > active_bus->ram_size) {
        guard_high = (uint32_t)active_bus->ram_size;
    }
    const uint32_t end = start + length;
    return start < guard_high && end > guard_low;
}

static void lc_musashi_bus_post_reset_coalesce_heap_tail(void) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < sizeof(post_reset_free_blocks) / sizeof(post_reset_free_blocks[0]); i++) {
            if (post_reset_free_blocks[i].total != 0u &&
                post_reset_free_blocks[i].header + post_reset_free_blocks[i].total == post_reset_heap_bump) {
                post_reset_heap_bump = post_reset_free_blocks[i].header;
                memset(&post_reset_free_blocks[i], 0, sizeof(post_reset_free_blocks[i]));
                changed = true;
            }
        }
    }
}

static uint32_t lc_musashi_bus_post_reset_resource_map_protect_end_for_range(uint32_t start,
                                                                               uint32_t length) {
    if (active_bus == NULL || active_bus->ram == NULL || length == 0u ||
        start >= active_bus->ram_size || length > active_bus->ram_size - start) {
        return 0;
    }
    const uint32_t end = start + length;
    const uint32_t handles[] = {
        lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL),
        lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL),
        lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL),
        post_reset_rom_map_handle_dynamic,
    };
    uint32_t max_protect_end = 0;
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
        const uint32_t handle = handles[i];
        if (handle == 0u || handle + 3u >= active_bus->ram_size) {
            continue;
        }
        const uint32_t map_ptr = lc_musashi_bus_peek_ram32(handle);
        if (map_ptr == 0u || map_ptr >= active_bus->ram_size) {
            continue;
        }
        uint32_t protect_end = map_ptr + LC_POST_RESET_RESOURCE_MAP_PROTECT_SIZE;
        if (protect_end < map_ptr || protect_end > active_bus->ram_size) {
            protect_end = (uint32_t)active_bus->ram_size;
        }
        if (start < protect_end && end > map_ptr && protect_end > max_protect_end) {
            max_protect_end = protect_end;
        }
    }
    return max_protect_end;
}

static bool lc_musashi_bus_post_reset_range_overlaps_current_resource_map(uint32_t start,
                                                                           uint32_t length) {
    return lc_musashi_bus_post_reset_resource_map_protect_end_for_range(start, length) != 0u;
}

static void lc_musashi_bus_post_reset_free_alloc(uint32_t ptr) {
    if (active_bus == NULL || active_bus->ram == NULL || ptr < 8u || ptr >= active_bus->ram_size) {
        return;
    }
    if (lc_musashi_bus_post_reset_range_overlaps_current_resource_map(ptr, 1u)) {
        static unsigned protected_map_free_logs = 0;
        if (protected_map_free_logs < 8u) {
            ESP_LOGW(TAG,
                     "LC ignored free of current Resource Manager map body: ptr=0x%08" PRIx32
                     " top=0x%08" PRIx32 " sys=0x%08" PRIx32 " rom=0x%08" PRIx32,
                     ptr, lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL),
                     lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL),
                     lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL));
            protected_map_free_logs++;
        }
        return;
    }

    // The compact post-reset Memory Manager model now preserves a tiny reusable
    // free-list.  Resource Manager repeatedly grows and releases ROM-map bodies;
    // LIFO-only reclamation still leaked enough non-tail blocks to push later
    // maps into the direct-probe stack/ROM window.  This is not a full zone
    // allocator, but it models the reference Memory Manager's immediate reuse of
    // freed relocatable blocks well enough for ReDoMap's grow/copy/free pattern.
    for (uint32_t header_size = 8u; header_size <= 12u; header_size += 4u) {
        if (ptr < header_size) {
            continue;
        }
        const uint32_t header = ptr - header_size;
        if (header + 3u >= active_bus->ram_size) {
            continue;
        }
        const uint32_t raw_size = lc_musashi_bus_peek_ram32(header) & LC_MEMORY_BLOCK_SIZE_MASK;
        if (raw_size < header_size || raw_size > active_bus->ram_size - header) {
            continue;
        }
        const uint32_t end = header + raw_size;
        if (end == post_reset_heap_bump) {
            post_reset_heap_bump = header;
            lc_musashi_bus_post_reset_coalesce_heap_tail();
            return;
        }
        (void)lc_musashi_bus_post_reset_remember_free_block(header, raw_size, header_size);
        return;
    }
}

static uint32_t lc_musashi_bus_post_reset_alloc_block(uint32_t size,
                                                      uint32_t header_size,
                                                      bool clear) {
    if (active_bus == NULL || active_bus->ram == NULL || active_bus->ram_size == 0 ||
        post_reset_heap_bump >= active_bus->ram_size) {
        return 0;
    }
    const uint32_t aligned = (size + 3u) & ~3u;
    if (aligned == 0u || aligned < size) {
        return 0;
    }
    uint32_t total = aligned + header_size;
    if (total < aligned || total < header_size) {
        return 0;
    }
    if (header_size == 8u && total < 12u) {
        total = 12u;
    } else if (header_size == 12u && total < 16u) {
        total = 16u;
    }
    total = (total + 3u) & ~3u;

    for (size_t i = 0; i < sizeof(post_reset_free_blocks) / sizeof(post_reset_free_blocks[0]); i++) {
        lc_post_reset_free_block_t *free_block = &post_reset_free_blocks[i];
        if (free_block->total < total || free_block->header_size != header_size) {
            continue;
        }
        const uint32_t header = free_block->header;
        const uint32_t block_total = free_block->total;
        const uint32_t ptr = header + header_size;
        if (lc_musashi_bus_post_reset_range_overlaps_current_resource_map(header, block_total)) {
            static unsigned free_map_overlap_logs = 0;
            if (free_map_overlap_logs < 8u) {
                ESP_LOGW(TAG,
                         "LC skipped free-list allocation overlapping current Resource Manager map:"
                         " header=0x%08" PRIx32 " total=0x%08" PRIx32,
                         header, block_total);
                free_map_overlap_logs++;
            }
            continue;
        }
        if (lc_musashi_bus_post_reset_range_overlaps_active_stack(ptr, aligned)) {
            static unsigned free_stack_overlap_logs = 0;
            if (free_stack_overlap_logs < 8u) {
                ESP_LOGW(TAG,
                         "LC skipped free-list allocation overlapping active stack: ptr=0x%08" PRIx32
                         " size=0x%08" PRIx32 " sp=0x%08x",
                         ptr, aligned, m68k_get_reg(NULL, M68K_REG_SP));
                free_stack_overlap_logs++;
            }
            continue;
        }
        memset(free_block, 0, sizeof(*free_block));
        const uint32_t leftover = block_total - total;
        if (leftover >= header_size + 16u) {
            (void)lc_musashi_bus_post_reset_remember_free_block(header + total, leftover,
                                                                header_size);
        } else {
            total = block_total;
        }
        lc_musashi_bus_post_reset_write_alloc_header(header, total, header_size);
        if (clear) {
            for (uint32_t j = 0; j < aligned; j++) {
                lc_musashi_bus_ram_write8(ptr + j, 0);
            }
        }
        return ptr;
    }

    const uint32_t stack_floor = lc_musashi_bus_post_reset_heap_stack_floor();
    if (post_reset_heap_bump >= stack_floor || total > stack_floor - post_reset_heap_bump) {
        return 0;
    }

    uint32_t protected_end = lc_musashi_bus_post_reset_resource_map_protect_end_for_range(post_reset_heap_bump,
                                                                                           total);
    if (protected_end != 0u) {
        static unsigned bump_map_overlap_logs = 0;
        if (bump_map_overlap_logs < 8u) {
            ESP_LOGW(TAG,
                     "LC advanced heap bump past current Resource Manager map:"
                     " old_bump=0x%08" PRIx32 " new_bump=0x%08" PRIx32
                     " total=0x%08" PRIx32,
                     post_reset_heap_bump, protected_end, total);
            bump_map_overlap_logs++;
        }
        post_reset_heap_bump = (protected_end + 3u) & ~3u;
        if (post_reset_heap_bump >= stack_floor || total > stack_floor - post_reset_heap_bump) {
            return 0;
        }
    }

    const uint32_t header = post_reset_heap_bump;
    const uint32_t ptr = header + header_size;
    if (lc_musashi_bus_post_reset_range_overlaps_active_stack(ptr, aligned)) {
        return 0;
    }
    post_reset_heap_bump += total;

    lc_musashi_bus_post_reset_write_alloc_header(header, total, header_size);
    if (clear) {
        for (uint32_t i = 0; i < aligned; i++) {
            lc_musashi_bus_ram_write8(ptr + i, 0);
        }
    }
    return ptr;
}

static uint32_t lc_musashi_bus_post_reset_alloc(uint32_t size, bool clear) {
    return lc_musashi_bus_post_reset_alloc_block(size, 8u, clear);
}

static uint32_t lc_musashi_bus_post_reset_alloc_handle_data(uint32_t size, bool clear) {
    return lc_musashi_bus_post_reset_alloc_block(size, 12u, clear);
}

static uint32_t lc_musashi_bus_post_reset_resize_last_handle_data(uint32_t ptr,
                                                                  uint32_t size) {
    const uint32_t header_size = 12u;
    if (active_bus == NULL || active_bus->ram == NULL || ptr < header_size ||
        ptr >= active_bus->ram_size) {
        return 0;
    }
    const uint32_t header = ptr - header_size;
    const uint32_t old_total = lc_musashi_bus_peek_ram32(header) & LC_MEMORY_BLOCK_SIZE_MASK;
    if (old_total < header_size || header + old_total != post_reset_heap_bump) {
        return 0;
    }
    const uint32_t aligned = (size + 3u) & ~3u;
    if (aligned == 0u || aligned < size) {
        return 0;
    }
    uint32_t total = aligned + header_size;
    if (total < aligned || total < header_size) {
        return 0;
    }
    if (total < 16u) {
        total = 16u;
    }
    total = (total + 3u) & ~3u;
    const uint32_t stack_floor = lc_musashi_bus_post_reset_heap_stack_floor();
    if (header >= stack_floor || total > stack_floor - header) {
        return 0;
    }
    if (total > old_total) {
        for (uint32_t i = old_total; i < total; i++) {
            lc_musashi_bus_ram_write8(header + i, 0);
        }
    }
    lc_musashi_bus_ram_write32(header + 0u, 0x40000000u | (total & LC_MEMORY_BLOCK_SIZE_MASK));
    lc_musashi_bus_ram_write32(header + 4u, total & LC_MEMORY_BLOCK_SIZE_MASK);
    lc_musashi_bus_ram_write32(header + 8u, lc_musashi_bus_peek_ram32(LC_LOWMEM_THE_ZONE));
    post_reset_heap_bump = header + total;
    return ptr;
}

static size_t lc_musashi_bus_post_reset_handle_record_slot(uint32_t handle, bool create) {
    if (handle == 0u) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < sizeof(post_reset_handle_size_handle) / sizeof(post_reset_handle_size_handle[0]); i++) {
        if (post_reset_handle_size_handle[i] == handle) {
            return i;
        }
    }
    if (!create) {
        return SIZE_MAX;
    }
    for (size_t i = 0; i < sizeof(post_reset_handle_size_handle) / sizeof(post_reset_handle_size_handle[0]); i++) {
        if (post_reset_handle_size_handle[i] == 0u) {
            post_reset_handle_size_handle[i] = handle;
            return i;
        }
    }
    return SIZE_MAX;
}

static void lc_musashi_bus_post_reset_set_handle_record(uint32_t handle, uint32_t data_ptr,
                                                        uint32_t size) {
    const size_t slot = lc_musashi_bus_post_reset_handle_record_slot(handle, true);
    if (slot == SIZE_MAX) {
        return;
    }
    post_reset_handle_data_ptr[slot] = data_ptr;
    post_reset_handle_size_value[slot] = size;
}

static uint32_t lc_musashi_bus_post_reset_get_handle_size_record(uint32_t handle) {
    const size_t slot = lc_musashi_bus_post_reset_handle_record_slot(handle, false);
    return slot != SIZE_MAX ? post_reset_handle_size_value[slot] : 0;
}

static uint32_t lc_musashi_bus_post_reset_get_handle_ptr_record(uint32_t handle) {
    const size_t slot = lc_musashi_bus_post_reset_handle_record_slot(handle, false);
    return slot != SIZE_MAX ? post_reset_handle_data_ptr[slot] : 0;
}

static bool lc_musashi_bus_post_reset_handle_is_locked(uint32_t handle) {
    const size_t slot = lc_musashi_bus_post_reset_handle_record_slot(handle, false);
    return slot != SIZE_MAX && post_reset_handle_locked[slot];
}

static bool lc_musashi_bus_post_reset_is_resource_map_handle(uint32_t handle) {
    if (handle == 0u || active_bus == NULL || active_bus->ram == NULL ||
        handle + 3u >= active_bus->ram_size) {
        return false;
    }
    return handle == post_reset_rom_map_handle_dynamic ||
           handle == lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL) ||
           handle == lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL) ||
           handle == lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL);
}

static void lc_musashi_bus_post_reset_note_rom_map_handle(uint32_t handle) {
    if (handle < LC_POST_RESET_MASTER_PTR_BASE || handle >= LC_POST_RESET_MASTER_PTR_LIMIT) {
        return;
    }
    if (post_reset_rom_map_handle_dynamic == 0u) {
        post_reset_rom_map_handle_dynamic = handle;
    }
}

static void lc_musashi_bus_post_reset_repair_rom_map_handle_identity(uint32_t pc) {
    if (post_reset_rom_map_handle_dynamic == 0u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (!((rom_offset >= 0x0001ab00u && rom_offset < 0x0001c100u) ||
          (rom_offset >= 0x000099b0u && rom_offset < 0x00009a40u))) {
        return;
    }
    const uint32_t handle = post_reset_rom_map_handle_dynamic;
    const uint32_t ptr = lc_musashi_bus_post_reset_get_handle_ptr_record(handle);
    const uint32_t old_top = lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL);
    const uint32_t old_sys = lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL);
    const uint32_t old_rom = lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL);
    bool repaired = false;
    if (old_top != handle) {
        lc_musashi_bus_ram_write32(LC_LOWMEM_TOP_MAP_HNDL, handle);
        repaired = true;
    }
    if (old_sys != handle) {
        lc_musashi_bus_ram_write32(LC_LOWMEM_SYS_MAP_HNDL, handle);
        repaired = true;
    }
    if (old_rom != handle) {
        lc_musashi_bus_ram_write32(LC_LOWMEM_ROM_MAP_HNDL, handle);
        repaired = true;
    }
    if (ptr != 0u && handle + 3u < active_bus->ram_size &&
        lc_musashi_bus_peek_ram32(handle) != ptr) {
        lc_musashi_bus_ram_write32(handle, ptr);
        repaired = true;
    }
    if (repaired && !post_reset_rom_map_handle_repair_logged) {
        post_reset_rom_map_handle_repair_logged = true;
        ESP_LOGW(TAG,
                 "LC repaired ROM map handle identity: pc=0x%08" PRIx32
                 " handle=0x%08" PRIx32 " ptr=0x%08" PRIx32
                 " old_top=0x%08" PRIx32 " old_sys=0x%08" PRIx32 " old_rom=0x%08" PRIx32,
                 pc, handle, ptr, old_top, old_sys, old_rom);
    }
}

static uint32_t lc_musashi_bus_post_reset_payload_size_from_header(uint32_t ptr,
                                                                   uint32_t header_size) {
    if (ptr < header_size || active_bus == NULL || active_bus->ram == NULL ||
        ptr > active_bus->ram_size) {
        return 0;
    }
    const uint32_t header = ptr - header_size;
    if (header + 3u >= active_bus->ram_size) {
        return 0;
    }
    const uint32_t raw_size = lc_musashi_bus_peek_ram32(header) & LC_MEMORY_BLOCK_SIZE_MASK;
    if (raw_size < header_size || raw_size > active_bus->ram_size - header) {
        return 0;
    }
    return raw_size - header_size;
}

static uint32_t lc_musashi_bus_post_reset_infer_handle_size(uint32_t handle) {
    uint32_t size = lc_musashi_bus_post_reset_get_handle_size_record(handle);
    if (size != 0u) {
        return size;
    }
    if (handle == 0x00008000u) {
        // The transitional Resource Manager map handle is seeded directly in
        // RAM before the Memory Manager model sees a trap for it.
        return 0x00000320u;
    }
    uint32_t ptr = lc_musashi_bus_post_reset_get_handle_ptr_record(handle);
    if (ptr == 0u && active_bus != NULL && active_bus->ram != NULL &&
        handle + 3u < active_bus->ram_size) {
        ptr = lc_musashi_bus_peek_ram32(handle);
    }
    return lc_musashi_bus_post_reset_payload_size_from_header(ptr, 12u);
}

static void lc_musashi_bus_maybe_skip_repeated_post_reset_slot_init_scan(uint32_t pc) {
    if (lc_musashi_bus_basilisk_slot_rom_active()) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000060a0u) {
        return;
    }
    post_reset_slot_init_scan_calls++;
    if (post_reset_slot_init_scan_calls < 256u) {
        return;
    }
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_PC, 0x408060eau);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC skipped repeated post-reset Slot Manager init scan: pc=0x%08" PRIx32
                 " calls=%u target=0x408060ea",
                 pc, post_reset_slot_init_scan_calls);
    }
}

static void lc_musashi_bus_maybe_cap_post_reset_slot_first_scan_loop(uint32_t pc) {
    if (lc_musashi_bus_basilisk_slot_rom_active()) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000060e4u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    post_reset_slot_first_scan_loop_hits++;
    if (post_reset_slot_first_scan_loop_hits < 256u) {
        return;
    }
    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    if (a0 + 54u < active_bus->ram_size) {
        active_bus->ram[a0 + 49u] = 15u;
    }
    m68k_set_reg(M68K_REG_PC, 0x408060e6u);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC capped repeated post-reset Slot Manager first scan loop: pc=0x%08" PRIx32
                 " hits=%u target=0x408060e6 a0=0x%08" PRIx32,
                 pc, post_reset_slot_first_scan_loop_hits, a0);
    }
}

static void lc_musashi_bus_maybe_cap_post_reset_slot_scan_loop(uint32_t pc) {
    if (lc_musashi_bus_basilisk_slot_rom_active()) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000061bcu) {
        return;
    }
    post_reset_slot_scan_loop_hits++;
    if (post_reset_slot_scan_loop_hits < 256u) {
        return;
    }
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_PC, 0x40806240u);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC capped repeated post-reset Slot Manager scan loop: pc=0x%08" PRIx32
                 " hits=%u target=0x40806240",
                 pc, post_reset_slot_scan_loop_hits);
    }
}

static void lc_musashi_bus_maybe_lift_low_resource_stack(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0001b714u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & ~1u;
    if (sp >= 0x00030000u) {
        return;
    }
    const uint32_t new_sp = 0x00070000u | (sp & 0x00000fffu);
    const uint32_t before = 0x400u;
    const uint32_t after = 0x1000u;
    if (sp < before || new_sp < before || sp + after >= active_bus->ram_size ||
        new_sp + after >= active_bus->ram_size) {
        return;
    }
    for (uint32_t i = 0; i < before + after; i++) {
        active_bus->ram[new_sp - before + i] = active_bus->ram[sp - before + i];
    }
    m68k_set_reg(M68K_REG_SP, new_sp);
    static unsigned lifted_logs = 0;
    if (lifted_logs < 8u) {
        ESP_LOGW(TAG,
                 "LC lifted low ResourceMgr stack away from map body: pc=0x%08" PRIx32
                 " old_sp=0x%08" PRIx32 " new_sp=0x%08" PRIx32,
                 pc, sp, new_sp);
        lifted_logs++;
    }
}

static void lc_musashi_bus_post_reset_maybe_register_resource_map_handle(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_resource_map_handle_registered || rom_offset < 0x0001b700u ||
        rom_offset >= 0x0001c100u || active_bus == NULL || active_bus->ram == NULL ||
        active_bus->ram_size <= 0x00008340u) {
        return;
    }
    const uint32_t top_map_handle = lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL);
    const uint32_t sys_map_handle = lc_musashi_bus_peek_ram32(LC_LOWMEM_SYS_MAP_HNDL);
    const uint32_t map_ptr = lc_musashi_bus_peek_ram32(top_map_handle);
    if (top_map_handle != 0x00008000u || sys_map_handle != top_map_handle ||
        map_ptr != 0x00008020u) {
        return;
    }
    lc_musashi_bus_post_reset_set_handle_record(top_map_handle, map_ptr, 0x00000320u);
    for (uint32_t addr = LC_RESOURCE_ROM_MASTER_PTR_BASE;
         addr < LC_RESOURCE_ROM_MASTER_PTR_LIMIT; addr += 4u) {
        lc_musashi_bus_ram_write32(addr, 0);
    }
    post_reset_resource_map_handle_registered = true;
    ESP_LOGI(TAG,
             "LC registered RAM-backed Resource Manager map handle with Memory Manager: handle=0x%08" PRIx32
             " data=0x%08" PRIx32 " size=0x00000320 rom_master_ptrs=0x%08" PRIx32 "..0x%08" PRIx32,
             top_map_handle, map_ptr, LC_RESOURCE_ROM_MASTER_PTR_BASE,
             LC_RESOURCE_ROM_MASTER_PTR_LIMIT);
}

static void lc_musashi_bus_post_reset_set_handle_locked(uint32_t handle, bool locked) {
    const size_t slot = lc_musashi_bus_post_reset_handle_record_slot(handle, true);
    if (slot != SIZE_MAX) {
        post_reset_handle_locked[slot] = locked;
    }
}

static uint32_t lc_musashi_bus_post_reset_alloc_handle(uint32_t data_ptr, uint32_t size) {
    if (active_bus == NULL || active_bus->ram == NULL ||
        post_reset_handle_bump < LC_POST_RESET_MASTER_PTR_BASE ||
        post_reset_handle_bump + 4u > LC_POST_RESET_MASTER_PTR_LIMIT ||
        post_reset_handle_bump + 4u >= active_bus->ram_size) {
        return 0;
    }
    const uint32_t handle = post_reset_handle_bump;
    post_reset_handle_bump += 4u;
    lc_musashi_bus_ram_write32(handle, data_ptr);
    lc_musashi_bus_post_reset_set_handle_record(handle, data_ptr, size);
    return handle;
}

static bool lc_musashi_bus_post_reset_initzone_rom_table(uint32_t a0) {
    uint32_t rom_offset = 0;
    return lc_musashi_bus_rom_offset_for_address(a0, &rom_offset) &&
           rom_offset >= 0x00000500u && rom_offset < 0x00000530u;
}

static uint32_t lc_musashi_bus_post_reset_initzone_start(uint32_t a0,
                                                         uint32_t a1,
                                                         bool *is_main_zone,
                                                         bool *is_ram_param) {
    if (is_main_zone != NULL) {
        *is_main_zone = false;
    }
    if (is_ram_param != NULL) {
        *is_ram_param = false;
    }
    if (lc_musashi_bus_post_reset_initzone_rom_table(a0)) {
        // The LC reset path calls InitZone with A0 pointing at a ROM table near
        // 0x4080050a/0x40800518.  Its second word can contain the desired low-RAM
        // zone start on adjacent reset paths; this LC path falls back to 0x3800.
        if (is_main_zone != NULL) {
            *is_main_zone = true;
        }
        const uint32_t table_start = (uint32_t)lc_musashi_bus_peek_rom16(a0 + 2u) & ~1u;
        if (table_start >= LC_MEMORY_ZONE_MIN_START && active_bus != NULL &&
            table_start + LC_MEMORY_ZONE_HEADER_SIZE < active_bus->ram_size) {
            return table_start;
        }
        return LC_MEMORY_ZONE_DEFAULT_START;
    }
    if (active_bus != NULL && active_bus->ram != NULL && a0 + 8u < active_bus->ram_size) {
        const uint32_t param_limit = lc_musashi_bus_peek_ram32(a0 + 4u) & ~1u;
        const uint32_t param_start = a1 & ~1u;
        if (param_start >= LC_MEMORY_ZONE_MIN_START &&
            param_start + LC_MEMORY_ZONE_HEADER_SIZE < param_limit &&
            param_limit <= active_bus->ram_size) {
            // Resource Manager builds short-lived zones from RAM parameter blocks
            // at 0x4081aafc.  Seed their heap headers, but do not rewrite global
            // low-memory zone/heap limits the way the reset-table InitZone does.
            if (is_ram_param != NULL) {
                *is_ram_param = true;
            }
            return param_start;
        }
    }
    return 0;
}

static uint32_t lc_musashi_bus_post_reset_initzone_limit(uint32_t a0,
                                                         uint32_t a1,
                                                         uint32_t zone_start,
                                                         bool is_ram_param) {
    if (active_bus == NULL || active_bus->ram == NULL || active_bus->ram_size == 0) {
        return 0;
    }
    if (is_ram_param && a0 + 8u < active_bus->ram_size) {
        const uint32_t param_limit = lc_musashi_bus_peek_ram32(a0 + 4u) & ~1u;
        if (param_limit > zone_start + LC_MEMORY_ZONE_HEADER_SIZE &&
            param_limit <= active_bus->ram_size) {
            return param_limit;
        }
    }
    if (a1 > zone_start + LC_MEMORY_ZONE_HEADER_SIZE && a1 <= active_bus->ram_size) {
        return a1 & ~1u;
    }
    return ((uint32_t)active_bus->ram_size) & ~1u;
}

static uint32_t lc_musashi_bus_post_reset_initzone_grow_proc(uint32_t a0,
                                                             uint32_t a2) {
    if (lc_musashi_bus_post_reset_initzone_rom_table(a0)) {
        return 0;
    }
    return a2;
}

static void lc_musashi_bus_post_reset_seed_low_trap(uint8_t trap_low, uint32_t handler) {
    lc_musashi_bus_ram_write32(LC_LOWMEM_LOW_TRAP_TABLE + ((uint32_t)(trap_low & 0x7fu) * 4u),
                               handler);
}

static bool lc_musashi_bus_post_reset_plausible_rom_pc(uint32_t value);

static void lc_musashi_bus_maybe_restore_post_reset_device_bases(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (active_bus == NULL || active_bus->ram == NULL || rom_offset < 0x0000b080u ||
        rom_offset > 0x0000b138u) {
        return;
    }

    const uint32_t old_via = lc_musashi_bus_peek_ram32(0x000001d4u);
    const uint32_t old_scc_read = lc_musashi_bus_peek_ram32(0x000001d8u);
    const uint32_t old_scc_write = lc_musashi_bus_peek_ram32(0x000001dcu);
    const uint32_t old_iwm = lc_musashi_bus_peek_ram32(0x000001e0u);
    if (old_via == 0x00f00000u && old_scc_read == 0x00f04000u &&
        old_scc_write == 0x00f04000u && old_iwm == 0x00f16000u) {
        return;
    }

    lc_musashi_bus_ram_write32(0x000001d4u, 0x00f00000u); // VIA base.
    lc_musashi_bus_ram_write32(0x000001d8u, 0x00f04000u); // SCC read base.
    lc_musashi_bus_ram_write32(0x000001dcu, 0x00f04000u); // SCC write base.
    lc_musashi_bus_ram_write32(0x000001e0u, 0x00f16000u); // IWM/SWIM base.
    if (!post_reset_device_base_restore_logged) {
        post_reset_device_base_restore_logged = true;
        ESP_LOGW(TAG,
                 "LC restored post-reset low-memory device bases before VIA/SCC routine: pc=0x%08" PRIx32
                 " old_via=0x%08" PRIx32 " old_scc_read=0x%08" PRIx32
                 " old_scc_write=0x%08" PRIx32 " old_iwm=0x%08" PRIx32,
                 pc, old_via, old_scc_read, old_scc_write, old_iwm);
    }
}

static uint32_t lc_musashi_bus_post_reset_slot_record(uint8_t slot) {
    const uint32_t record = LC_SLOT_RECORD_BASE + ((uint32_t)(slot & 0x0fu) * LC_SLOT_RECORD_STRIDE);
    if (active_bus == NULL || active_bus->ram == NULL ||
        record + LC_SLOT_RECORD_STRIDE > active_bus->ram_size) {
        return 0;
    }
    for (uint32_t i = 0; i < LC_SLOT_RECORD_STRIDE; i++) {
        lc_musashi_bus_ram_write8(record + i, 0);
    }
    // The LC/V8 reference machine has no declaration-ROM-backed NuBus card in
    // these internal slots; MAME only exposes an optional LC PDS slot IRQ line.
    // The reset ROM's slot scan treats a negative word at +4 in the record
    // returned through the SPBlock result field as "no usable sResource" and
    // skips the deeper declaration-ROM probe for that slot.
    lc_musashi_bus_ram_write16(record + LC_SLOT_RECORD_STATUS, 0xffffu);
    return record;
}

static void lc_musashi_bus_log_memory_trap(uint32_t pc,
                                           uint16_t trap_word,
                                           const char *action,
                                           uint32_t size,
                                           uint32_t ptr,
                                           uint32_t handle);
static void lc_musashi_bus_seed_post_reset_srt_table(uint32_t pc, const char *reason);

static void lc_musashi_bus_seed_post_reset_slot_spblock(uint32_t sp_block) {
    if (active_bus == NULL || active_bus->ram == NULL || sp_block + 0x19u >= active_bus->ram_size) {
        return;
    }
    lc_musashi_bus_ram_write32(sp_block + 0x00u, 0x00010000u);
    lc_musashi_bus_ram_write32(sp_block + 0x04u, 0x00000001u);
    lc_musashi_bus_ram_write32(sp_block + 0x08u, 0x00000001u);
    lc_musashi_bus_ram_write8(sp_block + 0x0cu, 0x00u);
    lc_musashi_bus_ram_write8(sp_block + 0x0du, 0x01u);
    lc_musashi_bus_ram_write32(sp_block + 0x0eu, 0x5a932bc7u);
    lc_musashi_bus_ram_write8(sp_block + 0x12u, 0x00u);
}

static void lc_musashi_bus_maybe_skip_post_reset_slot_srt_builder(uint32_t pc) {
    if (lc_musashi_bus_basilisk_slot_rom_active()) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00005e20u) {
        return;
    }
    lc_musashi_bus_seed_post_reset_srt_table(pc, "skip-srt-builder");
    m68k_set_reg(M68K_REG_D0, 0);
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t top = (active_bus != NULL && active_bus->ram != NULL && sp + 3u < active_bus->ram_size)
                             ? lc_musashi_bus_peek_ram32(sp)
                             : 0u;
    const bool direct_continue = sp < 0x00100000u &&
                                 !lc_musashi_bus_post_reset_plausible_rom_pc(top);
    m68k_set_reg(M68K_REG_PC, direct_continue ? 0x40805deeu : 0x40805e46u);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC skipped post-reset Slot Manager SRT builder: pc=0x%08" PRIx32
                 " new_pc=0x%08" PRIx32 " sp=0x%08" PRIx32 " top=0x%08" PRIx32,
                 pc, direct_continue ? 0x40805deeu : 0x40805e46u, sp, top);
    }
}

static void lc_musashi_bus_maybe_skip_post_reset_slot_dispatch_rebuild(uint32_t pc) {
    if (lc_musashi_bus_basilisk_slot_rom_active()) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00006d60u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t low_db8 = lc_musashi_bus_peek_ram32(0x00000db8u);
    if (low_db8 != 0x00009000u && !post_reset_probe_tables_seeded) {
        return;
    }
    lc_musashi_bus_ram_write32(0x00000db8u, 0x00009000u);
    lc_musashi_bus_ram_write32(0x000090e8u, 0x40800d88u);
    lc_musashi_bus_ram_write32(0x000090ecu, 0x40800d88u);
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_PC, 0x40806d96u);
    if (!post_reset_slot_dispatch_rebuild_skip_logged) {
        post_reset_slot_dispatch_rebuild_skip_logged = true;
        ESP_LOGW(TAG,
                 "LC skipped repeated post-reset Slot Manager dispatch-table rebuild: pc=0x%08" PRIx32
                 " old_0db8=0x%08" PRIx32 " new_pc=0x40806d96",
                 pc, low_db8);
    }
}

static void lc_musashi_bus_maybe_log_post_reset_srt_entry(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_srt_entry_logged || rom_offset != 0x00005f2au ||
        active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    post_reset_srt_entry_logged = true;
    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    const uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
    const uint32_t a2 = m68k_get_reg(NULL, M68K_REG_A2);
    ESP_LOGW(TAG,
             "LC entered post-reset Slot Manager SRT verifier: pc=0x%08" PRIx32
             " a0=0x%08" PRIx32 " a1=0x%08" PRIx32 " a2=0x%08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x sp=0x%08x"
             " a0_rec=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " a1_rec=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
             pc, a0, a1, a2, m68k_get_reg(NULL, M68K_REG_D0),
             m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
             m68k_get_reg(NULL, M68K_REG_SP),
             a0 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a0 + 0u) : 0xffffffffu,
             a0 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a0 + 4u) : 0xffffffffu,
             a0 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a0 + 8u) : 0xffffffffu,
             a0 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a0 + 12u) : 0xffffffffu,
             a1 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a1 + 0u) : 0xffffffffu,
             a1 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a1 + 4u) : 0xffffffffu,
             a1 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a1 + 8u) : 0xffffffffu,
             a1 + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(a1 + 12u) : 0xffffffffu);
}

static void lc_musashi_bus_seed_post_reset_srt_table(uint32_t pc, const char *reason) {
    if (active_bus == NULL || active_bus->ram == NULL ||
        LC_POST_RESET_SRT_BASE + ((LC_POST_RESET_SRT_RECORD_COUNT + 1u) *
                                  LC_POST_RESET_SRT_RECORD_STRIDE) + 4u >= active_bus->ram_size) {
        return;
    }
    const uint32_t bytes = (LC_POST_RESET_SRT_RECORD_COUNT + 1u) * LC_POST_RESET_SRT_RECORD_STRIDE + 4u;
    for (uint32_t i = 0; i < bytes; i++) {
        lc_musashi_bus_ram_write8(LC_POST_RESET_SRT_BASE + i, 0);
    }
    for (uint32_t i = 0; i < LC_POST_RESET_SRT_RECORD_COUNT; i++) {
        lc_musashi_bus_ram_write16(LC_POST_RESET_SRT_BASE + i * LC_POST_RESET_SRT_RECORD_STRIDE,
                                   0xff01u);
    }
    const uint32_t terminator = LC_POST_RESET_SRT_BASE +
                                LC_POST_RESET_SRT_RECORD_COUNT * LC_POST_RESET_SRT_RECORD_STRIDE;
    lc_musashi_bus_ram_write16(terminator, 0xffffu);
    lc_musashi_bus_ram_write32(terminator + 2u, 0x00000000u);
    lc_musashi_bus_ram_write32(0x00000d24u, LC_POST_RESET_SRT_BASE);
    // $0CBC is used by adjacent Slot Manager helper paths as a scratch/table
    // anchor.  Point it at the same RAM-owned synthetic SRT so later paths do
    // not consume the RAM-fill pattern as a pointer.
    lc_musashi_bus_ram_write32(0x00000cbcu, LC_POST_RESET_SRT_BASE);
    if (!post_reset_srt_table_seed_logged) {
        post_reset_srt_table_seed_logged = true;
        ESP_LOGW(TAG,
                 "LC seeded synthetic post-reset Slot Manager SRT table: pc=0x%08" PRIx32
                 " reason=%s base=0x%08x terminator=0x%08" PRIx32,
                 pc, reason != NULL ? reason : "unknown", LC_POST_RESET_SRT_BASE, terminator);
    }
}

static void lc_musashi_bus_maybe_seed_post_reset_srt_register(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000060b2u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
    const bool a1_plausible = a1 + 0x17u < active_bus->ram_size &&
                              a1 < 0x001f0000u;
    if (a1_plausible && lc_musashi_bus_peek_ram16(a1) != 0xb6dbu) {
        return;
    }
    lc_musashi_bus_seed_post_reset_srt_table(pc, "slot-table-callback-missing");
    m68k_set_reg(M68K_REG_A1, LC_POST_RESET_SRT_BASE);
}

static void lc_musashi_bus_maybe_guard_post_reset_srt_io_fill(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset < 0x0000696eu || rom_offset > 0x0000697au) {
        return;
    }
    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    const uint32_t a0_24 = a0 & 0x00ffffffu;
    if (a0_24 < 0x00f00000u || a0_24 >= 0x01000000u) {
        return;
    }
    // The direct host path can let the SRT allocator's initialized-record loop
    // run with A0 in the LC I/O aperture, producing the all-white VRAM clear
    // rather than a valid RAM table.  Treat this as an allocation failure and
    // return through the allocator epilogue instead of filling I/O space.
    m68k_set_reg(M68K_REG_D0, 0xffffff05u);
    m68k_set_reg(M68K_REG_PC, 0x40806980u);
    if (!post_reset_srt_io_fill_guard_logged) {
        post_reset_srt_io_fill_guard_logged = true;
        ESP_LOGW(TAG,
                 "LC guarded bogus Slot Manager SRT I/O fill: pc=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " a0_24=0x%08" PRIx32
                 " new_pc=0x40806980 d0=0xffffff05",
                 pc, a0, a0_24);
    }
}

static void lc_musashi_bus_maybe_log_post_reset_srt_alloc_entry(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_srt_alloc_entry_logs >= 12u ||
        (rom_offset != 0x00006958u && rom_offset != 0x0000695au &&
         rom_offset != 0x0000695cu && rom_offset != 0x00006964u &&
         rom_offset != 0x00009a20u && rom_offset != 0x00009a22u) ||
        active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    if ((rom_offset == 0x00009a20u || rom_offset == 0x00009a22u) &&
        (m68k_get_reg(NULL, M68K_REG_A2) != 0x40806966u ||
         m68k_get_reg(NULL, M68K_REG_D0) != 0x000000c6u)) {
        return;
    }
    post_reset_srt_alloc_entry_logs++;
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    ESP_LOGW(TAG,
             "LC Slot Manager SRT allocator/dispatch entry trace: pc=0x%08" PRIx32
             " sp=0x%08" PRIx32
             " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " a0=0x%08x a1=0x%08x a2=0x%08x d0=0x%08x d1=0x%08x d2=0x%08x",
             pc, sp,
             sp + 3u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 0u) : 0xffffffffu,
             sp + 7u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 4u) : 0xffffffffu,
             sp + 11u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 8u) : 0xffffffffu,
             sp + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 12u) : 0xffffffffu,
             m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
             m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_D0),
             m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2));
}

static void lc_musashi_bus_maybe_escape_post_reset_srt_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00006982u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 23u >= active_bus->ram_size) {
        return;
    }
    const uint32_t ret0 = lc_musashi_bus_peek_ram32(sp + 0u);
    const uint32_t ret4 = lc_musashi_bus_peek_ram32(sp + 4u);
    const uint32_t ret8 = lc_musashi_bus_peek_ram32(sp + 8u);
    const uint32_t saved_a1 = lc_musashi_bus_peek_ram32(sp + 12u);
    const uint32_t saved_a2 = lc_musashi_bus_peek_ram32(sp + 16u);
    const uint32_t outer = lc_musashi_bus_peek_ram32(sp + 20u);
    if (ret0 != 0x40806966u || ret4 != 0x40806d7cu || ret8 != 0x408060b2u ||
        outer != 0x40805df2u) {
        return;
    }
    m68k_set_reg(M68K_REG_A1, saved_a1);
    m68k_set_reg(M68K_REG_A2, saved_a2);
    m68k_set_reg(M68K_REG_D0, 0);
    const uint32_t continuation = outer == 0x40805df2u ? 0x40805df6u : outer;
    m68k_set_reg(M68K_REG_SP, sp + 24u);
    m68k_set_reg(M68K_REG_PC, continuation);
    if (!post_reset_srt_escape_logged) {
        post_reset_srt_escape_logged = true;
        ESP_LOGW(TAG,
                 "LC escaped recursive Slot Manager SRT allocator loop to outer continuation: pc=0x%08" PRIx32
                 " old_sp=0x%08" PRIx32 " new_sp=0x%08" PRIx32
                 " saved_a1=0x%08" PRIx32 " saved_a2=0x%08" PRIx32
                 " outer=0x%08" PRIx32 " continuation=0x%08" PRIx32,
                 pc, sp, sp + 24u, saved_a1, saved_a2, outer, continuation);
    }
}

static void lc_musashi_bus_maybe_escape_post_reset_slot_first_pass_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_slot_first_pass_escape_logged || rom_offset != 0x000060deu ||
        active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    if (sp >= 0x00010000u || a0 + 0x31u >= active_bus->ram_size) {
        return;
    }
    const uint8_t old_index = active_bus->ram[a0 + 0x31u];
    if (old_index > 14u || sp + 7u >= active_bus->ram_size) {
        return;
    }
    const uint32_t saved_a1 = lc_musashi_bus_peek_ram32(sp + 0u);
    const uint32_t saved_a2 = lc_musashi_bus_peek_ram32(sp + 4u);
    active_bus->ram[a0 + 0x31u] = 15u;
    m68k_set_reg(M68K_REG_A1, saved_a1);
    m68k_set_reg(M68K_REG_A2, saved_a2);
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_SP, sp + 8u);
    m68k_set_reg(M68K_REG_PC, 0x40805df6u);
    if (!post_reset_slot_first_pass_escape_logged) {
        post_reset_slot_first_pass_escape_logged = true;
        ESP_LOGW(TAG,
                 "LC escaped low-stack Slot Manager first-pass loop to continuation: pc=0x%08" PRIx32
                 " old_sp=0x%08" PRIx32 " new_sp=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " old_index=%u saved_a1=0x%08" PRIx32
                 " saved_a2=0x%08" PRIx32 " target=0x40805df6",
                 pc, sp, sp + 8u, a0, (unsigned)old_index, saved_a1, saved_a2);
    }
}

static void lc_musashi_bus_maybe_seed_basilisk_slot_sresource_result(uint32_t pc) {
    if (!lc_musashi_bus_basilisk_slot_rom_active() || active_bus == NULL || active_bus->rom == NULL) {
        return;
    }
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00005f3au && rom_offset != 0x00005fc6u) {
        return;
    }
    const uint32_t slot_header = LC_BASILISK_ROM_BASE_32 + (uint32_t)active_bus->rom_size - 20u;
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_A2, slot_header);
    const uint32_t sp_block = m68k_get_reg(NULL, M68K_REG_A0);
    if (active_bus->ram != NULL && sp_block + 3u < active_bus->ram_size) {
        lc_musashi_bus_ram_write32(sp_block, slot_header);
    }
    static unsigned logged = 0;
    if (logged < 8u) {
        ESP_LOGW(TAG,
                 "LC supplied Basilisk Slot ROM sResource result: pc=0x%08" PRIx32
                 " slot_header=0x%08" PRIx32 " spblock=0x%08x test=0x%08" PRIx32,
                 pc, slot_header, sp_block, lc_musashi_bus_peek_guest32(slot_header + 0x0eu));
        logged++;
    }
}

static void lc_musashi_bus_maybe_log_post_reset_srt_alloc_rts(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_srt_alloc_rts_logs >= 8u || rom_offset != 0x00006982u ||
        active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    post_reset_srt_alloc_rts_logs++;
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    ESP_LOGW(TAG,
             "LC Slot Manager SRT allocator RTS trace: pc=0x%08" PRIx32
             " sp=0x%08" PRIx32
             " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " %08" PRIx32 " %08" PRIx32
             " a0=0x%08x a1=0x%08x d0=0x%08x d1=0x%08x",
             pc, sp,
             sp + 3u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 0u) : 0xffffffffu,
             sp + 7u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 4u) : 0xffffffffu,
             sp + 11u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 8u) : 0xffffffffu,
             sp + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 12u) : 0xffffffffu,
             sp + 19u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 16u) : 0xffffffffu,
             sp + 23u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 20u) : 0xffffffffu,
             m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
             m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1));
}

static void lc_musashi_bus_maybe_log_post_reset_slot_srt_scan(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_srt_scan_logs >= 24u || active_bus == NULL || active_bus->ram == NULL ||
        (rom_offset != 0x000066a0u && rom_offset != 0x000066b2u &&
         rom_offset != 0x00006984u && rom_offset != 0x00006988u &&
         rom_offset != 0x0000699eu && rom_offset != 0x000069b0u)) {
        return;
    }
    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    const uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
    const uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    const uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    const uint32_t d3 = m68k_get_reg(NULL, M68K_REG_D3);
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const bool a0_in_ram = a0 + 0x37u < active_bus->ram_size;
    const bool a1_in_ram = a1 + 0x17u < active_bus->ram_size;
    ESP_LOGW(TAG,
             "LC Slot Manager SRT scan trace: pc=0x%08" PRIx32
             " a0=0x%08" PRIx32 " a1=0x%08" PRIx32
             " d1=0x%08" PRIx32 " d2=0x%08" PRIx32 " d3=0x%08" PRIx32
             " sp=0x%08" PRIx32 " low_d24=0x%08" PRIx32
             " low_cbc=0x%08" PRIx32 " low_a06=0x%08" PRIx32
             " low_db8=0x%08" PRIx32 " ret0=0x%08" PRIx32
             " ret12=0x%08" PRIx32 " ret16=0x%08" PRIx32
             " a0_flags=0x%02x a0_key=0x%08" PRIx32
             " a1_rec=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
             pc, a0, a1, d1, d2, d3, sp,
             lc_musashi_bus_peek_ram32(0x00000d24u),
             lc_musashi_bus_peek_ram32(0x00000cbcu),
             lc_musashi_bus_peek_ram32(0x00000a06u),
             lc_musashi_bus_peek_ram32(0x00000db8u),
             sp + 3u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp) : 0xffffffffu,
             sp + 15u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 12u) : 0xffffffffu,
             sp + 19u < active_bus->ram_size ? lc_musashi_bus_peek_ram32(sp + 16u) : 0xffffffffu,
             a0_in_ram ? active_bus->ram[a0 + 0x1bu] : 0xffu,
             a0_in_ram ? lc_musashi_bus_peek_ram32(a0 + 0x31u) : 0xffffffffu,
             a1_in_ram ? lc_musashi_bus_peek_ram32(a1 + 0u) : 0xffffffffu,
             a1_in_ram ? lc_musashi_bus_peek_ram32(a1 + 4u) : 0xffffffffu,
             a1_in_ram ? lc_musashi_bus_peek_ram32(a1 + 8u) : 0xffffffffu,
             a1_in_ram ? lc_musashi_bus_peek_ram32(a1 + 12u) : 0xffffffffu);
    post_reset_srt_scan_logs++;
}

static void lc_musashi_bus_maybe_handle_post_reset_slotmanager_opcode(uint32_t pc) {
    uint32_t rom_offset = 0;
    if (active_bus == NULL || active_bus->ram == NULL ||
        !lc_musashi_bus_rom_offset_for_address(pc, &rom_offset) ||
        rom_offset >= active_bus->rom_size || cpu_read_word(pc) != 0xa06eu) {
        return;
    }

    const uint32_t sp_block = m68k_get_reg(NULL, M68K_REG_A0);
    const uint16_t selector = (uint16_t)(m68k_get_reg(NULL, M68K_REG_D0) & 0xffffu);
    uint8_t slot = 0;
    uint32_t record = 0;
    if (sp_block + LC_SLOT_SP_SLOT < active_bus->ram_size) {
        slot = active_bus->ram[sp_block + LC_SLOT_SP_SLOT];
        record = lc_musashi_bus_post_reset_slot_record(slot);
        if (record != 0u) {
            if (selector == 0x0013u && sp_block + 0x19u < active_bus->ram_size) {
                // SlotInfo-like calls in the SRT scan expect the SPBlock result
                // pointer (at SPBlock+0) to reference a descriptor whose fields
                // are checked immediately by the ROM verifier.  Seed that
                // result record when available; fall back to the SPBlock itself.
                const uint32_t result_record = lc_musashi_bus_peek_ram32(sp_block + 0u);
                if (result_record + 0x19u < active_bus->ram_size) {
                    lc_musashi_bus_seed_post_reset_slot_spblock(result_record);
                } else {
                    lc_musashi_bus_seed_post_reset_slot_spblock(sp_block);
                }
            } else if (selector == 0x0028u) {
                lc_musashi_bus_ram_write32(sp_block + 0x00u, 0x00000001u);
            } else if (selector != 0x002cu) {
                lc_musashi_bus_ram_write32(sp_block + LC_SLOT_SP_RESULT, record);
                lc_musashi_bus_ram_write32(sp_block + LC_SLOT_SP_MISC, 0);
            }
        }
    }
    // The slot scan later calls through ([0x0db8] + 0xec).  Until the real
    // Slot Manager globals are built, route that deferred callback through a
    // harmless ROM RTS and keep the event chain head empty so Event Manager
    // walks do not chase RAM-fill pointers.
    lc_musashi_bus_ram_write32(0x00000db8u, 0x00009000u);
    lc_musashi_bus_ram_write32(0x000090e8u, 0x40800d88u);
    lc_musashi_bus_ram_write32(0x000090ecu, 0x40800d88u);
    lc_musashi_bus_ram_write32(0x00000358u, 0x00000000u);
    lc_musashi_bus_ram_write32(0x0000035cu, 0x00000000u);
    // Selector 0x000c is used by the ROM's per-slot verifier after SGetSRsrc.
    // Returning noErr for every synthetic slot makes the verifier treat all
    // placeholder records as real devices and eventually recurse until the
    // low SRT/stack area loses its RTS frame.  Report the same -313 failure the
    // wrapper uses on a failed selector-0x000c call so the surrounding scan can
    // advance/terminate instead of descending through synthetic records forever.
    const bool selector_not_found = selector == 0x000cu;
    const uint32_t result = selector_not_found ? 0xfffffec7u : 0u;
    m68k_set_reg(M68K_REG_D0, result);
    const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    m68k_set_reg(M68K_REG_SR, selector_not_found ? ((sr & ~0x0004u) | 0x0008u) :
                                                   ((sr & ~0x000bu) | 0x0004u));
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    lc_musashi_bus_log_memory_trap(pc, 0xa06eu,
                                   selector == 0x002fu ? "SlotNoPDSDirect" :
                                   selector == 0x0013u ? "SlotInfoNoPDSDirect" :
                                   selector == 0x002cu ? "SlotUpdateNoPDSDirect" :
                                   selector_not_found ? "SlotManagerSelectorNotFoundDirect" :
                                                        "SlotManagerNoPDSDirect",
                                   selector, record, slot);
}

static void lc_musashi_bus_post_reset_seed_zone(uint32_t zone_start,
                                                uint32_t zone_limit,
                                                uint32_t grow_proc,
                                                uint16_t more_masters,
                                                bool update_lowmem) {
    if (active_bus == NULL || active_bus->ram == NULL || zone_limit > active_bus->ram_size ||
        zone_start < LC_MEMORY_ZONE_MIN_START ||
        zone_start + LC_MEMORY_ZONE_HEADER_SIZE >= zone_limit) {
        return;
    }

    const uint32_t free_start = zone_start + LC_MEMORY_ZONE_HEADER_SIZE;
    const uint32_t free_bytes = (zone_limit - free_start) & LC_MEMORY_BLOCK_SIZE_MASK;

    for (uint32_t i = 0; i < LC_MEMORY_ZONE_HEADER_SIZE; i++) {
        lc_musashi_bus_ram_write8(zone_start + i, 0);
    }

    // Classic Memory Manager zone header fields that the reset/Resource Manager
    // path consumes first: bkLim, hFstFree/zcbFree, grow-zone proc, moreMasters,
    // minCBFree, and the allocation rover at +0x30.  The first heap block begins
    // immediately at zone+0x34 in this ROM; leaving those bytes zero made the
    // ROM's compaction/free-space scan loop forever at 0x4080e9fc/0x4080ea0a.
    // Seed a real free-block header instead of keeping diagnostic anchors inside
    // what the ROM considers heap block storage.
    lc_musashi_bus_ram_write32(zone_start + 0x00u, zone_limit);  // bkLim
    lc_musashi_bus_ram_write32(zone_start + 0x04u, 0);           // purgePtr
    lc_musashi_bus_ram_write32(zone_start + 0x08u, free_start);  // hFstFree
    lc_musashi_bus_ram_write32(zone_start + 0x0cu, free_bytes);  // zcbFree
    lc_musashi_bus_ram_write32(zone_start + 0x10u, grow_proc);   // gzProc
    lc_musashi_bus_ram_write16(zone_start + 0x14u, more_masters);
    lc_musashi_bus_ram_write32(zone_start + 0x24u, 0);           // minCBFree
    lc_musashi_bus_ram_write32(zone_start + 0x30u, free_start);  // allocPtr/rover
    lc_musashi_bus_ram_write32(free_start, free_bytes);          // first free block

    if (!update_lowmem) {
        return;
    }

    // Keep the direct reset probe's RAM-owned A-line vector and immediate
    // follow-on SetApplLimit trap intact after InitZone mutates low memory.
    lc_musashi_bus_ram_write32(LC_LOWMEM_LINE_A_VECTOR, 0x408099b0u);
    lc_musashi_bus_post_reset_seed_low_trap(0x19u, 0x40800d88u);
    lc_musashi_bus_post_reset_seed_low_trap(0x2du, 0x40800d88u);

    lc_musashi_bus_ram_write32(LC_LOWMEM_MEM_TOP, zone_limit);
    lc_musashi_bus_ram_write32(LC_LOWMEM_BUF_PTR, zone_limit);
    lc_musashi_bus_ram_write32(LC_LOWMEM_HEAP_END, zone_limit);
    lc_musashi_bus_ram_write32(LC_LOWMEM_THE_ZONE, zone_start);
    lc_musashi_bus_ram_write32(LC_LOWMEM_APPL_LIMIT, zone_limit);
    lc_musashi_bus_ram_write16(LC_LOWMEM_MEM_ERR, 0);
    // Notification/error callbacks consulted by the ROM error path at
    // 0x4080f0d6/0x4080f0e0.  In the direct reset probe these low-memory cells
    // can still contain RAM-fill patterns, which makes the ROM JSR through
    // 0x6db6db6d instead of treating the callback as absent.
    lc_musashi_bus_ram_write32(0x000003e6u, 0);
    lc_musashi_bus_ram_write32(0x000003f2u, 0);
}

static void lc_musashi_bus_log_memory_trap(uint32_t pc,
                                           uint16_t trap_word,
                                           const char *action,
                                           uint32_t size,
                                           uint32_t ptr,
                                           uint32_t handle) {
    if (post_reset_memory_trap_logs >= 1000u) {
        return;
    }
    ESP_LOGW(TAG,
             "LC synthetic post-reset memory trap: pc=0x%08" PRIx32
             " trap=0x%04x name=%s action=%s size=0x%08" PRIx32
             " ptr=0x%08" PRIx32 " handle=0x%08" PRIx32,
             pc, trap_word, lc_musashi_bus_post_reset_trap_name(trap_word), action,
             size, ptr, handle);
    post_reset_memory_trap_logs++;
}

static void lc_musashi_bus_maybe_apply_post_reset_memory_trap(uint32_t pc) {
    // Note: previously gated by !basilisk_slot_rom_active() but the Basilisk
    // boot path also needs the synthetic Memory Manager for boot block execution.
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009a04u && rom_offset != 0x00009a20u && rom_offset != 0x00009a22u) {
        return;
    }
    const uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    const uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    uint16_t trap_word = (uint16_t)(d1 & 0xffffu);
    if ((trap_word & 0xf000u) != 0xa000u) {
        trap_word = (uint16_t)(0xa000u | (d2 & 0x0fffu));
    }
    const uint16_t selector = trap_word & 0x01ffu;
    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
    const bool clear = (trap_word & 0x0200u) != 0 || (trap_word & 0x0600u) == 0x0600u;

    if (selector == 0x06eu) { // SlotManager
        const uint32_t sp_block = m68k_get_reg(NULL, M68K_REG_A0);
        const uint16_t slot_selector = (uint16_t)(size & 0xffffu);
        uint8_t slot = 0;
        uint32_t record = 0;
        if (active_bus != NULL && active_bus->ram != NULL &&
            sp_block + LC_SLOT_SP_SLOT < active_bus->ram_size) {
            slot = active_bus->ram[sp_block + LC_SLOT_SP_SLOT];
            record = lc_musashi_bus_post_reset_slot_record(slot);
            if (record != 0u) {
                lc_musashi_bus_ram_write32(sp_block + LC_SLOT_SP_RESULT, record);
                lc_musashi_bus_ram_write32(sp_block + LC_SLOT_SP_MISC, 0);
            }
        }
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word,
                                       slot_selector == 0x002fu ? "SlotNoPDS" :
                                       slot_selector == 0x0013u ? "SlotInfoNoPDS" :
                                       slot_selector == 0x002cu ? "SlotUpdateNoPDS" :
                                                               "SlotManagerNoPDS",
                                       slot_selector, record, slot);
    } else if (selector == 0x019u) { // InitZone
        const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
        const uint32_t a1 = m68k_get_reg(NULL, M68K_REG_A1);
        const uint32_t a2 = m68k_get_reg(NULL, M68K_REG_A2);
        bool is_main_zone = false;
        bool is_ram_param = false;
        const uint32_t zone_start = lc_musashi_bus_post_reset_initzone_start(a0, a1,
                                                                             &is_main_zone,
                                                                             &is_ram_param);
        const uint32_t zone_limit = zone_start != 0u
                                        ? lc_musashi_bus_post_reset_initzone_limit(a0, a1, zone_start,
                                                                                  is_ram_param)
                                        : 0u;
        const uint32_t grow_proc = lc_musashi_bus_post_reset_initzone_grow_proc(a0, a2);
        bool seeded = false;
        if (zone_start != 0u && is_main_zone && !post_reset_main_zone_seeded) {
            lc_musashi_bus_post_reset_seed_zone(zone_start, zone_limit, grow_proc,
                                                (uint16_t)(size & 0xffffu), true);
            post_reset_main_zone_seeded = true;
            seeded = true;
        }
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word,
                                       seeded ? "InitZone" :
                                       zone_start != 0u && is_main_zone ? "InitZoneAlreadySeeded" :
                                                                         "InitZoneNoOp",
                                       seeded && zone_limit > zone_start ? zone_limit - zone_start : 0,
                                       zone_start, 0);
    } else if (selector == 0x051u && ((size >> 16u) & 0xffffu) == 1u &&
               (size & 0xffffu) == 0x00aeu) { // ReadXPRam combo selector
        const uint32_t dst = m68k_get_reg(NULL, M68K_REG_A0);
        uint32_t written = 0;
        if (active_bus != NULL && active_bus->ram != NULL && dst < active_bus->ram_size) {
            // ReDoMap uses xPRAM byte 0xAE as a combo *index*: 0 or >MaxComInd
            // means use ProductInfo.DefaultRSRCs.  Leave the more delicate 0x8A
            // Memory Manager mode byte to the ROM/default path for now; forcing
            // it to zero regressed the reset continuation.  Non-zero combo
            // indexes require a more complete ProductInfo/resource-map model.
            lc_musashi_bus_ram_write8(dst, 0x00u);
            written = 1;
        }
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "ReadXPRamComboDefault", 0x00aeu,
                                       dst, written);
    } else if (selector == 0x02du) { // SetApplLimit
        const uint32_t limit = m68k_get_reg(NULL, M68K_REG_A0) & ~1u;
        if (active_bus != NULL && active_bus->ram != NULL && limit < active_bus->ram_size) {
            lc_musashi_bus_ram_write32(LC_LOWMEM_APPL_LIMIT, limit);
            lc_musashi_bus_ram_write16(LC_LOWMEM_MEM_ERR, 0);
        }
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "SetApplLimit", 0, limit, 0);
    } else if (selector == 0x11eu) { // NewPtr variants
        const uint32_t old_a0 = m68k_get_reg(NULL, M68K_REG_A0);
        const uint32_t return_a2 = m68k_get_reg(NULL, M68K_REG_A2);
        uint32_t return_offset = 0;
        const bool resource_jump_table_alloc = false && size == 0x000000f4u &&
            lc_musashi_bus_rom_offset_for_address(return_a2, &return_offset) &&
            return_offset == 0x00006d7cu;
        uint32_t ptr = resource_jump_table_alloc ? LC_POST_RESET_EMERGENCY_HEAP_BASE :
                                                   lc_musashi_bus_post_reset_alloc(size, clear);
        bool fallback_reuse = resource_jump_table_alloc;
        if (resource_jump_table_alloc && active_bus != NULL && active_bus->ram != NULL && clear) {
            for (uint32_t i = 0; i < size && LC_POST_RESET_EMERGENCY_HEAP_BASE + i < active_bus->ram_size; i++) {
                lc_musashi_bus_ram_write8(LC_POST_RESET_EMERGENCY_HEAP_BASE + i, 0);
            }
        }
        if (ptr == 0u && active_bus != NULL && active_bus->ram != NULL &&
            size <= 0x00010000u) {
            // Resource Manager frequently DisposePtr/NewPtr-rebuilds small
            // tables.  The compact host allocator can lose a non-tail free
            // block after many growth/copy passes.  Reuse the just-presented RAM
            // pointer only if it will not collide with the active trap stack;
            // otherwise draw from a small emergency slab while the full zone
            // free-list is still being ported.
            const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP) & ~1u;
            const uint32_t aligned = (size + 3u) & ~3u;
            const uint32_t frame_top = sp + (rom_offset == 0x00009a04u ? 28u : 24u);
            const uint32_t guard_low = sp > 0x80u ? sp - 0x80u : 0u;
            uint32_t guard_high = frame_top + 0x80u;
            if (guard_high < frame_top || guard_high > active_bus->ram_size) {
                guard_high = (uint32_t)active_bus->ram_size;
            }
            const bool old_a0_in_ram = old_a0 >= 0x00001000u && old_a0 < active_bus->ram_size &&
                                       aligned <= active_bus->ram_size - old_a0;
            const uint32_t old_a0_end = old_a0_in_ram ? old_a0 + aligned : old_a0;
            const bool old_a0_overlaps_trap_stack = old_a0_in_ram && aligned != 0u &&
                                                    old_a0 < guard_high &&
                                                    old_a0_end > guard_low;
            const bool old_a0_overlaps_resource_map = old_a0_in_ram && aligned != 0u &&
                lc_musashi_bus_post_reset_range_overlaps_current_resource_map(old_a0, aligned);
            const bool old_a0_safe = old_a0_in_ram && !old_a0_overlaps_trap_stack &&
                                     !old_a0_overlaps_resource_map &&
                                     (sp < 0x00100000u || old_a0_end + 0x100u < sp);
            if (old_a0_overlaps_trap_stack || old_a0_overlaps_resource_map) {
                static unsigned overlap_logs = 0;
                if (overlap_logs < 16u) {
                    ESP_LOGW(TAG,
                             "LC rejected NewPtr old-A0 fallback overlapping %s: pc=0x%08" PRIx32
                             " trap=0x%04x size=0x%08" PRIx32 " old_a0=0x%08" PRIx32
                             " old_end=0x%08" PRIx32 " sp=0x%08" PRIx32
                             " guard=0x%08" PRIx32 "..0x%08" PRIx32,
                             old_a0_overlaps_resource_map ? "ResourceMgr map" : "trap stack",
                             pc, trap_word, size, old_a0, old_a0_end, sp, guard_low, guard_high);
                    overlap_logs++;
                }
            }
            if (old_a0_safe) {
                ptr = old_a0;
            } else {
                if (post_reset_emergency_bump == 0u) {
                    post_reset_emergency_bump = LC_POST_RESET_EMERGENCY_HEAP_BASE;
                }
                const uint32_t candidate = (post_reset_emergency_bump + 3u) & ~3u;
                if (aligned != 0u && candidate >= LC_POST_RESET_EMERGENCY_HEAP_BASE &&
                    candidate + aligned <= LC_POST_RESET_EMERGENCY_HEAP_LIMIT &&
                    candidate + aligned <= active_bus->ram_size) {
                    ptr = candidate;
                    post_reset_emergency_bump = candidate + aligned;
                }
                if (ptr == 0u) {
                    if (post_reset_low_emergency_bump == 0u) {
                        post_reset_low_emergency_bump = LC_POST_RESET_LOW_EMERGENCY_HEAP_BASE;
                    }
                    uint32_t low_candidate = (post_reset_low_emergency_bump + 3u) & ~3u;
                    if (aligned != 0u &&
                        (low_candidate + aligned > LC_POST_RESET_LOW_EMERGENCY_HEAP_LIMIT ||
                         low_candidate + aligned < low_candidate)) {
                        // The direct reset probe lacks a full temporary-memory
                        // free list.  Recycle this low emergency slab instead
                        // of letting NewPtr fail with A0 still pointing into the
                        // Resource Manager map header.
                        low_candidate = LC_POST_RESET_LOW_EMERGENCY_HEAP_BASE;
                        post_reset_low_emergency_bump = low_candidate;
                    }
                    const bool below_active_stack = sp < LC_POST_RESET_LOW_EMERGENCY_HEAP_BASE ||
                        low_candidate + aligned + 0x1000u < sp;
                    if (aligned != 0u && low_candidate >= LC_POST_RESET_LOW_EMERGENCY_HEAP_BASE &&
                        low_candidate + aligned <= LC_POST_RESET_LOW_EMERGENCY_HEAP_LIMIT &&
                        low_candidate + aligned <= active_bus->ram_size && below_active_stack &&
                        !lc_musashi_bus_post_reset_range_overlaps_current_resource_map(low_candidate, aligned)) {
                        ptr = low_candidate;
                        post_reset_low_emergency_bump = low_candidate + aligned;
                        static unsigned low_emergency_logs = 0;
                        if (low_emergency_logs < 24u) {
                            ESP_LOGW(TAG,
                                     "LC used low emergency NewPtr slab: pc=0x%08" PRIx32
                                     " size=0x%08" PRIx32 " ptr=0x%08" PRIx32
                                     " sp=0x%08" PRIx32,
                                     pc, size, ptr, sp);
                            low_emergency_logs++;
                        }
                    }
                }
            }
            fallback_reuse = ptr != 0u;
            if (ptr != 0u && clear) {
                uint32_t end = aligned;
                if (end > active_bus->ram_size - ptr) {
                    end = active_bus->ram_size - ptr;
                }
                for (uint32_t i = 0; i < end; i++) {
                    lc_musashi_bus_ram_write8(ptr + i, 0);
                }
            }
        }
        if (ptr != 0u) {
            m68k_set_reg(M68K_REG_A0, ptr);
            m68k_set_reg(M68K_REG_D0, 0);
            const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
            m68k_set_reg(M68K_REG_SR, (sr & 0xfff0u) | 0x0004u); // Z=1/noErr.
        } else {
            const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
            m68k_set_reg(M68K_REG_SR, sr & ~0x0004u);
        }
        const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
        static unsigned newptr_dispatch_logs = 0;
        if (newptr_dispatch_logs < 40u) {
            ESP_LOGW(TAG,
                     "LC NewPtr dispatch frame: pc=0x%08" PRIx32
                     " trap=0x%04x size=0x%08" PRIx32 " ptr=0x%08" PRIx32
                     " return_a2=0x%08" PRIx32 " sp=0x%08" PRIx32
                     " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                     pc, trap_word, size, ptr, return_a2, sp,
                     lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
                     lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
                     lc_musashi_bus_peek_ram32(sp + 16u), lc_musashi_bus_peek_ram32(sp + 20u),
                     lc_musashi_bus_peek_ram32(sp + 24u), lc_musashi_bus_peek_ram32(sp + 28u));
            newptr_dispatch_logs++;
        }
        // The low A-trap dispatcher stores its caller return in the stack slot
        // consumed by the final RTS (`move.l a2, sp@(20)` at 0x408099f6).
        // At 0x9a04 A0 has been pushed after that store, so the slot is 24
        // bytes above the current SP; at 0x9a22 no A0 save exists, so it is
        // 20 bytes above the current SP.  Writing one word too low corrupts the
        // dispatch epilogue frame and can make later code treat ROM addresses as
        // NewPtr results.
        const uint32_t return_slot_a = sp + (rom_offset == 0x00009a04u ? 24u : 20u);
        if (return_a2 != 0u && return_slot_a + 3u < (active_bus != NULL ? active_bus->ram_size : 0u)) {
            lc_musashi_bus_ram_write32(return_slot_a, return_a2);
        }

        lc_musashi_bus_log_memory_trap(pc, trap_word,
                                       fallback_reuse ? "NewPtrReuseFallback" : "NewPtr",
                                       size, ptr, 0);
        // Skip exactly the indexed-indirect JSR instruction.  The 0x9a04 form is
        // six bytes (`4eb0 25a1 0400`); the 0x9a22 form is four bytes
        // (`4eb0 2591`).  The following epilogue must still restore A1/D1/D2/A2.
        m68k_set_reg(M68K_REG_PC, pc + (rom_offset == 0x00009a04u ? 6u : 4u));
    } else if (selector == 0x122u) { // NewHandle variants
        const uint32_t return_a2 = m68k_get_reg(NULL, M68K_REG_A2);
        const bool event_manager_init_newhandle = return_a2 == 0x4081ade0u && size == 0x00000024u;
        const bool b2_slot_rom_active = lc_musashi_bus_basilisk_slot_rom_active();
        uint32_t ptr = 0;
        uint32_t handle = 0;
        if (event_manager_init_newhandle && !b2_slot_rom_active && post_reset_event_newhandle != 0u) {
            handle = post_reset_event_newhandle;
            ptr = post_reset_event_newhandle_ptr;
            if (ptr != 0u && ptr + size <= (active_bus != NULL ? active_bus->ram_size : 0u)) {
                for (uint32_t i = 0; i < size; i++) {
                    lc_musashi_bus_ram_write8(ptr + i, 0);
                }
            }
        } else {
            ptr = lc_musashi_bus_post_reset_alloc_handle_data(size, clear);
            handle = ptr != 0u ? lc_musashi_bus_post_reset_alloc_handle(ptr, size) : 0u;
        }
        if (handle == 0u && size > 0x00010000u) {
            // The Resource Manager path can present a provisional negative/huge
            // size just before a real SetHandleSize.  Model the master pointer
            // first and let the following SetHandleSize allocate the movable
            // block, instead of leaving A0 pointing at a previous Ptr block.
            ptr = 0;
            handle = lc_musashi_bus_post_reset_alloc_handle(0, 0);
        }
        if (handle != 0u) {
            m68k_set_reg(M68K_REG_A0, handle);
            m68k_set_reg(M68K_REG_D0, 0);
        }
        if (event_manager_init_newhandle && !b2_slot_rom_active && handle != 0u && post_reset_event_newhandle == 0u) {
            post_reset_event_newhandle = handle;
            post_reset_event_newhandle_ptr = ptr;
            ESP_LOGW(TAG,
                     "LC pinned Resource Manager/Event NewHandle result for reuse: handle=0x%08" PRIx32
                     " ptr=0x%08" PRIx32,
                     handle, ptr);
        }
        {
            const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
            m68k_set_reg(M68K_REG_SR, handle != 0u ? ((sr & 0xfff0u) | 0x0004u) :
                                                     (sr & ~0x0004u));
            const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
            const uint32_t return_slot_a = sp + (rom_offset == 0x00009a04u ? 24u : 20u);
            if (return_a2 != 0u && return_slot_a + 3u < (active_bus != NULL ? active_bus->ram_size : 0u)) {
                lc_musashi_bus_ram_write32(return_slot_a, return_a2);
            }
            m68k_set_reg(M68K_REG_PC, pc + (rom_offset == 0x00009a04u ? 6u : 4u));
        }
        lc_musashi_bus_log_memory_trap(pc, trap_word,
                                       event_manager_init_newhandle ? "NewHandleEventInit" : "NewHandle",
                                       size, ptr, handle);
    } else if (selector == 0x024u) { // SetHandleSize
        const uint32_t handle = m68k_get_reg(NULL, M68K_REG_A0);
        uint32_t trap_return_offset = 0;
        const uint32_t trap_return = m68k_get_reg(NULL, M68K_REG_A2);
        const bool resource_map_resize_call =
            lc_musashi_bus_rom_offset_for_address(trap_return, &trap_return_offset) &&
            (trap_return_offset == 0x0001bfccu || trap_return_offset == 0x0001ab30u);
        const bool resource_map_handle =
            lc_musashi_bus_post_reset_is_resource_map_handle(handle) || resource_map_resize_call;
        if (resource_map_resize_call) {
            lc_musashi_bus_post_reset_note_rom_map_handle(handle);
        }
        const uint32_t requested_size = size;
        if (resource_map_handle && size < 0x0000001eu) {
            // Classic resource maps are never smaller than NullRMSize (30
            // bytes).  If transient map-offset arithmetic presents a smaller
            // size, do not let the synthetic Memory Manager physically shrink
            // the map body until the Resource Manager has restored its fields.
            size = 0x0000001eu;
        }
        uint32_t old_ptr = lc_musashi_bus_post_reset_get_handle_ptr_record(handle);
        if (old_ptr == 0u) {
            old_ptr = lc_musashi_bus_peek_ram32(handle);
        }
        uint32_t old_size = lc_musashi_bus_post_reset_infer_handle_size(handle);
        if (resource_map_handle && old_size < 0x0000001eu) {
            old_size = 0x0000001eu;
        }
        uint32_t ptr = 0;
        if (old_ptr != 0u &&
            (resource_map_handle || lc_musashi_bus_post_reset_handle_is_locked(handle))) {
            const uint32_t capacity = lc_musashi_bus_post_reset_payload_size_from_header(old_ptr, 12u);
            if (capacity != 0u && size <= capacity) {
                // Resource-map handles are live map-chain anchors even when the
                // ROM temporarily unlocks/shrinks them while rebuilding.  Keep
                // the physical block in place and avoid moving the heap bump
                // back into the map body; only the logical handle size changes.
                ptr = old_ptr;
                if (size > old_size && active_bus != NULL && active_bus->ram != NULL &&
                    ptr < active_bus->ram_size) {
                    uint32_t end = size;
                    if (end > active_bus->ram_size - ptr) {
                        end = active_bus->ram_size - ptr;
                    }
                    for (uint32_t i = old_size; i < end; i++) {
                        lc_musashi_bus_ram_write8(ptr + i, 0);
                    }
                }
            }
        }
        if (ptr == 0u) {
            ptr = old_ptr != 0u ?
                      lc_musashi_bus_post_reset_resize_last_handle_data(old_ptr, size) :
                      0u;
        }
        if (ptr == 0u) {
            ptr = lc_musashi_bus_post_reset_alloc_handle_data(size, false);
            if (ptr != 0u && handle + 3u < (active_bus != NULL ? active_bus->ram_size : 0u)) {
                uint32_t copy = old_size < size ? old_size : size;
                if (old_ptr >= (active_bus != NULL ? active_bus->ram_size : 0u) ||
                    ptr >= (active_bus != NULL ? active_bus->ram_size : 0u)) {
                    copy = 0;
                } else if (copy > active_bus->ram_size - old_ptr) {
                    copy = active_bus->ram_size - old_ptr;
                } else if (copy > active_bus->ram_size - ptr) {
                    copy = active_bus->ram_size - ptr;
                }
                for (uint32_t i = 0; i < copy; i++) {
                    lc_musashi_bus_ram_write8(ptr + i, active_bus->ram[old_ptr + i]);
                }
                for (uint32_t i = copy; i < size; i++) {
                    lc_musashi_bus_ram_write8(ptr + i, 0);
                }
                if (old_ptr != 0u && old_ptr != ptr) {
                    lc_musashi_bus_post_reset_free_alloc(old_ptr);
                }
            }
        }
        if (ptr != 0u && handle + 3u < (active_bus != NULL ? active_bus->ram_size : 0u)) {
            lc_musashi_bus_ram_write32(handle, ptr);
            lc_musashi_bus_post_reset_set_handle_record(handle, ptr, size);
            if (resource_map_handle && requested_size != size && ptr + 15u < active_bus->ram_size) {
                lc_musashi_bus_ram_write32(ptr + 12u, size);
            }
            m68k_set_reg(M68K_REG_D0, 0);
        }
        lc_musashi_bus_log_memory_trap(pc, trap_word,
                                       requested_size != size ? "SetHandleSizeMapClamped" :
                                                               "SetHandleSize",
                                       size, ptr, handle);
    } else if (selector == 0x025u) { // GetHandleSize
        const uint32_t handle = m68k_get_reg(NULL, M68K_REG_A0);
        const uint32_t known_size = lc_musashi_bus_post_reset_infer_handle_size(handle);
        m68k_set_reg(M68K_REG_D0, known_size);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "GetHandleSize", known_size, 0,
                                       handle);
    } else if (selector == 0x061u) { // MaxBlock
        uint32_t max_block = 0;
        if (active_bus != NULL && active_bus->ram != NULL && post_reset_heap_bump < active_bus->ram_size) {
            // Keep clear of the ROM's top-of-RAM stack/direct-probe locals.  This
            // is still a compact Memory Manager surface, but returning a real
            // bounded maximum lets Resource Manager growth code size temporary
            // blocks without falling through to stale ROM dispatch state.
            const uint32_t stack_floor = lc_musashi_bus_post_reset_heap_stack_floor();
            if (post_reset_heap_bump + 0x100u < stack_floor) {
                max_block = (stack_floor - post_reset_heap_bump - 0x100u) & ~3u;
            }
        }
        m68k_set_reg(M68K_REG_D0, max_block);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "MaxBlock", max_block, 0, 0);
    } else if (selector == 0x126u) { // HandleZone
        const uint32_t handle = m68k_get_reg(NULL, M68K_REG_A0);
        uint32_t zone = lc_musashi_bus_peek_ram32(LC_LOWMEM_THE_ZONE);
        const uint32_t rom_map_handle = lc_musashi_bus_peek_ram32(LC_LOWMEM_ROM_MAP_HNDL);
        if (handle != 0u && handle == rom_map_handle && active_bus != NULL &&
            LC_RESOURCE_ROM_MASTER_PTR_LIMIT < active_bus->ram_size) {
            // ReDoMap uses HandleZone(RomMapHndl) only as the base for ROM
            // resource relative handles.  Those RelHandles are offsets 0x5c..
            // 0x24c in this LC ROM; returning the real zone base writes them
            // into the free block at zone+0x34 and destabilizes later Memory
            // Manager scans.  Keep this ROM-map master-pointer slab RAM-backed
            // and outside the zone heap until a full ROZ/master-pointer block
            // model exists.
            zone = LC_RESOURCE_ROM_MASTER_PTR_BASE;
        }
        m68k_set_reg(M68K_REG_A0, zone);
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "HandleZone", 0, zone, handle);
    } else if (selector == 0x01fu) { // DisposePtr
        const uint32_t ptr = m68k_get_reg(NULL, M68K_REG_A0);
        lc_musashi_bus_post_reset_free_alloc(ptr);
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "DisposePtr", 0, ptr, 0);
    } else if (selector == 0x029u || selector == 0x02au || selector == 0x023u ||
               selector == 0x02bu) {
        const uint32_t handle = m68k_get_reg(NULL, M68K_REG_A0);
        if (selector == 0x029u) {
            lc_musashi_bus_post_reset_set_handle_locked(handle, true);
        } else if (selector == 0x02au) {
            lc_musashi_bus_post_reset_set_handle_locked(handle, false);
        } else if (selector == 0x023u) {
            if (!lc_musashi_bus_post_reset_is_resource_map_handle(handle)) {
                const uint32_t ptr = lc_musashi_bus_post_reset_get_handle_ptr_record(handle);
                lc_musashi_bus_post_reset_free_alloc(ptr != 0u ? ptr : lc_musashi_bus_peek_ram32(handle));
                lc_musashi_bus_ram_write32(handle, 0);
                lc_musashi_bus_post_reset_set_handle_record(handle, 0, 0);
            }
        } else if (selector == 0x02bu) {
            if (!lc_musashi_bus_post_reset_is_resource_map_handle(handle)) {
                const uint32_t ptr = lc_musashi_bus_post_reset_get_handle_ptr_record(handle);
                lc_musashi_bus_post_reset_free_alloc(ptr != 0u ? ptr : lc_musashi_bus_peek_ram32(handle));
                lc_musashi_bus_ram_write32(handle, 0);
                lc_musashi_bus_post_reset_set_handle_record(handle, 0, 0);
            }
        }
        m68k_set_reg(M68K_REG_D0, 0);
        lc_musashi_bus_log_memory_trap(pc, trap_word,
                                       selector == 0x023u ? "DisposeHandle" :
                                       selector == 0x02bu ? "EmptyHandle" : "HandleNoOp",
                                       0, 0, handle);
    } else if (selector == 0x055u) { // StripAddress
        const uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
        const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
        // In 32-bit mode (MMU32Bit at $0CB2 != 0), StripAddress is a no-op;
        // all 32 bits are significant.  Without this, ROM resource pointers
        // like 0x4081xxxx get corrupted to 0x0081xxxx.
        const uint8_t mmu32bit = (active_bus->ram_size > 0x00000cb3u)
                                 ? active_bus->ram[0x00000cb2u] : 0u;
        const uint32_t stripped_d0 = mmu32bit ? d0 : (d0 & 0x00ffffffu);
        const uint32_t stripped_a0 = mmu32bit ? a0 : (a0 & 0x00ffffffu);
        m68k_set_reg(M68K_REG_D0, stripped_d0);
        m68k_set_reg(M68K_REG_A0, stripped_a0);
        lc_musashi_bus_log_memory_trap(pc, trap_word, "StripAddress", d0,
                                       stripped_d0, stripped_a0);
    } else if (selector == 0x060u) { // HGetState — return 0 (unlocked, not a resource)
        m68k_set_reg(M68K_REG_D0, 0);
    } else if (selector == 0x06cu || selector == 0x01cu) { // FreeMem / MaxBlock-like
        // Return a large free memory value so boot code doesn't bail out.
        const uint32_t free_bytes = active_bus->ram_size > 0x80000u
                                    ? active_bus->ram_size - 0x80000u : 0x100000u;
        m68k_set_reg(M68K_REG_D0, free_bytes);
        m68k_set_reg(M68K_REG_A0, free_bytes);
    } else if (selector == 0x061u) { // MaxBlock
        m68k_set_reg(M68K_REG_D0, active_bus->ram_size > 0x80000u
                                   ? active_bus->ram_size - 0x80000u : 0x100000u);
    } else if (selector == 0x015u) { // GetZone
        // Return SysZone pointer (set during RESET seeding).
        m68k_set_reg(M68K_REG_A0, 0x00002800u);
        m68k_set_reg(M68K_REG_D0, 0);
    } else if (selector == 0x06du) { // MaxApplZone
        m68k_set_reg(M68K_REG_D0, 0);
    }
}

static void lc_musashi_bus_maybe_apply_post_reset_set_trap_address(uint32_t pc) {
    if (!lc_musashi_bus_post_reset_dispatch_matches(pc, 0xa047u, 0x0047u)) {
        return;
    }
    const uint32_t trap_word = m68k_get_reg(NULL, M68K_REG_D0);
    const uint32_t handler = m68k_get_reg(NULL, M68K_REG_A0);
    const bool handler_plausible =
        (handler >= 0x40800000u && handler < 0x40880000u) ||
        (handler >= 0x00400000u && handler < 0x00480000u) ||
        (active_bus != NULL && handler < active_bus->ram_size);
    if (handler_plausible) {
        lc_memory_set_post_reset_atrap_handler((uint16_t)trap_word, handler);
    }
    if (post_reset_set_trap_address_logs < 12u) {
        ESP_LOGW(TAG,
                 "LC synthetic post-reset SetTrapAddress A047: pc=0x%08" PRIx32
                 " trap=0x%04" PRIx32 " low=0x%02" PRIx32
                 " handler=0x%08" PRIx32 " applied=%s",
                 pc, trap_word & 0xffffu, trap_word & 0x7fu, handler,
                 handler_plausible ? "yes" : "no");
        post_reset_set_trap_address_logs++;
    }
}

static void lc_musashi_bus_maybe_apply_post_reset_block_move(uint32_t pc) {
    if (!lc_musashi_bus_post_reset_dispatch_matches(pc, 0xa02eu, 0x002eu) ||
        active_bus == NULL || !active_bus->initialized) {
        return;
    }

    const uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
    const uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
    const uint32_t count = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t copied = count;
    if (dst >= active_bus->ram_size || count > 0x00010000u) {
        // Bogus provisional Resource Manager state can present negative/huge
        // sizes or non-RAM destinations.  Do not turn those into destructive
        // host-side copies; leave the no-op table handler to return failure-ish
        // state and expose the bad caller instead.
        copied = 0;
    } else if (copied > active_bus->ram_size - dst) {
        copied = active_bus->ram_size - dst;
    }

    if (copied != 0u) {
        if (dst > src && dst < src + copied) {
            for (uint32_t i = copied; i != 0u; i--) {
                const uint8_t value = lc_memory_bus_read8(active_bus, src + i - 1u);
                (void)lc_memory_bus_write8(active_bus, dst + i - 1u, value);
            }
        } else {
            for (uint32_t i = 0; i < copied; i++) {
                const uint8_t value = lc_memory_bus_read8(active_bus, src + i);
                (void)lc_memory_bus_write8(active_bus, dst + i, value);
            }
        }
    }
    m68k_set_reg(M68K_REG_D0, 0);

    const uint32_t top_map_handle = lc_musashi_bus_peek_ram32(LC_LOWMEM_TOP_MAP_HNDL);
    const uint32_t top_map_ptr = top_map_handle + 3u < (active_bus != NULL ? active_bus->ram_size : 0u)
                                     ? lc_musashi_bus_peek_ram32(top_map_handle)
                                     : 0u;
    const bool touches_map_header = top_map_ptr != 0u &&
        dst < top_map_ptr + 0x30u && dst + copied > top_map_ptr;
    static unsigned map_header_block_move_logs = 0;
    if (post_reset_block_move_logs < 80u ||
        (touches_map_header && map_header_block_move_logs < 32u)) {
        ESP_LOGW(TAG,
                 "%s synthetic post-reset BlockMove A02E: pc=0x%08" PRIx32
                 " src=0x%08" PRIx32 " dst=0x%08" PRIx32
                 " count=0x%08" PRIx32 " copied=0x%08" PRIx32
                 " top_handle=0x%08" PRIx32 " top_map=0x%08" PRIx32
                 " d1=0x%08x d2=0x%08x a2=0x%08x sp=0x%08x",
                 touches_map_header ? "LC map-header" : "LC",
                 pc, src, dst, count, copied, top_map_handle, top_map_ptr,
                 m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                 m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_SP));
        if (touches_map_header) {
            map_header_block_move_logs++;
        } else {
            post_reset_block_move_logs++;
        }
    }
}

static void lc_musashi_bus_maybe_stub_post_reset_swap_mmu_dispatch(uint32_t pc) {
    if (!lc_musashi_bus_post_reset_dispatch_matches(pc, 0xa05du, 0x005du)) {
        return;
    }
    const uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    const uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    if ((d1 & 0xffffu) != 0xa05du && (d2 & 0xffffu) != 0x005du) {
        return;
    }
    // A05D is SwapMMUMode.  The current direct reset probe reaches the generic
    // A-line dispatcher before the ROM-owned low-memory trap table has been
    // built, and executing the indirect JSR through the provisional table falls
    // into low RAM exception-vector recursion.  Skip just the callee portion of
    // the dispatcher, leaving its saved-register stack layout intact so the
    // normal epilogue at 0x40809a0a restores A0/A1/D1/D2/A2 and returns.
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    // Enable the accompanying ROM-read override for this one A05D dispatcher
    // visit.  Other A-traps continue through the synthetic low-memory trap
    // table, so missing table entries remain visible instead of being hidden by
    // a broad dispatcher NOP.
    post_reset_swap_mmu_dispatch_nop_active = true;
    // The override NOPs the generic dispatcher's indirect JSR at 0x40809a04, so
    // execution falls through to its normal epilogue at 0x40809a0a.  On this
    // EC020 exception frame that epilogue's RTS would
    // otherwise consume the low PC/format-vector words as an address and jump
    // to low RAM.  Seed the exact return slot consumed by that epilogue with the
    // post-trap PC.
    lc_musashi_bus_ram_write32(sp + 24u, 0x408001f4u);
    m68k_set_reg(M68K_REG_D0, 0);
    if (!post_reset_swap_mmu_dispatch_stub_logged) {
        post_reset_swap_mmu_dispatch_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed post-reset SwapMMUMode A05D dispatch: pc=0x%08" PRIx32
                 " d1=0x%08" PRIx32 " d2=0x%08" PRIx32
                 " sp=0x%08" PRIx32 " return_slot=0x%08" PRIx32
                 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " fallthrough=0x40809a0a return_pc=0x408001f4",
                 pc, d1, d2, sp, sp + 24u, lc_musashi_bus_peek_ram32(sp + 0u),
                 lc_musashi_bus_peek_ram32(sp + 4u), lc_musashi_bus_peek_ram32(sp + 8u),
                 lc_musashi_bus_peek_ram32(sp + 12u), lc_musashi_bus_peek_ram32(sp + 16u),
                 lc_musashi_bus_peek_ram32(sp + 20u), lc_musashi_bus_peek_ram32(sp + 24u));
    }
}

static void lc_musashi_bus_maybe_shortcut_post_reset_get_startup_string(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0001af1eu || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 5u >= active_bus->ram_size || lc_musashi_bus_peek_ram16(sp) != 0xe000u) {
        return;
    }
    lc_musashi_bus_ram_write32(sp + 2u, 0x00008320u);
    m68k_set_reg(M68K_REG_SP, sp + 2u);
    m68k_set_reg(M68K_REG_D0, 0x00008320u);
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    if (!post_reset_getstring_startup_logged) {
        post_reset_getstring_startup_logged = true;
        ESP_LOGW(TAG,
                 "LC shortcut post-reset GetString(-8192) through seeded STR handle: pc=0x%08" PRIx32
                 " old_sp=0x%08" PRIx32 " new_sp=0x%08" PRIx32 " handle=0x00008320",
                 pc, sp, sp + 2u);
    }
}

static void lc_musashi_bus_maybe_fix_post_reset_high_trap_handler_sr(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00009930u && rom_offset != 0x00009932u &&
        rom_offset != 0x00009952u && rom_offset != 0x00009956u &&
        rom_offset != 0x00009958u) {
        return;
    }
    const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    const uint32_t fixed = (sr & 0x00ffu) | 0x2700u;
    bool changed = false;
    uint32_t saved_sr = 0xffffu;
    if (rom_offset == 0x00009930u || rom_offset == 0x00009956u) {
        if ((sr & 0x6000u) != 0x2000u) {
            m68k_set_reg(M68K_REG_SR, fixed);
            changed = true;
        }
    } else if (active_bus != NULL && active_bus->ram != NULL) {
        const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
        if (sp + 1u < active_bus->ram_size) {
            saved_sr = lc_musashi_bus_peek_ram16(sp);
            if ((saved_sr & 0x6000u) != 0x2000u) {
                lc_musashi_bus_ram_write16(sp, (uint16_t)fixed);
                changed = true;
            }
        }
    }
    if (changed && !post_reset_high_trap_handler_sr_logged) {
        post_reset_high_trap_handler_sr_logged = true;
        ESP_LOGW(TAG,
                 "LC restored supervisor SR for high A-trap handler: pc=0x%08" PRIx32
                 " old_sr=0x%04" PRIx32 " saved_sr=0x%04" PRIx32
                 " new_sr=0x%04" PRIx32 " sp=0x%08x usp=0x%08x",
                 pc, sr & 0xffffu, saved_sr & 0xffffu, fixed & 0xffffu,
                 m68k_get_reg(NULL, M68K_REG_SP), m68k_get_reg(NULL, M68K_REG_USP));
    }
}

static void lc_musashi_bus_maybe_log_post_reset_high_trap_dispatch_entry(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000099c6u && rom_offset != 0x000099e0u) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t d2 = m68k_get_reg(NULL, M68K_REG_D2);
    const int16_t d2w = (int16_t)(d2 & 0xffffu);
    const uint32_t table_base = rom_offset == 0x000099e0u ? 0x00000e00u : 0x00001e00u;
    const uint32_t computed = table_base + ((int32_t)d2w * 4);
    const uint32_t selected_handler = lc_musashi_bus_peek_ram32(computed);
    const uint32_t post_trap_pc = m68k_get_reg(NULL, M68K_REG_A2);
    const uint32_t frame_pc = ((uint32_t)lc_musashi_bus_peek_ram16(sp + 10u) << 16u) |
                              (uint32_t)lc_musashi_bus_peek_ram16(sp + 12u);
    const uint16_t trap_word = lc_musashi_bus_peek_ram16(frame_pc);
    const uint32_t second_return = lc_musashi_bus_peek_ram32(sp + 16u);

    // If the trap word reads as 0xffff, the trap table entry is uninitialized.
    // Do NOT try to stub here — the stack frame layout varies.  The ROM patch
    // NOPs individual problematic trap call sites instead (A895, A995, etc.).
    // Just log and let the dispatch proceed; it will either reach a valid
    // handler or be caught by other guards.
    if (trap_word == 0xffffu && post_reset_high_dispatch_logs < 32u) {
        ESP_LOGW(TAG,
                 "LC uninitialized high A-trap dispatch (0xffff): pc=0x%08" PRIx32
                 " handler=0x%08" PRIx32 " frame_pc=0x%08" PRIx32
                 " post_trap=0x%08" PRIx32 " sp=0x%08" PRIx32,
                 pc, selected_handler, frame_pc, post_trap_pc, sp);
    }

    // Intercept _GetResource (A9A0) for 'boot' resources.
    // The boot block code at $976 pushes: type(4) + id(2) then calls A9A0.
    // Pascal calling convention: result space(4) is at SP above the params.
    // At dispatch entry (0x99c6), the original stack frame has been rearranged.
    // The params were pushed before the A-line, so they're above the exception frame.
    // We detect 'boot' type from the stack and return pre-loaded handles.
    if (trap_word == 0xa9a0u && active_bus != NULL && active_bus->ram != NULL) {
        // For the >AC00 path at 0x99c6: params are at sp + saved_regs + exception_frame
        // sp+0=saved_d2, sp+4=saved_a2, sp+8=handler_slot, sp+12=return_slot
        // Above that: sp+16..sp+19 = saved SR+PC exception (now overwritten)
        // The actual params pushed by caller are further up.
        // Simpler: read the type from frame_pc context. The caller pushed:
        //   subq.l #4, sp        ; result space
        //   move.l #'boot', -(sp) ; type
        //   move.w #id, -(sp)    ; id
        //   _GetResource
        // So at frame_pc-6 we have the ID word, frame_pc-10 has the type.
        // But frame_pc is in RAM at $976. Let's read from there:
        const uint32_t id_addr = frame_pc - 4u; // The move.w #id instruction's immediate
        const uint16_t res_id = lc_musashi_bus_peek_ram16(frame_pc - 4u);
        // Actually the pushed values are on the STACK, not at the code.
        // After the A-line fires, SP has exception frame. Above that are the pushed params.
        // Let's just check if frame_pc == $976 (our known boot block GetResource site)
        if (frame_pc == 0x00000976u || frame_pc == 0x00000a76u) {
            // This is the boot block's GetResource call. Read ID from code:
            // At $972: move.w #2, -(sp) → the 2 is at $974
            const uint16_t boot_id = lc_musashi_bus_peek_ram16(frame_pc - 2u);
            uint32_t handle = 0;
            if (boot_id == 2u) handle = 0x0004ff00u;
            else if (boot_id == 3u) handle = 0x0004ff08u;
            if (handle != 0u) {
                // Write handle to the result space on stack.
                // The ROM dispatcher will RTS to post_trap_pc (A2).
                // For Pascal convention: result is at specific stack offset.
                // Simplest: just set A0 and D0 to the handle (some callers check either).
                m68k_set_reg(M68K_REG_A0, handle);
                m68k_set_reg(M68K_REG_D0, handle);
                ESP_LOGW(TAG,
                         "LC intercepted _GetResource(boot, %u): handle=0x%08" PRIx32,
                         boot_id, handle);
            }
        }
    }

    if (post_reset_high_dispatch_logs >= 256u) {
        return;
    }
    ESP_LOGW(TAG,
             "LC post-reset high A-trap dispatch trace: pc=0x%08" PRIx32
             " trap=0x%04x sp=0x%08" PRIx32 " stack=%08" PRIx32 " %08" PRIx32
             " %08" PRIx32 " %08" PRIx32 " next=%08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x table=0x%08" PRIx32
             " handler=0x%08" PRIx32 " frame_pc=0x%08" PRIx32
             " post_trap=0x%08" PRIx32 " second_return=0x%08" PRIx32
             " a0=0x%08x a1=0x%08x a2=0x%08x",
             pc, trap_word, sp,
             lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
             lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
             second_return, m68k_get_reg(NULL, M68K_REG_D0),
             m68k_get_reg(NULL, M68K_REG_D1), d2, computed, selected_handler,
             frame_pc, post_trap_pc, second_return, m68k_get_reg(NULL, M68K_REG_A0),
             m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2));
    post_reset_high_dispatch_logs++;
}

static void lc_musashi_bus_maybe_fix_post_reset_high_trap_dispatch_return(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000099c6u && rom_offset != 0x000099d6u &&
        rom_offset != 0x000099e0u && rom_offset != 0x000099ecu) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t target = lc_musashi_bus_peek_ram32(sp);
    if ((target & 1u) == 0u && target >= 0x00000400u && target < 0xff000000u) {
        return;
    }
    const uint32_t old_next = lc_musashi_bus_peek_ram32(sp + 4u);
    const uint32_t frame_pc = ((uint32_t)lc_musashi_bus_peek_ram16(sp + 10u) << 16u) |
                              (uint32_t)lc_musashi_bus_peek_ram16(sp + 12u);
    lc_musashi_bus_ram_write32(sp, 0x40800d88u);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC repaired post-reset high A-trap dispatch return: pc=0x%08" PRIx32
                 " sp=0x%08" PRIx32 " old_target=0x%08" PRIx32
                 " new_target=0x40800d88 old_next=0x%08" PRIx32
                 " frame_pc=0x%08" PRIx32
                 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
                 " a0=0x%08x a1=0x%08x a2=0x%08x d0=0x%08x d1=0x%08x d2=0x%08x",
                 pc, sp, target, old_next, frame_pc,
                 lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
                 lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
                 lc_musashi_bus_peek_ram32(sp + 16u), lc_musashi_bus_peek_ram32(sp + 20u),
                 lc_musashi_bus_peek_ram32(sp + 24u), lc_musashi_bus_peek_ram32(sp + 28u),
                 m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                 m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_D0),
                 m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2));
    }
}

static void lc_musashi_bus_maybe_stub_post_reset_no_mmu_a001(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00001314u) {
        return;
    }
    // After the no-MMU InitMMU return, the ROM reaches an early A001 OS trap
    // before the normal low-memory trap vectors are usable in this direct
    // micro-probe.  Skip only this observed reset-continuation trap so the
    // following ROM code can initialize the memory globals and continue toward
    // the main reset path.
    m68k_set_reg(M68K_REG_PC, pc + 2u);
    if (!post_reset_no_mmu_a001_stub_logged) {
        post_reset_no_mmu_a001_stub_logged = true;
        ESP_LOGW(TAG,
                 "LC stubbed post-reset no-MMU A001 trap: pc=0x%08" PRIx32
                 " next_pc=0x%08" PRIx32,
                 pc, pc + 2u);
    }
}

static void lc_musashi_bus_maybe_seed_post_reset_no_mmu_return(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x000416a2u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t a5 = m68k_get_reg(NULL, M68K_REG_A5);
    if (a5 + 7u < active_bus->ram_size) {
        // This diagnostic enters the reset body directly at 0x2e00, so the
        // threaded InitMMU tail-call frame that should return to 0x4080130a is
        // not naturally present.  Seed only the observed return slot when the
        // ROM takes the no-MMU path; this replaces the later expanded-path
        // finalizer/FPU detour with the continuation the ROM's 0x40801306 JMP
        // is arranged to use.
        lc_musashi_bus_ram_write32(a5 + 4u, 0x4080130au);
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGW(TAG,
                     "LC seeded post-reset no-MMU return frame: pc=0x%08" PRIx32
                     " a5=0x%08" PRIx32 " return_slot=0x%08" PRIx32
                     " value=0x4080130a",
                     pc, a5, a5 + 4u);
        }
    }
}

static void lc_musashi_bus_maybe_fix_post_reset_handoff_state(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00041814u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t a5 = m68k_get_reg(NULL, M68K_REG_A5);
    const uint32_t save = a5 - 0x18u;
    if (save + 23u < active_bus->ram_size) {
        // Rebuild the register save area consumed by MOVEM.L -$18(A5),D0-D2/D7/A0-A1.
        // The temporary post-reset descriptor path currently overwrites this
        // top-of-RAM save area with RAM-test/finalizer data; restore the values
        // seen at the post-reset memory-layout entry so the next handoff failure
        // is about real no-FPU/address-map behavior instead of smashed saves.
        lc_musashi_bus_ram_write32(save + 0u, 0x00001f3fu);    // D0
        lc_musashi_bus_ram_write32(save + 4u, 0x00000000u);    // D1
        lc_musashi_bus_ram_write32(save + 8u, 0xdc000304u);    // D2
        lc_musashi_bus_ram_write32(save + 12u, 0x00030002u);   // D7
        lc_musashi_bus_ram_write32(save + 16u, 0x408033e8u);   // A0
        lc_musashi_bus_ram_write32(save + 20u, 0x40803640u);   // A1
    }
    m68k_set_reg(M68K_REG_SP, 0x001fff7cu);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC repaired post-reset handoff save area: pc=0x%08" PRIx32
                 " a5=0x%08" PRIx32 " save=0x%08" PRIx32 " sp=0x001fff7c",
                 pc, a5, save);
    }
}

static bool lc_musashi_bus_maybe_canonicalize_sr_prefixed_rom_pc(uint32_t pc) {
    if ((pc & 0xff000000u) != 0x27000000u || active_bus == NULL || active_bus->rom == NULL) {
        return false;
    }
    uint32_t ignored_offset = 0;
    if (lc_musashi_bus_rom_offset_for_address(pc, &ignored_offset)) {
        return false;
    }
    const uint32_t offset = pc & 0x000fffffu;
    if (offset < 0x000000aau || offset >= active_bus->rom_size || (offset & 1u) != 0u) {
        return false;
    }
    const uint32_t canonical = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE + offset;
    m68k_set_reg(M68K_REG_PC, canonical);
    if (post_reset_trap_return_canonicalize_logs < 16u) {
        ESP_LOGW(TAG,
                 "LC canonicalized SR-prefixed ROM PC: pc=0x%08" PRIx32
                 " canonical=0x%08" PRIx32 " prev_pc=0x%08" PRIx32
                 " sp=0x%08x d0=0x%08x d1=0x%08x d2=0x%08x a2=0x%08x",
                 pc, canonical, previous_instruction_pc, m68k_get_reg(NULL, M68K_REG_SP),
                 m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                 m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_A2));
        post_reset_trap_return_canonicalize_logs++;
    }
    return true;
}

static void lc_musashi_bus_maybe_log_post_reset_ram_execution(uint32_t pc) {
    const bool top_stack_trampoline = pc >= 0x001fdfb0u && pc < 0x001fe020u;
    const bool heap_code = pc >= 0x00100000u && pc < 0x00110000u;
    if (post_reset_ram_exec_logs >= 16u || (!top_stack_trampoline && !heap_code)) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    ESP_LOGW(TAG,
             "LC post-reset RAM execution trace: pc=0x%08" PRIx32
             " prev_pc=0x%08" PRIx32 " class=%s words=%04x %04x %04x %04x"
             " sp=0x%08" PRIx32 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d7=0x%08x"
             " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x sr=0x%04x",
             pc, previous_instruction_pc, heap_code ? "heap" : "top-stack",
             lc_musashi_bus_peek_ram16(pc + 0u), lc_musashi_bus_peek_ram16(pc + 2u),
             lc_musashi_bus_peek_ram16(pc + 4u), lc_musashi_bus_peek_ram16(pc + 6u), sp,
             lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
             lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
             m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D7),
             m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
             m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3),
             m68k_get_reg(NULL, M68K_REG_A4), m68k_get_reg(NULL, M68K_REG_SR));
    post_reset_ram_exec_logs++;
}

static bool lc_musashi_bus_post_reset_plausible_univ_info(uint32_t value) {
    return (value >= 0x40800000u && value < 0x40880000u) ||
           (value >= 0x00001000u && active_bus != NULL &&
            value + 0x40u < active_bus->ram_size);
}

static void lc_musashi_bus_maybe_capture_post_reset_univ_info(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00000120u || post_reset_univ_info_observed != 0u) {
        return;
    }
    const uint32_t candidate = m68k_get_reg(NULL, M68K_REG_A1);
    if (lc_musashi_bus_post_reset_plausible_univ_info(candidate)) {
        post_reset_univ_info_observed = candidate;
        ESP_LOGI(TAG,
                 "LC captured ROM-selected ProductInfo/UnivInfo pointer: pc=0x%08" PRIx32
                 " a1=0x%08" PRIx32,
                 pc, candidate);
    }
}

static void lc_musashi_bus_maybe_seed_post_reset_probe_tables(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_probe_tables_seeded || rom_offset != 0x00005e48u ||
        active_bus == NULL || active_bus->ram == NULL || active_bus->ram_size <= 0x00009120u) {
        return;
    }
    post_reset_probe_tables_seeded = true;

    // The direct post-reset probe has not run the ROM initializer at 0x40806d60
    // that allocates the low-memory dispatch table at $0DB8.  Seed only the two
    // fault-probe trampoline entries consumed by 0x40805e6e/0x40805e9e.  Each
    // entry points at the ROM "moveq #0,d0; rts" helper so the helper consumes
    // the continuation already pushed by the ROM probe code.  Seed $0DD8 to a
    // tiny zero descriptor so the probe's bitmap base is RAM-owned instead of
    // RAM-fill pattern bytes.  Non-zero ProductInfo.DefaultRSRCs values were
    // tested separately and currently drive the direct probe into diagnostic
    // monitor paths before the surrounding ProductInfo/ROM resource structures
    // are complete.
    lc_musashi_bus_ram_write32(0x00000db8u, 0x00009000u);
    const uint32_t old_univ_info = lc_musashi_bus_peek_ram32(0x00000dd8u);
    const bool old_univ_info_plausible =
        lc_musashi_bus_post_reset_plausible_univ_info(old_univ_info);
    uint32_t active_univ_info = old_univ_info;
    if (old_univ_info_plausible) {
        // Keep a valid ROM-selected ProductInfo pointer if it survived the RAM
        // fill/check path.
    } else if (post_reset_univ_info_observed != 0u) {
        active_univ_info = post_reset_univ_info_observed;
        lc_musashi_bus_ram_write32(0x00000dd8u, active_univ_info);
    } else if (lc_musashi_bus_basilisk_slot_rom_active()) {
        const uint32_t univ = lc_basilisk_find_universal_info(active_bus->rom, active_bus->rom_size);
        if (univ != 0u) {
            active_univ_info = LC_BASILISK_ROM_BASE_32 + univ;
            lc_musashi_bus_ram_write32(0x00000dd8u, active_univ_info);
        } else {
            active_univ_info = 0x00009100u;
            lc_musashi_bus_ram_write32(0x00000dd8u, active_univ_info);
        }
    } else {
        active_univ_info = 0x00009100u;
        lc_musashi_bus_ram_write32(0x00000dd8u, active_univ_info);
    }
    for (uint32_t addr = 0x00009000u; addr < 0x000090f4u; addr += 4u) {
        lc_musashi_bus_ram_write32(addr, 0x40800d88u);
    }
    if (active_univ_info == 0x00009100u) {
        for (uint32_t addr = 0x00009100u; addr < 0x00009120u; addr += 4u) {
            lc_musashi_bus_ram_write32(addr, 0x00000000u);
        }
        lc_musashi_bus_ram_write8(0x00009116u, (uint8_t)LC_PRODUCTINFO_DEFAULT_RSRCS);
        lc_musashi_bus_ram_write8(0x00009117u, 0x01u); // ProductInfoVersion.
    }
    ESP_LOGW(TAG,
             "LC seeded post-reset probe low-memory tables: pc=0x%08" PRIx32
             " dispatch_0db8=0x00009000 old_0dd8=0x%08" PRIx32
             " observed_0dd8=0x%08" PRIx32 " active_0dd8=0x%08" PRIx32
             " product_default_rsrcs=%u basilisk_rom_univ=%u trampoline=0x40800d88",
             pc, old_univ_info, post_reset_univ_info_observed,
             lc_musashi_bus_peek_ram32(0x00000dd8u),
             (unsigned)LC_PRODUCTINFO_DEFAULT_RSRCS,
             active_univ_info >= LC_BASILISK_ROM_BASE_32 && active_univ_info < LC_BASILISK_ROM_BASE_32 + 0x80000u);
}

static void lc_musashi_bus_maybe_skip_bad_high_trap_handler(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00031abau) {
        return;
    }
    const uint32_t a4 = m68k_get_reg(NULL, M68K_REG_A4);
    // The current direct host reset path can enter the high A-trap handler at
    // 0x40831ab0 with a bogus frame_pc/header-derived table selection.  Let the
    // function's prologue run so its stack frame exists, then skip the bad
    // double-dereference at 0x40831aba by returning through its normal epilogue.
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_PC, 0x40831b24u);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC skipped bogus high A-trap handler dereference: pc=0x%08" PRIx32
                 " a4=0x%08" PRIx32 " epilogue=0x40831b24",
                 pc, a4);
    }
}

static void lc_musashi_bus_maybe_repair_bad_heap_rts(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    // The ROM helper at 0x4080b0c6 builds a temporary memory/header block, then
    // RTSes.  With the compact host Memory Manager, its RTS slot can contain
    // the newly allocated heap data pointer (0x0010038e in the current path)
    // while the following stack long is the real ROM continuation.  Repair only
    // this observed helper epilogue so the next frontier is the caller's ROM
    // path, not execution of resource/header data as code.
    if (rom_offset != 0x0000b138u) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 7u >= active_bus->ram_size) {
        return;
    }
    const uint32_t target = lc_musashi_bus_peek_ram32(sp);
    const uint32_t next = lc_musashi_bus_peek_ram32(sp + 4u);
    if (!(target >= 0x00100000u && target < 0x00110000u &&
          lc_musashi_bus_post_reset_plausible_rom_pc(next))) {
        return;
    }
    lc_musashi_bus_ram_write32(sp, next);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC repaired heap-data RTS target: pc=0x%08" PRIx32
                 " sp=0x%08" PRIx32 " old_target=0x%08" PRIx32
                 " new_target=0x%08" PRIx32,
                 pc, sp, target, next);
    }
}

static void lc_musashi_bus_maybe_repair_post_reset_redomap_rts(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0001ab20u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 7u >= active_bus->ram_size) {
        return;
    }
    const uint32_t target = lc_musashi_bus_peek_ram32(sp);
    const uint32_t next = lc_musashi_bus_peek_ram32(sp + 4u);
    if (target != LC_MEMORY_ZONE_DEFAULT_START || !lc_musashi_bus_post_reset_plausible_rom_pc(next)) {
        return;
    }
    lc_musashi_bus_ram_write32(sp, next);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC repaired ReDoMap RTS target: pc=0x%08" PRIx32
                 " sp=0x%08" PRIx32 " old_target=0x%08" PRIx32
                 " new_target=0x%08" PRIx32,
                 pc, sp, target, next);
    }
}

static void lc_musashi_bus_maybe_log_post_reset_shutdown_rts(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (post_reset_shutdown_rts_logged || rom_offset != 0x00000584u) {
        return;
    }
    post_reset_shutdown_rts_logged = true;
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    ESP_LOGW(TAG,
             "LC post-reset ShutDown helper RTS trace: pc=0x%08" PRIx32
             " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
             " return=0x%08" PRIx32 " stack=%08" PRIx32 " %08" PRIx32
             " %08" PRIx32 " %08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d7=0x%08x"
             " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x sr=0x%04x",
             pc, previous_instruction_pc, sp, lc_musashi_bus_peek_ram32(sp),
             lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
             lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
             m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_D7),
             m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
             m68k_get_reg(NULL, M68K_REG_A2), m68k_get_reg(NULL, M68K_REG_A3),
             m68k_get_reg(NULL, M68K_REG_SR));
}

static bool lc_musashi_bus_post_reset_plausible_rom_pc(uint32_t value) {
    uint32_t offset = 0;
    return lc_musashi_bus_rom_offset_for_address(value, &offset) &&
           active_bus != NULL && active_bus->rom != NULL && offset < active_bus->rom_size &&
           (value & 1u) == 0u;
}

static bool lc_musashi_bus_maybe_rescue_post_reset_low_dispatch_fallthrough(uint32_t pc) {
    uint32_t ignored_offset = 0;
    const bool plausible_rom = lc_musashi_bus_rom_offset_for_address(pc, &ignored_offset);
    const bool suspicious_low = instruction_callback_count > 1000u && pc < 0x00010000u;
    const bool suspicious_high = instruction_callback_count > 1000u && !plausible_rom &&
                                  pc >= 0x01000000u;
    if ((!suspicious_low && !suspicious_high) || active_bus == NULL || active_bus->ram == NULL ||
        !active_bus->initialized) {
        return false;
    }
    uint32_t previous_rom_offset = 0;
    const bool previous_plausible_rom = lc_musashi_bus_rom_offset_for_address(previous_instruction_pc,
                                                                              &previous_rom_offset);
    if (suspicious_high && !previous_plausible_rom) {
        return false;
    }
    const uint32_t swapped_word_pc = ((pc & 0x0000ffffu) << 16u) | ((pc >> 16u) & 0x0000ffffu);
    if (suspicious_high && lc_musashi_bus_post_reset_plausible_rom_pc(swapped_word_pc)) {
        m68k_set_reg(M68K_REG_PC, swapped_word_pc);
        if (post_reset_trap_return_canonicalize_logs < 16u) {
            ESP_LOGW(TAG,
                     "LC rescued word-swapped ROM PC fallthrough: pc=0x%08" PRIx32
                     " prev_pc=0x%08" PRIx32 " canonical=0x%08" PRIx32
                     " sp=0x%08x d0=0x%08x d1=0x%08x d2=0x%08x a2=0x%08x",
                     pc, previous_instruction_pc, swapped_word_pc,
                     m68k_get_reg(NULL, M68K_REG_SP), m68k_get_reg(NULL, M68K_REG_D0),
                     m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                     m68k_get_reg(NULL, M68K_REG_A2));
            post_reset_trap_return_canonicalize_logs++;
        }
        return true;
    }

    if (suspicious_low && (!previous_plausible_rom ||
                           (previous_rom_offset != 0x000060b0u &&
                            previous_rom_offset != 0x000060eau &&
                            previous_rom_offset != 0x0000636cu &&
                            previous_rom_offset != 0x000063a4u &&
                            previous_rom_offset != 0x0001ab20u &&
                            previous_rom_offset != 0x0001d78cu &&
                            previous_rom_offset != 0x0001d79cu))) {
        return false;
    }

    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (sp + 3u >= active_bus->ram_size) {
        return false;
    }
    if ((suspicious_low || suspicious_high) &&
        (pc == 0u || pc == 0xffff0000u) && previous_rom_offset == 0x000063a4u &&
        (m68k_get_reg(NULL, M68K_REG_D0) & 0xffffu) == 0xfec7u) {
        // The synthetic Slot Manager selector-0x000c failure path can reach the
        // helper RTS with the low synthetic SRT frame top already consumed.  The
        // real caller branches to the per-slot cleanup at 0x40806358 when D0 is
        // non-zero; resume there rather than decoding the zeroed SRT frame as a
        // return address.
        m68k_set_reg(M68K_REG_PC, 0x40806358u);
        if (post_reset_trap_return_canonicalize_logs < 16u) {
            ESP_LOGW(TAG,
                     "LC repaired Slot Manager selector helper zero RTS return: pc=0x%08" PRIx32
                     " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
                     " d0=0x%08x d1=0x%08x a0=0x%08x target=0x40806358",
                     pc, previous_instruction_pc, sp, m68k_get_reg(NULL, M68K_REG_D0),
                     m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_A0));
            post_reset_trap_return_canonicalize_logs++;
        }
        return true;
    }
    if (suspicious_low && pc == 0u && previous_rom_offset == 0x000060eau) {
        m68k_set_reg(M68K_REG_PC, 0x40805df6u);
        if (post_reset_trap_return_canonicalize_logs < 16u) {
            ESP_LOGW(TAG,
                     "LC repaired Slot Manager first-pass zero RTS return: pc=0x%08" PRIx32
                     " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
                     " d0=0x%08x d1=0x%08x a0=0x%08x target=0x40805df6",
                     pc, previous_instruction_pc, sp, m68k_get_reg(NULL, M68K_REG_D0),
                     m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_A0));
            post_reset_trap_return_canonicalize_logs++;
        }
        return true;
    }
    if (suspicious_low && previous_rom_offset == 0x0000636cu &&
        (pc == 0u || (pc >= 0x00009000u && pc < 0x0000a000u))) {
        m68k_set_reg(M68K_REG_PC, 0x40805df6u);
        if (post_reset_trap_return_canonicalize_logs < 16u) {
            ESP_LOGW(TAG,
                     "LC repaired Slot Manager SRT helper low RTS return: pc=0x%08" PRIx32
                     " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
                     " d0=0x%08x d1=0x%08x a0=0x%08x target=0x40805df6",
                     pc, previous_instruction_pc, sp, m68k_get_reg(NULL, M68K_REG_D0),
                     m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_A0));
            post_reset_trap_return_canonicalize_logs++;
        }
        return true;
    }
    if (suspicious_low && pc == 0u && sp + 7u < active_bus->ram_size) {
        const uint32_t arg = lc_musashi_bus_peek_ram32(sp);
        const uint32_t outer_return = lc_musashi_bus_peek_ram32(sp + 4u);
        uint32_t target = 0;
        if (previous_rom_offset == 0x0001d78cu && arg == 0x00000034u) {
            target = 0x4081cc68u;
        } else if (previous_rom_offset == 0x0001d79cu && arg == 0x00000020u) {
            target = 0x4081cc7eu;
        }
        if (target != 0u && lc_musashi_bus_post_reset_plausible_rom_pc(outer_return)) {
            m68k_set_reg(M68K_REG_PC, target);
            if (post_reset_trap_return_canonicalize_logs < 16u) {
                ESP_LOGW(TAG,
                         "LC repaired ResourceMgr wrapper zero RTS return: pc=0x%08" PRIx32
                         " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
                         " arg=0x%08" PRIx32 " outer=0x%08" PRIx32
                         " target=0x%08" PRIx32,
                         pc, previous_instruction_pc, sp, arg, outer_return, target);
                post_reset_trap_return_canonicalize_logs++;
            }
            return true;
        }
    }
    const uint32_t slots[] = {sp, sp + 4u, sp + 8u, sp + 12u};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        const uint32_t slot = slots[i];
        if (slot + 3u >= active_bus->ram_size) {
            continue;
        }
        uint32_t target = lc_musashi_bus_peek_ram32(slot);
        if (!lc_musashi_bus_post_reset_plausible_rom_pc(target) &&
            lc_musashi_bus_post_reset_plausible_stripped_rom_return(target)) {
            target += LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
        }
        if (!lc_musashi_bus_post_reset_plausible_rom_pc(target)) {
            continue;
        }
        const uint32_t new_sp = slot + 4u;
        m68k_set_reg(M68K_REG_PC, target);
        m68k_set_reg(M68K_REG_SP, new_sp);
        if (post_reset_trap_return_canonicalize_logs < 16u) {
            ESP_LOGW(TAG,
                     "LC rescued invalid-PC fallthrough using stack ROM target: pc=0x%08" PRIx32
                     " prev_pc=0x%08" PRIx32 " sp=0x%08" PRIx32
                     " slot=0x%08" PRIx32 " target=0x%08" PRIx32 " new_sp=0x%08" PRIx32
                     " d0=0x%08x d1=0x%08x d2=0x%08x a2=0x%08x",
                     pc, previous_instruction_pc, sp, slot, target, new_sp,
                     m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
                     m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_A2));
            post_reset_trap_return_canonicalize_logs++;
        }
        return true;
    }
    return false;
}

static void lc_musashi_bus_maybe_log_post_reset_vbl_init_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0000a350u || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    post_reset_vbl_init_trace_hits++;
    const uint32_t old_vbl_flags = lc_musashi_bus_peek_ram16(0x00000160u);
    const uint32_t old_vbl_head = lc_musashi_bus_peek_ram32(0x00000162u);
    const uint32_t old_vbl_tail = lc_musashi_bus_peek_ram32(0x00000166u);
    const bool bad_head = old_vbl_head != 0u &&
                          ((old_vbl_head & 1u) != 0u || old_vbl_head < 0x00001000u ||
                           old_vbl_head + 0x0fu >= active_bus->ram_size);
    const bool bad_tail = old_vbl_tail != 0u &&
                          ((old_vbl_tail & 1u) != 0u || old_vbl_tail < 0x00001000u ||
                           old_vbl_tail + 0x0fu >= active_bus->ram_size);
    if (bad_head || bad_tail) {
        lc_musashi_bus_ram_write16(0x00000160u, 0);
        lc_musashi_bus_ram_write32(0x00000162u, 0);
        lc_musashi_bus_ram_write32(0x00000166u, 0);
        if (!post_reset_vbl_queue_repair_logged) {
            post_reset_vbl_queue_repair_logged = true;
            ESP_LOGW(TAG,
                     "LC repaired corrupted post-reset VBL queue header: pc=0x%08" PRIx32
                     " hits=%" PRIu32 " old_flags=0x%04" PRIx32
                     " old_head=0x%08" PRIx32 " old_tail=0x%08" PRIx32,
                     pc, post_reset_vbl_init_trace_hits, old_vbl_flags & 0xffffu,
                     old_vbl_head, old_vbl_tail);
        }
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t stack0 = lc_musashi_bus_peek_ram32(sp + 0u);
    if (post_reset_vbl_init_trace_hits >= 2u && stack0 == 0x40801224u) {
        m68k_set_reg(M68K_REG_D0, 0);
        m68k_set_reg(M68K_REG_SP, sp + 4u);
        m68k_set_reg(M68K_REG_PC, stack0);
        if (!post_reset_vbl_loop_escape_logged) {
            post_reset_vbl_loop_escape_logged = true;
            ESP_LOGW(TAG,
                     "LC escaped repeated post-reset VBL/time hardware-init loop: pc=0x%08" PRIx32
                     " hits=%" PRIu32 " old_sp=0x%08" PRIx32 " new_sp=0x%08" PRIx32
                     " return=0x%08" PRIx32 " d1=0x%08x d2=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x",
                     pc, post_reset_vbl_init_trace_hits, sp, sp + 4u, stack0,
                     m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
                     m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A1),
                     m68k_get_reg(NULL, M68K_REG_A2));
        }
        return;
    }
    const bool log_hit = post_reset_vbl_init_trace_hits <= 8u ||
                         post_reset_vbl_init_trace_hits == 16u ||
                         post_reset_vbl_init_trace_hits == 64u ||
                         post_reset_vbl_init_trace_hits == 256u ||
                         post_reset_vbl_init_trace_hits == 1024u ||
                         (post_reset_vbl_init_trace_hits % 4096u) == 0u;
    if (!log_hit) {
        return;
    }
    const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    ESP_LOGW(TAG,
             "LC post-reset VBL/time init trace: pc=0x%08" PRIx32
             " hits=%" PRIu32 " sr=0x%04" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x"
             " sp=0x%08" PRIx32 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " vec1=0x%08" PRIx32 " vbl_head=0x%08" PRIx32 " ticks=0x%08" PRIx32
             " jvbl=0x%08" PRIx32 " active_vbl=0x%08" PRIx32 " qhdr=0x%08" PRIx32,
             pc, post_reset_vbl_init_trace_hits, sr & 0xffffu,
             m68k_get_reg(NULL, M68K_REG_D0), m68k_get_reg(NULL, M68K_REG_D1),
             m68k_get_reg(NULL, M68K_REG_D2), m68k_get_reg(NULL, M68K_REG_A0),
             m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
             m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_A4),
             sp, stack0, lc_musashi_bus_peek_ram32(sp + 4u),
             lc_musashi_bus_peek_ram32(sp + 8u), lc_musashi_bus_peek_ram32(sp + 12u),
             lc_musashi_bus_peek_ram32(0x00000064u), lc_musashi_bus_peek_ram32(0x00000162u),
             lc_musashi_bus_peek_ram32(0x0000016au), lc_musashi_bus_peek_ram32(0x00000d28u),
             lc_musashi_bus_peek_ram32(0x00000d10u), lc_musashi_bus_peek_ram32(0x00000362u));
}

static void lc_musashi_bus_maybe_allow_post_reset_slot_init_handoff(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x00001228u || !post_reset_vbl_loop_escape_logged) {
        return;
    }
    const uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
    if (d0 == 0u) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    const uint32_t sr = m68k_get_reg(NULL, M68K_REG_SR);
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_SR, (sr & ~0x000bu) | 0x0004u);
    if (!post_reset_slot_init_success_logged) {
        post_reset_slot_init_success_logged = true;
        ESP_LOGW(TAG,
                 "LC allowed post-reset Slot Manager init handoff after synthetic no-PDS scan: pc=0x%08" PRIx32
                 " old_d0=0x%08" PRIx32 " sp=0x%08" PRIx32
                 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                 pc, d0, sp, lc_musashi_bus_peek_ram32(sp + 0u),
                 lc_musashi_bus_peek_ram32(sp + 4u), lc_musashi_bus_peek_ram32(sp + 8u));
    }
}

static void lc_musashi_bus_maybe_capture_post_reset_event_wait_record(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if ((rom_offset != 0x0000ef98u && rom_offset != 0x0000efa2u &&
         rom_offset != 0x0000efd8u && rom_offset != 0x0000efdau) ||
        active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    const uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    if (a0 + 0x31u >= active_bus->ram_size) {
        return;
    }
    post_reset_event_wait_saved_a0 = a0;
    post_reset_event_wait_saved_sp = m68k_get_reg(NULL, M68K_REG_SP);
    if (rom_offset == 0x0000ef98u && a0 + 0x11u < active_bus->ram_size) {
        lc_musashi_bus_ram_write32(0x00000362u, a0);
    }
    if (!post_reset_event_wait_capture_logged) {
        post_reset_event_wait_capture_logged = true;
        ESP_LOGW(TAG,
                 "LC captured post-reset Event Manager local record: pc=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " sp=0x%08" PRIx32
                 " status=0x%04x callback=0x%08" PRIx32 " qhdr=0x%08" PRIx32,
                 pc, a0, post_reset_event_wait_saved_sp,
                 lc_musashi_bus_peek_ram16(a0 + 0x10u),
                 lc_musashi_bus_peek_ram32(a0 + 0x08u),
                 lc_musashi_bus_peek_ram32(0x00000362u));
    }
}

static void lc_musashi_bus_maybe_skip_repeated_post_reset_event_wait_loop(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0000efdau || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    if (a0 + 0x11u >= active_bus->ram_size) {
        a0 = lc_musashi_bus_peek_ram32(0x00000362u);
    }
    if (a0 + 0x11u >= active_bus->ram_size) {
        return;
    }
    post_reset_event_wait_loop_calls++;
    if (post_reset_event_wait_loop_calls < 1024u) {
        return;
    }
    const uint16_t old_status = lc_musashi_bus_peek_ram16(a0 + 0x10u);
    lc_musashi_bus_ram_write16(a0 + 0x10u, 0);
    m68k_set_reg(M68K_REG_A0, a0);
    m68k_set_reg(M68K_REG_D0, 0);
    m68k_set_reg(M68K_REG_PC, 0x4080efdcu);
    static bool logged = false;
    if (!logged) {
        logged = true;
        ESP_LOGW(TAG,
                 "LC skipped repeated post-reset Event Manager wait loop: pc=0x%08" PRIx32
                 " calls=%" PRIu32 " a0=0x%08" PRIx32 " old_status=0x%04x target=0x4080efdc",
                 pc, post_reset_event_wait_loop_calls, a0, old_status);
    }
}

static void lc_musashi_bus_maybe_complete_post_reset_event_wait(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset != 0x0000efdeu || active_bus == NULL || active_bus->ram == NULL) {
        return;
    }
    uint32_t a0 = m68k_get_reg(NULL, M68K_REG_A0);
    uint32_t restored_a0 = 0;
    const bool a0_in_ram = a0 + 0x11u < active_bus->ram_size;
    if (!a0_in_ram && post_reset_event_wait_saved_a0 != 0u &&
        post_reset_event_wait_saved_a0 + 0x11u < active_bus->ram_size) {
        restored_a0 = post_reset_event_wait_saved_a0;
        a0 = restored_a0;
    }
    if (a0 + 0x11u >= active_bus->ram_size) {
        const uint32_t candidate = lc_musashi_bus_peek_ram32(0x00000362u);
        if (candidate + 0x11u >= active_bus->ram_size) {
            static bool rejected_logged = false;
            if (!rejected_logged) {
                rejected_logged = true;
                ESP_LOGW(TAG,
                         "LC post-reset Event Manager wait has no RAM queue block: pc=0x%08" PRIx32
                         " a0=0x%08" PRIx32 " saved_a0=0x%08" PRIx32
                         " saved_sp=0x%08" PRIx32 " qhdr=0x%08" PRIx32
                         " sp=0x%08x stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32,
                         pc, m68k_get_reg(NULL, M68K_REG_A0), post_reset_event_wait_saved_a0,
                         post_reset_event_wait_saved_sp, candidate, m68k_get_reg(NULL, M68K_REG_SP),
                         lc_musashi_bus_peek_ram32(m68k_get_reg(NULL, M68K_REG_SP) + 0u),
                         lc_musashi_bus_peek_ram32(m68k_get_reg(NULL, M68K_REG_SP) + 4u),
                         lc_musashi_bus_peek_ram32(m68k_get_reg(NULL, M68K_REG_SP) + 8u));
            }
            return;
        }
        restored_a0 = candidate;
        a0 = candidate;
    }
    const uint16_t status = lc_musashi_bus_peek_ram16(a0 + 0x10u);
    if (restored_a0 != 0u) {
        m68k_set_reg(M68K_REG_A0, restored_a0);
    }
    if ((int16_t)status <= 0) {
        if (restored_a0 != 0u) {
            m68k_set_reg(M68K_REG_D0, 0);
        }
        post_reset_event_wait_hits = 0;
        return;
    }
    post_reset_event_wait_hits++;
    if (post_reset_event_wait_hits < 1024u) {
        return;
    }
    lc_musashi_bus_ram_write16(a0 + 0x10u, 0);
    m68k_set_reg(M68K_REG_A0, a0);
    m68k_set_reg(M68K_REG_D0, 0);
    if (!post_reset_event_wait_complete_logged) {
        post_reset_event_wait_complete_logged = true;
        ESP_LOGW(TAG,
                 "LC completed post-reset Event Manager wait: pc=0x%08" PRIx32
                 " a0=0x%08" PRIx32 " restored_a0=0x%08" PRIx32
                 " saved_sp=0x%08" PRIx32 " old_status=0x%04x hits=%" PRIu32
                 " callback=0x%08" PRIx32 " qhdr=0x%08" PRIx32,
                 pc, a0, restored_a0, post_reset_event_wait_saved_sp, status,
                 post_reset_event_wait_hits, lc_musashi_bus_peek_ram32(a0 + 0x08u),
                 lc_musashi_bus_peek_ram32(0x00000362u));
    }
}

static void lc_musashi_bus_maybe_log_post_reset_invalid_execution(uint32_t pc) {
    if (post_reset_invalid_exec_logs >= 8u) {
        return;
    }
    const bool suspicious_high = pc >= 0xff000000u;
    const bool suspicious_low = instruction_callback_count > 1000u && pc < 0x00010000u;
    const bool suspicious_fill = pc == 0xb6db6db6u || pc == 0x6db6db6du ||
                                  pc == 0xdb6db6dbu || pc == 0x92492492u;
    if (!suspicious_high && !suspicious_low && !suspicious_fill) {
        return;
    }
    const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
    ESP_LOGW(TAG,
             "LC post-reset invalid execution trace: pc=0x%08" PRIx32
             " prev_pc=0x%08" PRIx32 " words=%04x %04x %04x %04x"
             " sp=0x%08" PRIx32 " stack=%08" PRIx32 " %08" PRIx32 " %08" PRIx32
             " d0=0x%08x d1=0x%08x d2=0x%08x d7=0x%08x"
             " a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x a4=0x%08x a5=0x%08x a6=0x%08x sr=0x%04x",
             pc, previous_instruction_pc, cpu_read_word(pc + 0u), cpu_read_word(pc + 2u),
             cpu_read_word(pc + 4u), cpu_read_word(pc + 6u), sp,
             lc_musashi_bus_peek_ram32(sp + 0u), lc_musashi_bus_peek_ram32(sp + 4u),
             lc_musashi_bus_peek_ram32(sp + 8u), m68k_get_reg(NULL, M68K_REG_D0),
             m68k_get_reg(NULL, M68K_REG_D1), m68k_get_reg(NULL, M68K_REG_D2),
             m68k_get_reg(NULL, M68K_REG_D7), m68k_get_reg(NULL, M68K_REG_A0),
             m68k_get_reg(NULL, M68K_REG_A1), m68k_get_reg(NULL, M68K_REG_A2),
             m68k_get_reg(NULL, M68K_REG_A3), m68k_get_reg(NULL, M68K_REG_A4),
             m68k_get_reg(NULL, M68K_REG_A5), m68k_get_reg(NULL, M68K_REG_A6),
             m68k_get_reg(NULL, M68K_REG_SR));
    post_reset_invalid_exec_logs++;
}

static void lc_musashi_bus_maybe_pulse_reset_scc_timer_irq(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    // Diagnostic-only model for the reset subtest at 0x4084703e.  The ROM
    // installs a level-4 autovector handler at VBR+0x70, enables interrupts,
    // waits for one pulse to advance d1 from -1 to 0, then waits for a second
    // pulse and checks the elapsed loop count.  Pulse only at those two wait
    // loops and space the pulses enough to satisfy the ROM's timing sanity
    // bounds without creating a general interrupt controller yet.
    if (reset_scc_timer_irq_pulses == 0 && rom_offset == 0x00047080u) {
        reset_scc_timer_wait0_hits++;
        if (reset_scc_timer_wait0_hits >= 512u && !reset_scc_timer_irq_asserted) {
            reset_scc_timer_irq_asserted = true;
            reset_scc_timer_irq_pulses++;
            ESP_LOGI(TAG,
                     "LC reset SCC timer synthetic IRQ pulse: phase=first level=4 pc=0x%08" PRIx32
                     " hits=%" PRIu32,
                     pc, reset_scc_timer_wait0_hits);
            m68k_set_irq(M68K_IRQ_4);
        }
    } else if (reset_scc_timer_irq_pulses == 1 && rom_offset == 0x0004708au) {
        reset_scc_timer_wait1_hits++;
        if (reset_scc_timer_wait1_hits >= 512u && !reset_scc_timer_irq_asserted) {
            reset_scc_timer_irq_asserted = true;
            reset_scc_timer_irq_pulses++;
            ESP_LOGI(TAG,
                     "LC reset SCC timer synthetic IRQ pulse: phase=second level=4 pc=0x%08" PRIx32
                     " hits=%" PRIu32,
                     pc, reset_scc_timer_wait1_hits);
            m68k_set_irq(M68K_IRQ_4);
        }
    }
}

void cpu_instr_callback(int pc) {
    instruction_callback_count++;
    current_instruction_pc = (uint32_t)pc;

    // In Basilisk-compatible mode, intercept A-line trap dispatcher entry.
    if (lc_musashi_bus_basilisk_slot_rom_active()) {
        if (current_instruction_pc == 0x408099b0u) {
            static unsigned disp_entries = 0;
            disp_entries++;
            if (disp_entries <= 200u) {
                ESP_LOGI(TAG, "LC DISP entry #%u: sp=0x%08" PRIx32 " sr_at_sp=0x%04x pc_at_sp2=0x%08" PRIx32,
                         disp_entries, m68k_get_reg(NULL, M68K_REG_SP),
                         (unsigned)lc_musashi_bus_peek_ram16(m68k_get_reg(NULL, M68K_REG_SP)),
                         lc_musashi_bus_peek_ram32(m68k_get_reg(NULL, M68K_REG_SP) + 2u));
            }
            // A-line trap fired. Read trap word from exception frame.
            // Stack at entry: [format(2), PC(4), SR(2)] = 8 bytes.
            // PC in frame points to the A-line instruction itself.
            const uint32_t sp = m68k_get_reg(NULL, M68K_REG_SP);
            const uint32_t trap_pc = lc_musashi_bus_peek_ram32(sp + 2u); // PC from frame
            const uint16_t trap_word = (uint16_t)lc_memory_bus_read16(active_bus, trap_pc);
            const uint32_t return_pc = trap_pc + 2u; // instruction after trap
            const uint16_t saved_sr = (uint16_t)lc_memory_bus_read16(active_bus, sp); // SR at top of frame
            bool handled = false;
            uint16_t trap_num = trap_word & 0x00ffu; // OS trap number (low 8 bits)
            bool is_toolbox = (trap_word & 0x0800u) != 0u;
            if (is_toolbox) trap_num = trap_word & 0x03ffu; // toolbox: low 10 bits

            switch (trap_word & 0xf0ffu) { // mask out flag bits for OS traps
            case 0xa000u: { // _Open: A0=paramBlock
                uint32_t pb_o = m68k_get_reg(NULL, M68K_REG_A0);
                // Write ioRefNum=-5 (Sony driver) at pb+24
                lc_musashi_bus_ram_write16(pb_o + 24u, (uint16_t)(int16_t)-5);
                m68k_set_reg(M68K_REG_D0, 0); // noErr
                handled = true;
                break;
            }
            case 0xa002u: { // _Read: A0=paramBlock
                // Read from disk — delegate to DISK_PRIME
                uint32_t pb_r = m68k_get_reg(NULL, M68K_REG_A0);
                int16_t result = lc_musashi_bus_basilisk_disk_prime(false, pb_r, 0x8800u);
                m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)result);
                handled = true;
                break;
            }
            case 0xa011u: { // _GetEOF: A0=paramBlock, returns ioMisc=EOF
                uint32_t pb_e = m68k_get_reg(NULL, M68K_REG_A0);
                // Return disk size as EOF (200MB)
                lc_musashi_bus_ram_write32(pb_e + 28u, 209715200u); // ioMisc = file size
                m68k_set_reg(M68K_REG_D0, 0); // noErr
                handled = true;
                break;
            }
            case 0xa02eu: { // _BlockMove: A0=src, A1=dst, D0=count
                uint32_t src = m68k_get_reg(NULL, M68K_REG_A0);
                uint32_t dst = m68k_get_reg(NULL, M68K_REG_A1);
                uint32_t cnt = m68k_get_reg(NULL, M68K_REG_D0);
                if (dst <= 0x0000FB8Eu && dst + cnt > 0x0000FB8Eu) {
                    ESP_LOGI(TAG, "LC WATCHPOINT: BlockMove to $FB8E! src=$%08" PRIx32
                             " dst=$%08" PRIx32 " cnt=%" PRIu32 " from_pc=$%08" PRIx32,
                             src, dst, cnt, trap_pc);
                }
                for (uint32_t i = 0; i < cnt && i < 0x100000u; i++) {
                    uint8_t b = (uint8_t)lc_memory_bus_read8(active_bus, src + i);
                    lc_memory_bus_write8(active_bus, dst + i, b);
                }
                m68k_set_reg(M68K_REG_D0, 0); // noErr
                handled = true;
                break;
            }
            case 0xa06cu: // _FreeMem: returns free bytes in D0
                m68k_set_reg(M68K_REG_D0, (uint32_t)(active_bus->ram_size / 2u));
                m68k_set_reg(M68K_REG_A0, (uint32_t)(active_bus->ram_size / 2u));
                handled = true;
                break;
            case 0xa06du: // _MaxMem: returns max block in D0, grow in A0
                m68k_set_reg(M68K_REG_D0, (uint32_t)(active_bus->ram_size / 4u));
                m68k_set_reg(M68K_REG_A0, 0);
                handled = true;
                break;
            case 0xa01cu: // _GetHandleSize: A0=handle, returns size in D0
            case 0xa025u: // _GetHandleSize (alternate trap $25)
            {
                uint32_t h = m68k_get_reg(NULL, M68K_REG_A0);
                uint32_t sz = 0x10000u; // default: 64KB
                if (h == 0x0004ff00u) sz = 648u;
                else if (h == 0x0004ff08u) sz = 31420u;
                else if (h == 0u) sz = 0x10000u; // NULL → large to break loops
                m68k_set_reg(M68K_REG_D0, sz);
                ESP_LOGW(TAG, "LC GetHandleSize: h=0x%08" PRIx32 " sz=%u trap=0x%04x",
                         h, (unsigned)sz, trap_word);
                handled = true;
                break;
            }
            case 0xa00cu: // _GetPtrSize: A0=ptr, returns size in D0
                m68k_set_reg(M68K_REG_D0, 0x1000u); // return 4KB as default ptr size
                handled = true;
                break;
            case 0xa06eu: // _SlotManager: return -300 (smEmptySlot) in D0
                m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)(int16_t)-300);
                handled = true;
                break;
            case 0xa029u: // _HLock: no-op, return noErr
            case 0xa02au: // _HUnlock: no-op
            case 0xa004u: // _GetZone: return SysZone in A0
                m68k_set_reg(M68K_REG_A0, 0x00002800u);
                m68k_set_reg(M68K_REG_D0, 0);
                handled = true;
                break;
            case 0xa02cu: // _FlushCache: no-op
            case 0xa04fu: // _RmvTime: no-op
            case 0xa057u: // _SetTrapAddress: no-op (we handle traps ourselves)
            case 0xa077u: // _CountADBs: return 0 in D0
                m68k_set_reg(M68K_REG_D0, 0);
                handled = true;
                break;
            default:
                break;
            }
            // OS traps with variant flags (NewPtr etc):
            if (!handled && (trap_word & 0xf000u) == 0xa000u) {
                uint16_t base_trap = trap_word & 0x00ffu;
                switch (base_trap) {
                case 0x001eu: // _NewPtr (A01E/A11E/A21E/A31E)
                case 0x0007u: { // _NewPtr variant with extra flags
                    // Simple bump allocator from top of SysZone
                    static uint32_t heap_ptr = 0x00080000u; // Start at 512KB
                    uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
                    if (size == 0) size = 4;
                    size = (size + 3u) & ~3u; // align to 4
                    uint32_t ptr = heap_ptr;
                    heap_ptr += size;
                    if (heap_ptr >= active_bus->ram_size - 0x10000u) {
                        m68k_set_reg(M68K_REG_A0, 0);
                        m68k_set_reg(M68K_REG_D0, (uint32_t)-108); // memFullErr
                    } else {
                        // Clear if CLEAR flag (bit 9) is set
                        if ((trap_word & 0x0200u) != 0) {
                            for (uint32_t i = 0; i < size; i++)
                                lc_memory_bus_write8(active_bus, ptr + i, 0);
                        }
                        m68k_set_reg(M68K_REG_A0, ptr);
                        m68k_set_reg(M68K_REG_D0, 0); // noErr
                    }
                    handled = true;
                    break;
                }
                case 0x000eu: // _HPurge — no-op
                case 0x000fu: // _HNoPurge — no-op
                case 0x0015u: // _DisposHandle — no-op
                    m68k_set_reg(M68K_REG_D0, 0);
                    handled = true;
                    break;
                case 0x0057u: // _SetFileInfo — no-op return noErr
                    m68k_set_reg(M68K_REG_D0, 0);
                    handled = true;
                    break;
                case 0x0060u: // _HGetState — return 0 in D0
                    m68k_set_reg(M68K_REG_D0, 0);
                    handled = true;
                    break;
                default:
                    break;
                }
            }
            // Toolbox traps — Pascal calling convention:
            static uint32_t last_count_type_val = 0; // shared between CountRes/GetIndRes
            static uint16_t last_count_val = 0; // last count result
            // Before trap: stack has [result_space] [params...] from bottom up.
            // After our exception frame removal, SP points to params.
            // We pop params and write result to result_space.
            if (!handled && is_toolbox) {
                // After exception frame pop, return_sp = sp + 8.
                // Params start at return_sp. We compute param_bytes and result_bytes.
                uint32_t param_bytes = 0;
                uint32_t result_bytes = 0;
                uint32_t result_value = 0;
                switch (trap_word & 0x0bffu) { // mask auto-pop bit
                case 0x08feu: // _InitAllPacks: no params, no result (procedure)
                    param_bytes = 0;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x08a5u: // _Pack3 (StdFile): no-op, assume 0 params
                case 0x08a8u: // _ADBReInit: no-op
                case 0x0895u: // _SysEnvirons: no-op procedure
                    param_bytes = 0;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x09bcu: { // _GetIndResource(index:w) → Handle:l
                    // In boot_3 context: type is IMPLICIT from last CountResources.
                    // Stack: [frame:8] [index:2] [result_space:4 already allocated]
                    // So param_bytes=2 (just index), result_bytes=4.
                    param_bytes = 2;
                    result_bytes = 4;
                    uint16_t gi_index = lc_musashi_bus_peek_ram16(sp + 8u); // 0-based in boot_3!
                    result_value = 0;
                    // Use the type from last CountResources call
                    uint32_t gi_type = last_count_type_val;
                    if (gi_type != 0 && gi_index < last_count_val && active_bus && active_bus->ram) {
                        uint32_t sys_base = 0x00A00000u;
                        uint32_t data_off_hdr = lc_musashi_bus_peek_ram32(sys_base + 0u);
                        uint32_t map_off_val = lc_musashi_bus_peek_ram32(sys_base + 4u);
                        uint32_t map_addr = sys_base + map_off_val;
                        uint16_t tl_off = lc_musashi_bus_peek_ram16(map_addr + 24u);
                        uint32_t tl_addr = map_addr + tl_off;
                        int16_t num_types = (int16_t)lc_musashi_bus_peek_ram16(tl_addr) + 1;
                        for (int ti = 0; ti < num_types; ti++) {
                            uint32_t te_addr = tl_addr + 2u + (uint32_t)ti * 8u;
                            uint32_t te_type = lc_musashi_bus_peek_ram32(te_addr);
                            if (te_type == gi_type) {
                                uint16_t rcount = lc_musashi_bus_peek_ram16(te_addr + 4u) + 1u;
                                uint16_t ref_off = lc_musashi_bus_peek_ram16(te_addr + 6u);
                                // gi_index is 0-based in this context
                                if (gi_index < rcount) {
                                    uint32_t rl_addr = tl_addr + ref_off + (uint32_t)gi_index * 12u;
                                    uint8_t d0b = (uint8_t)lc_memory_bus_read8(active_bus, rl_addr + 5u);
                                    uint8_t d1b = (uint8_t)lc_memory_bus_read8(active_bus, rl_addr + 6u);
                                    uint8_t d2b = (uint8_t)lc_memory_bus_read8(active_bus, rl_addr + 7u);
                                    uint32_t d_off = ((uint32_t)d0b << 16) | ((uint32_t)d1b << 8) | d2b;
                                    uint32_t abs_data = sys_base + data_off_hdr + d_off;
                                    uint32_t rdata_addr = abs_data + 4u;
                                    static uint32_t gi_handle_ptr = 0x004F1000u;
                                    uint32_t handle = gi_handle_ptr;
                                    gi_handle_ptr += 4u;
                                    lc_musashi_bus_ram_write32(handle, rdata_addr);
                                    result_value = handle;
                                }
                                break;
                            }
                        }
                    }
                    {
                        char t[5] = {0};
                        t[0] = (char)(gi_type >> 24); t[1] = (char)(gi_type >> 16);
                        t[2] = (char)(gi_type >> 8); t[3] = (char)gi_type;
                        ESP_LOGI(TAG, "LC GetIndResource('%s', %u) = 0x%08" PRIx32,
                                 t, (unsigned)gi_index, result_value);
                    }
                    handled = true;
                    break;
                }
                case 0x09c9u: // _HOpenResFile(vRefNum:w, dirID:l, fileName:l, perm:b→w) → refNum:w
                    param_bytes = 12; // 2+4+4+2
                    result_bytes = 2;
                    result_value = 2; // refNum=2 (System file)
                    // Set up minimal resource file entry so boot_3 can use it.
                    // TopMapHndl ($A50) = resource map handle for the top (current) file.
                    // We'll set it to a handle pointing to our System.rsrc map in RAM.
                    if (active_bus && active_bus->ram) {
                        // System.rsrc is loaded at $A00000 in guest RAM.
                        // Resource map offset is stored at bytes 4-7 of the resource header.
                        uint32_t sys_rsrc_base = 0x00A00000u;
                        uint32_t map_offset = lc_musashi_bus_peek_ram32(sys_rsrc_base + 4u);
                        uint32_t map_addr = sys_rsrc_base + map_offset;
                        // Create a handle for the resource map:
                        // Allocate a master pointer at $4F0020 → points to map_addr
                        uint32_t map_handle = 0x0004F020u;
                        lc_musashi_bus_ram_write32(map_handle, map_addr);
                        // TopMapHndl ($A50): handle to top resource map
                        lc_musashi_bus_ram_write32(0x0A50u, map_handle);
                        // SysMapHndl ($A54): handle to system resource map
                        lc_musashi_bus_ram_write32(0x0A54u, map_handle);
                        // SysMap ($A58): refNum of system resource file
                        lc_musashi_bus_ram_write16(0x0A58u, 2);
                        // CurMap ($A5A): refNum of current resource file
                        lc_musashi_bus_ram_write16(0x0A5Au, 2);
                        ESP_LOGI(TAG, "LC _HOpenResFile: set TopMapHndl=$%08" PRIx32
                                 " map_addr=$%08" PRIx32 " map_offset=$%08" PRIx32,
                                 map_handle, map_addr, map_offset);
                    }
                    handled = true;
                    break;
                case 0x09a0u: // _GetResource(theType:l, theID:w) → Handle:l
                case 0x081fu: { // _Get1Resource(theType:l, theID:w) → Handle:l
                    param_bytes = 6u;
                    result_bytes = 4;
                    // Read resource type from stack: sp + 8(frame) + 2(id) = sp+10
                    uint32_t res_type = lc_musashi_bus_peek_ram32(sp + 10u);
                    uint16_t res_id = lc_musashi_bus_peek_ram16(sp + 8u);
                    result_value = 0; // default: not found
                    if (res_type == 0x626f6f74u) { // 'boot'
                        if (res_id == 2u) result_value = 0x0004ff00u;
                        else if (res_id == 3u) result_value = 0x0004ff08u;
                    }
                    // Search System.rsrc for the requested resource:
                    // Skip 'gbly' — it's a machine-specific boot check that loops if content mismatches
                    if (result_value == 0 && res_type != 0x67626c79u && // skip 'gbly'
                        active_bus != NULL && active_bus->ram != NULL) {
                        extern uint32_t host_find_system_resource(const uint8_t *, size_t,
                                                                  uint32_t, int16_t, uint32_t *);
                        uint32_t rsz = 0;
                        uint32_t raddr = host_find_system_resource(
                            active_bus->ram, active_bus->ram_size,
                            res_type, (int16_t)res_id, &rsz);
                        if (raddr != 0) {
                            // Cache: return same handle for same resource
                            static struct { uint32_t type; int16_t id; uint32_t handle; } cache[64];
                            static unsigned cache_count = 0;
                            uint32_t h = 0;
                            for (unsigned ci = 0; ci < cache_count; ci++) {
                                if (cache[ci].type == res_type && cache[ci].id == (int16_t)res_id) {
                                    h = cache[ci].handle; break;
                                }
                            }
                            if (h == 0) {
                                static uint32_t handle_alloc = 0x004f0000u;
                                h = handle_alloc; handle_alloc += 4u;
                                lc_musashi_bus_ram_write32(h, raddr);
                                // Verify handle chain for vers resources
                                if (res_type == 0x76657273u) { // 'vers'
                                    uint32_t mp = lc_musashi_bus_peek_ram32(h);
                                    uint16_t first_word = lc_musashi_bus_peek_ram16(mp);
                                    ESP_LOGI(TAG, "LC vers handle $%08" PRIx32
                                             " -> mp=$%08" PRIx32 " -> first_word=$%04x",
                                             h, mp, (unsigned)first_word);
                                }
                                if (cache_count < 64) {
                                    cache[cache_count].type = res_type;
                                    cache[cache_count].id = (int16_t)res_id;
                                    cache[cache_count].handle = h;
                                    cache_count++;
                                }
                            }
                            result_value = h;
                        }
                    }
                    {
                        static unsigned res_log = 0;
                        if (res_log < 20u) {
                            ESP_LOGW(TAG, "LC GetResource: type='%c%c%c%c' id=%d result=0x%08" PRIx32,
                                     (char)(res_type>>24), (char)(res_type>>16),
                                     (char)(res_type>>8), (char)res_type,
                                     (int)(int16_t)res_id, result_value);
                            res_log++;
                        }
                    }
                    m68k_set_reg(M68K_REG_A0, result_value);
                    handled = true;
                    break;
                }
                case 0x0995u: // _CurResFile() → refNum:w (no params)
                    param_bytes = 0;
                    result_bytes = 2;
                    // Return -1 (no resource file) so BMI at $968 skips the
                    // GetResource code path (which is overwritten by BlockMove).
                    result_value = 0xFFFFu; // -1 as unsigned 16-bit
                    handled = true;
                    break;
                case 0x0997u: // _CountResources(theType:l) → count:w
                case 0x09a7u: { // _Count1Resources(theType:l) → count:w
                    param_bytes = 4;
                    result_bytes = 2;
                    // Read the resource type from the param on the stack
                    // Stack: [exception_frame:8] [theType:4] [result_space:2]
                    // The type parameter may be passed by value OR by reference (PEA).
                    // If the value looks like a valid RAM pointer, dereference it.
                    uint32_t raw_type = lc_musashi_bus_peek_ram32(sp + 8u);
                    uint32_t count_type;
                    if (raw_type > 0x00008000u && raw_type < active_bus->ram_size - 4u) {
                        // Likely a pointer — dereference to get actual type
                        count_type = lc_musashi_bus_peek_ram32(raw_type);
                    } else {
                        count_type = raw_type; // Use directly as type value
                    }
                    // Debug: dump stack around params
                    {
                        static unsigned crd = 0;
                        if (crd < 5) {
                            ESP_LOGI(TAG, "LC CountRes stack: sp=$%08" PRIx32
                                     " [+0]=$%04x [+2]=$%08" PRIx32 " [+6]=$%04x"
                                     " [+8]=$%08" PRIx32 " [+12]=$%04x",
                                     sp,
                                     (unsigned)lc_musashi_bus_peek_ram16(sp),
                                     lc_musashi_bus_peek_ram32(sp + 2u),
                                     (unsigned)lc_musashi_bus_peek_ram16(sp + 6u),
                                     lc_musashi_bus_peek_ram32(sp + 8u),
                                     (unsigned)lc_musashi_bus_peek_ram16(sp + 12u));
                            crd++;
                        }
                    }
                    uint16_t count_val = 0;
                    // Count resources of this type in System.rsrc
                    if (active_bus && active_bus->ram) {
                        uint32_t sys_base = 0x00A00000u;
                        uint32_t map_off_val = lc_musashi_bus_peek_ram32(sys_base + 4u);
                        uint32_t map_addr = sys_base + map_off_val;
                        uint16_t tl_off = lc_musashi_bus_peek_ram16(map_addr + 24u);
                        uint32_t tl_addr = map_addr + tl_off;
                        int16_t num_types = (int16_t)lc_musashi_bus_peek_ram16(tl_addr);
                        num_types += 1;
                        for (int i = 0; i < num_types; i++) {
                            uint32_t te_addr = tl_addr + 2u + (uint32_t)i * 8u;
                            uint32_t te_type = lc_musashi_bus_peek_ram32(te_addr);
                            if (te_type == count_type) {
                                count_val = lc_musashi_bus_peek_ram16(te_addr + 4u) + 1u;
                                break;
                            }
                        }
                    }
                    // Remember last counted type for GetIndResource
                    last_count_type_val = count_type;
                    // Skip PTCH resources — patches require full system state
                    if (count_type == 0x50544348u) count_val = 0; // 'PTCH'
                    last_count_val = count_val;
                    result_value = count_val;
                    {
                        char t[5] = {0};
                        t[0] = (char)(count_type >> 24);
                        t[1] = (char)(count_type >> 16);
                        t[2] = (char)(count_type >> 8);
                        t[3] = (char)(count_type);
                        ESP_LOGI(TAG, "LC CountResources('%s' $%08" PRIx32 ") = %u sp=$%08" PRIx32 " A5=$%08x",
                                 t, count_type, (unsigned)count_val, sp,
                                 m68k_get_reg(NULL, M68K_REG_A5));
                    }
                    handled = true;
                    break;
                }
                case 0x09a3u: // _ReleaseResource(theResource:l) → void
                    param_bytes = 4;
                    result_bytes = 0;
                    // Fix boot_3 stack: if SP is near code ($8372), relocate stack high.
                    // boot_3's first instruction is _ReleaseResource at $8374.
                    // boot_2 leaves SP near code base; real Mac has it at ~$7FFC00.
                    if (trap_pc == 0x00008374u && sp < 0x00009000u) {
                        // Relocate stack: move exception frame and param to high address
                        uint32_t new_stack_top = 0x003FFC00u; // 4MB mark
                        // Copy 12 bytes (frame:8 + param:4) from current sp to new location
                        uint32_t new_frame_sp = new_stack_top - 12u;
                        for (uint32_t i = 0; i < 12u; i++) {
                            uint8_t b = (uint8_t)lc_memory_bus_read8(active_bus, sp + i);
                            lc_memory_bus_write8(active_bus, new_frame_sp + i, b);
                        }
                        // Update sp to new location (Musashi will use this)
                        m68k_set_reg(M68K_REG_SP, new_frame_sp);
                        // Also write MemTop, BufPtr if not set
                        if (lc_musashi_bus_peek_ram32(0x010Cu) == 0) {
                            lc_musashi_bus_ram_write32(0x010Cu, (uint32_t)active_bus->ram_size);
                        }
                        if (lc_musashi_bus_peek_ram32(0x02A6u) == 0) {
                            lc_musashi_bus_ram_write32(0x02A6u, active_bus->ram_size - 0x8000u);
                        }
                        ESP_LOGI(TAG, "LC boot_3 stack relocated: old_sp=$%08" PRIx32
                                 " new_sp=$%08" PRIx32 " A5=$%08x",
                                 sp, new_frame_sp, m68k_get_reg(NULL, M68K_REG_A5));
                        // Set A5 = boot_3 globals base.
                        // boot_3 code starts at $8372. Globals/constants are embedded
                        // within boot_3 at offset $10BE from code start ($8372+$10BE=$9430).
                        // PEA $02BA(A5) should yield 'PTCH' at $9430+$02BA=$96EA.
                        m68k_set_reg(M68K_REG_A5, 0x00009430u);
                        // Also set CurrentA5 ($904) for ROM code that reads it
                        lc_musashi_bus_ram_write32(0x0904u, 0x00009430u);
                        // Re-read sp for the epilogue
                        // Note: we need to update our local sp variable
                        // Actually, the epilogue uses the 'sp' variable for new_sp computation.
                        // We can't easily change it here since the epilogue does sp + 8 + param_bytes.
                        // Instead, just return early with manual handling:
                        uint32_t ret_pc = lc_musashi_bus_peek_ram32(new_frame_sp + 2u) + 2u;
                        m68k_set_reg(M68K_REG_SP, new_frame_sp + 8u + 4u); // skip frame + param
                        m68k_set_reg(M68K_REG_PC, ret_pc);
                        m68k_set_reg(M68K_REG_SR, lc_musashi_bus_peek_ram16(new_frame_sp));
                        m68k_set_reg(M68K_REG_D0, 0);
                        previous_instruction_pc = current_instruction_pc;
                        return;
                    }
                    handled = true;
                    break;
                case 0x099au: // _CloseResFile(refNum:w) → void
                    param_bytes = 2;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x099bu: // _SetResLoad(load:w) → void
                    param_bytes = 2;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x09a4u: // _DetachResource(theResource:l) → void
                    param_bytes = 4;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x09a1u: // _GetNamedResource(theType:l, name:l) → Handle:l
                    param_bytes = 8;
                    result_bytes = 4;
                    result_value = 0; // NULL
                    handled = true;
                    break;
                case 0x09a2u: // _LoadResource(theResource:l) → void
                    param_bytes = 4;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x099cu: // _UseResFile(refNum:w) → void
                    param_bytes = 2;
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x09aau: // _RmveResource(theResource:l) → void
                case 0x09adu: // _AddResource(theData:l, theType:l, theID:w, name:l) → void
                    param_bytes = 4; // RmveResource; AddResource uses 14 but rare
                    result_bytes = 0;
                    handled = true;
                    break;
                case 0x09a8u: // _GetResAttrs(theResource:l) → attrs:w
                    param_bytes = 4;
                    result_bytes = 2;
                    result_value = 0;
                    handled = true;
                    break;
                case 0x099fu: // _Get1Resource(theType:l, theID:w) → Handle:l
                    param_bytes = 6;
                    result_bytes = 4;
                    result_value = 0; // NULL (fallback — main GetResource path handles real lookups)
                    handled = true;
                    break;
                default: {
                    // Unknown toolbox trap — log it for debugging.
                    static unsigned unk_tb_log = 0;
                    if (unk_tb_log < 100u) {
                        ESP_LOGW(TAG, "LC UNKNOWN TB trap: 0x%04x (masked=0x%04x) from=0x%08" PRIx32
                                 " sp=0x%08" PRIx32,
                                 trap_word, (unsigned)(trap_word & 0x0BFFu), trap_pc, sp);
                        unk_tb_log++;
                    }
                    // Can't determine param sizes. Assume no-param procedure.
                    param_bytes = 0;
                    result_bytes = 0;
                    m68k_set_reg(M68K_REG_D0, 0);
                    m68k_set_reg(M68K_REG_A0, 0);
                    handled = true;
                    break;
                }
                }
                if (handled) {
                    // Pop exception frame + params, write result to result_space.
                    uint32_t new_sp = sp + 8u + param_bytes; // skip frame + params
                    if (result_bytes == 4u) {
                        lc_musashi_bus_ram_write32(new_sp, result_value);
                    } else if (result_bytes == 2u) {
                        lc_musashi_bus_ram_write16(new_sp, (uint16_t)result_value);
                    }
                    m68k_set_reg(M68K_REG_SP, new_sp);
                    m68k_set_reg(M68K_REG_PC, return_pc);
                    m68k_set_reg(M68K_REG_SR, saved_sr);
                    m68k_set_reg(M68K_REG_D0, 0); // ResErr = noErr
                    {
                        static unsigned tb_log = 0;
                        if (tb_log < 200u) {
                            ESP_LOGI(TAG, "LC TB trap: trap=0x%04x from=0x%08" PRIx32
                                     " ret=0x%08" PRIx32 " result=0x%08" PRIx32
                                     " param_bytes=%u result_bytes=%u",
                                     trap_word, trap_pc, return_pc, result_value,
                                     (unsigned)param_bytes, (unsigned)result_bytes);
                            tb_log++;
                        }
                    }
                    previous_instruction_pc = current_instruction_pc;
                    return;
                }
            }
            if (!handled) {
                // Unknown OS trap — return noErr
                m68k_set_reg(M68K_REG_D0, 0);
                handled = true;
            }
            if (handled) {
                static unsigned trap_log_count = 0;
                if (trap_log_count < 200u) {
                    ESP_LOGI(TAG, "LC trap intercept: trap=0x%04x from_pc=0x%08" PRIx32
                             " return=0x%08" PRIx32 " d0=0x%08x a0=0x%08x sp=0x%08" PRIx32
                             " [$28]=0x%08" PRIx32,
                             trap_word, trap_pc, return_pc,
                             m68k_get_reg(NULL, M68K_REG_D0),
                             m68k_get_reg(NULL, M68K_REG_A0), sp,
                             lc_musashi_bus_peek_ram32(0x28u));
                    trap_log_count++;
                }
                // Skip the ROM dispatcher: set PC to return address and
                // restore SR, pop exception frame.
                m68k_set_reg(M68K_REG_PC, return_pc);
                m68k_set_reg(M68K_REG_SP, sp + 8u); // pop exception frame
                // Restore SR: supervisor mode, CCR reflects D0 (like ROM dispatcher's TST.W D0)
                {
                    uint16_t new_sr = saved_sr & 0xFF00u; // keep supervisor/IPL bits
                    uint32_t d0_val = m68k_get_reg(NULL, M68K_REG_D0);
                    // TST.W D0 sets: N from bit 15, Z if low word=0, V=0, C=0
                    uint16_t d0w = (uint16_t)d0_val;
                    if (d0w == 0) new_sr |= 0x0004u; // Z
                    if (d0w & 0x8000u) new_sr |= 0x0008u; // N
                    m68k_set_reg(M68K_REG_SR, new_sr);
                }
                previous_instruction_pc = current_instruction_pc;
                return;
            }
        }
        // Intercept at the generic trap handler ($40800D88) for traps dispatched
        // through the ROM's trap table. D2.B = OS trap number at entry.
        if (current_instruction_pc == (LC_BASILISK_ROM_BASE_32 + 0x0d88u)) {
            uint8_t trap_sel = (uint8_t)m68k_get_reg(NULL, M68K_REG_D2);
            switch (trap_sel) {
            case 0x1eu: { // _NewPtr (dispatched via table)
                static uint32_t heap_ptr2 = 0x00100000u; // secondary heap at 1MB
                uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
                if (size == 0) size = 4;
                size = (size + 3u) & ~3u;
                uint32_t ptr = heap_ptr2;
                heap_ptr2 += size;
                if (heap_ptr2 >= active_bus->ram_size - 0x10000u) {
                    m68k_set_reg(M68K_REG_A0, 0);
                    m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)(int16_t)-108);
                } else {
                    for (uint32_t i = 0; i < size && i < 0x10000u; i++)
                        lc_memory_bus_write8(active_bus, ptr + i, 0);
                    m68k_set_reg(M68K_REG_A0, ptr);
                    m68k_set_reg(M68K_REG_D0, 0);
                }
                break;
            }
            case 0x22u: { // _NewHandle
                static uint32_t hheap = 0x00200000u; // handle heap at 2MB
                uint32_t size = m68k_get_reg(NULL, M68K_REG_D0);
                if (size == 0) size = 4;
                size = (size + 3u) & ~3u;
                uint32_t data = hheap; hheap += size;
                uint32_t handle = hheap; hheap += 4u;
                if (hheap >= active_bus->ram_size - 0x10000u) {
                    m68k_set_reg(M68K_REG_A0, 0);
                    m68k_set_reg(M68K_REG_D0, (uint32_t)(uint16_t)(int16_t)-108);
                } else {
                    for (uint32_t i = 0; i < size && i < 0x10000u; i++)
                        lc_memory_bus_write8(active_bus, data + i, 0);
                    lc_musashi_bus_ram_write32(handle, data);
                    m68k_set_reg(M68K_REG_A0, handle);
                    m68k_set_reg(M68K_REG_D0, 0);
                }
                break;
            }
            default: // keep moveq#0,d0; rts behavior
                break;
            }
            previous_instruction_pc = current_instruction_pc;
            return;
        }
        // Guard: if PC enters the filename-string area at $AD8 (boot blocks
        // write "System" there), redirect to a safe RTS.
        // Watchpoint: log when ROM calls through $DBC at $1AC
        // Watchpoint: detect if boot_2 code at $900000 is reached
        if (current_instruction_pc >= 0x008f0000u && current_instruction_pc <= 0x00910000u) {
            static unsigned b2_log = 0;
            if (b2_log < 5u) {
                ESP_LOGW(TAG, "LC BOOT2 reached! pc=0x%08" PRIx32 " opcode=0x%04x a3=0x%08x sp=0x%08" PRIx32,
                         current_instruction_pc,
                         (unsigned)lc_memory_bus_read16(active_bus, current_instruction_pc),
                         m68k_get_reg(NULL, M68K_REG_A3),
                         m68k_get_reg(NULL, M68K_REG_SP));
                b2_log++;
            }
        }
        // Watchpoint: detect trampoline at $E00000
        if (current_instruction_pc >= 0x00e00000u && current_instruction_pc <= 0x00e00010u) {
            static unsigned tr_log = 0;
            if (tr_log < 3u) {
                ESP_LOGW(TAG, "LC TRAMPOLINE! pc=0x%08" PRIx32 " a3=0x%08x",
                         current_instruction_pc, m68k_get_reg(NULL, M68K_REG_A3));
                tr_log++;
            }
        }
        // Intercept boot block re-entry: on second+ call, redirect to boot_2.
        if (current_instruction_pc == 0x0000088au) {
            static unsigned bb_entry_count = 0;
            bb_entry_count++;
            if (bb_entry_count >= 2u) {
                // Skip boot blocks, jump directly to boot_2 with A3=handle.
                m68k_set_reg(M68K_REG_A3, 0x0004ff00u); // boot_2 handle
                m68k_set_reg(M68K_REG_PC, 0x00900000u); // boot_2 code
                previous_instruction_pc = current_instruction_pc;
                return;
            }
        }
        if (current_instruction_pc == (LC_BASILISK_ROM_BASE_32 + 0x01acu)) {
            static unsigned dbc_log = 0;
            if (dbc_log < 2u) {
                uint32_t dbc_val = lc_musashi_bus_peek_ram32(0xdbcu);
                ESP_LOGW(TAG, "LC ROM $1AC: JSR ($DBC) dbc=0x%08" PRIx32
                         " a0=0x%08x d0=0x%08x sp=0x%08" PRIx32
                         " [$900000]=0x%04x [$E00000]=0x%04x",
                         dbc_val, m68k_get_reg(NULL, M68K_REG_A0),
                         m68k_get_reg(NULL, M68K_REG_D0),
                         m68k_get_reg(NULL, M68K_REG_SP),
                         (unsigned)lc_memory_bus_read16(active_bus, 0x900000u),
                         (unsigned)lc_memory_bus_read16(active_bus, 0xe00000u));
                dbc_log++;
            }
        }
        // Watchpoint: log when PC enters $960-$97A range (boot block GetResource path)
        if (current_instruction_pc >= 0x00000960u && current_instruction_pc < 0x0000097au) {
            static unsigned bb_log = 0;
            if (bb_log < 20u) {
                ESP_LOGI(TAG, "LC BB watch: pc=0x%08" PRIx32 " opcode=0x%04x sp=0x%08" PRIx32
                         " [$976]=0x%04x [$28]=0x%08" PRIx32,
                         current_instruction_pc,
                         (unsigned)lc_memory_bus_read16(active_bus, current_instruction_pc),
                         m68k_get_reg(NULL, M68K_REG_SP),
                         (unsigned)lc_memory_bus_read16(active_bus, 0x976u),
                         lc_musashi_bus_peek_ram32(0x28u));
                bb_log++;
            }
        }
        if (current_instruction_pc >= 0x00000ad8u && current_instruction_pc < 0x00000af0u) {
            // Something called through $AD8 as code. Return safely.
            uint32_t ret_addr = lc_musashi_bus_peek_ram32(m68k_get_reg(NULL, M68K_REG_SP));
            m68k_set_reg(M68K_REG_SP, m68k_get_reg(NULL, M68K_REG_SP) + 4u);
            m68k_set_reg(M68K_REG_PC, ret_addr);
            previous_instruction_pc = current_instruction_pc;
            return;
        }
        lc_musashi_bus_maybe_pulse_reset_scc_timer_irq(current_instruction_pc);
        lc_musashi_bus_maybe_pulse_reset_via_irq(current_instruction_pc);
#if LC_MUSASHI_TRACE_ROM_WATCHPOINTS
        lc_musashi_bus_log_rom_watchpoint(current_instruction_pc);
#endif
        previous_instruction_pc = current_instruction_pc;
        return;
    }

    if (lc_musashi_bus_maybe_canonicalize_sr_prefixed_rom_pc(current_instruction_pc)) {
        current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    }
    const uint32_t current_rom_offset = current_instruction_pc & 0x000fffffu;
    if (post_reset_swap_mmu_dispatch_nop_active &&
        (current_rom_offset < 0x00009a04u || current_rom_offset >= 0x00009a0au)) {
        post_reset_swap_mmu_dispatch_nop_active = false;
    }
    lc_musashi_bus_maybe_stub_get_video_default(current_instruction_pc);
    lc_musashi_bus_maybe_stub_video_default_bad_indirect(current_instruction_pc);
    lc_musashi_bus_maybe_stub_slot_manager_video_default(current_instruction_pc);
    lc_musashi_bus_maybe_stub_control_video_default(current_instruction_pc);
    lc_musashi_bus_maybe_stub_disposeptr_video_default(current_instruction_pc);
    lc_musashi_bus_maybe_stub_swap_mmu_video_default(current_instruction_pc);
    lc_musashi_bus_maybe_restore_post_reset_device_bases(current_instruction_pc);
    lc_musashi_bus_maybe_basilisk_open_driver_trap(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_force_basilisk_install_drivers_branch(current_instruction_pc);
    lc_musashi_bus_maybe_handle_post_reset_slotmanager_opcode(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_skip_post_reset_slot_srt_builder(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_skip_post_reset_slot_dispatch_rebuild(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_log_post_reset_srt_entry(current_instruction_pc);
    lc_musashi_bus_maybe_skip_repeated_post_reset_slot_init_scan(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_seed_post_reset_srt_register(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_srt_alloc_entry(current_instruction_pc);
    lc_musashi_bus_maybe_guard_post_reset_srt_io_fill(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_escape_post_reset_srt_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_escape_post_reset_slot_first_pass_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_log_post_reset_srt_alloc_rts(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_slot_srt_scan(current_instruction_pc);
    lc_musashi_bus_maybe_seed_basilisk_slot_sresource_result(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_atrap_dispatch(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_attr_low_dispatch_return(current_instruction_pc);
    lc_musashi_bus_maybe_fix_post_reset_low_dispatch_return(current_instruction_pc);
    lc_musashi_bus_maybe_canonicalize_post_reset_trap_return(current_instruction_pc);
    lc_musashi_bus_maybe_cap_post_reset_slot_first_scan_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_cap_post_reset_slot_scan_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_lift_low_resource_stack(current_instruction_pc);
    lc_musashi_bus_maybe_apply_post_reset_memory_trap(current_instruction_pc);
    lc_musashi_bus_maybe_apply_post_reset_set_trap_address(current_instruction_pc);
    lc_musashi_bus_maybe_apply_post_reset_block_move(current_instruction_pc);
    lc_musashi_bus_post_reset_repair_rom_map_handle_identity(current_instruction_pc);
    lc_musashi_bus_maybe_stub_post_reset_swap_mmu_dispatch(current_instruction_pc);
    lc_musashi_bus_maybe_stub_basilisk_unit_table_newptr(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_shortcut_post_reset_get_startup_string(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_fix_post_reset_high_trap_handler_sr(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_high_trap_dispatch_entry(current_instruction_pc);
    // Note: high-trap dispatch return fixup disabled — the ROM's dispatcher
    // writes the handler address to SP+8 itself; our fixup was overwriting
    // the wrong stack slot when called from boot block code.
    // lc_musashi_bus_maybe_fix_post_reset_high_trap_dispatch_return(current_instruction_pc);
    lc_musashi_bus_maybe_stub_post_reset_no_mmu_a001(current_instruction_pc);
    lc_musashi_bus_maybe_add_dynamic_str_resource(current_instruction_pc);
    lc_musashi_bus_maybe_cap_post_reset_resource_type_scan(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_log_post_reset_resource_lookup(current_instruction_pc);
    lc_musashi_bus_maybe_escape_post_reset_resource_map_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_log_post_reset_count_combos(current_instruction_pc);
    lc_musashi_bus_post_reset_maybe_register_resource_map_handle(current_instruction_pc);
    lc_musashi_bus_maybe_cap_post_reset_resource_copy_loop(current_instruction_pc);
    lc_musashi_bus_maybe_cap_post_reset_finalizer_loop(current_instruction_pc);
    lc_musashi_bus_maybe_fix_post_reset_pack_empty_count(current_instruction_pc);
    lc_musashi_bus_maybe_seed_post_reset_no_mmu_return(current_instruction_pc);
    lc_musashi_bus_maybe_fix_post_reset_handoff_state(current_instruction_pc);
    lc_musashi_bus_maybe_capture_post_reset_univ_info(current_instruction_pc);
    lc_musashi_bus_maybe_seed_post_reset_probe_tables(current_instruction_pc);
    lc_musashi_bus_maybe_skip_bad_high_trap_handler(current_instruction_pc);
    lc_musashi_bus_maybe_repair_bad_heap_rts(current_instruction_pc);
    lc_musashi_bus_maybe_repair_post_reset_redomap_rts(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_ram_execution(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_shutdown_rts(current_instruction_pc);
    lc_musashi_bus_maybe_log_post_reset_vbl_init_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_allow_post_reset_slot_init_handoff(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_capture_post_reset_event_wait_record(current_instruction_pc);
    lc_musashi_bus_maybe_skip_repeated_post_reset_event_wait_loop(current_instruction_pc);
    current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    lc_musashi_bus_maybe_complete_post_reset_event_wait(current_instruction_pc);
    if (lc_musashi_bus_maybe_rescue_post_reset_low_dispatch_fallthrough(current_instruction_pc)) {
        current_instruction_pc = m68k_get_reg(NULL, M68K_REG_PC);
    }
    lc_musashi_bus_maybe_log_post_reset_invalid_execution(current_instruction_pc);
    lc_musashi_bus_maybe_pulse_reset_scc_timer_irq(current_instruction_pc);
    lc_musashi_bus_maybe_pulse_reset_via_irq(current_instruction_pc);
#if LC_MUSASHI_TRACE_ROM_WATCHPOINTS
    lc_musashi_bus_log_rom_watchpoint(current_instruction_pc);
#endif
    lc_trace_record(LC_TRACE_EVENT_MARKER, current_instruction_pc, 0, 0x4c434943u, 0,
                    false); // 'LCIC'
    previous_instruction_pc = current_instruction_pc;
}
