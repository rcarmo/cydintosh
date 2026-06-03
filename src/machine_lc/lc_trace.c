#include "lc_trace.h"

#include "esp_log.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_trace";

typedef struct {
    uint32_t seq;
    uint32_t timestamp_ms;
    lc_trace_event_type_t type;
    uint32_t pc;
    uint32_t address;
    uint32_t value;
    uint16_t size;
    bool write;
} lc_trace_event_t;

#ifndef LC_TRACE_ENABLED
#define LC_TRACE_ENABLED 1
#endif

static lc_trace_event_t trace_ring[LC_TRACE_RING_SIZE];
static uint32_t trace_seq;
static uint32_t trace_count;
static uint32_t trace_next;

static const char *trace_type_name(lc_trace_event_type_t type) {
    switch (type) {
    case LC_TRACE_EVENT_MARKER:
        return "marker";
    case LC_TRACE_EVENT_CPU_CONFIG:
        return "cpu-config";
    case LC_TRACE_EVENT_ROM_VECTOR_CANDIDATE:
        return "rom-vector-candidate";
    case LC_TRACE_EVENT_MEM_ACCESS:
        return "mem-access";
    case LC_TRACE_EVENT_UNMAPPED_ACCESS:
        return "unmapped-access";
    case LC_TRACE_EVENT_EXCEPTION:
        return "exception";
    case LC_TRACE_EVENT_ILLEGAL_INSTRUCTION:
        return "illegal-instruction";
    case LC_TRACE_EVENT_BUS_ERROR:
        return "bus-error";
    case LC_TRACE_EVENT_ADDRESS_ERROR:
        return "address-error";
    case LC_TRACE_EVENT_INTERRUPT:
        return "interrupt";
    case LC_TRACE_EVENT_DISK_IO:
        return "disk-io";
    case LC_TRACE_EVENT_DISK_WRITE_BLOCKED:
        return "disk-write-blocked";
    default:
        return "unknown";
    }
}

void lc_trace_reset(void) {
    memset(trace_ring, 0, sizeof(trace_ring));
    trace_seq = 0;
    trace_count = 0;
    trace_next = 0;
}

void lc_trace_record(lc_trace_event_type_t type, uint32_t pc, uint32_t address,
                     uint32_t value, uint16_t size, bool write) {
#if !LC_TRACE_ENABLED
    (void)type;
    (void)pc;
    (void)address;
    (void)value;
    (void)size;
    (void)write;
    return;
#else
    lc_trace_event_t *event = &trace_ring[trace_next];
    event->seq = ++trace_seq;
    event->timestamp_ms = esp_log_timestamp();
    event->type = type;
    event->pc = pc;
    event->address = address;
    event->value = value;
    event->size = size;
    event->write = write;

    trace_next = (trace_next + 1u) % LC_TRACE_RING_SIZE;
    if (trace_count < LC_TRACE_RING_SIZE) {
        trace_count++;
    }
#endif
}

void lc_trace_record_marker(uint32_t marker) {
    lc_trace_record(LC_TRACE_EVENT_MARKER, 0, 0, marker, 0, false);
}

void lc_trace_dump_recent(unsigned max_entries) {
    if (trace_count == 0) {
        ESP_LOGI(TAG, "LC trace ring is empty");
        return;
    }

    if (max_entries == 0 || max_entries > trace_count) {
        max_entries = trace_count;
    }

    ESP_LOGI(TAG, "LC trace dump: showing %u/%u entries ring_size=%u next_seq=%" PRIu32,
             max_entries, trace_count, LC_TRACE_RING_SIZE, trace_seq + 1u);

    uint32_t start = (trace_next + LC_TRACE_RING_SIZE - max_entries) % LC_TRACE_RING_SIZE;
    for (unsigned i = 0; i < max_entries; i++) {
        const lc_trace_event_t *event = &trace_ring[(start + i) % LC_TRACE_RING_SIZE];
        ESP_LOGI(TAG,
                 "trace seq=%" PRIu32 " t=%" PRIu32 "ms type=%s pc=0x%08" PRIx32
                 " %s%u addr=0x%08" PRIx32 " value=0x%08" PRIx32,
                 event->seq, event->timestamp_ms, trace_type_name(event->type), event->pc,
                 event->write ? "write" : "read", (unsigned)event->size, event->address,
                 event->value);
    }
}
