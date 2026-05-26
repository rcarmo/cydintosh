#ifndef MACHINE_LC_TRACE_H
#define MACHINE_LC_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef LC_TRACE_RING_SIZE
#define LC_TRACE_RING_SIZE 128u
#endif

typedef enum {
    LC_TRACE_EVENT_MARKER,
    LC_TRACE_EVENT_CPU_CONFIG,
    LC_TRACE_EVENT_ROM_VECTOR_CANDIDATE,
    LC_TRACE_EVENT_MEM_ACCESS,
    LC_TRACE_EVENT_UNMAPPED_ACCESS,
    LC_TRACE_EVENT_EXCEPTION,
    LC_TRACE_EVENT_ILLEGAL_INSTRUCTION,
    LC_TRACE_EVENT_BUS_ERROR,
    LC_TRACE_EVENT_ADDRESS_ERROR,
    LC_TRACE_EVENT_INTERRUPT,
    LC_TRACE_EVENT_DISK_IO,
    LC_TRACE_EVENT_DISK_WRITE_BLOCKED,
} lc_trace_event_type_t;

void lc_trace_reset(void);
void lc_trace_record(lc_trace_event_type_t type, uint32_t pc, uint32_t address,
                     uint32_t value, uint16_t size, bool write);
void lc_trace_record_marker(uint32_t marker);
void lc_trace_dump_recent(unsigned max_entries);

#endif
