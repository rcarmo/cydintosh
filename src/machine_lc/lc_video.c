#include "lc_video.h"

#include "board_profiles.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lc_perf.h"

#include <inttypes.h>
#include <string.h>

static const char *TAG = "lc_video";

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)((r & 0xF8u) << 8) | (uint16_t)((g & 0xFCu) << 3) |
           (uint16_t)(b >> 3);
}

void lc_video_get_model(lc_video_model_t *model) {
    if (model == NULL) {
        return;
    }
    model->width = LC_VIDEO_WIDTH;
    model->height = LC_VIDEO_HEIGHT;
    model->rowbytes = LC_VIDEO_ROWBYTES;
    model->bpp = LC_VIDEO_BPP;
    model->monitor_sense = LC_VIDEO_MONITOR_SENSE;
    model->vbl_hz = LC_VIDEO_VBL_HZ;
    model->indexed_size = LC_VIDEO_INDEXED_SIZE;
    model->vram_placement = "separate-device-buffer-in-psram-first";
}

void lc_video_log_model(void) {
    lc_video_model_t model = {0};
    lc_video_get_model(&model);
    ESP_LOGI(TAG,
             "LC video scaffold: %ux%u %ubpp rowbytes=%u indexed_size=%zu vbl=%uHz monitor_sense=0x%02x",
             model.width, model.height, model.bpp, model.rowbytes, model.indexed_size,
             model.vbl_hz, model.monitor_sense);
    ESP_LOGI(TAG, "LC video scaffold: vram_placement=%s renderer=dirty-row/strip-target",
             model.vram_placement);
}

void lc_video_init_debug_palette(uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES]) {
    if (palette_rgb565 == NULL) {
        return;
    }
    for (unsigned i = 0; i < LC_VIDEO_CLUT_ENTRIES; i++) {
        const uint8_t r = (uint8_t)i;
        const uint8_t g = (uint8_t)((i * 5u) & 0xffu);
        const uint8_t b = (uint8_t)(255u - i);
        palette_rgb565[i] = rgb565(r, g, b);
    }
}

void lc_video_mark_all_dirty(uint8_t dirty_rows[LC_VIDEO_HEIGHT]) {
    if (dirty_rows == NULL) {
        return;
    }
    memset(dirty_rows, 1, LC_VIDEO_HEIGHT);
}

lc_video_dirty_summary_t lc_video_summarize_dirty_rows(const uint8_t dirty_rows[LC_VIDEO_HEIGHT]) {
    lc_video_dirty_summary_t summary = {
        .any_dirty = false,
        .first_row = 0,
        .last_row = 0,
        .dirty_rows = 0,
        .rendered_strips = 0,
        .rgb565_checksum = 2166136261u,
    };
    if (dirty_rows == NULL) {
        return summary;
    }

    for (uint16_t y = 0; y < LC_VIDEO_HEIGHT; y++) {
        if (!dirty_rows[y]) {
            continue;
        }
        if (!summary.any_dirty) {
            summary.first_row = y;
            summary.any_dirty = true;
        }
        summary.last_row = y;
        summary.dirty_rows++;
    }
    return summary;
}

uint32_t lc_video_fill_test_pattern(uint8_t *indexed_pixels, size_t size) {
    if (indexed_pixels == NULL || size < LC_VIDEO_INDEXED_SIZE) {
        return 0;
    }

    uint32_t checksum = 2166136261u;
    for (unsigned y = 0; y < LC_VIDEO_HEIGHT; y++) {
        uint8_t *row = &indexed_pixels[(size_t)y * LC_VIDEO_ROWBYTES];
        for (unsigned x = 0; x < LC_VIDEO_WIDTH; x++) {
            const uint8_t ramp = (uint8_t)((x * 255u) / (LC_VIDEO_WIDTH - 1u));
            const uint8_t stripe = (uint8_t)(((y / 16u) & 1u) ? 0x40u : 0x00u);
            const uint8_t marker = (x < 16u || y < 16u || x >= LC_VIDEO_WIDTH - 16u ||
                                    y >= LC_VIDEO_HEIGHT - 16u)
                                       ? 0x80u
                                       : 0x00u;
            const uint8_t pixel = (uint8_t)(ramp ^ stripe ^ marker);
            row[x] = pixel;
            checksum ^= pixel;
            checksum *= 16777619u;
        }
    }
    return checksum;
}

uint32_t lc_video_convert_indexed_rows_to_rgb565(const uint8_t *indexed_pixels, size_t indexed_size,
                                                 uint16_t start_row, uint16_t row_count,
                                                 const uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES],
                                                 uint16_t *rgb565_out, size_t out_pixels) {
    if (indexed_pixels == NULL || palette_rgb565 == NULL || rgb565_out == NULL ||
        indexed_size < LC_VIDEO_INDEXED_SIZE || start_row >= LC_VIDEO_HEIGHT) {
        return 0;
    }

    if ((uint32_t)start_row + row_count > LC_VIDEO_HEIGHT) {
        row_count = LC_VIDEO_HEIGHT - start_row;
    }
    const size_t needed_pixels = (size_t)row_count * LC_VIDEO_WIDTH;
    if (out_pixels < needed_pixels) {
        return 0;
    }

    uint32_t checksum = 2166136261u;
    for (uint16_t row = 0; row < row_count; row++) {
        const uint8_t *src = &indexed_pixels[(size_t)(start_row + row) * LC_VIDEO_ROWBYTES];
        uint16_t *dst = &rgb565_out[(size_t)row * LC_VIDEO_WIDTH];
        for (uint16_t x = 0; x < LC_VIDEO_WIDTH; x++) {
            const uint16_t rgb = palette_rgb565[src[x]];
            dst[x] = rgb;
            checksum ^= rgb;
            checksum *= 16777619u;
        }
    }
    return checksum;
}

lc_video_dirty_summary_t lc_video_render_dirty_rows_to_rgb565(
    const uint8_t *indexed_pixels, size_t indexed_size, const uint8_t dirty_rows[LC_VIDEO_HEIGHT],
    const uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES], uint16_t *rgb565_strip,
    size_t strip_pixels) {
    lc_video_dirty_summary_t summary = lc_video_summarize_dirty_rows(dirty_rows);
    if (!summary.any_dirty || indexed_pixels == NULL || palette_rgb565 == NULL || rgb565_strip == NULL ||
        strip_pixels < LC_VIDEO_RGB565_STRIP_PIXELS) {
        return summary;
    }

    const uint64_t start = lc_perf_now_us();
    uint16_t row = summary.first_row;
    summary.rgb565_checksum = 2166136261u;
    while (row <= summary.last_row) {
        while (row <= summary.last_row && !dirty_rows[row]) {
            row++;
        }
        if (row > summary.last_row) {
            break;
        }

        uint16_t strip_start = row;
        uint16_t strip_rows = 0;
        while (row <= summary.last_row && dirty_rows[row] && strip_rows < LC_VIDEO_RENDER_STRIP_LINES) {
            strip_rows++;
            row++;
        }

        const uint32_t strip_checksum = lc_video_convert_indexed_rows_to_rgb565(
            indexed_pixels, indexed_size, strip_start, strip_rows, palette_rgb565, rgb565_strip,
            strip_pixels);
        summary.rgb565_checksum ^= strip_checksum;
        summary.rgb565_checksum *= 16777619u;
        summary.rendered_strips++;
    }
    lc_perf_record_us(LC_PERF_COUNTER_HOST_RENDER, lc_perf_now_us() - start);
    return summary;
}

void lc_video_probe_test_pattern(void) {
    lc_video_log_model();

    uint16_t palette[LC_VIDEO_CLUT_ENTRIES] = {0};
    lc_video_init_debug_palette(palette);
    ESP_LOGI(TAG, "LC debug CLUT sample: [0]=0x%04x [64]=0x%04x [128]=0x%04x [255]=0x%04x",
             palette[0], palette[64], palette[128], palette[255]);

    uint8_t *pixels = (uint8_t *)heap_caps_malloc(LC_VIDEO_INDEXED_SIZE,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        ESP_LOGW(TAG, "LC video test-pattern allocation failed: size=%zu", LC_VIDEO_INDEXED_SIZE);
        return;
    }

    const uint64_t start = lc_perf_now_us();
    const uint32_t checksum = lc_video_fill_test_pattern(pixels, LC_VIDEO_INDEXED_SIZE);
    lc_perf_record_us(LC_PERF_COUNTER_VIDEO_UPDATE, lc_perf_now_us() - start);

    uint8_t dirty_rows[LC_VIDEO_HEIGHT] = {0};
    lc_video_mark_all_dirty(dirty_rows);
    uint16_t *rgb_strip = (uint16_t *)heap_caps_malloc(LC_VIDEO_RGB565_STRIP_PIXELS * sizeof(uint16_t),
                                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT);
    lc_video_dirty_summary_t dirty = {0};
    if (rgb_strip == NULL) {
        ESP_LOGW(TAG, "LC RGB565 render-strip allocation failed in video probe: pixels=%zu",
                 LC_VIDEO_RGB565_STRIP_PIXELS);
    } else {
        dirty = lc_video_render_dirty_rows_to_rgb565(pixels, LC_VIDEO_INDEXED_SIZE, dirty_rows,
                                                     palette, rgb_strip,
                                                     LC_VIDEO_RGB565_STRIP_PIXELS);
        heap_caps_free(rgb_strip);
    }
    lc_perf_increment_frame();

    ESP_LOGI(TAG, "LC video test pattern generated: pixels=%p size=%zu checksum=0x%08" PRIx32
                  " dirty_rows=%u strips=%u rgb565_checksum=0x%08" PRIx32,
             (void *)pixels, LC_VIDEO_INDEXED_SIZE, checksum, dirty.dirty_rows,
             dirty.rendered_strips, dirty.rgb565_checksum);
    heap_caps_free(pixels);
}
