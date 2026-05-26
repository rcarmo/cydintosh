#ifndef MACHINE_LC_VIDEO_H
#define MACHINE_LC_VIDEO_H

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

#define LC_VIDEO_ROWBYTES ((size_t)LC_VIDEO_WIDTH * (size_t)LC_VIDEO_BPP / 8u)
#define LC_VIDEO_INDEXED_SIZE (LC_VIDEO_ROWBYTES * (size_t)LC_VIDEO_HEIGHT)
#define LC_VIDEO_CLUT_ENTRIES 256u

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

void lc_video_get_model(lc_video_model_t *model);
void lc_video_log_model(void);
void lc_video_init_debug_palette(uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES]);
uint32_t lc_video_fill_test_pattern(uint8_t *indexed_pixels, size_t size);
void lc_video_probe_test_pattern(void);

#endif
