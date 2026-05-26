#ifndef MACHINE_LC_PERF_H
#define MACHINE_LC_PERF_H

#include <stdint.h>

typedef enum {
    LC_PERF_COUNTER_CPU_LOOP,
    LC_PERF_COUNTER_VIDEO_UPDATE,
    LC_PERF_COUNTER_HOST_RENDER,
    LC_PERF_COUNTER_DISPLAY_FLUSH,
    LC_PERF_COUNTER_COUNT,
} lc_perf_counter_t;

void lc_perf_reset(void);
uint64_t lc_perf_now_us(void);
void lc_perf_record_us(lc_perf_counter_t counter, uint64_t elapsed_us);
void lc_perf_increment_frame(void);
void lc_perf_log_summary(void);
const char *lc_perf_counter_name(lc_perf_counter_t counter);

#endif
