#ifndef MACHINE_LC_VIDEO_H
#define MACHINE_LC_VIDEO_H

#include "board_profiles.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef LC_VIDEO_WIDTH
#define LC_VIDEO_WIDTH DISP_WIDTH
#endif

#ifndef LC_VIDEO_HEIGHT
#define LC_VIDEO_HEIGHT DISP_HEIGHT
#endif

#ifndef LC_VIDEO_BPP
#define LC_VIDEO_BPP LC_GUEST_COLOR_DEPTH_BITS
#endif

#ifndef LC_VIDEO_MONITOR_SENSE
#define LC_VIDEO_MONITOR_SENSE 0x01u
#endif

#ifndef LC_VIDEO_VBL_HZ
#define LC_VIDEO_VBL_HZ 60u
#endif

#ifndef LC_VIDEO_RENDER_STRIP_LINES
#define LC_VIDEO_RENDER_STRIP_LINES 16u
#endif

#define LC_VIDEO_ROWBYTES ((size_t)LC_VIDEO_WIDTH * (size_t)LC_VIDEO_BPP / 8u)
#define LC_VIDEO_INDEXED_SIZE (LC_VIDEO_ROWBYTES * (size_t)LC_VIDEO_HEIGHT)
#define LC_VIDEO_CLUT_ENTRIES 256u
#define LC_VIDEO_RGB565_STRIP_PIXELS ((size_t)LC_VIDEO_WIDTH * (size_t)LC_VIDEO_RENDER_STRIP_LINES)

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t rowbytes;
    uint8_t bpp;
    uint8_t monitor_sense;
    uint8_t vbl_hz;
    size_t indexed_size;
    const char *vram_placement;
} lc_video_model_t;

typedef struct {
    bool any_dirty;
    uint16_t first_row;
    uint16_t last_row;
    uint16_t dirty_rows;
    uint16_t rendered_strips;
    uint32_t rgb565_checksum;
} lc_video_dirty_summary_t;

void lc_video_get_model(lc_video_model_t *model);
void lc_video_log_model(void);
void lc_video_init_debug_palette(uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES]);
void lc_video_mark_all_dirty(uint8_t dirty_rows[LC_VIDEO_HEIGHT]);
lc_video_dirty_summary_t lc_video_summarize_dirty_rows(const uint8_t dirty_rows[LC_VIDEO_HEIGHT]);
uint32_t lc_video_fill_test_pattern(uint8_t *indexed_pixels, size_t size);
uint32_t lc_video_convert_indexed_rows_to_rgb565(const uint8_t *indexed_pixels, size_t indexed_size,
                                                 uint16_t start_row, uint16_t row_count,
                                                 const uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES],
                                                 uint16_t *rgb565_out, size_t out_pixels);
lc_video_dirty_summary_t lc_video_render_dirty_rows_to_rgb565(
    const uint8_t *indexed_pixels, size_t indexed_size, const uint8_t dirty_rows[LC_VIDEO_HEIGHT],
    const uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES], uint16_t *rgb565_strip,
    size_t strip_pixels);
void lc_video_probe_test_pattern(void);

#endif
