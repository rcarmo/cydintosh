#include "lc_perf.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_perf";

typedef struct {
    uint64_t count;
    uint64_t total_us;
    uint64_t min_us;
    uint64_t max_us;
} lc_perf_stats_t;

static lc_perf_stats_t counters[LC_PERF_COUNTER_COUNT];
static uint64_t frame_count;
static uint64_t reset_time_us;

const char *lc_perf_counter_name(lc_perf_counter_t counter) {
    switch (counter) {
    case LC_PERF_COUNTER_CPU_LOOP:
        return "cpu-loop";
    case LC_PERF_COUNTER_VIDEO_UPDATE:
        return "video-update";
    case LC_PERF_COUNTER_HOST_RENDER:
        return "host-render";
    case LC_PERF_COUNTER_DISPLAY_FLUSH:
        return "display-flush";
    case LC_PERF_COUNTER_COUNT:
    default:
        return "unknown";
    }
}

uint64_t lc_perf_now_us(void) {
    return (uint64_t)esp_timer_get_time();
}

void lc_perf_reset(void) {
    memset(counters, 0, sizeof(counters));
    frame_count = 0;
    reset_time_us = lc_perf_now_us();
}

void lc_perf_record_us(lc_perf_counter_t counter, uint64_t elapsed_us) {
    if (counter >= LC_PERF_COUNTER_COUNT) {
        return;
    }

    lc_perf_stats_t *stats = &counters[counter];
    stats->count++;
    stats->total_us += elapsed_us;
    if (stats->count == 1 || elapsed_us < stats->min_us) {
        stats->min_us = elapsed_us;
    }
    if (elapsed_us > stats->max_us) {
        stats->max_us = elapsed_us;
    }
}

void lc_perf_increment_frame(void) {
    frame_count++;
}

void lc_perf_log_summary(void) {
    const uint64_t now = lc_perf_now_us();
    const uint64_t elapsed = now > reset_time_us ? now - reset_time_us : 0;
    const double fps = elapsed > 0 ? ((double)frame_count * 1000000.0) / (double)elapsed : 0.0;

    ESP_LOGI(TAG, "LC perf summary: elapsed_us=%" PRIu64 " frames=%" PRIu64 " fps=%.2f",
             elapsed, frame_count, fps);

    for (unsigned i = 0; i < LC_PERF_COUNTER_COUNT; i++) {
        const lc_perf_stats_t *stats = &counters[i];
        if (stats->count == 0) {
            ESP_LOGI(TAG, "LC perf counter %s: no samples", lc_perf_counter_name((lc_perf_counter_t)i));
            continue;
        }
        const uint64_t avg = stats->total_us / stats->count;
        ESP_LOGI(TAG,
                 "LC perf counter %s: count=%" PRIu64 " total_us=%" PRIu64
                 " min_us=%" PRIu64 " avg_us=%" PRIu64 " max_us=%" PRIu64,
                 lc_perf_counter_name((lc_perf_counter_t)i), stats->count, stats->total_us,
                 stats->min_us, avg, stats->max_us);
    }
}
