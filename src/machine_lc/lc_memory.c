#include "lc_memory.h"

#include "board_profiles.h"
#include "lc_musashi_bus.h"
#include "lc_trace.h"
#include "lc_video.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_memory";

#define LC_IO_STUB_KIND_COUNT 10u
#define LC_EARLY_VIA_REGISTER_COUNT 16u
#define LC_EARLY_F04000_REGISTER_COUNT 256u
#define LC_EARLY_F10000_REGISTER_COUNT 256u
#define LC_EARLY_F14000_REGISTER_COUNT 4096u
#define LC_EARLY_F16000_REGISTER_COUNT 8192u
#define LC_EARLY_F16000_SHIFT_QUEUE_SIZE 64u
#define LC_EARLY_F14000_POLL_STATUS_OFFSET 0x0804u
#define LC_EARLY_F14000_POLL_READY_BITS 0x03u
#ifndef LC_ENABLE_BASILISK_NO_FPU_CAPABILITY_SHADOW
#define LC_ENABLE_BASILISK_NO_FPU_CAPABILITY_SHADOW 1
#endif
#ifndef LC_ENABLE_RESET_DIAGNOSTIC_SUBTEST_ENTRY
#define LC_ENABLE_RESET_DIAGNOSTIC_SUBTEST_ENTRY 0
#endif
#ifndef LC_ENABLE_RAM_OWNED_LOW_MEMORY
#define LC_ENABLE_RAM_OWNED_LOW_MEMORY 0
#endif
#define LC_IO_OFFSET_STATS_SLOTS 24u
#define LC_RAM_SIZE_TOP_PROBE_BYTES 0x10u
#define LC_EARLY_VIA_IER_REGISTER 14u
#define LC_EARLY_VIA_IFR_REGISTER 13u
#define LC_EARLY_VIA_ORA_NO_HANDSHAKE_REGISTER 15u
#define LC_EARLY_VIA_ORA_EXTERNAL_LOW_MASK 0x01u
#define LC_EARLY_VIA_RESET_IRQ_PHASE1_T1_LATCH_OFFSET 0x0a00u
#define LC_EARLY_VIA_RESET_IRQ_IFR_OFFSET 0x1a00u
#define LC_VRAM_IO_OFFSET_BASE 0x00040000u
#define LC_VRAM_IO_OFFSET_LIMIT 0x000c0000u

typedef struct {
    uint32_t reads;
    uint32_t writes;
    uint32_t first_pc;
    uint32_t first_address;
    uint32_t last_pc;
    uint32_t last_address;
    uint8_t last_value;
    bool seen;
} lc_io_stub_stats_t;

typedef struct {
    uint32_t reads;
    uint32_t writes;
    uint32_t first_pc;
    uint32_t last_pc;
    uint16_t offset;
    uint8_t last_value;
    bool seen;
} lc_io_offset_stats_t;

typedef struct {
    uint32_t start;
    uint32_t end_exclusive;
    const char *alias;
} lc_io_device_alias_t;

static const lc_io_device_alias_t lc_v8_io_device_aliases[] = {
    {0x00000000u, 0x00002000u, "lc_via1"},
    {0x00004000u, 0x00006000u, "lc_scc"},
    {0x00006000u, 0x00008000u, "lc_scsi_drq"},
    {0x00010000u, 0x00012000u, "lc_scsi"},
    {0x00012000u, 0x00014000u, "lc_scsi_drq"},
    {0x00014000u, 0x00016000u, "lc_asc"},
    {0x00016000u, 0x00018000u, "lc_swim"},
    {0x00024000u, 0x00026000u, "lc_ariel"},
    {0x00026000u, 0x00028000u, "lc_pseudovia"},
    {0x00040000u, 0x000c0000u, "lc_vram"},
};

static lc_io_stub_stats_t io_stub_stats[LC_IO_STUB_KIND_COUNT];
static lc_io_offset_stats_t early_f04000_offset_stats[LC_IO_OFFSET_STATS_SLOTS];
static lc_io_offset_stats_t early_f10000_offset_stats[LC_IO_OFFSET_STATS_SLOTS];
static lc_io_offset_stats_t early_f14000_offset_stats[LC_IO_OFFSET_STATS_SLOTS];
static lc_io_offset_stats_t early_f16000_offset_stats[LC_IO_OFFSET_STATS_SLOTS];
static uint8_t early_probe_via_ier;
static uint8_t early_lc_via_registers[LC_EARLY_VIA_REGISTER_COUNT];
static uint8_t early_lc_via_reset_irq_phase;
static uint32_t early_lc_via_reset_irq_ifr_reads[3];
static uint8_t early_f04000_registers[LC_EARLY_F04000_REGISTER_COUNT];
static uint8_t early_f10000_registers[LC_EARLY_F10000_REGISTER_COUNT];
static uint8_t early_f10000_control_latch;
static uint8_t early_f04000_scc_registers[2][16];
static uint8_t early_f04000_scc_selected_register[2];
static uint8_t early_f04000_scc_loopback_data;
static bool early_f04000_scc_loopback_ready;
static uint8_t early_f14000_registers[LC_EARLY_F14000_REGISTER_COUNT];
static uint8_t early_f16000_registers[LC_EARLY_F16000_REGISTER_COUNT];
static uint8_t early_f16000_shift_queue[LC_EARLY_F16000_SHIFT_QUEUE_SIZE];
static uint8_t early_f16000_shift_read_index;
static uint8_t early_f16000_shift_write_index;
static uint8_t early_f16000_shift_count;
static uint8_t early_f16000_lines_latch;
static bool early_f16000_shift_reset_pending;
static bool early_f04000_tx_ready_once;
static uint16_t rom_serial_overlay_cursor_x;
static uint16_t rom_serial_overlay_cursor_y;
static uint32_t rom_serial_overlay_chars;
static uint32_t masked_rom_shadow_writes;
static uint32_t masked_rom_shadow_first_pc;
static uint32_t masked_rom_shadow_first_addr;
static uint32_t masked_rom_shadow_last_pc;
static uint32_t masked_rom_shadow_last_addr;
static uint8_t masked_rom_shadow_last_value;
static uint32_t post_reset_atrap_table_overrides[0x80];
static bool post_reset_atrap_table_owned[0x80];
static bool post_reset_core_lowmem_ram_seeded;
static bool post_reset_memory_layout_ram_seeded;
static bool post_reset_resource_lowmem_ram_seeded;
static bool post_reset_resource_map_ram_seeded;

static void lc_memory_seed8(lc_memory_bus_t *bus, uint32_t address, uint8_t value) {
    if (bus != NULL && bus->ram != NULL && address < bus->ram_size) {
        bus->ram[address] = value;
    }
}

static void lc_memory_seed16(lc_memory_bus_t *bus, uint32_t address, uint16_t value) {
    lc_memory_seed8(bus, address + 0u, (uint8_t)(value >> 8u));
    lc_memory_seed8(bus, address + 1u, (uint8_t)value);
}

static void lc_memory_seed32(lc_memory_bus_t *bus, uint32_t address, uint32_t value) {
    lc_memory_seed16(bus, address + 0u, (uint16_t)(value >> 16u));
    lc_memory_seed16(bus, address + 2u, (uint16_t)value);
}

void lc_memory_reset_post_reset_atrap_table(void) {
    memset(post_reset_atrap_table_overrides, 0, sizeof(post_reset_atrap_table_overrides));
    memset(post_reset_atrap_table_owned, 0, sizeof(post_reset_atrap_table_owned));
}

static void lc_memory_seed_post_reset_low_atrap_entry(lc_memory_bus_t *bus, uint8_t trap_low,
                                                      uint32_t handler) {
    const uint32_t index = trap_low & 0x7fu;
    post_reset_atrap_table_overrides[index] = handler;
    post_reset_atrap_table_owned[index] = true;
    if (bus != NULL && bus->ram != NULL && bus->ram_size > 0x00000600u) {
        const uint32_t table_addr = 0x00000400u + (index * 4u);
        lc_memory_seed32(bus, table_addr, handler);
    }
}

void lc_memory_set_post_reset_atrap_handler(uint16_t trap_word, uint32_t handler) {
    lc_memory_bus_t *bus = lc_musashi_bus_active();
    lc_memory_seed_post_reset_low_atrap_entry(bus, (uint8_t)(trap_word & 0x7fu), handler);
}

static bool address_in_window(uint32_t address, uint32_t base, uint32_t size) {
    return address >= base && address < (base + size);
}

static bool lc_memory_f14000_should_report_poll_ready(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x0007ffffu;
    return rom_offset == 0x00045e3au || rom_offset == 0x00045e42u ||
           rom_offset == 0x00045f2cu || rom_offset == 0x0004759au ||
           rom_offset == 0x000475b6u;
}

static void lc_memory_f16000_shift_push(uint8_t value) {
    early_f16000_shift_queue[early_f16000_shift_write_index] = value;
    early_f16000_shift_write_index =
        (uint8_t)((early_f16000_shift_write_index + 1u) % LC_EARLY_F16000_SHIFT_QUEUE_SIZE);
    if (early_f16000_shift_count < LC_EARLY_F16000_SHIFT_QUEUE_SIZE) {
        early_f16000_shift_count++;
    } else {
        early_f16000_shift_read_index =
            (uint8_t)((early_f16000_shift_read_index + 1u) % LC_EARLY_F16000_SHIFT_QUEUE_SIZE);
    }
}

static uint8_t lc_memory_f16000_shift_pop(void) {
    if (early_f16000_shift_count == 0) {
        return early_f16000_registers[0x0600u];
    }
    const uint8_t value = early_f16000_shift_queue[early_f16000_shift_read_index];
    early_f16000_shift_read_index =
        (uint8_t)((early_f16000_shift_read_index + 1u) % LC_EARLY_F16000_SHIFT_QUEUE_SIZE);
    early_f16000_shift_count--;
    return value;
}

static void lc_memory_f16000_shift_clear(void) {
    early_f16000_shift_read_index = 0;
    early_f16000_shift_write_index = 0;
    early_f16000_shift_count = 0;
}

static uint8_t lc_memory_apply_rom_diagnostic_overrides(uint8_t value,
                                                        uint32_t pc,
                                                        uint32_t offset) {
#if LC_ENABLE_BASILISK_NO_FPU_CAPABILITY_SHADOW
    const uint32_t rom_offset = pc & 0x0007ffffu;
    if (offset == 0x00003610u && rom_offset >= 0x00047c30u && rom_offset < 0x000483fcu) {
        // BasiliskII clears HWCfgFlags bit 28 when FPUType==0 before entering
        // the ROM reset continuation (`src/emul_op.cpp`, M68K_EMUL_OP_RESET),
        // and makes FPU resources optional in `src/rom_patches.cpp`.  Apply the
        // no-FPU capability view only while the ROM is in reset subtest 0x8d;
        // earlier reset dispatch still needs the unmodified machine flags.
        value = (uint8_t)(value & (uint8_t)~0x10u);
    }
#else
    (void)pc;
    (void)offset;
#endif
    return value;
}

static uint8_t lc_memory_read_rom_byte_with_diagnostics(const lc_memory_bus_t *bus,
                                                        uint32_t pc,
                                                        uint32_t offset) {
    return lc_memory_apply_rom_diagnostic_overrides(bus->rom[offset], pc, offset);
}

static bool lc_memory_should_read_synthetic_ram_test_list(uint32_t pc, uint32_t address) {
#if LC_ENABLE_SYNTHETIC_RAM_TEST_LIST
    const uint32_t rom_offset = pc & 0x000fffffu;
    const bool pc_in_bootglobs_load = rom_offset >= 0x0000010eu && rom_offset < 0x00000114u;
    const bool pc_in_bootglobs_list_walk = rom_offset >= 0x00000a70u && rom_offset < 0x00000aa0u;
    const bool pc_in_reset_list = rom_offset >= 0x00046576u && rom_offset < 0x000465d2u;
    if (!pc_in_bootglobs_load && !pc_in_bootglobs_list_walk && !pc_in_reset_list) {
        return false;
    }
    uint32_t normalized = address;
    const bool alias_read = address_in_window(address, LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE,
                                             LC_GUEST_RAM_SIZE);
    if (alias_read) {
        normalized = address - LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE;
    }
    const uint32_t top_candidates[] = {
        LC_GUEST_RAM_SIZE,
        LC_GUEST_RAM_SIZE - 0x00100000u,
        LC_GUEST_RAM_SIZE - 0x00200000u,
        LC_GUEST_RAM_SIZE - 0x00300000u,
    };
    bool in_boot_globs = false;
    for (size_t i = 0; i < sizeof(top_candidates) / sizeof(top_candidates[0]); i++) {
        const uint32_t top = top_candidates[i];
        const bool one_mb_alias_only = top == (LC_GUEST_RAM_SIZE - 0x00300000u) &&
                                       !address_in_window(address, LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE,
                                                          LC_GUEST_RAM_SIZE);
        if (!one_mb_alias_only && top >= LC_SYNTHETIC_BOOT_GLOBS_SIZE &&
            address_in_window(normalized, top - LC_SYNTHETIC_BOOT_GLOBS_SIZE,
                              LC_SYNTHETIC_BOOT_GLOBS_SIZE)) {
            in_boot_globs = true;
            break;
        }
    }
    if (!in_boot_globs) {
        return false;
    }
    // The reset RAM fill/check routine deliberately writes and verifies the
    // whole region, including the top-of-RAM BootGlobs/list address.  Let it
    // see the physical RAM contents so the memory test can pass, but keep the
    // Basilisk-style synthetic BootGlobs/list visible to reset code before and
    // after that destructive probe.
    if (!alias_read && rom_offset >= 0x00046850u && rom_offset < 0x00046990u) {
        return false;
    }
    return true;
#else
    (void)pc;
    (void)address;
    return false;
#endif
}

static uint8_t lc_memory_read_synthetic_ram_test_list(uint32_t address) {
#if LC_ENABLE_SYNTHETIC_RAM_TEST_LIST
    uint32_t normalized = address;
    if (address_in_window(address, LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE, LC_GUEST_RAM_SIZE)) {
        normalized = address - LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE;
    }

    uint32_t top = LC_GUEST_RAM_SIZE;
    const uint32_t top_candidates[] = {
        LC_GUEST_RAM_SIZE,
        LC_GUEST_RAM_SIZE - 0x00100000u,
        LC_GUEST_RAM_SIZE - 0x00200000u,
        LC_GUEST_RAM_SIZE - 0x00300000u,
    };
    for (size_t i = 0; i < sizeof(top_candidates) / sizeof(top_candidates[0]); i++) {
        const uint32_t candidate = top_candidates[i];
        const bool one_mb_alias_only = candidate == (LC_GUEST_RAM_SIZE - 0x00300000u) &&
                                       !address_in_window(address, LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE,
                                                          LC_GUEST_RAM_SIZE);
        if (!one_mb_alias_only && candidate >= LC_SYNTHETIC_BOOT_GLOBS_SIZE &&
            address_in_window(normalized, candidate - LC_SYNTHETIC_BOOT_GLOBS_SIZE,
                              LC_SYNTHETIC_BOOT_GLOBS_SIZE)) {
            top = candidate;
            break;
        }
    }

    uint32_t word = 0;
    uint32_t word_base = normalized & ~3u;
    const uint32_t list_base = top - 0x1cu;
    if (word_base == top - 0x24u) {
        // BasiliskII's M68K_EMUL_OP_PATCH_BOOT_GLOBS writes MemTop at
        // A4-20 and then sets A6 to RAMBase+RAMSize.  The unpatched LC ROM
        // instruction at 0x4080010e loads A6 from this field.
        word = top;
    } else if (word_base == list_base) {
        word = 0x00000000u;
    } else if (word_base == list_base + 4u) {
        word = top;
    } else if (word_base == list_base + 8u) {
        word = 0xffffffffu;
    } else if (word_base == list_base + 12u) {
        word = 0x00000000u;
    }

    const uint32_t offset = normalized & 3u;
    uint8_t value = (uint8_t)(word >> (24u - (offset * 8u)));
    if (normalized == top - 0x2au) {
        // BasiliskII marks BootGlobs as no-MMU: *(A4-26)=0.
        value = 0x00u;
    } else if (normalized == top - 0x29u) {
        // BasiliskII sets the paired no-MMU flag bit: *(A4-25)|=1.
        value = 0x01u;
    }
    return value;
#else
    (void)address;
    return 0xffu;
#endif
}

static bool lc_memory_should_read_post_reset_no_mmu_flag(uint32_t pc, uint32_t address) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    (void)pc;
    (void)address;
    return false;
#else
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset < 0x0004169au || rom_offset > 0x000416a0u) {
        return false;
    }
    uint32_t normalized = address;
    if (address_in_window(address, LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE, LC_GUEST_RAM_SIZE)) {
        normalized = address - LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE;
    }
    // The unpatched LC InitMMU path tests byte -0x1a(A6) and should take the
    // no-MMU return path on this 68EC020 scaffold.  BasiliskII patches the
    // equivalent branch directly; keep the ROM unmodified and expose the
    // observed BootGlobs-relative no-MMU flag instead.
    return normalized == (LC_GUEST_RAM_SIZE - 0x36u);
#endif
}

static bool lc_memory_should_read_post_reset_dispatch_kind(uint32_t pc, uint32_t address) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    (void)pc;
    (void)address;
    return false;
#else
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset == 0x000418f8u && address == 0x001fffebu;
#endif
}

static bool lc_memory_should_read_post_reset_second_pass_locals(uint32_t pc, uint32_t address) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    (void)pc;
    (void)address;
    return false;
#else
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset == 0x000418e8u && address >= 0x001fffa0u && address < 0x001fffb0u;
#endif
}

static bool lc_memory_should_read_post_reset_record_table(uint32_t pc, uint32_t address) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset == 0x00041b1eu && address >= 0x001fffa0u && address < 0x001fffb0u;
}

static uint8_t lc_memory_read_post_reset_record_table(uint32_t address) {
    const uint32_t word_base = address & ~3u;
    uint32_t word = 0;
    if (word_base == 0x001fffa0u) {
        word = 0x00000000u;
    } else if (word_base == 0x001fffa4u) {
        word = LC_GUEST_RAM_SIZE;
    } else if (word_base == 0x001fffa8u) {
        word = 0xffffffffu;
    } else {
        word = 0x00000000u;
    }
    return (uint8_t)(word >> (24u - ((address & 3u) * 8u)));
}

static bool lc_memory_should_read_post_reset_finalizer_descriptor(uint32_t pc, uint32_t address) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset >= 0x00041c46u && rom_offset < 0x00041ce0u &&
           address >= 0x00600000u && address < 0x00600010u;
}

static uint8_t lc_memory_read_post_reset_finalizer_descriptor(uint32_t address) {
    // The current synthetic address-map path leaves the finalizer pointing at a
    // logical descriptor table near 0x00600000.  That range is not backed yet,
    // so the ROM previously read 0xffffffff as both the range limit and case
    // descriptor, dispatching into the middle of an instruction at 0x40841cbe.
    // Provide a one-entry descriptor plus terminator while the real address-map
    // table is being modeled.
    const uint32_t word_base = address & ~3u;
    uint32_t word = 0x00000000u;
    if (word_base == 0x00600000u) {
        word = 0x00000008u;
    } else if (word_base == 0x00600008u) {
        word = 0xffffffffu;
    }
    return (uint8_t)(word >> (24u - ((address & 3u) * 8u)));
}

static bool lc_memory_should_read_post_reset_finalizer_rom_shape(uint32_t pc, uint32_t offset) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset >= 0x00041df4u && rom_offset < 0x00041e10u &&
           offset >= 0x00040f02u && offset < 0x00040f06u;
}

static bool lc_memory_should_read_post_reset_swap_mmu_dispatch_nop(uint32_t pc,
                                                                   uint32_t offset) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    // The direct EC020 micro-probe reaches the ROM's generic A-line dispatcher
    // for A05D before low-memory trap tables/exception unwinding are real.  The
    // bus callback seeds D0 and the stack return slot; expose three NOPs in
    // place of the indirect JSR so the ROM's own epilogue runs normally.
    return lc_musashi_bus_should_nop_post_reset_swap_mmu_dispatch() &&
           rom_offset >= 0x00009a04u && rom_offset < 0x00009a0au &&
           offset >= 0x00009a04u && offset < 0x00009a0au;
}

static uint8_t lc_memory_read_post_reset_swap_mmu_dispatch_nop(uint32_t offset) {
    static const uint8_t nops[] = {0x4eu, 0x71u, 0x4eu, 0x71u, 0x4eu, 0x71u};
    return nops[offset - 0x00009a04u];
}

static bool lc_memory_should_read_basilisk_dispatch_magic(uint32_t pc, uint32_t address) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    // In the RAM-owned low-memory slice, $0DB0 is seeded into guest RAM and
    // should be read through the normal RAM path instead of a PC-gated hook.
    (void)pc;
    (void)address;
    return false;
#else
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset >= 0x00002316u && rom_offset < 0x00002322u &&
           address >= 0x00000db0u && address < 0x00000db4u;
#endif
}

static uint8_t lc_memory_read_basilisk_dispatch_magic(uint32_t address) {
    static const uint8_t magic[] = {0x5au, 0x93u, 0x2bu, 0xc7u};
    return magic[address - 0x00000db0u];
}

static bool lc_memory_post_reset_core_lowmem_seed_point(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset == 0x000001acu ||
           (rom_offset >= 0x000099b0u && rom_offset < 0x00009a40u);
}

static bool lc_memory_should_read_post_reset_lowmem_callback(uint32_t pc, uint32_t address) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    // The seed point immediately before the $0DBC callback read refreshes this
    // cell in RAM.  Let the read path fall through to the RAM byte.
    (void)pc;
    (void)address;
    return false;
#else

    const uint32_t rom_offset = pc & 0x000fffffu;
    return !post_reset_core_lowmem_ram_seeded && rom_offset == 0x000001acu &&
           address >= 0x00000dbcu && address < 0x00000dc0u;
#endif
}

static uint8_t lc_memory_read_post_reset_lowmem_callback(uint32_t address) {
    // The direct reset micro-probe does not yet own the full low-memory globals
    // table.  The no-MMU continuation calls through long $0DBC immediately
    // after InitMMU; point it at a ROM RTS while the real startup vector/table
    // initialization is being modeled.
    static const uint8_t callback[] = {0x40u, 0x80u, 0x03u, 0x38u};
    return callback[address - 0x00000dbcu];
}

static uint32_t lc_memory_peek_ram32_direct(const lc_memory_bus_t *bus, uint32_t address) {
    if (bus == NULL || bus->ram == NULL || address + 3u >= bus->ram_size) {
        return 0xffffffffu;
    }
    return ((uint32_t)bus->ram[address + 0u] << 24u) |
           ((uint32_t)bus->ram[address + 1u] << 16u) |
           ((uint32_t)bus->ram[address + 2u] << 8u) |
           (uint32_t)bus->ram[address + 3u];
}

static bool lc_memory_plausible_rom_handler(uint32_t handler) {
    return address_in_window(handler, LC_ROM_WINDOW_32BIT_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE) ||
           address_in_window(handler, LC_ROM_WINDOW_24BIT_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE);
}

static bool lc_memory_low_atrap_table_address(uint32_t address) {
    return address >= 0x00000400u && address < 0x00000600u;
}

static bool lc_memory_high_atrap_table_address(uint32_t address) {
    return (address >= 0x00000020u && address < 0x00000400u) ||
           (address >= 0x00000e00u && address < 0x00002e00u);
}

static bool lc_memory_atrap_table_has_ram_handler(const lc_memory_bus_t *bus,
                                                   uint32_t address) {
    if (!lc_memory_low_atrap_table_address(address) &&
        !lc_memory_high_atrap_table_address(address)) {
        return false;
    }
    const uint32_t base = address & ~3u;
    const uint32_t handler = lc_memory_peek_ram32_direct(bus, base);
    return lc_memory_plausible_rom_handler(handler);
}

static bool lc_memory_low_atrap_entry_owned(uint32_t address) {
    if (!lc_memory_low_atrap_table_address(address)) {
        return false;
    }
    const uint32_t trap = (address - 0x00000400u) / 4u;
    return trap < 0x80u && post_reset_atrap_table_owned[trap];
}

static bool lc_memory_should_read_post_reset_atrap_table(const lc_memory_bus_t *bus,
                                                         uint32_t pc,
                                                         uint32_t address) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    // ROM A-line dispatch handles low-numbered OS traps through the low-memory
    // table at $0400 + (trap & 0xff) * 4.  The current direct reset probe has
    // not yet let the ROM table builder own that low-memory range, so it still
    // contains destructive RAM-fill pattern bytes.  Provide a bounded default
    // table only while the dispatcher is reading handler longs; this is narrower
    // than NOPing the dispatcher and exposes the actual trap numbers used by the
    // post-reset path.  For high A-trap tables, prefer RAM when the ROM has
    // already installed a plausible handler; the old synthetic default masked
    // real entries such as A895/ShutDown at $1054 -> 0x4080ed7e and caused the
    // later low-RAM 0x00007fba callback.
    if (rom_offset >= 0x00009a04u && rom_offset <= 0x00009a22u &&
        lc_memory_low_atrap_table_address(address)) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
        if (lc_memory_low_atrap_entry_owned(address)) {
            return false;
        }
#endif
        return true;
    }
    if ((rom_offset == 0x000099c6u || rom_offset == 0x000099e0u) &&
        lc_memory_high_atrap_table_address(address)) {
        return !lc_memory_atrap_table_has_ram_handler(bus, address);
    }
    return false;
}

static uint8_t lc_memory_read_post_reset_atrap_table(uint32_t address) {
    // Route provisional low-memory trap entries to a ROM "moveq #0,d0; rts"
    // sequence unless the reset path has installed a handler through
    // SetTrapAddress.  A05D / SwapMMUMode is still skipped at the dispatch JSR
    // by a bus-side EC020 frame repair.
    uint32_t handler = 0;
    if (address >= 0x00000400u && address < 0x00000600u) {
        const uint32_t trap = (address - 0x00000400u) / 4u;
        handler = trap < 0x80u ? post_reset_atrap_table_overrides[trap] : 0u;
    }
    if (handler == 0u) {
        handler = 0x40800d88u;
    }
    return (uint8_t)(handler >> (24u - ((address & 3u) * 8u)));
}

static bool lc_memory_pc_in_post_reset_resource_manager(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset >= 0x0001b600u && rom_offset < 0x0001c100u;
}

static void lc_memory_resource_seed8(lc_memory_bus_t *bus, uint32_t address, uint8_t value) {
    lc_memory_seed8(bus, address, value);
}

static void lc_memory_resource_seed16(lc_memory_bus_t *bus, uint32_t address, uint16_t value) {
    lc_memory_seed16(bus, address, value);
}

static void lc_memory_resource_seed32(lc_memory_bus_t *bus, uint32_t address, uint32_t value) {
    lc_memory_seed32(bus, address, value);
}

static bool lc_memory_basilisk_slot_rom_active(const lc_memory_bus_t *bus) {
    if (bus == NULL || bus->rom == NULL || bus->rom_size < 8u) {
        return false;
    }
    const size_t off = bus->rom_size - 6u;
    return bus->rom[off + 0u] == 0x5au && bus->rom[off + 1u] == 0x93u &&
           bus->rom[off + 2u] == 0x2bu && bus->rom[off + 3u] == 0xc7u;
}

static void lc_memory_seed_post_reset_resource_map_ram(lc_memory_bus_t *bus) {
    if (post_reset_resource_map_ram_seeded || bus == NULL || bus->ram == NULL ||
        bus->ram_size <= 0x00008340u) {
        return;
    }

    // Seed a RAM-backed synthetic resource map so Resource Manager mutations
    // performed through SetHandleSize/BlockMove are visible to later reads. The
    // low-memory globals remain synthetic because the direct reset micro-probe
    // RAM-fill test overwrites the real low-memory area before Resource Manager
    // starts.
    memset(&bus->ram[0x00008000u], 0, 0x00000340u);

    lc_memory_resource_seed32(bus, 0x00008000u, 0x00008020u); // TopMapHndl handle cell.
    lc_memory_resource_seed32(bus, 0x00008020u, 0x00008000u); // resource data area base.
    lc_memory_resource_seed32(bus, 0x00008024u, 0x00008020u); // map offset.
    lc_memory_resource_seed32(bus, 0x00008028u, 0x00000340u); // resource data length.
    lc_memory_resource_seed32(bus, 0x0000802cu, 0x00000320u); // map length.
    lc_memory_resource_seed32(bus, 0x00008030u, 0x00000000u); // next map handle.
    lc_memory_resource_seed32(bus, 0x00008034u, 0x00010000u); // fileRefNum/map id + attrs.
    lc_memory_resource_seed32(bus, 0x00008038u, 0x001e0088u); // type list, name list.
    lc_memory_resource_seed32(bus, 0x0000803cu, 0x00000003u); // type count: four minus one.

    lc_memory_resource_seed32(bus, 0x00008040u, 0x53545220u); // 'STR '
    lc_memory_resource_seed32(bus, 0x00008044u, 0x00000022u); // one ref, ref list 0x8060.
    lc_memory_resource_seed32(bus, 0x00008048u, 0x43555253u); // 'CURS'
    lc_memory_resource_seed32(bus, 0x0000804cu, 0x0001002eu); // two refs, ref list 0x806c.
    lc_memory_resource_seed32(bus, 0x00008050u, 0x464f4e54u); // 'FONT'
    lc_memory_resource_seed32(bus, 0x00008054u, 0x00000046u); // one ref, ref list 0x8084.
    lc_memory_resource_seed32(bus, 0x00008058u, 0x4b4d4150u); // 'KMAP'
    lc_memory_resource_seed32(bus, 0x0000805cu, 0x00000052u); // one ref, ref list 0x8090.

    lc_memory_resource_seed32(bus, 0x00008060u, 0xe000ffffu); // STR id -8192, no name.
    lc_memory_resource_seed32(bus, 0x00008064u, 0x00000100u);
    lc_memory_resource_seed32(bus, 0x00008068u, 0x00008320u); // preloaded resource handle.
    lc_memory_resource_seed32(bus, 0x0000806cu, 0x0008ffffu); // CURS id 8, no name.
    lc_memory_resource_seed32(bus, 0x00008070u, 0x00000180u);
    lc_memory_resource_seed32(bus, 0x00008074u, 0x00000000u);
    lc_memory_resource_seed32(bus, 0x00008078u, 0x0009ffffu); // spare CURS id 9.
    lc_memory_resource_seed32(bus, 0x0000807cu, 0x00000200u);
    lc_memory_resource_seed32(bus, 0x00008080u, 0x00000000u);
    lc_memory_resource_seed32(bus, 0x00008084u, 0x0008ffffu); // FONT id 8.
    lc_memory_resource_seed32(bus, 0x00008088u, 0x00000280u);
    lc_memory_resource_seed32(bus, 0x0000808cu, 0x00000000u);
    lc_memory_resource_seed32(bus, 0x00008090u, 0x0008ffffu); // KMAP id 8.
    lc_memory_resource_seed32(bus, 0x00008094u, 0x00000300u);
    lc_memory_resource_seed32(bus, 0x00008098u, 0x00000000u);

    lc_memory_resource_seed32(bus, 0x00008100u, 0x00000010u); // STR data length.
    lc_memory_resource_seed32(bus, 0x00008104u, 0x0f4c4320u); // Pascal string: "LC ROM HOST OK".
    lc_memory_resource_seed32(bus, 0x00008108u, 0x524f4d20u);
    lc_memory_resource_seed32(bus, 0x0000810cu, 0x484f5354u);
    lc_memory_resource_seed32(bus, 0x00008110u, 0x204f4b00u);
    lc_memory_resource_seed32(bus, 0x00008180u, 0x00000044u);
    lc_memory_resource_seed32(bus, 0x00008200u, 0x00000044u);
    lc_memory_resource_seed32(bus, 0x00008280u, 0x00000044u);
    lc_memory_resource_seed32(bus, 0x00008300u, 0x00000044u);
    lc_memory_resource_seed32(bus, 0x00008320u, 0x00008104u); // STR resource master pointer.

    post_reset_resource_map_ram_seeded = true;
    ESP_LOGI(TAG,
             "LC seeded RAM-backed post-reset Resource Manager map: handle=0x00008000 map=0x00008020 nameListOffset=0x0088");
}

static void lc_memory_seed_post_reset_core_lowmem_ram(lc_memory_bus_t *bus) {
    if (post_reset_core_lowmem_ram_seeded || bus == NULL || bus->ram == NULL ||
        bus->ram_size <= 0x00000dc0u) {
        return;
    }

#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    lc_memory_seed32(bus, 0x00000028u, 0x408099b0u); // A-line vector -> LC ROM dispatcher.
#endif
    lc_memory_seed32(bus, 0x00000db0u, 0x5a932bc7u);
    if (!lc_memory_basilisk_slot_rom_active(bus)) {
        lc_memory_seed32(bus, 0x00000dbcu, 0x40800338u);
    }

#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    lc_memory_seed32(bus, 0x000001d4u, 0x00f00000u); // VIA base.
    lc_memory_seed32(bus, 0x000001d8u, 0x00f04000u); // SCC read base.
    lc_memory_seed32(bus, 0x000001dcu, 0x00f04000u); // SCC write base.
    lc_memory_seed32(bus, 0x000001e0u, 0x00f16000u); // IWM/SWIM base.

    // Retire only table entries that have proven safe when read from RAM.  A019
    // now has a minimal RAM-owned InitZone/zone-header seed in the Musashi bus
    // trap surface, so the dispatcher can read its low table entry from RAM too.
    static const uint8_t ram_owned_low_traps[] = {
        0x47u, 0x3fu, 0x51u, 0x2du, 0x1eu, 0x2eu,
        0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x29u, 0x2au, 0x2bu,
        0x0au, 0x10u, 0x11u, 0x01u, 0x1fu, 0x55u, 0x5bu, 0x6eu, 0x19u,
    };
    for (size_t i = 0; i < sizeof(ram_owned_low_traps) / sizeof(ram_owned_low_traps[0]); i++) {
        lc_memory_seed_post_reset_low_atrap_entry(bus, ram_owned_low_traps[i], 0x40800d88u);
    }
    // ShutDown (A05B) handler lives at ROM+0x580.  Seed its table entry so the
    // A-line dispatcher finds it instead of jumping to zero.
    lc_memory_seed_post_reset_low_atrap_entry(bus, 0x5bu, LC_ROM_WINDOW_32BIT_BASE_CANDIDATE + 0x00000580u);
    for (uint32_t addr = 0x00000e00u; addr < 0x00002e00u; addr += 4u) {
        lc_memory_seed32(bus, addr, 0x40800d88u);
    }
#endif

    post_reset_core_lowmem_ram_seeded = true;
    ESP_LOGI(TAG,
             "LC seeded RAM-owned post-reset core low memory: dispatch_magic=0x5a932bc7 callback_0dbc=0x40800338");
}

static bool lc_memory_post_reset_memory_layout_seed_point(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    // Do not seed the 0x001fffa0 stack/local window as early as the memory-layout
    // entry/header; it is still active stack then.  Seed it at the second-pass
    // entry immediately before the former synthetic reads consumed those bytes.
    return rom_offset >= 0x000418e4u && rom_offset < 0x00041910u;
}

static void lc_memory_seed_post_reset_memory_layout_ram(lc_memory_bus_t *bus) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    if (post_reset_memory_layout_ram_seeded || bus == NULL || bus->ram == NULL ||
        bus->ram_size <= 0x003fffcau) {
        return;
    }

    // Move the post-reset no-MMU byte and second-pass address-map locals out of
    // per-PC synthetic reads. These cells are restored after the destructive RAM
    // fill/copy diagnostics, before the memory-layout code reads them.
    lc_memory_seed8(bus, LC_GUEST_RAM_SIZE - 0x36u, 0x01u);
    for (uint32_t addr = 0x001fffa0u; addr < 0x001fffb0u; addr++) {
        lc_memory_seed8(bus, addr, 0x00u);
    }
    lc_memory_seed8(bus, 0x001fffebu, 0x06u);

    post_reset_memory_layout_ram_seeded = true;
    ESP_LOGI(TAG,
             "LC seeded RAM-owned post-reset memory-layout cells: no_mmu=0x%08x dispatch_kind=0x001fffeb second_pass=0x001fffa0..0x001fffaf",
             LC_GUEST_RAM_SIZE - 0x36u);
#else
    (void)bus;
#endif
}

static void lc_memory_seed_post_reset_resource_lowmem_ram(lc_memory_bus_t *bus) {
    if (post_reset_resource_lowmem_ram_seeded || bus == NULL || bus->ram == NULL ||
        bus->ram_size <= 0x00008340u) {
        return;
    }

    // Move Resource Manager globals out of PC-gated synthetic read returns and
    // into RAM-visible low-memory cells.  This is still a transitional seed for
    // the direct ROM-entry probe, but after this point ordinary RAM reads see
    // and mutate the same state as the synthetic map/Memory Manager helpers.
    lc_memory_seed32(bus, 0x000002aeu, LC_ROM_WINDOW_32BIT_BASE_CANDIDATE); // ROMBase.
    lc_memory_seed32(bus, 0x0000031au, 0x00ffffffu); // Resource data offset mask.
    lc_memory_seed32(bus, 0x00000698u, 0x4081b70eu); // Resource Manager prologue vector.
    lc_memory_seed32(bus, 0x0000069cu, 0x4081b7cau); // Resource Manager epilogue/error vector.
    lc_memory_seed32(bus, 0x000007f0u, 0x4081b8fau); // Resource data load/realize vector.
    lc_memory_seed32(bus, 0x000001d4u, 0x00f00000u); // VIA base.
    lc_memory_seed32(bus, 0x000001d8u, 0x00f04000u); // SCC read base.
    lc_memory_seed32(bus, 0x000001dcu, 0x00f04000u); // SCC write base.
    lc_memory_seed32(bus, 0x000001e0u, 0x00f16000u); // IWM/SWIM base.
    lc_memory_seed32(bus, 0x00000a50u, 0x00008000u); // TopMapHndl handle cell pointer.
    lc_memory_seed32(bus, 0x00000a54u, 0x00008000u); // SysMapHndl handle cell pointer.
    lc_memory_seed16(bus, 0x00000a58u, 0x0001u);     // SysMap.
    lc_memory_seed16(bus, 0x00000a5au, 0x0001u);     // CurMap.
    lc_memory_seed16(bus, 0x00000a60u, 0x0000u);     // Resource Manager no deferred return proc.
    lc_memory_seed32(bus, 0x00000af2u, 0x00000000u); // Deferred return proc pointer.
    lc_memory_seed_post_reset_resource_map_ram(bus);

    post_reset_resource_lowmem_ram_seeded = true;
    ESP_LOGI(TAG,
             "LC seeded RAM-owned post-reset Resource Manager low memory: ROMBase=0x%08x TopMapHndl=0x00008000 SysMapHndl=0x00008000 SysMap=1 CurMap=1",
             LC_ROM_WINDOW_32BIT_BASE_CANDIDATE);
}

static void lc_memory_seed_ram_owned_low_memory(lc_memory_bus_t *bus) {
#if LC_ENABLE_RAM_OWNED_LOW_MEMORY
    if (bus == NULL || bus->ram == NULL || bus->ram_size <= 0x00002e00u) {
        return;
    }

    const uint32_t default_trap_handler = 0x40800d88u;
    lc_memory_seed32(bus, 0x00000028u, 0x408099b0u); // A-line vector -> LC ROM dispatcher.
    for (uint32_t addr = 0x00000400u; addr < 0x00000600u; addr += 4u) {
        lc_memory_seed32(bus, addr, default_trap_handler);
    }
    for (uint32_t addr = 0x00000e00u; addr < 0x00002e00u; addr += 4u) {
        lc_memory_seed32(bus, addr, default_trap_handler);
    }

    lc_memory_seed32(bus, 0x00000db0u, 0x5a932bc7u); // Basilisk dispatch magic.
    if (!lc_memory_basilisk_slot_rom_active(bus)) {
        lc_memory_seed32(bus, 0x00000dbcu, 0x40800338u); // Post-InitMMU callback: ROM RTS.
    }

    lc_memory_seed32(bus, 0x000002aeu, LC_ROM_WINDOW_32BIT_BASE_CANDIDATE); // ROMBase.
    lc_memory_seed32(bus, 0x0000031au, 0x00ffffffu); // Resource data offset mask.
    lc_memory_seed32(bus, 0x00000698u, 0x4081b70eu); // Resource Manager prologue vector.
    lc_memory_seed32(bus, 0x0000069cu, 0x4081b7cau); // Resource Manager epilogue/error vector.
    lc_memory_seed32(bus, 0x000007f0u, 0x4081b8fau); // Resource data load/realize vector.
    lc_memory_seed32(bus, 0x000001d4u, 0x00f00000u); // VIA base.
    lc_memory_seed32(bus, 0x000001d8u, 0x00f04000u); // SCC read base.
    lc_memory_seed32(bus, 0x000001dcu, 0x00f04000u); // SCC write base.
    lc_memory_seed32(bus, 0x000001e0u, 0x00f16000u); // IWM/SWIM base.
    lc_memory_seed32(bus, 0x00000a50u, 0x00008000u); // TopMapHndl handle cell pointer.
    lc_memory_seed32(bus, 0x00000a54u, 0x00008000u); // SysMapHndl handle cell pointer.
    lc_memory_seed16(bus, 0x00000a58u, 0x0001u);     // SysMap.
    lc_memory_seed16(bus, 0x00000a5au, 0x0001u);     // CurMap.
    lc_memory_seed16(bus, 0x00000a60u, 0x0000u);     // Resource Manager no deferred return proc.
    lc_memory_seed32(bus, 0x00000af2u, 0x00000000u); // Deferred return proc pointer.
    // Do not seed the Resource Manager map body here: early RAM-fill diagnostics
    // can overwrite 0x00008000.  The post-reset Resource Manager entry reseeds
    // that map after the destructive RAM test has completed.

    ESP_LOGW(TAG,
             "LC RAM-owned low-memory seed enabled: dispatch_magic=0x5a932bc7 callback_0dbc=0x40800338 default_trap=0x%08" PRIx32,
             default_trap_handler);
#else
    (void)bus;
#endif
}

static bool lc_memory_should_read_fake_gdevice_chain(uint32_t pc, uint32_t address) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset < 0x000023aeu || rom_offset >= 0x000023b8u) {
        return false;
    }
    return (address >= 0x000008a8u && address < 0x000008acu) ||
           (address >= 0x00001080u && address < 0x00001084u) ||
           (address >= 0x00001090u && address < 0x00001094u);
}

static uint8_t lc_memory_read_fake_gdevice_chain(uint32_t address) {
    uint32_t word = 0;
    uint32_t base = address & ~3u;
    if (base == 0x000008a8u) {
        word = 0x00001080u;
    } else if (base == 0x00001080u) {
        word = 0x00001090u;
    } else if (base == 0x00001090u) {
        word = 0x00000000u;
    }
    return (uint8_t)(word >> (24u - ((address & 3u) * 8u)));
}

static bool lc_memory_should_read_fake_video_globals(uint32_t pc, uint32_t address) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset < 0x000023f2u || rom_offset >= 0x00002416u) {
        return false;
    }
    return (address >= 0x00000824u && address < 0x00000828u) ||
           (address >= 0x00000c24u && address < 0x00000c28u);
}

static uint8_t lc_memory_read_fake_video_globals(uint32_t address) {
    uint32_t word = 0;
    uint32_t base = address & ~3u;
    if (base == 0x00000824u) {
        word = 0x00020000u;
    } else if (base == 0x00000c24u) {
        word = 0x00001000u;
    }
    return (uint8_t)(word >> (24u - ((address & 3u) * 8u)));
}

static bool lc_memory_pc_in_reset_scc_probe(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset >= 0x00046df6u && rom_offset < 0x00046fa0u;
}

static bool lc_memory_pc_in_reset_via_irq_probe(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset >= 0x0004713eu && rom_offset < 0x0004730cu;
}

static bool lc_memory_pc_in_post_reset_mode_probe(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    return rom_offset == 0x0004188cu || rom_offset == 0x00041890u ||
           rom_offset == 0x000418a0u;
}

static uint8_t lc_memory_read_post_reset_mode_probe(uint32_t pc) {
    const uint32_t rom_offset = pc & 0x000fffffu;
    if (rom_offset == 0x0004188cu) {
        return 0x7fu;
    }
    if (rom_offset == 0x000418a0u) {
        // Let the probe observe the byte+1 value written through the paired
        // address.  This preserves the ROM's observed mode=3 result while the
        // underlying MMU/address-map descriptor is still synthetic.
        return 0x21u;
    }
    return 0x20u;
}

static uint8_t lc_memory_read_reset_via_irq_ifr(uint32_t pc) {
    uint8_t value = 0;
    if (early_lc_via_reset_irq_phase == 1u) {
        const uint32_t read = ++early_lc_via_reset_irq_ifr_reads[1];
        // First timer phase: the ROM waits until CA1-like bit 1 has fired 10
        // times, but expects timer bits 6 and 5 to show a wider distribution.
        // Feed 128 bit-6 events, one bit-5 event, then ten bit-1 events.
        if (read <= 128u) {
            value |= 0x40u;
        }
        if (read == 1u) {
            value |= 0x20u;
        }
        if (read > 128u && read <= 138u) {
            value |= 0x02u;
        }
        if (read <= 3u || read == 128u || read == 129u || read == 138u) {
            ESP_LOGI(TAG,
                     "LC reset VIA IRQ synthetic IFR: phase=1 read=%" PRIu32
                     " value=0x%02x pc=0x%08" PRIx32,
                     read, value, pc);
        }
    } else if (early_lc_via_reset_irq_phase == 2u) {
        const uint32_t read = ++early_lc_via_reset_irq_ifr_reads[2];
        // Second timer phase: ten bit-1 events, with timer bits 6 and 5 set
        // only once so the ROM's d4==1 and d5==1 checks pass.
        if (read <= 10u) {
            value |= 0x02u;
        }
        if (read == 1u) {
            value |= 0x60u;
        }
        if (read <= 3u || read == 10u) {
            ESP_LOGI(TAG,
                     "LC reset VIA IRQ synthetic IFR: phase=2 read=%" PRIu32
                     " value=0x%02x pc=0x%08" PRIx32,
                     read, value, pc);
        }
    }
    return value;
}

static void lc_memory_note_reset_via_irq_write(uint32_t pc, uint32_t offset, uint8_t value) {
    if (!lc_memory_pc_in_reset_via_irq_probe(pc) ||
        offset != LC_EARLY_VIA_RESET_IRQ_PHASE1_T1_LATCH_OFFSET) {
        return;
    }
    if (value == 0x02u) {
        early_lc_via_reset_irq_phase = 1u;
        early_lc_via_reset_irq_ifr_reads[1] = 0;
        ESP_LOGI(TAG, "LC reset VIA IRQ synthetic phase start: phase=1 pc=0x%08" PRIx32,
                 pc);
    } else if (value == 0x04u) {
        early_lc_via_reset_irq_phase = 2u;
        early_lc_via_reset_irq_ifr_reads[2] = 0;
        ESP_LOGI(TAG, "LC reset VIA IRQ synthetic phase start: phase=2 pc=0x%08" PRIx32,
                 pc);
    }
}

static unsigned lc_memory_f04000_scc_channel(uint32_t reg_offset) {
    return (reg_offset & 0x02u) != 0 ? 1u : 0u;
}

static void lc_memory_write_f04000_scc_control(uint32_t reg_offset, uint8_t value) {
    const unsigned channel = lc_memory_f04000_scc_channel(reg_offset);
    uint8_t selected = early_f04000_scc_selected_register[channel];
    if (selected < 16u) {
        early_f04000_scc_registers[channel][selected] = value;
        early_f04000_scc_selected_register[channel] = 0xffu;
        return;
    }
    early_f04000_scc_selected_register[channel] = (uint8_t)(value & 0x0fu);
}

static uint8_t lc_memory_read_f04000_scc_control(uint32_t reg_offset) {
    const unsigned channel = lc_memory_f04000_scc_channel(reg_offset);
    const uint8_t selected = early_f04000_scc_selected_register[channel];
    if (selected < 16u) {
        const uint8_t value = early_f04000_scc_registers[channel][selected];
        // SCC register reads return the selected RR and then fall back to RR0
        // status.  The ROM's reset test relies on this before selecting the
        // same register again for a write/readback cycle.
        early_f04000_scc_selected_register[channel] = 0xffu;
        return value;
    }
    // No selected register: expose RR0-like idle status for the early reset
    // probe rather than the generic all-ones stub.  Bit 2 is transmit-ready;
    // bit 0 is receive-ready for the diagnostic loopback byte test.
    return (uint8_t)(0x04u | (early_f04000_scc_loopback_ready ? 0x01u : 0x00u));
}

const char *lc_memory_region_name(lc_addr_region_t region) {
    switch (region) {
    case LC_ADDR_REGION_RAM:
        return "ram";
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
        return "rom-24bit-candidate";
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        return "rom-32bit-candidate";
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        return "rom-32bit-masked-candidate";
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
        return "ram-size-probe";
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
        return "io-24bit-candidate";
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return "io-32bit-candidate";
    case LC_ADDR_REGION_UNMAPPED:
    default:
        return "unmapped";
    }
}

const char *lc_memory_io_stub_kind_name(lc_io_stub_kind_t kind) {
    switch (kind) {
    case LC_IO_STUB_NONE:
        return "none";
    case LC_IO_STUB_WINDOW_BASE_HARNESS:
        return "io-window-base/harness";
    case LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE:
        return "early-rom-probe-1c00-stride";
    case LC_IO_STUB_EARLY_LC_VIA_REGISTER:
        return "early-lc-via-register";
    case LC_IO_STUB_EARLY_F04000_DEVICE:
        return "early-f04000-device";
    case LC_IO_STUB_EARLY_F10000_DEVICE:
        return "early-f10000-device";
    case LC_IO_STUB_EARLY_F14000_DEVICE:
        return "early-f14000-device";
    case LC_IO_STUB_EARLY_F16000_DEVICE:
        return "early-f16000-device";
    case LC_IO_STUB_VRAM:
        return "lc-vram";
    case LC_IO_STUB_GENERIC:
    default:
        return "generic-io-stub";
    }
}

static bool lc_memory_is_early_via_register_offset(uint32_t offset) {
    if ((offset & 0x000001ffu) != 0) {
        return false;
    }
    const uint32_t slot_base = offset & ~0x00001fffu;
    return slot_base == 0 || slot_base == 0x00002000u || (slot_base & 0x0001ffffu) == 0;
}

static unsigned lc_memory_via_register_index(uint32_t offset) {
    return (unsigned)((offset >> 9u) & 0x0fu);
}

static const char *lc_memory_io_device_alias_name(uint32_t offset) {
    for (size_t i = 0; i < sizeof(lc_v8_io_device_aliases) / sizeof(lc_v8_io_device_aliases[0]);
         i++) {
        const lc_io_device_alias_t *alias = &lc_v8_io_device_aliases[i];
        if (offset >= alias->start && offset < alias->end_exclusive) {
            return alias->alias;
        }
    }
    return NULL;
}

static lc_io_stub_kind_t lc_memory_classify_io_stub(uint32_t offset) {
    if (offset >= LC_VRAM_IO_OFFSET_BASE && offset < LC_VRAM_IO_OFFSET_LIMIT) {
        return LC_IO_STUB_VRAM;
    }
    if (offset >= 0x00004000u && offset < 0x00006000u) {
        return LC_IO_STUB_EARLY_F04000_DEVICE;
    }
    if (offset >= 0x00010000u && offset < 0x00012000u) {
        return LC_IO_STUB_EARLY_F10000_DEVICE;
    }
    if (offset >= 0x00014000u && offset < 0x00016000u) {
        return LC_IO_STUB_EARLY_F14000_DEVICE;
    }
    if (offset >= 0x00016000u && offset < 0x00018000u) {
        return LC_IO_STUB_EARLY_F16000_DEVICE;
    }
    if ((offset & 0x0001ffffu) == 0x00001c00u) {
        return LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE;
    }
    if (offset == 0) {
        return LC_IO_STUB_WINDOW_BASE_HARNESS;
    }
    if (lc_memory_is_early_via_register_offset(offset)) {
        return LC_IO_STUB_EARLY_LC_VIA_REGISTER;
    }
    return LC_IO_STUB_GENERIC;
}

lc_addr_decode_t lc_memory_decode_address(uint32_t address) {
    lc_addr_decode_t decoded = {
        .region = LC_ADDR_REGION_UNMAPPED,
        .base = 0,
        .size = 0,
        .offset = address,
        .name = lc_memory_region_name(LC_ADDR_REGION_UNMAPPED),
        .writable = false,
    };

    // In Basilisk-compatible host probes, some LC ROM startup continuations can
    // run through a low ROM alias around 0x00100000 after address manipulation.
    // Prefer ROM bytes for that alias before the generic RAM window so PC-
    // relative ROM code/data continues to decode coherently instead of executing
    // zero-filled RAM.
    const lc_memory_bus_t *active = lc_musashi_bus_active();
    if (lc_memory_basilisk_slot_rom_active(active) &&
        address_in_window(address, 0x00100000u, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_24BIT_CANDIDATE;
        decoded.base = 0x00100000u;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = "basilisk-low-rom-alias";
        return decoded;
    }

    if (address < LC_GUEST_RAM_SIZE) {
        decoded.region = LC_ADDR_REGION_RAM;
        decoded.base = 0;
        decoded.size = LC_GUEST_RAM_SIZE;
        decoded.offset = address;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address_in_window(address, LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE, LC_GUEST_RAM_SIZE)) {
        decoded.region = LC_ADDR_REGION_RAM;
        decoded.base = LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE;
        decoded.size = LC_GUEST_RAM_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_24BIT_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_24BIT_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_32BIT_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_32BIT_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_32BIT_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        return decoded;
    }

    if (address_in_window(address, LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE, LC_ROM_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE;
        decoded.base = LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE;
        decoded.size = LC_ROM_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = LC_ENABLE_ROM_MASKED_SHADOW != 0;
        return decoded;
    }

    if (address >= LC_GUEST_RAM_SIZE && address < LC_RAM_SIZE_PROBE_LIMIT) {
        decoded.region = LC_ADDR_REGION_RAM_SIZE_PROBE;
        decoded.base = LC_GUEST_RAM_SIZE;
        decoded.size = LC_RAM_SIZE_PROBE_LIMIT - LC_GUEST_RAM_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address >= LC_IO_24BIT_BASE_CANDIDATE + LC_IO_WINDOW_SIZE - LC_RAM_SIZE_TOP_PROBE_BYTES &&
        address < LC_IO_24BIT_BASE_CANDIDATE + LC_IO_WINDOW_SIZE) {
        decoded.region = LC_ADDR_REGION_RAM_SIZE_PROBE;
        decoded.base = LC_IO_24BIT_BASE_CANDIDATE + LC_IO_WINDOW_SIZE - LC_RAM_SIZE_TOP_PROBE_BYTES;
        decoded.size = LC_RAM_SIZE_TOP_PROBE_BYTES;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        return decoded;
    }

    if (address_in_window(address, LC_IO_24BIT_BASE_CANDIDATE, LC_IO_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_IO_24BIT_CANDIDATE;
        decoded.base = LC_IO_24BIT_BASE_CANDIDATE;
        decoded.size = LC_IO_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        decoded.io_stub = lc_memory_classify_io_stub(decoded.offset);
        decoded.io_stub_name = lc_memory_io_stub_kind_name(decoded.io_stub);
        decoded.io_device_alias = lc_memory_io_device_alias_name(decoded.offset);
        return decoded;
    }

    if (address_in_window(address, LC_BASILISK_FRAME_BASE_CANDIDATE,
                          LC_VRAM_IO_OFFSET_LIMIT - LC_VRAM_IO_OFFSET_BASE)) {
        decoded.region = LC_ADDR_REGION_IO_32BIT_CANDIDATE;
        decoded.base = LC_BASILISK_FRAME_BASE_CANDIDATE;
        decoded.size = LC_VRAM_IO_OFFSET_LIMIT - LC_VRAM_IO_OFFSET_BASE;
        decoded.offset = LC_VRAM_IO_OFFSET_BASE + (address - decoded.base);
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        decoded.io_stub = LC_IO_STUB_VRAM;
        decoded.io_stub_name = lc_memory_io_stub_kind_name(decoded.io_stub);
        decoded.io_device_alias = "basilisk-framebuffer";
        return decoded;
    }

    if (address_in_window(address, LC_IO_32BIT_BASE_CANDIDATE, LC_IO_WINDOW_SIZE)) {
        decoded.region = LC_ADDR_REGION_IO_32BIT_CANDIDATE;
        decoded.base = LC_IO_32BIT_BASE_CANDIDATE;
        decoded.size = LC_IO_WINDOW_SIZE;
        decoded.offset = address - decoded.base;
        decoded.name = lc_memory_region_name(decoded.region);
        decoded.writable = true;
        decoded.io_stub = lc_memory_classify_io_stub(decoded.offset);
        decoded.io_stub_name = lc_memory_io_stub_kind_name(decoded.io_stub);
        decoded.io_device_alias = lc_memory_io_device_alias_name(decoded.offset);
        return decoded;
    }

    return decoded;
}

bool lc_memory_write_is_expected(const lc_addr_decode_t *decoded) {
    if (decoded == NULL) {
        return false;
    }
    switch (decoded->region) {
    case LC_ADDR_REGION_RAM:
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        return decoded->writable;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
    case LC_ADDR_REGION_UNMAPPED:
    default:
        return false;
    }
}

bool lc_memory_should_panic_on_write(uint32_t address) {
#if LC_PANIC_ON_UNEXPECTED_WRITE
    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    return !lc_memory_write_is_expected(&decoded);
#else
    (void)address;
    return false;
#endif
}

void lc_memory_log_unmapped_access(uint32_t pc, uint32_t address, unsigned size, bool write) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 32;

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    if (decoded.region != LC_ADDR_REGION_UNMAPPED) {
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, decoded.offset, (uint16_t)size,
                        write);
        ESP_LOGI(TAG, "LC access pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32 " region=%s offset=0x%08" PRIx32,
                 pc, write ? "write" : "read", size, address, decoded.name, decoded.offset);
        return;
    }

    lc_trace_record(LC_TRACE_EVENT_UNMAPPED_ACCESS, pc, address, 0, (uint16_t)size, write);
    if (logged < log_limit) {
        ESP_LOGW(TAG, "LC unmapped access pc=0x%08" PRIx32 " %s%u addr=0x%08" PRIx32 "%s",
                 pc, write ? "write" : "read", size, address,
                 (write && lc_memory_should_panic_on_write(address)) ? " panic_policy=would-panic" : "");
        logged++;
        if (logged == log_limit) {
            ESP_LOGW(TAG, "LC unmapped access logger reached %u entries; suppressing further unmapped logs", log_limit);
        }
    } else {
        suppressed++;
        if ((suppressed & 0xffu) == 0) {
            ESP_LOGW(TAG, "LC unmapped access logger suppressed %u additional entries", suppressed);
        }
    }
}

static void log_heap_caps(const char *label, uint32_t caps) {
    ESP_LOGI(TAG, "%s free=%u largest=%u", label,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}

void lc_memory_log_write_policy(void) {
    ESP_LOGI(TAG, "LC write policy: panic_on_unexpected_write=%d masked_rom_shadow=%d allowed=RAM,I/O-candidate%s denied=ROM,unmapped",
             LC_PANIC_ON_UNEXPECTED_WRITE, LC_ENABLE_ROM_MASKED_SHADOW,
             LC_ENABLE_ROM_MASKED_SHADOW ? ",masked-ROM-alias-shadow" : "");
    const uint32_t examples[] = {
        0x00000000u,
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_IO_24BIT_BASE_CANDIDATE,
        0x00E00000u,
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        lc_addr_decode_t decoded = lc_memory_decode_address(examples[i]);
        ESP_LOGI(TAG, "LC write policy example addr=0x%08" PRIx32 " region=%s expected=%s would_panic=%s",
                 examples[i], decoded.name,
                 lc_memory_write_is_expected(&decoded) ? "yes" : "no",
                 lc_memory_should_panic_on_write(examples[i]) ? "yes" : "no");
    }
}

void lc_memory_log_initial_map(void) {
    const uint32_t ram_base = LC_RAM_BASE_CANDIDATE;
    const uint32_t ram_size = LC_GUEST_RAM_SIZE;
    const uint32_t ram_end = ram_base + ram_size - 1u;
    const uint32_t ram26_base = LC_RAM_WINDOW_26BIT_ALIAS_BASE_CANDIDATE;
    const uint32_t ram26_end = ram26_base + ram_size - 1u;
    const uint32_t rom24_base = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
    const uint32_t rom24_end = rom24_base + LC_ROM_WINDOW_SIZE - 1u;
    const uint32_t rom32_base = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
    const uint32_t rom32_end = rom32_base + LC_ROM_WINDOW_SIZE - 1u;
    const uint32_t rom32_reset_base = LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE;
    const uint32_t rom32_reset_end = rom32_reset_base + LC_ROM_WINDOW_SIZE - 1u;
    const uint32_t io24_base = LC_IO_24BIT_BASE_CANDIDATE;
    const uint32_t io24_end = io24_base + LC_IO_WINDOW_SIZE - 1u;
    const uint32_t io32_base = LC_IO_32BIT_BASE_CANDIDATE;
    const uint32_t io32_end = io32_base + LC_IO_WINDOW_SIZE - 1u;

    ESP_LOGI(TAG, "LC initial memory scaffold: address_mode=24-bit-first, 32-bit candidates logged only, masked_rom_shadow=%d",
             LC_ENABLE_ROM_MASKED_SHADOW);
    ESP_LOGI(TAG, "LC RAM candidate: base=0x%08" PRIx32 " size=0x%08" PRIx32 " end=0x%08" PRIx32,
             ram_base, ram_size, ram_end);
    ESP_LOGI(TAG, "LC RAM 26-bit alias candidate: base=0x%08" PRIx32 " size=0x%08" PRIx32 " end=0x%08" PRIx32,
             ram26_base, ram_size, ram26_end);
    ESP_LOGI(TAG, "LC ROM 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom24_base, LC_ROM_WINDOW_SIZE, rom24_end);
    ESP_LOGI(TAG, "LC ROM 32-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom32_base, LC_ROM_WINDOW_SIZE, rom32_end);
    ESP_LOGI(TAG, "LC ROM 32-bit reset/header candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             rom32_reset_base, LC_ROM_WINDOW_SIZE, rom32_reset_end);
    ESP_LOGI(TAG, "LC I/O 24-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             io24_base, LC_IO_WINDOW_SIZE, io24_end);
    ESP_LOGI(TAG, "LC I/O 32-bit candidate: base=0x%08" PRIx32 " size=0x%08x end=0x%08" PRIx32,
             io32_base, LC_IO_WINDOW_SIZE, io32_end);
    ESP_LOGW(TAG, "LC ROM and I/O address windows are provisional; reset-vector execution must verify them");
    ESP_LOGW(TAG, "LC masked ROM alias shadow is diagnostic-only reset-overlay scaffolding; it is not a boot claim");
    ESP_LOGI(TAG, "LC VRAM/framebuffer target: %dx%d@%dbpp size=0x%zx placement=undecided",
             DISP_WIDTH, DISP_HEIGHT, LC_GUEST_COLOR_DEPTH_BITS, LC_VRAM_SIZE);
}

void lc_memory_log_decoder_examples(void) {
    const uint32_t examples[] = {
        0x00000000u,
        LC_GUEST_RAM_SIZE - 1u,
        LC_ROM_WINDOW_24BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_RESET_BASE_CANDIDATE,
        LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE,
        LC_GUEST_RAM_SIZE,
        LC_RAM_SIZE_PROBE_LIMIT - 4u,
        LC_IO_24BIT_BASE_CANDIDATE,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00001c00u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00004000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00006000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00010000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00012000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00014000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00016000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00024000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00026000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00040000u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00021c00u,
        LC_IO_24BIT_BASE_CANDIDATE + 0x00041c00u,
        LC_IO_32BIT_BASE_CANDIDATE,
        0x00E00000u,
    };

    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        lc_addr_decode_t decoded = lc_memory_decode_address(examples[i]);
        ESP_LOGI(TAG,
                 "LC decode example addr=0x%08" PRIx32 " region=%s base=0x%08" PRIx32
                 " offset=0x%08" PRIx32 " writable=%s io_stub=%s io_alias=%s",
                 examples[i], decoded.name, decoded.base, decoded.offset,
                 decoded.writable ? "yes" : "no", decoded.io_stub_name != NULL ? decoded.io_stub_name : "none",
                 decoded.io_device_alias != NULL ? decoded.io_device_alias : "none");
    }
}

void lc_memory_probe_guest_ram_allocation(void) {
    log_heap_caps("heap internal before LC RAM probe", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_caps("heap psram before LC RAM probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    size_t requested = LC_GUEST_RAM_SIZE;
    uint8_t *ram = (uint8_t *)heap_caps_malloc(requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ram == NULL) {
        ESP_LOGW(TAG, "LC guest RAM allocation failed for %zu bytes; trying fallback %u bytes",
                 requested, (unsigned)LC_GUEST_RAM_FALLBACK_SIZE);
        requested = LC_GUEST_RAM_FALLBACK_SIZE;
        ram = (uint8_t *)heap_caps_malloc(requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (ram == NULL) {
        ESP_LOGE(TAG, "LC guest RAM allocation probe failed for both primary and fallback sizes");
        log_heap_caps("heap psram after failed LC RAM probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return;
    }

    // Touch both ends so the diagnostic catches obvious allocation/backing issues.
    ram[0] = 0;
    ram[requested - 1u] = 0;
    ESP_LOGI(TAG, "LC guest RAM allocation probe succeeded: ptr=%p size=%zu", (void *)ram,
             requested);
    heap_caps_free(ram);
    log_heap_caps("heap psram after LC RAM probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lc_memory_probe_display_buffer_allocation(void) {
    ESP_LOGI(TAG, "LC display buffer targets: indexed_vram=%zu rgb565_full=%zu rgb565_strip_lines=%u rgb565_strip=%zu",
             LC_VRAM_SIZE, LC_RGB565_FRAMEBUFFER_SIZE, (unsigned)LC_DISPLAY_DMA_STRIP_LINES,
             LC_RGB565_DMA_STRIP_SIZE);
    log_heap_caps("heap dma/internal before display probe", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_caps("heap psram before display probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    uint8_t *vram = (uint8_t *)heap_caps_malloc(LC_VRAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (vram == NULL) {
        ESP_LOGW(TAG, "LC indexed VRAM PSRAM allocation probe failed: size=%zu", LC_VRAM_SIZE);
    } else {
        vram[0] = 0;
        vram[LC_VRAM_SIZE - 1u] = 0;
        ESP_LOGI(TAG, "LC indexed VRAM PSRAM allocation probe succeeded: ptr=%p size=%zu",
                 (void *)vram, LC_VRAM_SIZE);
        heap_caps_free(vram);
    }

    uint8_t *strip = (uint8_t *)heap_caps_malloc(
        LC_RGB565_DMA_STRIP_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (strip == NULL) {
        ESP_LOGW(TAG, "LC RGB565 DMA strip allocation probe failed: size=%zu", LC_RGB565_DMA_STRIP_SIZE);
    } else {
        strip[0] = 0;
        strip[LC_RGB565_DMA_STRIP_SIZE - 1u] = 0;
        ESP_LOGI(TAG, "LC RGB565 DMA strip allocation probe succeeded: ptr=%p size=%zu",
                 (void *)strip, LC_RGB565_DMA_STRIP_SIZE);
        heap_caps_free(strip);
    }

    ESP_LOGI(TAG, "LC full RGB565 framebuffer is diagnostic-only for now; dirty-row/strip rendering remains preferred");
    log_heap_caps("heap dma/internal after display probe", MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap_caps("heap psram after display probe", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void lc_memory_update_io_stub_stats(lc_io_stub_kind_t kind, uint32_t pc,
                                           uint32_t address, uint8_t value, bool write) {
    if ((unsigned)kind >= LC_IO_STUB_KIND_COUNT) {
        kind = LC_IO_STUB_GENERIC;
    }
    lc_io_stub_stats_t *stats = &io_stub_stats[kind];
    if (!stats->seen) {
        stats->first_pc = pc;
        stats->first_address = address;
        stats->seen = true;
    }
    stats->last_pc = pc;
    stats->last_address = address;
    stats->last_value = value;
    if (write) {
        stats->writes++;
    } else {
        stats->reads++;
    }
}

static void lc_memory_update_offset_stats(lc_io_offset_stats_t *stats, uint16_t offset,
                                          uint32_t pc, uint8_t value, bool write) {
    lc_io_offset_stats_t *slot = NULL;
    for (unsigned i = 0; i < LC_IO_OFFSET_STATS_SLOTS; i++) {
        if (stats[i].seen && stats[i].offset == offset) {
            slot = &stats[i];
            break;
        }
        if (!stats[i].seen && slot == NULL) {
            slot = &stats[i];
        }
    }
    if (slot == NULL) {
        slot = &stats[LC_IO_OFFSET_STATS_SLOTS - 1u];
    }
    if (!slot->seen) {
        slot->seen = true;
        slot->offset = offset;
        slot->first_pc = pc;
    }
    slot->last_pc = pc;
    slot->last_value = value;
    if (write) {
        slot->writes++;
    } else {
        slot->reads++;
    }
}

static void lc_memory_log_offset_stats(const char *name, const lc_io_offset_stats_t *stats) {
    for (unsigned i = 0; i < LC_IO_OFFSET_STATS_SLOTS; i++) {
        if (!stats[i].seen) {
            continue;
        }
        ESP_LOGI(TAG,
                 "LC I/O offset summary: name=%s offset=0x%04x reads=%" PRIu32
                 " writes=%" PRIu32 " first_pc=0x%08" PRIx32 " last_pc=0x%08" PRIx32
                 " last_value=0x%02x",
                 name, stats[i].offset, stats[i].reads, stats[i].writes,
                 stats[i].first_pc, stats[i].last_pc, stats[i].last_value);
    }
}

static void lc_memory_log_io_stub_access(const lc_addr_decode_t *decoded, uint32_t pc,
                                         uint32_t address, uint8_t value, bool write) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 64;

    if (logged < log_limit) {
        ESP_LOGI(TAG,
                 "LC I/O stub %s pc=0x%08" PRIx32 " addr=0x%08" PRIx32
                 " offset=0x%08" PRIx32 " value=0x%02x name=%s alias=%s",
                 write ? "write" : "read", pc, address, decoded->offset, value,
                 decoded->io_stub_name,
                 decoded->io_device_alias != NULL ? decoded->io_device_alias : "none");
        logged++;
        if (logged == log_limit) {
            ESP_LOGI(TAG, "LC I/O stub logger reached %u entries; suppressing further I/O logs",
                     log_limit);
        }
    } else {
        suppressed++;
        if ((suppressed & 0xffffu) == 0) {
            ESP_LOGI(TAG,
                     "LC I/O stub logger suppressed %u additional entries; last_%s pc=0x%08" PRIx32
                     " addr=0x%08" PRIx32 " offset=0x%08" PRIx32
                     " value=0x%02x name=%s alias=%s",
                     suppressed, write ? "write" : "read", pc, address, decoded->offset, value,
                     decoded->io_stub_name,
                     decoded->io_device_alias != NULL ? decoded->io_device_alias : "none");
        }
    }
}

void lc_memory_log_io_stub_summary(void) {
    for (unsigned i = 0; i < LC_IO_STUB_KIND_COUNT; i++) {
        const lc_io_stub_stats_t *stats = &io_stub_stats[i];
        if (!stats->seen) {
            continue;
        }
        ESP_LOGI(TAG,
                 "LC I/O stub summary: name=%s reads=%" PRIu32 " writes=%" PRIu32
                 " first_pc=0x%08" PRIx32 " first_addr=0x%08" PRIx32
                 " last_pc=0x%08" PRIx32 " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                 lc_memory_io_stub_kind_name((lc_io_stub_kind_t)i), stats->reads, stats->writes,
                 stats->first_pc, stats->first_address, stats->last_pc, stats->last_address,
                 stats->last_value);
    }
    lc_memory_log_offset_stats("early-f04000-device", early_f04000_offset_stats);
    lc_memory_log_offset_stats("early-f10000-device", early_f10000_offset_stats);
    lc_memory_log_offset_stats("early-f14000-device", early_f14000_offset_stats);
    lc_memory_log_offset_stats("early-f16000-device", early_f16000_offset_stats);
    lc_memory_bus_t *bus = lc_musashi_bus_active();
    if (bus != NULL && (bus->vram_reads != 0u || bus->vram_writes != 0u ||
                        rom_serial_overlay_chars != 0u)) {
        ESP_LOGI(TAG,
                 "LC VRAM summary: reads=%" PRIu32 " writes=%" PRIu32
                 " serial_overlay_chars=%" PRIu32 " serial_total=%" PRIu32
                 " first_pc=0x%08" PRIx32 " first_addr=0x%08" PRIx32
                 " last_pc=0x%08" PRIx32 " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                 bus->vram_reads, bus->vram_writes, rom_serial_overlay_chars,
                 bus->rom_serial_total, bus->vram_first_pc, bus->vram_first_addr, bus->vram_last_pc,
                 bus->vram_last_addr, bus->vram_last_value);
    }
    if (masked_rom_shadow_writes != 0) {
        ESP_LOGI(TAG,
                 "LC masked ROM shadow summary: writes=%" PRIu32
                 " first_pc=0x%08" PRIx32 " first_addr=0x%08" PRIx32
                 " last_pc=0x%08" PRIx32 " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                 masked_rom_shadow_writes, masked_rom_shadow_first_pc,
                 masked_rom_shadow_first_addr, masked_rom_shadow_last_pc,
                 masked_rom_shadow_last_addr, masked_rom_shadow_last_value);
    }
}

static void lc_memory_log_masked_rom_shadow_write(uint32_t pc, uint32_t address, uint8_t value) {
    static unsigned logged = 0;
    static unsigned suppressed = 0;
    const unsigned log_limit = 48;

    if (masked_rom_shadow_writes == 0) {
        masked_rom_shadow_first_pc = pc;
        masked_rom_shadow_first_addr = address;
    }
    masked_rom_shadow_writes++;
    masked_rom_shadow_last_pc = pc;
    masked_rom_shadow_last_addr = address;
    masked_rom_shadow_last_value = value;

    if (logged < log_limit) {
        ESP_LOGW(TAG,
                 "LC masked ROM shadow write pc=0x%08" PRIx32 " addr=0x%08" PRIx32
                 " value=0x%02x diagnostic_reset_overlay=yes",
                 pc, address, value);
        logged++;
        if (logged == log_limit) {
            ESP_LOGW(TAG,
                     "LC masked ROM shadow write logger reached %u entries; suppressing further shadow logs",
                     log_limit);
        }
    } else {
        suppressed++;
        if ((suppressed & 0xffu) == 0) {
            ESP_LOGW(TAG, "LC masked ROM shadow write logger suppressed %u additional entries",
                     suppressed);
        }
    }
}

static uint32_t lc_memory_vram_offset_from_io_offset(uint32_t offset) {
    return offset - LC_VRAM_IO_OFFSET_BASE;
}

static void lc_memory_note_vram_access(lc_memory_bus_t *bus, uint32_t pc, uint32_t address,
                                       uint8_t value, bool write) {
    if (bus == NULL) {
        return;
    }
    if (write) {
        if (bus->vram_writes == 0u) {
            bus->vram_first_pc = pc;
            bus->vram_first_addr = address;
        }
        bus->vram_writes++;
    } else {
        if (bus->vram_reads == 0u && bus->vram_writes == 0u) {
            bus->vram_first_pc = pc;
            bus->vram_first_addr = address;
        }
        bus->vram_reads++;
    }
    bus->vram_last_pc = pc;
    bus->vram_last_addr = address;
    bus->vram_last_value = value;
}

static uint16_t lc_memory_tiny_glyph(char c) {
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    switch (c) {
    case '0': return 0x7b6fu; case '1': return 0x2492u; case '2': return 0x73e7u;
    case '3': return 0x73cfu; case '4': return 0x5bc9u; case '5': return 0x79cfu;
    case '6': return 0x79efu; case '7': return 0x7249u; case '8': return 0x7befu;
    case '9': return 0x7bcfu; case 'A': return 0x7bedu; case 'B': return 0x7aefu;
    case 'C': return 0x7927u; case 'D': return 0x6b6eu; case 'E': return 0x79e7u;
    case 'F': return 0x79e4u; case 'G': return 0x79afu; case 'H': return 0x5bedu;
    case 'I': return 0x7497u; case 'J': return 0x2496u; case 'K': return 0x5badu;
    case 'L': return 0x4927u; case 'M': return 0x5ffdu; case 'N': return 0x5fedu;
    case 'O': return 0x5b6du; case 'P': return 0x7be4u; case 'Q': return 0x5b7fu;
    case 'R': return 0x7bedu; case 'S': return 0x79cfu; case 'T': return 0x7492u;
    case 'U': return 0x5b6fu; case 'V': return 0x5b6au; case 'W': return 0x5fffu;
    case 'X': return 0x5aadu; case 'Y': return 0x5ba4u; case 'Z': return 0x72e7u;
    case ':': return 0x0909u; case '.': return 0x0002u; case '-': return 0x01c0u;
    case '/': return 0x1248u; case '_': return 0x0007u; case '*': return 0x2a80u;
    case '+': return 0x0ba0u; case '=': return 0x0e38u; case '?': return 0x7282u;
    case ' ': return 0x0000u; default: return 0x01c0u;
    }
}

static void lc_memory_vram_overlay_newline(void) {
    rom_serial_overlay_cursor_x = 0;
    rom_serial_overlay_cursor_y++;
    if ((size_t)(rom_serial_overlay_cursor_y + 1u) * 8u >= LC_VIDEO_HEIGHT) {
        rom_serial_overlay_cursor_y = 0;
    }
}

static void lc_memory_vram_overlay_char(uint8_t value) {
    lc_memory_bus_t *bus = lc_musashi_bus_active();
    if (bus == NULL) {
        return;
    }
    if (bus->rom_serial_len < sizeof(bus->rom_serial_bytes)) {
        bus->rom_serial_bytes[bus->rom_serial_len++] = value;
    }
    bus->rom_serial_total++;
    if (bus->vram == NULL || bus->vram_size < LC_VIDEO_INDEXED_SIZE) {
        return;
    }
    if (value == '\r' || value == '\n') {
        lc_memory_vram_overlay_newline();
        return;
    }
    if (value < 0x20u || value > 0x7eu) {
        return;
    }
    if ((size_t)(rom_serial_overlay_cursor_x + 1u) * 8u >= LC_VIDEO_WIDTH) {
        lc_memory_vram_overlay_newline();
    }
    const uint16_t glyph = lc_memory_tiny_glyph((char)value);
    const size_t origin_x = (size_t)rom_serial_overlay_cursor_x * 8u;
    const size_t origin_y = (size_t)rom_serial_overlay_cursor_y * 8u;
    for (size_t row = 0; row < 5u; row++) {
        const unsigned bits = (glyph >> ((4u - row) * 3u)) & 0x7u;
        for (size_t col = 0; col < 3u; col++) {
            if ((bits & (1u << (2u - col))) == 0) {
                continue;
            }
            for (size_t sy = 0; sy < 2u; sy++) {
                for (size_t sx = 0; sx < 2u; sx++) {
                    const size_t x = origin_x + col * 2u + sx;
                    const size_t y = origin_y + row * 2u + sy;
                    if (x < LC_VIDEO_WIDTH && y < LC_VIDEO_HEIGHT) {
                        bus->vram[y * LC_VIDEO_ROWBYTES + x] = 0xffu;
                    }
                }
            }
        }
    }
    rom_serial_overlay_cursor_x++;
    rom_serial_overlay_chars++;
}

static uint8_t lc_memory_io_stub_read8(const lc_addr_decode_t *decoded, uint32_t address) {
    const uint32_t pc = lc_musashi_bus_current_pc();
    uint8_t value = 0xffu;
    if (decoded->io_stub == LC_IO_STUB_VRAM) {
        lc_memory_bus_t *bus = lc_musashi_bus_active();
        const uint32_t vram_offset = lc_memory_vram_offset_from_io_offset(decoded->offset);
        if (bus != NULL && bus->vram != NULL && vram_offset < bus->vram_size) {
            value = bus->vram[vram_offset];
        } else {
            value = 0x00u;
        }
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
        lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, false);
        lc_memory_note_vram_access(bus, pc, address, value, false);
        lc_memory_log_io_stub_access(decoded, pc, address, value, false);
        return value;
    }
    if (lc_memory_pc_in_post_reset_mode_probe(pc)) {
        static unsigned logged = 0;
        value = lc_memory_read_post_reset_mode_probe(pc);
        if (logged < 6u) {
            ESP_LOGI(TAG,
                     "LC post-reset mode-probe synthetic read: pc=0x%08" PRIx32
                     " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                     pc, address, decoded->offset, value);
            logged++;
        }
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
        lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, false);
        lc_memory_log_io_stub_access(decoded, pc, address, value, false);
        return value;
    }
    if (decoded->io_stub == LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE) {
        // Offset 0x1c00 is the 6522 VIA IER register when VIA registers are
        // decoded with A[12:9]. The early ROM probe writes 0xff, then clears
        // one enable bit at a time and expects reads to return bit7 set plus
        // the current enable mask. Treat the observed 0x00f?1c00 mirrors as
        // one provisional VIA-style IER alias until the real LC VIA windows are
        // fully decoded.
        value = (uint8_t)(0x80u | early_probe_via_ier);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_LC_VIA_REGISTER) {
        const unsigned reg = lc_memory_via_register_index(decoded->offset);
        if (reg == LC_EARLY_VIA_IER_REGISTER) {
            value = (uint8_t)(0x80u | early_probe_via_ier);
        } else if (reg == LC_EARLY_VIA_IFR_REGISTER) {
            if (lc_memory_pc_in_reset_via_irq_probe(pc) &&
                (decoded->offset & 0x00001fffu) == LC_EARLY_VIA_RESET_IRQ_IFR_OFFSET) {
                value = lc_memory_read_reset_via_irq_ifr(pc);
            } else {
                value = 0;
            }
        } else if (reg == LC_EARLY_VIA_ORA_NO_HANDSHAKE_REGISTER) {
            // Register 15 is the 6522 ORA/no-handshake alias under the
            // provisional A[12:9] decode. Reads reflect external pin state,
            // not just the output latch. Holding bit 0 low at reset forces the
            // ROM into its diagnostic subtest table; keep that available behind
            // LC_ENABLE_RESET_DIAGNOSTIC_SUBTEST_ENTRY. The default high/no-low
            // state follows the normal reset branch after those subtests have
            // now been characterized.
#if LC_ENABLE_RESET_DIAGNOSTIC_SUBTEST_ENTRY
            value = (uint8_t)(early_lc_via_registers[reg] &
                              (uint8_t)~LC_EARLY_VIA_ORA_EXTERNAL_LOW_MASK);
#else
            value = (uint8_t)(early_lc_via_registers[reg] |
                              LC_EARLY_VIA_ORA_EXTERNAL_LOW_MASK);
#endif
        } else {
            value = early_lc_via_registers[reg];
        }
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F04000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F04000_REGISTER_COUNT - 1u);
        if (lc_memory_pc_in_reset_scc_probe(pc) && (reg_offset == 0u || reg_offset == 2u)) {
            // The reset subtests exercise SCC control register select/read/write
            // behavior and a minimal local loopback path.  Model just those
            // control-port semantics here so the later monitor remains no-input.
            value = lc_memory_read_f04000_scc_control(reg_offset);
        } else if (lc_memory_pc_in_reset_scc_probe(pc) && reg_offset == 6u) {
            value = early_f04000_scc_loopback_data;
            early_f04000_scc_loopback_ready = false;
        } else if (reg_offset == 0u || reg_offset == 4u) {
            // Provisional SCC-like status aliases: transmitter empty, no
            // receive character available.  The ROM diagnostic monitor also
            // polls +2 after writing a byte to +6; that path gets a single
            // bit0-ready pulse below so transmit can complete without faking
            // serial input for the later command/read loop.
            value = 0x04u;
        } else if (reg_offset == 2u) {
            value = early_f04000_tx_ready_once ? 0x05u : 0x04u;
            early_f04000_tx_ready_once = false;
        } else if (reg_offset == 6u) {
            value = 0x00u;
        } else {
            value = early_f04000_registers[reg_offset];
        }
        lc_memory_update_offset_stats(early_f04000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, false);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F10000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F10000_REGISTER_COUNT - 1u);
        value = early_f10000_registers[reg_offset];
        if (reg_offset == 0x10u) {
            value = early_f10000_control_latch;
        } else if (reg_offset == 0x40u) {
            if (early_f10000_control_latch == 0x80u) {
                value = 0x80u;
            } else if (early_f10000_control_latch == 0x08u) {
                value = 0x40u;
            } else if (early_f10000_control_latch == 0x04u) {
                value = 0x02u;
            }
        } else if (reg_offset == 0x50u) {
            if (early_f10000_control_latch == 0x10u) {
                value = 0x01u;
            } else if (early_f10000_control_latch == 0x02u) {
                value = 0x02u;
            }
        } else if (reg_offset == 0x70u) {
            value = 0x00u;
        }
        lc_memory_update_offset_stats(early_f10000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, false);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F14000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F14000_REGISTER_COUNT - 1u);
        value = early_f14000_registers[reg_offset];
        if (reg_offset == LC_EARLY_F14000_POLL_STATUS_OFFSET &&
            lc_memory_f14000_should_report_poll_ready(pc)) {
            // The seeded LC reset-body probe reaches a ROM routine that matches
            // BasiliskII/macemu's documented physical NuBus/slot video probe
            // family (`0x50f00000 / 0x50f14000`).  That earlier routine polls
            // +0x0804 bit 1 in its inner loop, then bit 0 in an outer wait.
            // Report those ready/complete bits only for the observed poll PCs:
            // the later reset subtest at 0x408473f4 uses +0x0804 as a normal
            // read/write register and must see its own latched values.
            value = (uint8_t)(value | LC_EARLY_F14000_POLL_READY_BITS);
        }
        lc_memory_update_offset_stats(early_f14000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, false);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F16000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F16000_REGISTER_COUNT - 1u);
        value = early_f16000_registers[reg_offset];
        if (reg_offset == 0x1800u) {
            value = early_f16000_registers[0x0800u];
        } else if (reg_offset == 0x1600u) {
            value = lc_memory_f16000_shift_pop();
            if ((pc & 0x0007ffffu) == 0x00047a32u && value == 0xeeu) {
                value = 0xecu;
            }
        } else if (reg_offset == 0x1a00u) {
            value = early_f16000_registers[0x0a00u];
        } else if (reg_offset == 0x1c00u) {
            value = early_f16000_lines_latch;
        }
        lc_memory_update_offset_stats(early_f16000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, false);
    }
    lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
    lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, false);
    lc_memory_log_io_stub_access(decoded, pc, address, value, false);
    return value;
}

static void lc_memory_write_early_via_register(uint32_t offset, uint8_t value) {
    const unsigned reg = lc_memory_via_register_index(offset);
    if (reg == LC_EARLY_VIA_IER_REGISTER) {
        const uint8_t mask = (uint8_t)(value & 0x7fu);
        if ((value & 0x80u) != 0) {
            early_probe_via_ier = (uint8_t)(early_probe_via_ier | mask);
        } else {
            early_probe_via_ier = (uint8_t)(early_probe_via_ier & (uint8_t)~mask);
        }
        early_lc_via_registers[LC_EARLY_VIA_IER_REGISTER] = early_probe_via_ier;
        return;
    }
    if (reg == LC_EARLY_VIA_IFR_REGISTER) {
        early_lc_via_registers[LC_EARLY_VIA_IFR_REGISTER] &= (uint8_t)~value;
        return;
    }
    early_lc_via_registers[reg] = value;
    lc_memory_note_reset_via_irq_write(lc_musashi_bus_current_pc(), offset, value);
}

static esp_err_t lc_memory_io_stub_write8(const lc_addr_decode_t *decoded, uint32_t address,
                                          uint8_t value) {
    const uint32_t pc = lc_musashi_bus_current_pc();
    const uint32_t rom_offset = pc & 0x000fffffu;
    const uint32_t address24 = address & 0x00ffffffu;
    if (rom_offset >= 0x0000696eu && rom_offset <= 0x00006984u &&
        address24 >= 0x00f00000u) {
        static bool logged_srt_io_write_guard = false;
        if (!logged_srt_io_write_guard) {
            logged_srt_io_write_guard = true;
            ESP_LOGW(TAG,
                     "LC ignored bogus Slot Manager SRT I/O fill write: pc=0x%08" PRIx32
                     " addr=0x%08" PRIx32 " value=0x%02x io_stub=%s alias=%s",
                     pc, address, value,
                     decoded->io_stub_name != NULL ? decoded->io_stub_name : "io",
                     decoded->io_device_alias != NULL ? decoded->io_device_alias : "none");
        }
        return ESP_OK;
    }
    if (decoded->io_stub == LC_IO_STUB_VRAM) {
        lc_memory_bus_t *bus = lc_musashi_bus_active();
        const uint32_t vram_offset = lc_memory_vram_offset_from_io_offset(decoded->offset);
        if (bus != NULL && bus->vram != NULL && vram_offset < bus->vram_size) {
            bus->vram[vram_offset] = value;
        }
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, true);
        lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, true);
        lc_memory_note_vram_access(bus, pc, address, value, true);
        lc_memory_log_io_stub_access(decoded, pc, address, value, true);
        return ESP_OK;
    }
    if (decoded->io_stub == LC_IO_STUB_EARLY_ROM_PROBE_1C00_STRIDE ||
        decoded->io_stub == LC_IO_STUB_EARLY_LC_VIA_REGISTER) {
        lc_memory_write_early_via_register(decoded->offset, value);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F04000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F04000_REGISTER_COUNT - 1u);
        early_f04000_registers[reg_offset] = value;
        if (lc_memory_pc_in_reset_scc_probe(pc) && (reg_offset == 0u || reg_offset == 2u)) {
            lc_memory_write_f04000_scc_control(reg_offset, value);
        } else if (lc_memory_pc_in_reset_scc_probe(pc) && reg_offset == 6u) {
            early_f04000_scc_loopback_data = value;
            early_f04000_scc_loopback_ready = true;
            lc_memory_vram_overlay_char(value);
        } else if (reg_offset == 6u) {
            early_f04000_tx_ready_once = true;
            lc_memory_vram_overlay_char(value);
        }
        lc_memory_update_offset_stats(early_f04000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, true);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F10000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F10000_REGISTER_COUNT - 1u);
        early_f10000_registers[reg_offset] = value;
        if (reg_offset == 0x10u) {
            early_f10000_control_latch = value;
        }
        lc_memory_update_offset_stats(early_f10000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, true);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F14000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F14000_REGISTER_COUNT - 1u);
        early_f14000_registers[reg_offset] = value;
        lc_memory_update_offset_stats(early_f14000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, true);
    } else if (decoded->io_stub == LC_IO_STUB_EARLY_F16000_DEVICE) {
        const uint32_t reg_offset = decoded->offset & (LC_EARLY_F16000_REGISTER_COUNT - 1u);
        early_f16000_registers[reg_offset] = value;
        if (reg_offset == 0x0600u) {
            if (early_f16000_shift_reset_pending) {
                lc_memory_f16000_shift_clear();
                early_f16000_shift_reset_pending = false;
            }
            lc_memory_f16000_shift_push(value);
        } else if (reg_offset == 0x0e00u) {
            early_f16000_lines_latch = value;
        } else if (reg_offset == 0x0c00u) {
            if (value == 0x38u) {
                early_f16000_shift_reset_pending = true;
            } else if (value != 0xf8u) {
                early_f16000_lines_latch = 0x00u;
            }
        }
        lc_memory_update_offset_stats(early_f16000_offset_stats, (uint16_t)reg_offset,
                                      pc, value, true);
    }
    lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, true);
    lc_memory_update_io_stub_stats(decoded->io_stub, pc, address, value, true);
    lc_memory_log_io_stub_access(decoded, pc, address, value, true);
    return ESP_OK;
}

esp_err_t lc_memory_bus_init(lc_memory_bus_t *bus, const lc_rom_map_t *rom_map) {
    if (bus == NULL || rom_map == NULL || !rom_map->mapped || rom_map->bytes == NULL ||
        rom_map->size < LC_ROM_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(bus, 0, sizeof(*bus));
    memset(io_stub_stats, 0, sizeof(io_stub_stats));
    memset(early_f04000_offset_stats, 0, sizeof(early_f04000_offset_stats));
    memset(early_f10000_offset_stats, 0, sizeof(early_f10000_offset_stats));
    memset(early_f14000_offset_stats, 0, sizeof(early_f14000_offset_stats));
    memset(early_f16000_offset_stats, 0, sizeof(early_f16000_offset_stats));
    memset(early_lc_via_registers, 0xff, sizeof(early_lc_via_registers));
    early_lc_via_reset_irq_phase = 0;
    rom_serial_overlay_cursor_x = 0;
    rom_serial_overlay_cursor_y = 0;
    rom_serial_overlay_chars = 0;
    memset(early_lc_via_reset_irq_ifr_reads, 0, sizeof(early_lc_via_reset_irq_ifr_reads));
    memset(early_f04000_registers, 0xff, sizeof(early_f04000_registers));
    memset(early_f10000_registers, 0xff, sizeof(early_f10000_registers));
    early_f10000_control_latch = 0;
    memset(early_f04000_scc_registers, 0xff, sizeof(early_f04000_scc_registers));
    memset(early_f04000_scc_selected_register, 0xff, sizeof(early_f04000_scc_selected_register));
    early_f04000_scc_loopback_data = 0;
    early_f04000_scc_loopback_ready = false;
    memset(early_f14000_registers, 0xff, sizeof(early_f14000_registers));
    memset(early_f16000_registers, 0, sizeof(early_f16000_registers));
    memset(early_f16000_shift_queue, 0, sizeof(early_f16000_shift_queue));
    lc_memory_f16000_shift_clear();
    early_f16000_lines_latch = 0;
    early_f16000_shift_reset_pending = false;
    early_f04000_tx_ready_once = false;
    masked_rom_shadow_writes = 0;
    masked_rom_shadow_first_pc = 0;
    masked_rom_shadow_first_addr = 0;
    masked_rom_shadow_last_pc = 0;
    masked_rom_shadow_last_addr = 0;
    masked_rom_shadow_last_value = 0;
    post_reset_core_lowmem_ram_seeded = false;
    post_reset_memory_layout_ram_seeded = false;
    post_reset_resource_lowmem_ram_seeded = false;
    post_reset_resource_map_ram_seeded = false;
    lc_memory_reset_post_reset_atrap_table();
    early_lc_via_registers[LC_EARLY_VIA_IFR_REGISTER] = 0;
    early_lc_via_registers[LC_EARLY_VIA_IER_REGISTER] = 0;
    early_probe_via_ier = 0;

    size_t requested = LC_GUEST_RAM_SIZE;
    bus->ram = (uint8_t *)heap_caps_calloc(1, requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bus->ram == NULL) {
        ESP_LOGW(TAG, "LC memory bus RAM allocation failed for %zu bytes; trying fallback %u bytes",
                 requested, (unsigned)LC_GUEST_RAM_FALLBACK_SIZE);
        requested = LC_GUEST_RAM_FALLBACK_SIZE;
        bus->ram = (uint8_t *)heap_caps_calloc(1, requested, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bus->using_fallback_ram = true;
    }
    if (bus->ram == NULL) {
        ESP_LOGE(TAG, "LC memory bus RAM allocation failed");
        return ESP_ERR_NO_MEM;
    }

    bus->vram_size = LC_VRAM_IO_OFFSET_LIMIT - LC_VRAM_IO_OFFSET_BASE;
    bus->vram = (uint8_t *)heap_caps_calloc(1, bus->vram_size,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bus->vram == NULL) {
        ESP_LOGE(TAG, "LC memory bus VRAM allocation failed: size=0x%zx", bus->vram_size);
        heap_caps_free(bus->ram);
        memset(bus, 0, sizeof(*bus));
        return ESP_ERR_NO_MEM;
    }

    bus->ram_size = requested;
    lc_memory_seed_ram_owned_low_memory(bus);
    bus->rom = rom_map->bytes;
    bus->rom_size = rom_map->size;
#if LC_ENABLE_ROM_MASKED_SHADOW
    bus->rom_masked_shadow_size = bus->rom_size < LC_ROM_WINDOW_SIZE ? bus->rom_size : LC_ROM_WINDOW_SIZE;
    bus->rom_masked_shadow = (uint8_t *)heap_caps_malloc(bus->rom_masked_shadow_size,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bus->rom_masked_shadow == NULL) {
        ESP_LOGW(TAG,
                 "LC masked ROM alias shadow allocation failed: size=0x%zx; masked alias remains read-only ROM",
                 bus->rom_masked_shadow_size);
        bus->rom_masked_shadow_size = 0;
    } else {
        memcpy(bus->rom_masked_shadow, bus->rom, bus->rom_masked_shadow_size);
        bus->rom_masked_shadow_enabled = true;
        ESP_LOGW(TAG,
                 "LC masked ROM alias shadow initialized: base=0x%08x size=0x%zx diagnostic_reset_overlay=yes",
                 LC_ROM_WINDOW_32BIT_MASKED_BASE_CANDIDATE, bus->rom_masked_shadow_size);
    }
    bus->rom_24_shadow_size = bus->rom_size < LC_ROM_WINDOW_SIZE ? bus->rom_size : LC_ROM_WINDOW_SIZE;
    bus->rom_24_shadow = (uint8_t *)heap_caps_malloc(bus->rom_24_shadow_size,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bus->rom_24_shadow == NULL) {
        ESP_LOGW(TAG,
                 "LC 24-bit ROM-window shadow allocation failed: size=0x%zx; 24-bit ROM window remains read-only ROM",
                 bus->rom_24_shadow_size);
        bus->rom_24_shadow_size = 0;
    } else {
        memcpy(bus->rom_24_shadow, bus->rom, bus->rom_24_shadow_size);
        bus->rom_24_shadow_enabled = true;
        ESP_LOGW(TAG,
                 "LC 24-bit ROM-window shadow initialized: base=0x%08x size=0x%zx diagnostic_overlay=yes",
                 LC_ROM_WINDOW_24BIT_BASE_CANDIDATE, bus->rom_24_shadow_size);
    }
#endif
    bus->initialized = true;
    ESP_LOGI(TAG, "LC memory bus initialized: ram=%p size=0x%zx%s vram=%p size=0x%zx rom=%p size=0x%zx masked_shadow=%p size=0x%zx enabled=%d rom24_shadow=%p size=0x%zx enabled=%d",
             (void *)bus->ram, bus->ram_size, bus->using_fallback_ram ? " fallback" : "",
             (void *)bus->vram, bus->vram_size, (const void *)bus->rom, bus->rom_size,
             (void *)bus->rom_masked_shadow, bus->rom_masked_shadow_size,
             bus->rom_masked_shadow_enabled, (void *)bus->rom_24_shadow,
             bus->rom_24_shadow_size, bus->rom_24_shadow_enabled);
    return ESP_OK;
}

void lc_memory_bus_free(lc_memory_bus_t *bus) {
    if (bus == NULL) {
        return;
    }
    if (bus->ram != NULL) {
        heap_caps_free(bus->ram);
    }
    if (bus->rom_masked_shadow != NULL) {
        heap_caps_free(bus->rom_masked_shadow);
    }
    if (bus->rom_24_shadow != NULL) {
        heap_caps_free(bus->rom_24_shadow);
    }
    if (bus->vram != NULL) {
        heap_caps_free(bus->vram);
    }
    memset(bus, 0, sizeof(*bus));
}

uint8_t lc_memory_bus_read8(lc_memory_bus_t *bus, uint32_t address) {
    if (bus == NULL || !bus->initialized) {
        lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, false);
        return 0xffu;
    }

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    switch (decoded.region) {
    case LC_ADDR_REGION_RAM:
        if (decoded.offset < bus->ram_size) {
            const uint32_t pc = lc_musashi_bus_current_pc();
            if (lc_memory_should_read_synthetic_ram_test_list(pc, address)) {
                const uint8_t value = lc_memory_read_synthetic_ram_test_list(address);
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_post_reset_memory_layout_seed_point(pc)) {
                lc_memory_seed_post_reset_memory_layout_ram(bus);
            }
            if (lc_memory_should_read_post_reset_no_mmu_flag(pc, address)) {
                const uint8_t value = 0x01u;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    ESP_LOGI(TAG,
                             "LC post-reset no-MMU synthetic flag read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " value=0x%02x",
                             pc, address, value);
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_dispatch_kind(pc, address)) {
                // The memory-layout second pass derives a tiny dispatch selector
                // from provisional low-memory state.  Current synthetic
                // BootGlobs leave that byte zero, which goes straight to the
                // ROM CritError path.  Feed selector 6 to return the ROM's
                // adjacent built-in descriptor table pair (`0x40840f02` /
                // `0x40840ef0`) instead of the fallback error table at
                // `0x40840dac`.
                const uint8_t value = 0x06u;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    ESP_LOGI(TAG,
                             "LC post-reset memory-layout dispatch synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " value=0x%02x",
                             pc, address, value);
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_second_pass_locals(pc, address)) {
                const uint8_t value = 0x00u;
                static unsigned logged = 0;
                if (logged < 8u) {
                    ESP_LOGI(TAG,
                             "LC post-reset second-pass local synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " value=0x%02x",
                             pc, address, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_record_table(pc, address)) {
                // The selected post-reset path expects a small RAM-bank record
                // table at -0x58(A5).  Without the real address-map handoff that
                // local still contains RAM-test fill bytes, so the ROM copies
                // records until it runs off the 4MB RAM top into the 24-bit ROM
                // window.  Provide one explicit 0..MemTop bank plus terminator.
                const uint8_t value = lc_memory_read_post_reset_record_table(address);
                static unsigned logged = 0;
                if (logged < 8u) {
                    ESP_LOGI(TAG,
                             "LC post-reset record-table synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " value=0x%02x",
                             pc, address, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_post_reset_core_lowmem_seed_point(pc)) {
                lc_memory_seed_post_reset_core_lowmem_ram(bus);
            }
            if (lc_memory_should_read_basilisk_dispatch_magic(pc, address)) {
                const uint8_t value = lc_memory_read_basilisk_dispatch_magic(address);
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_lowmem_callback(pc, address)) {
                const uint8_t value = lc_memory_read_post_reset_lowmem_callback(address);
                static unsigned logged = 0;
                if (logged < 4u) {
                    ESP_LOGI(TAG,
                             "LC post-reset low-memory callback synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " value=0x%02x",
                             pc, address, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_atrap_table(bus, pc, address)) {
                const uint8_t value = lc_memory_read_post_reset_atrap_table(address);
                static unsigned logged = 0;
                if (logged < 24u) {
                    uint32_t trap = 0xffffffffu;
                    if (address >= 0x00000400u && address < 0x00000600u) {
                        trap = (address - 0x00000400u) / 4u;
                    } else if (address >= 0x00000020u && address < 0x00000400u) {
                        trap = 0xad00u | ((address - 0x00000020u) / 4u);
                    } else if (address >= 0x00000e00u && address < 0x00001000u) {
                        trap = 0xa800u | ((address - 0x00000e00u) / 4u);
                    } else if (address >= 0x00001e00u && address < 0x00002000u) {
                        trap = 0xac00u | ((address - 0x00001e00u) / 4u);
                    }
                    ESP_LOGI(TAG,
                             "LC post-reset A-trap table synthetic read: pc=0x%08" PRIx32
                             " trap=0x%03" PRIx32 " addr=0x%08" PRIx32 " value=0x%02x",
                             pc, trap, address, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_pc_in_post_reset_resource_manager(pc) &&
                !lc_memory_basilisk_slot_rom_active(bus)) {
                lc_memory_seed_post_reset_resource_lowmem_ram(bus);
            }
            if (lc_memory_should_read_fake_gdevice_chain(pc, address)) {
                const uint8_t value = lc_memory_read_fake_gdevice_chain(address);
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_fake_video_globals(pc, address)) {
                const uint8_t value = lc_memory_read_fake_video_globals(address);
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, bus->ram[decoded.offset], 1,
                            false);
            return bus->ram[decoded.offset];
        }
        break;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE:
        if (decoded.offset < bus->rom_size) {
            const uint32_t pc = lc_musashi_bus_current_pc();
            if (lc_memory_should_read_post_reset_finalizer_rom_shape(pc, decoded.offset)) {
                const uint8_t value = 0x00u;
                static unsigned logged = 0;
                if (logged < 4u) {
                    ESP_LOGI(TAG,
                             "LC post-reset finalizer ROM-shape synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                             pc, address, decoded.offset, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_swap_mmu_dispatch_nop(pc, decoded.offset)) {
                const uint8_t value = lc_memory_read_post_reset_swap_mmu_dispatch_nop(decoded.offset);
                static unsigned logged = 0;
                if (logged < 6u) {
                    ESP_LOGI(TAG,
                             "LC post-reset SwapMMUMode dispatch NOP read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                             pc, address, decoded.offset, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (bus->rom_24_shadow_enabled && decoded.offset < bus->rom_24_shadow_size) {
                const uint8_t value = lc_memory_apply_rom_diagnostic_overrides(
                    bus->rom_24_shadow[decoded.offset], pc, decoded.offset);
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            const uint8_t value = lc_memory_read_rom_byte_with_diagnostics(bus, pc, decoded.offset);
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
            return value;
        }
        break;
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE:
        if (decoded.offset < bus->rom_size) {
            const uint32_t pc = lc_musashi_bus_current_pc();
            if (lc_memory_should_read_post_reset_finalizer_rom_shape(pc, decoded.offset)) {
                const uint8_t value = 0x00u;
                static unsigned logged = 0;
                if (logged < 4u) {
                    ESP_LOGI(TAG,
                             "LC post-reset finalizer ROM32-shape synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                             pc, address, decoded.offset, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_swap_mmu_dispatch_nop(pc, decoded.offset)) {
                const uint8_t value = lc_memory_read_post_reset_swap_mmu_dispatch_nop(decoded.offset);
                static unsigned logged = 0;
                if (logged < 6u) {
                    ESP_LOGI(TAG,
                             "LC post-reset ROM32 SwapMMUMode dispatch NOP read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                             pc, address, decoded.offset, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            const uint8_t value = lc_memory_read_rom_byte_with_diagnostics(bus, pc, decoded.offset);
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
            return value;
        }
        break;
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        if (decoded.offset < bus->rom_size) {
            const uint32_t pc = lc_musashi_bus_current_pc();
            if (lc_memory_should_read_post_reset_finalizer_rom_shape(pc, decoded.offset)) {
                const uint8_t value = 0x00u;
                static unsigned logged = 0;
                if (logged < 4u) {
                    ESP_LOGI(TAG,
                             "LC post-reset finalizer masked-ROM-shape synthetic read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                             pc, address, decoded.offset, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
            if (lc_memory_should_read_post_reset_swap_mmu_dispatch_nop(pc, decoded.offset)) {
                const uint8_t value = lc_memory_read_post_reset_swap_mmu_dispatch_nop(decoded.offset);
                static unsigned logged = 0;
                if (logged < 6u) {
                    ESP_LOGI(TAG,
                             "LC post-reset masked SwapMMUMode dispatch NOP read: pc=0x%08" PRIx32
                             " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                             pc, address, decoded.offset, value);
                    logged++;
                }
                lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
                return value;
            }
        }
        if (bus->rom_masked_shadow_enabled && decoded.offset < bus->rom_masked_shadow_size) {
            const uint32_t pc = lc_musashi_bus_current_pc();
            const uint8_t value = lc_memory_apply_rom_diagnostic_overrides(
                bus->rom_masked_shadow[decoded.offset], pc, decoded.offset);
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
            return value;
        }
        if (decoded.offset < bus->rom_size) {
            const uint32_t pc = lc_musashi_bus_current_pc();
            const uint8_t value = lc_memory_read_rom_byte_with_diagnostics(bus, pc, decoded.offset);
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
            return value;
        }
        break;
    case LC_ADDR_REGION_RAM_SIZE_PROBE: {
        const uint32_t pc = lc_musashi_bus_current_pc();
        if (lc_memory_should_read_post_reset_finalizer_descriptor(pc, address)) {
            const uint8_t value = lc_memory_read_post_reset_finalizer_descriptor(address);
            static unsigned logged = 0;
            if (logged < 8u) {
                ESP_LOGI(TAG,
                         "LC post-reset finalizer descriptor synthetic read: pc=0x%08" PRIx32
                         " addr=0x%08" PRIx32 " value=0x%02x",
                         pc, address, value);
                logged++;
            }
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, false);
            return value;
        }
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, 0xffu, 1, false);
        return 0xffu;
    }
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return lc_memory_io_stub_read8(&decoded, address);
    case LC_ADDR_REGION_UNMAPPED:
    default:
        break;
    }

    lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, false);
    return 0xffu;
}

uint16_t lc_memory_bus_read16(lc_memory_bus_t *bus, uint32_t address) {
    const uint16_t hi = lc_memory_bus_read8(bus, address);
    const uint16_t lo = lc_memory_bus_read8(bus, address + 1u);
    return (uint16_t)((hi << 8) | lo);
}

uint32_t lc_memory_bus_read32(lc_memory_bus_t *bus, uint32_t address) {
    const uint32_t hi = lc_memory_bus_read16(bus, address);
    const uint32_t lo = lc_memory_bus_read16(bus, address + 2u);
    return (hi << 16) | lo;
}

esp_err_t lc_memory_bus_write8(lc_memory_bus_t *bus, uint32_t address, uint8_t value) {
    if (bus == NULL || !bus->initialized) {
        lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, true);
        return ESP_ERR_INVALID_STATE;
    }

    lc_addr_decode_t decoded = lc_memory_decode_address(address);
    switch (decoded.region) {
    case LC_ADDR_REGION_RAM:
        if (decoded.offset < bus->ram_size) {
            static unsigned resource_map_header_watch_logs = 0;
            const uint8_t old_value = bus->ram[decoded.offset];
            if (decoded.offset >= 0x0001a518u && decoded.offset <= 0x0001a527u &&
                old_value != value && resource_map_header_watch_logs < 1200u) {
                ESP_LOGW(TAG,
                         "LC watch dynamic resource-map header write: pc=0x%08" PRIx32
                         " addr=0x%08" PRIx32 " offset=0x%08" PRIx32
                         " old=0x%02x new=0x%02x",
                         lc_musashi_bus_current_pc(), address, decoded.offset,
                         old_value, value);
                resource_map_header_watch_logs++;
            }
            bus->ram[decoded.offset] = value;
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, true);
            return ESP_OK;
        }
        break;
    case LC_ADDR_REGION_RAM_SIZE_PROBE:
        lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, lc_musashi_bus_current_pc(), address, value, 1, true);
        return ESP_OK;
    case LC_ADDR_REGION_IO_24BIT_CANDIDATE:
    case LC_ADDR_REGION_IO_32BIT_CANDIDATE:
        return lc_memory_io_stub_write8(&decoded, address, value);
    case LC_ADDR_REGION_ROM_32BIT_MASKED_CANDIDATE:
        if (bus->rom_masked_shadow_enabled && decoded.offset < bus->rom_masked_shadow_size) {
            bus->rom_masked_shadow[decoded.offset] = value;
            const uint32_t pc = lc_musashi_bus_current_pc();
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, true);
            lc_memory_log_masked_rom_shadow_write(pc, address, value);
            return ESP_OK;
        }
        lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, lc_musashi_bus_current_pc(), address, value, 1, true);
        ESP_LOGW(TAG, "LC blocked masked ROM alias write addr=0x%08" PRIx32
                      " offset=0x%08" PRIx32 " value=0x%02x shadow_enabled=%d",
                 address, decoded.offset, value, bus->rom_masked_shadow_enabled);
        return ESP_ERR_INVALID_STATE;
    case LC_ADDR_REGION_ROM_24BIT_CANDIDATE: {
        const uint32_t pc = lc_musashi_bus_current_pc();
        if (bus->rom_24_shadow_enabled && decoded.offset < bus->rom_24_shadow_size) {
            bus->rom_24_shadow[decoded.offset] = value;
            lc_trace_record(LC_TRACE_EVENT_MEM_ACCESS, pc, address, value, 1, true);
            static unsigned logged_shadow24 = 0;
            static unsigned suppressed_shadow24 = 0;
            if (logged_shadow24 < 32u) {
                ESP_LOGW(TAG,
                         "LC 24-bit ROM-window shadow write pc=0x%08" PRIx32
                         " addr=0x%08" PRIx32 " offset=0x%08" PRIx32
                         " value=0x%02x diagnostic_overlay=yes",
                         pc, address, decoded.offset, value);
                logged_shadow24++;
                if (logged_shadow24 == 32u) {
                    ESP_LOGW(TAG, "LC 24-bit ROM-window shadow write logger reached 32 entries; suppressing further logs");
                }
            } else {
                suppressed_shadow24++;
                if ((suppressed_shadow24 & 0x3ffu) == 0) {
                    ESP_LOGW(TAG,
                             "LC 24-bit ROM-window shadow write logger suppressed %u additional entries; last_pc=0x%08" PRIx32
                             " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                             suppressed_shadow24, pc, address, value);
                }
            }
            return ESP_OK;
        }
        lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, pc, address, value, 1, true);
        ESP_LOGW(TAG, "LC blocked 24-bit ROM-window write addr=0x%08" PRIx32
                      " offset=0x%08" PRIx32 " value=0x%02x shadow_enabled=%d",
                 address, decoded.offset, value, bus->rom_24_shadow_enabled);
        return ESP_ERR_INVALID_STATE;
    }
    case LC_ADDR_REGION_ROM_32BIT_CANDIDATE: {
        const uint32_t pc = lc_musashi_bus_current_pc();
        static unsigned logged = 0;
        static unsigned suppressed = 0;
        lc_trace_record(LC_TRACE_EVENT_BUS_ERROR, pc, address, value, 1, true);
        if (logged < 64u) {
            ESP_LOGW(TAG,
                     "LC blocked ROM32 write pc=0x%08" PRIx32
                     " addr=0x%08" PRIx32 " offset=0x%08" PRIx32 " value=0x%02x",
                     pc, address, decoded.offset, value);
            logged++;
            if (logged == 64u) {
                ESP_LOGW(TAG, "LC blocked ROM32 write logger reached 64 entries; suppressing further ROM32 logs");
            }
        } else {
            suppressed++;
            if ((suppressed & 0x3ffu) == 0) {
                ESP_LOGW(TAG,
                         "LC blocked ROM32 write logger suppressed %u additional entries; last_pc=0x%08" PRIx32
                         " last_addr=0x%08" PRIx32 " last_value=0x%02x",
                         suppressed, pc, address, value);
            }
        }
        return ESP_ERR_INVALID_STATE;
    }
    case LC_ADDR_REGION_UNMAPPED:
    default:
        break;
    }

    lc_memory_log_unmapped_access(lc_musashi_bus_current_pc(), address, 1, true);
    return ESP_ERR_INVALID_STATE;
}

esp_err_t lc_memory_bus_write16(lc_memory_bus_t *bus, uint32_t address, uint16_t value) {
    esp_err_t err = lc_memory_bus_write8(bus, address, (uint8_t)(value >> 8));
    if (err != ESP_OK) {
        return err;
    }
    return lc_memory_bus_write8(bus, address + 1u, (uint8_t)value);
}

esp_err_t lc_memory_bus_write32(lc_memory_bus_t *bus, uint32_t address, uint32_t value) {
    esp_err_t err = lc_memory_bus_write16(bus, address, (uint16_t)(value >> 16));
    if (err != ESP_OK) {
        return err;
    }
    return lc_memory_bus_write16(bus, address + 2u, (uint16_t)value);
}

void lc_memory_probe_bus_harness(const lc_rom_map_t *rom_map) {
    lc_memory_bus_t bus = {0};
    esp_err_t err = lc_memory_bus_init(&bus, rom_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LC memory bus harness init failed: %s", esp_err_to_name(err));
        return;
    }

    const uint32_t ram_addr = 0x00000000u;
    const uint32_t ram_tail_addr = (uint32_t)bus.ram_size - 4u;
    const uint32_t rom24_addr = LC_ROM_WINDOW_24BIT_BASE_CANDIDATE;
    const uint32_t rom32_addr = LC_ROM_WINDOW_32BIT_BASE_CANDIDATE;
    const uint32_t io_addr = LC_IO_24BIT_BASE_CANDIDATE;
    const uint32_t unmapped_addr = 0x00e00000u;

    const esp_err_t ram_write = lc_memory_bus_write32(&bus, ram_addr, 0x12345678u);
    const uint32_t ram_read = lc_memory_bus_read32(&bus, ram_addr);
    const esp_err_t ram_tail_write = lc_memory_bus_write32(&bus, ram_tail_addr, 0xa5a55a5au);
    const uint32_t ram_tail_read = lc_memory_bus_read32(&bus, ram_tail_addr);
    const uint32_t rom24_first = lc_memory_bus_read32(&bus, rom24_addr);
    const uint32_t rom24_second = lc_memory_bus_read32(&bus, rom24_addr + 4u);
    const uint32_t rom32_first = lc_memory_bus_read32(&bus, rom32_addr);
    const uint8_t io_read = lc_memory_bus_read8(&bus, io_addr);
    const esp_err_t io_write = lc_memory_bus_write8(&bus, io_addr, 0x5au);
    const esp_err_t rom_write = lc_memory_bus_write8(&bus, rom24_addr, 0x00u);
    const uint8_t unmapped_read = lc_memory_bus_read8(&bus, unmapped_addr);

    ESP_LOGI(TAG,
             "LC memory bus harness: ram_write=%s ram_read=0x%08" PRIx32
             " tail_write=%s tail_read=0x%08" PRIx32,
             esp_err_to_name(ram_write), ram_read, esp_err_to_name(ram_tail_write),
             ram_tail_read);
    ESP_LOGI(TAG,
             "LC memory bus harness: rom24[0]=0x%08" PRIx32 " rom24[4]=0x%08" PRIx32
             " rom32[0]=0x%08" PRIx32,
             rom24_first, rom24_second, rom32_first);
    ESP_LOGI(TAG,
             "LC memory bus harness: io_read=0x%02x io_write=%s rom_write_blocked=%s unmapped_read=0x%02x",
             io_read, esp_err_to_name(io_write), esp_err_to_name(rom_write), unmapped_read);

    lc_memory_bus_free(&bus);
    ESP_LOGI(TAG, "LC memory bus harness complete");
}
