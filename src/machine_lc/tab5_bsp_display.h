#ifndef MACHINE_LC_TAB5_BSP_DISPLAY_H
#define MACHINE_LC_TAB5_BSP_DISPLAY_H

#include "esp_err.h"
#include "lc_video.h"

#include <stddef.h>
#include <stdint.h>

#define TAB5_LC_VIEWPORT_SCALE_NUM 45u
#define TAB5_LC_VIEWPORT_SCALE_DEN 32u
#define TAB5_LC_VIEWPORT_W ((LC_VIDEO_WIDTH * TAB5_LC_VIEWPORT_SCALE_NUM) / TAB5_LC_VIEWPORT_SCALE_DEN)
#define TAB5_LC_VIEWPORT_H ((LC_VIDEO_HEIGHT * TAB5_LC_VIEWPORT_SCALE_NUM) / TAB5_LC_VIEWPORT_SCALE_DEN)

esp_err_t tab5_bsp_display_init(void);
esp_err_t tab5_bsp_display_set_brightness(uint8_t percent);
esp_err_t tab5_bsp_display_flush_indexed(const uint8_t *indexed_pixels, size_t indexed_size,
                                         const uint16_t palette_rgb565[LC_VIDEO_CLUT_ENTRIES]);
esp_err_t tab5_bsp_display_draw_lc_test_pattern(void);
_Noreturn void tab5_bsp_display_brightness_heartbeat_loop(void);
_Noreturn void tab5_bsp_display_smoke_run(void);

#endif
