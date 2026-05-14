#include "display.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd_cyd.h"
#include "hw.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "display";

// For SPI (ILI9341) panels: narrow strip buffer for column-by-column transfer.
// For RGB panels: direct pointer into the panel's PSRAM framebuffer.
static uint16_t *rgb_buf      = NULL; // SPI strip buffer; NULL on RGB builds
static uint16_t *rgb_framebuf = NULL; // RGB direct PSRAM framebuffer; NULL on SPI builds
static uint8_t  *fb_copy      = NULL; // Safe copy of the 1-bit Mac framebuffer
static uint8_t  *fb_prev      = NULL; // Previous 1-bit framebuffer for dirty-row rendering

#define RGB_BUF_LINES  LCD_TRANSFER_STRIP_WIDTH
#define FB_COPY_SIZE   (DISP_WIDTH * DISP_HEIGHT / 8)

#if defined(LCD_PANEL_ILI9341_SPI) && LCD_RENDER_FIT_TO_PANEL
#if LCD_RENDER_ROTATE_CW
#define LCD_FIT_SRC_WIDTH  DISP_HEIGHT
#define LCD_FIT_SRC_HEIGHT DISP_WIDTH
#else
#define LCD_FIT_SRC_WIDTH  DISP_WIDTH
#define LCD_FIT_SRC_HEIGHT DISP_HEIGHT
#endif
#define LCD_FIT_WIDTH_BY_HEIGHT  ((LCD_FIT_SRC_WIDTH * LCD_HEIGHT) / LCD_FIT_SRC_HEIGHT)
#define LCD_FIT_HEIGHT_BY_WIDTH  ((LCD_FIT_SRC_HEIGHT * LCD_WIDTH) / LCD_FIT_SRC_WIDTH)
#if LCD_FIT_WIDTH_BY_HEIGHT <= LCD_WIDTH
#define LCD_FIT_WIDTH  LCD_FIT_WIDTH_BY_HEIGHT
#define LCD_FIT_HEIGHT LCD_HEIGHT
#else
#define LCD_FIT_WIDTH  LCD_WIDTH
#define LCD_FIT_HEIGHT LCD_FIT_HEIGHT_BY_WIDTH
#endif
#define LCD_FIT_OFFSET_X ((LCD_WIDTH - LCD_FIT_WIDTH) / 2)
#define LCD_FIT_OFFSET_Y ((LCD_HEIGHT - LCD_FIT_HEIGHT) / 2)
#endif

static TaskHandle_t  display_task_handle  = NULL;
static const uint8_t *current_framebuffer = NULL;
static volatile bool  frame_pending       = false;

static int64_t display_render_total_us = 0;
static int64_t display_cache_total_us  = 0;
static int     display_profile_frames  = 0;
static int     display_dirty_total     = 0;
#define DISPLAY_PROFILE_INTERVAL 60

// ---------------------------------------------------------------------------
// Mac 1-bit pixel → RGB565
// ---------------------------------------------------------------------------
static inline uint8_t mac_pixel_on(int mac_col, int mac_row) {
    const int mac_bytes_per_row = DISP_WIDTH / 8;
    int byte_offset = mac_row * mac_bytes_per_row + (mac_col / 8);
    int bit_offset  = 7 - (mac_col % 8);
    uint8_t pixel   = (fb_copy[byte_offset] >> bit_offset) & 1;
#if LCD_RENDER_INVERT_MONO
    pixel ^= 1;
#endif
    return pixel;
}

static inline uint16_t panel_rgb565(uint16_t rgb565) {
#if LCD_PANEL_RGB565_BYTE_SWAP
    return (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
#else
    return rgb565;
#endif
}

static inline uint16_t gray8_to_panel_rgb565(uint8_t gray) {
    uint16_t rgb565 = ((gray & 0xF8) << 8) | ((gray & 0xFC) << 3) | (gray >> 3);
    return panel_rgb565(rgb565);
}

static inline uint16_t gray_coverage_to_panel_rgb565(uint32_t lit_weight, uint32_t total_weight) {
    // Perceptual-ish 17-step grayscale ramp for 1-bit UI downsampling.
    // The area filter yields linear white coverage; the palette brightens midtones
    // slightly so downsampled 1px Mac strokes remain readable on the ILI9341.
    static const uint8_t palette[17] = {
        0, 63, 88, 106, 122, 135, 147, 158, 168, 177, 186, 194, 202, 210, 218, 228, 255,
    };
    if (total_weight == 0) return gray8_to_panel_rgb565(0);
    uint32_t idx = (lit_weight * 16U + total_weight / 2U) / total_weight;
    if (idx > 16U) idx = 16U;
    return gray8_to_panel_rgb565(palette[idx]);
}

static inline uint16_t mac_pixel_to_rgb565(int mac_col, int mac_row) {
    return mac_pixel_on(mac_col, mac_row) ? panel_rgb565(0xFFFF) : panel_rgb565(0x0000);
}

// ---------------------------------------------------------------------------
// RGB panel path: render at Mac resolution directly into PSRAM framebuffer.
//
// Iterates DISP_HEIGHT × DISP_WIDTH Mac pixels (e.g. 400×240 = 96 000) and
// writes each as a LCD_RENDER_SCALE × LCD_RENDER_SCALE block — about 5× less
// CPU work than iterating the full 800×480 LCD pixel grid.
//
// Mapping for ROTATE_CW=1 (device held portrait, USB to the side):
//   mac_row axis  → LCD_X  (horizontal)
//   mac_col axis  → LCD_Y  (vertical)
//
// FLIP_X inverts the mac_row → LCD_X mapping (left/right mirror fix).
// FLIP_Y inverts the mac_col → LCD_Y mapping (top/bottom mirror fix).
// ---------------------------------------------------------------------------
#if defined(LCD_PANEL_RGB)
static inline uint16_t mac_pixel_to_rgb565_fast(int mac_col, int mac_row) {
    const int mac_bytes_per_row = DISP_WIDTH / 8;
    uint8_t byte = fb_copy[mac_row * mac_bytes_per_row + (mac_col >> 3)];
    uint8_t pixel = (byte >> (7 - (mac_col & 7))) & 1;
#if LCD_RENDER_INVERT_MONO
    pixel ^= 1;
#endif
    return pixel ? 0xFFFF : 0x0000;
}

static void render_mac_row_to_rgb_framebuffer(int mac_row) {
#if LCD_RENDER_ROTATE_CW && LCD_RENDER_SCALE == 2
#if LCD_RENDER_FLIP_X
    const int lcd_x_base = LCD_RENDER_OFFSET_X + mac_row * 2;
#else
    const int lcd_x_base = LCD_RENDER_OFFSET_X + (DISP_HEIGHT - 1 - mac_row) * 2;
#endif
    for (int mac_col = 0; mac_col < DISP_WIDTH; mac_col++) {
#if LCD_RENDER_FLIP_Y
        const int lcd_y_base = LCD_RENDER_OFFSET_Y + (DISP_WIDTH - 1 - mac_col) * 2;
#else
        const int lcd_y_base = LCD_RENDER_OFFSET_Y + mac_col * 2;
#endif
        const uint16_t rgb = mac_pixel_to_rgb565_fast(mac_col, mac_row);
        uint16_t *row0 = rgb_framebuf + lcd_y_base * LCD_WIDTH + lcd_x_base;
        uint16_t *row1 = row0 + LCD_WIDTH;
        row0[0] = rgb;
        row0[1] = rgb;
        row1[0] = rgb;
        row1[1] = rgb;
    }
#else
    (void)mac_row;
#endif
}

static void render_mac_col_to_rgb_framebuffer(int mac_col) {
#if LCD_RENDER_ROTATE_CW && LCD_RENDER_SCALE == 1
#if LCD_RENDER_FLIP_Y
    const int lcd_y = LCD_RENDER_OFFSET_Y + (DISP_WIDTH - 1 - mac_col);
#else
    const int lcd_y = LCD_RENDER_OFFSET_Y + mac_col;
#endif
    uint16_t *row = rgb_framebuf + lcd_y * LCD_WIDTH + LCD_RENDER_OFFSET_X;
    for (int lcd_x = 0; lcd_x < DISP_HEIGHT; lcd_x++) {
#if LCD_RENDER_FLIP_X
        const int mac_row = lcd_x;
#else
        const int mac_row = DISP_HEIGHT - 1 - lcd_x;
#endif
        row[lcd_x] = mac_pixel_to_rgb565_fast(mac_col, mac_row);
    }
#else
    (void)mac_col;
#endif
}

static int render_dirty_to_rgb_framebuffer(void) {
#if LCD_RENDER_ROTATE_CW && LCD_RENDER_SCALE == 1
    const int bytes_per_row = DISP_WIDTH / 8;
    uint8_t dirty_cols[DISP_WIDTH] = {0};
    int dirty_col_count = 0;

    for (int mac_row = 0; mac_row < DISP_HEIGHT; mac_row++) {
        uint8_t *cur = fb_copy + mac_row * bytes_per_row;
        uint8_t *old = fb_prev + mac_row * bytes_per_row;
        for (int byte_col = 0; byte_col < bytes_per_row; byte_col++) {
            if (cur[byte_col] != old[byte_col]) {
                const int base_col = byte_col * 8;
                for (int bit = 0; bit < 8; bit++) {
                    int mac_col = base_col + bit;
                    if (mac_col < DISP_WIDTH && !dirty_cols[mac_col]) {
                        dirty_cols[mac_col] = 1;
                        dirty_col_count++;
                    }
                }
                old[byte_col] = cur[byte_col];
            }
        }
    }

    for (int mac_col = 0; mac_col < DISP_WIDTH; mac_col++) {
        if (dirty_cols[mac_col]) render_mac_col_to_rgb_framebuffer(mac_col);
    }
    return dirty_col_count;
#elif LCD_RENDER_ROTATE_CW && LCD_RENDER_SCALE == 2
    const int bytes_per_row = DISP_WIDTH / 8;
    int dirty_rows = 0;
    for (int mac_row = 0; mac_row < DISP_HEIGHT; mac_row++) {
        uint8_t *cur = fb_copy + mac_row * bytes_per_row;
        uint8_t *old = fb_prev + mac_row * bytes_per_row;
        if (memcmp(cur, old, bytes_per_row) != 0) {
            render_mac_row_to_rgb_framebuffer(mac_row);
            memcpy(old, cur, bytes_per_row);
            dirty_rows++;
        }
    }
    return dirty_rows;
#else
    return -1;
#endif
}

static void render_to_rgb_framebuffer(void) {
    // Generic RGB fallback: iterate Mac pixels and expand to LCD_RENDER_SCALE² blocks.
#if LCD_RENDER_ROTATE_CW
#if LCD_RENDER_FLIP_X
    int lcd_x_base = LCD_RENDER_OFFSET_X;
    const int lcd_x_step = LCD_RENDER_SCALE;
#else
    int lcd_x_base = LCD_RENDER_OFFSET_X + (DISP_HEIGHT - 1) * LCD_RENDER_SCALE;
    const int lcd_x_step = -LCD_RENDER_SCALE;
#endif

    for (int mac_row = 0; mac_row < DISP_HEIGHT; mac_row++, lcd_x_base += lcd_x_step) {
#if LCD_RENDER_FLIP_Y
        int lcd_y_base = LCD_RENDER_OFFSET_Y + (DISP_WIDTH - 1) * LCD_RENDER_SCALE;
        const int lcd_y_step = -LCD_RENDER_SCALE;
#else
        int lcd_y_base = LCD_RENDER_OFFSET_Y;
        const int lcd_y_step = LCD_RENDER_SCALE;
#endif
        for (int mac_col = 0; mac_col < DISP_WIDTH; mac_col++, lcd_y_base += lcd_y_step) {
            const uint16_t rgb = mac_pixel_to_rgb565_fast(mac_col, mac_row);
            for (int dy = 0; dy < LCD_RENDER_SCALE; dy++) {
                uint16_t *out = rgb_framebuf + (lcd_y_base + dy) * LCD_WIDTH + lcd_x_base;
                for (int dx = 0; dx < LCD_RENDER_SCALE; dx++) out[dx] = rgb;
            }
        }
    }
#else // !LCD_RENDER_ROTATE_CW
    for (int mac_row = 0; mac_row < DISP_HEIGHT; mac_row++) {
#if LCD_RENDER_FLIP_Y
        const int lcd_y_base = LCD_RENDER_OFFSET_Y + (DISP_HEIGHT - 1 - mac_row) * LCD_RENDER_SCALE;
#else
        const int lcd_y_base = LCD_RENDER_OFFSET_Y + mac_row * LCD_RENDER_SCALE;
#endif
        for (int mac_col = 0; mac_col < DISP_WIDTH; mac_col++) {
            const uint16_t rgb = mac_pixel_to_rgb565_fast(mac_col, mac_row);
#if LCD_RENDER_FLIP_X
            const int lcd_x_base = LCD_RENDER_OFFSET_X + (DISP_WIDTH - 1 - mac_col) * LCD_RENDER_SCALE;
#else
            const int lcd_x_base = LCD_RENDER_OFFSET_X + mac_col * LCD_RENDER_SCALE;
#endif
            for (int dy = 0; dy < LCD_RENDER_SCALE; dy++) {
                uint16_t *out = rgb_framebuf + (lcd_y_base + dy) * LCD_WIDTH + lcd_x_base;
                for (int dx = 0; dx < LCD_RENDER_SCALE; dx++) out[dx] = rgb;
            }
        }
    }
#endif // LCD_RENDER_ROTATE_CW
}
#endif // LCD_PANEL_RGB

// ---------------------------------------------------------------------------
// SPI panel path: lcd_pixel_to_rgb565 (unchanged, used by strip loop)
// ---------------------------------------------------------------------------
#if defined(LCD_PANEL_ILI9341_SPI)
static inline uint16_t lcd_pixel_to_rgb565(int lcd_x, int lcd_y) {
#if LCD_RENDER_FIT_TO_PANEL
    int local_x = lcd_x - LCD_FIT_OFFSET_X;
    int local_y = lcd_y - LCD_FIT_OFFSET_Y;

    if (local_x < 0 || local_y < 0 || local_x >= LCD_FIT_WIDTH || local_y >= LCD_FIT_HEIGHT) {
        return 0x0000;
    }

    int rx = (local_x * LCD_FIT_SRC_WIDTH) / LCD_FIT_WIDTH;
    int ry = (local_y * LCD_FIT_SRC_HEIGHT) / LCD_FIT_HEIGHT;
    if (rx >= LCD_FIT_SRC_WIDTH)  rx = LCD_FIT_SRC_WIDTH - 1;
    if (ry >= LCD_FIT_SRC_HEIGHT) ry = LCD_FIT_SRC_HEIGHT - 1;

#if LCD_RENDER_FLIP_X
    rx = LCD_FIT_SRC_WIDTH - 1 - rx;
#endif
#if LCD_RENDER_FLIP_Y
    ry = LCD_FIT_SRC_HEIGHT - 1 - ry;
#endif

#if LCD_RENDER_FIT_GRAYSCALE
#if LCD_RENDER_ROTATE_CW && \
    (LCD_FIT_WIDTH == LCD_WIDTH) && (LCD_FIT_HEIGHT == LCD_HEIGHT) && \
    ((LCD_FIT_SRC_WIDTH * 5) == (LCD_FIT_WIDTH * 8)) && \
    ((LCD_FIT_SRC_HEIGHT * 5) == (LCD_FIT_HEIGHT * 8))
    // Fast exact area filter for the 512x384 -> rotate -> 384x512 -> 240x320 case.
    // Each LCD pixel covers 8/5 source pixels in both axes. Coverage weights repeat
    // every five destination pixels and sum to 8 per axis (64 per 2D pixel).
    static const uint8_t counts[5] = {2, 3, 2, 3, 2};
    static const uint8_t offs[5][3] = {
        {0, 1, 0}, {1, 2, 3}, {3, 4, 0}, {4, 5, 6}, {6, 7, 0},
    };
    static const uint8_t weights[5][3] = {
        {5, 3, 0}, {2, 5, 1}, {4, 4, 0}, {1, 5, 2}, {3, 5, 0},
    };

    const int x_group = local_x / 5;
    const int y_group = local_y / 5;
    const int x_phase = local_x - x_group * 5;
    const int y_phase = local_y - y_group * 5;
    const int src_x_base = x_group * 8;
    const int src_y_base = y_group * 8;
    uint32_t lit_weight = 0;

    for (int yi = 0; yi < counts[y_phase]; yi++) {
        int sample_y = src_y_base + offs[y_phase][yi];
#if LCD_RENDER_FLIP_Y
        sample_y = LCD_FIT_SRC_HEIGHT - 1 - sample_y;
#endif
        for (int xi = 0; xi < counts[x_phase]; xi++) {
            int sample_x = src_x_base + offs[x_phase][xi];
#if LCD_RENDER_FLIP_X
            sample_x = LCD_FIT_SRC_WIDTH - 1 - sample_x;
#endif
            int mac_col = sample_y;
            int mac_row = DISP_HEIGHT - 1 - sample_x;
            lit_weight += (uint32_t)mac_pixel_on(mac_col, mac_row) *
                          (uint32_t)weights[x_phase][xi] * (uint32_t)weights[y_phase][yi];
        }
    }

    return gray_coverage_to_panel_rgb565(lit_weight, 64U);
#else
    const int fp_shift = 8;
    const int fp_one = 1 << fp_shift;
    int x0 = (local_x * LCD_FIT_SRC_WIDTH * fp_one) / LCD_FIT_WIDTH;
    int x1 = ((local_x + 1) * LCD_FIT_SRC_WIDTH * fp_one) / LCD_FIT_WIDTH;
    int y0 = (local_y * LCD_FIT_SRC_HEIGHT * fp_one) / LCD_FIT_HEIGHT;
    int y1 = ((local_y + 1) * LCD_FIT_SRC_HEIGHT * fp_one) / LCD_FIT_HEIGHT;

    uint32_t lit_weight = 0;
    uint32_t total_weight = 0;

    for (int sy = y0 >> fp_shift; sy <= ((y1 - 1) >> fp_shift); sy++) {
        int sy0 = sy << fp_shift;
        int sy1 = sy0 + fp_one;
        int wy = (y1 < sy1 ? y1 : sy1) - (y0 > sy0 ? y0 : sy0);
        if (wy <= 0) continue;

        for (int sx = x0 >> fp_shift; sx <= ((x1 - 1) >> fp_shift); sx++) {
            int sx0 = sx << fp_shift;
            int sx1 = sx0 + fp_one;
            int wx = (x1 < sx1 ? x1 : sx1) - (x0 > sx0 ? x0 : sx0);
            if (wx <= 0) continue;

            int sample_x = sx;
            int sample_y = sy;
#if LCD_RENDER_FLIP_X
            sample_x = LCD_FIT_SRC_WIDTH - 1 - sample_x;
#endif
#if LCD_RENDER_FLIP_Y
            sample_y = LCD_FIT_SRC_HEIGHT - 1 - sample_y;
#endif

#if LCD_RENDER_ROTATE_CW
            int mac_col = sample_y;
            int mac_row = DISP_HEIGHT - 1 - sample_x;
#else
            int mac_col = sample_x;
            int mac_row = sample_y;
#endif
            uint32_t weight = (uint32_t)wx * (uint32_t)wy;
            lit_weight += (uint32_t)mac_pixel_on(mac_col, mac_row) * weight;
            total_weight += weight;
        }
    }

    return gray_coverage_to_panel_rgb565(lit_weight, total_weight);
#endif
#else
#if LCD_RENDER_ROTATE_CW
    int mac_col = ry;
    int mac_row = DISP_HEIGHT - 1 - rx;
#else
    int mac_col = rx;
    int mac_row = ry;
#endif

    return mac_pixel_to_rgb565(mac_col, mac_row);
#endif
#else
    int local_x = lcd_x - LCD_RENDER_OFFSET_X;
    int local_y = lcd_y - LCD_RENDER_OFFSET_Y;

#if LCD_RENDER_ROTATE_CW
    const int render_width  = DISP_HEIGHT * LCD_RENDER_SCALE;
    const int render_height = DISP_WIDTH  * LCD_RENDER_SCALE;
#else
    const int render_width  = DISP_WIDTH  * LCD_RENDER_SCALE;
    const int render_height = DISP_HEIGHT * LCD_RENDER_SCALE;
#endif

    if (local_x < 0 || local_y < 0 || local_x >= render_width || local_y >= render_height) {
        return 0x0000;
    }

#if LCD_RENDER_FLIP_X
    local_x = render_width  - 1 - local_x;
#endif
#if LCD_RENDER_FLIP_Y
    local_y = render_height - 1 - local_y;
#endif

    int rx = local_x / LCD_RENDER_SCALE;
    int ry = local_y / LCD_RENDER_SCALE;

#if LCD_RENDER_ROTATE_CW
    int mac_col = ry;
    int mac_row = DISP_HEIGHT - 1 - rx;
#else
    int mac_col = rx;
    int mac_row = ry;
#endif

    return mac_pixel_to_rgb565(mac_col, mac_row);
#endif
}

static int render_dirty_to_spi_panel(void) {
#if !LCD_RENDER_ROTATE_CW && LCD_RENDER_SCALE == 1
    const int bytes_per_row = DISP_WIDTH / 8;
    int dirty_rows = 0;

    for (int mac_row = 0; mac_row < DISP_HEIGHT;) {
        uint8_t *cur = fb_copy + mac_row * bytes_per_row;
        uint8_t *old = fb_prev + mac_row * bytes_per_row;
        if (memcmp(cur, old, bytes_per_row) == 0) {
            mac_row++;
            continue;
        }

        const int start_row = mac_row;
        int lines = 0;
        while (mac_row < DISP_HEIGHT && lines < RGB_BUF_LINES) {
            cur = fb_copy + mac_row * bytes_per_row;
            old = fb_prev + mac_row * bytes_per_row;
            if (memcmp(cur, old, bytes_per_row) != 0) {
                for (int x = 0; x < LCD_WIDTH; x++) {
                    rgb_buf[lines * LCD_WIDTH + x] = mac_pixel_to_rgb565(x, mac_row);
                }
                memcpy(old, cur, bytes_per_row);
                dirty_rows++;
                lines++;
                mac_row++;
            } else if (lines == 0) {
                mac_row++;
            } else {
                break;
            }
        }

        if (lines > 0) {
            lcd_draw_bitmap(0, start_row, LCD_WIDTH, lines, (const uint8_t *)rgb_buf);
            lcd_wait_trans_complete();
        }
    }

    return dirty_rows;
#else
    return -1;
#endif
}
#endif // LCD_PANEL_ILI9341_SPI

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------
void display_init(void) {
#if defined(LCD_PANEL_RGB)
    lcd_get_rgb_framebuffer((void **)&rgb_framebuf);
    if (rgb_framebuf == NULL) {
        ESP_LOGE(TAG, "Failed to get RGB panel framebuffer");
        return;
    }
    memset(rgb_framebuf, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    ESP_LOGI(TAG, "RGB framebuffer at %p (%dx%d, Mac %dx%d scale %d)",
             rgb_framebuf, LCD_WIDTH, LCD_HEIGHT, DISP_WIDTH, DISP_HEIGHT, LCD_RENDER_SCALE);
#else
    rgb_buf = heap_caps_malloc(LCD_HEIGHT * RGB_BUF_LINES * sizeof(uint16_t), LCD_TRANSFER_BUFFER_CAPS);
    if (rgb_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate RGB strip buffer");
        return;
    }
    ESP_LOGI(TAG, "Allocated RGB strip buffer: %d bytes (%d lines on %dx%d panel)",
             LCD_HEIGHT * RGB_BUF_LINES * 2, RGB_BUF_LINES, LCD_WIDTH, LCD_HEIGHT);
#endif

    fb_copy = heap_caps_malloc(FB_COPY_SIZE, MALLOC_CAP_8BIT);
    if (fb_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer copy");
        return;
    }
    ESP_LOGI(TAG, "Allocated framebuffer copy: %d bytes (%dx%d Mac FB)",
             FB_COPY_SIZE, DISP_WIDTH, DISP_HEIGHT);

    fb_prev = heap_caps_malloc(FB_COPY_SIZE, MALLOC_CAP_8BIT);
    if (fb_prev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate previous framebuffer copy");
        return;
    }
    memset(fb_prev, 0xFF, FB_COPY_SIZE); // force first frame to redraw all rows
    ESP_LOGI(TAG, "Allocated previous framebuffer copy: %d bytes", FB_COPY_SIZE);
}

// ---------------------------------------------------------------------------
// Notify / set framebuffer
// ---------------------------------------------------------------------------
void display_notify_update(void) {
    if (display_task_handle) {
        frame_pending = true;
        xTaskNotifyGive(display_task_handle);
    }
}

void display_set_framebuffer(const uint8_t *fb) {
    current_framebuffer = fb;
}

// ---------------------------------------------------------------------------
// Display task
// ---------------------------------------------------------------------------
static void display_task(void *arg) {
    ESP_LOGI(TAG, "Display task started on Core %d", xPortGetCoreID());

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!frame_pending || !current_framebuffer) continue;

        memcpy(fb_copy, current_framebuffer, FB_COPY_SIZE);
        frame_pending = false;

#if defined(LCD_PANEL_RGB)
        const int64_t render_start = esp_timer_get_time();
        int dirty_rows = render_dirty_to_rgb_framebuffer();
        if (dirty_rows < 0) {
            render_to_rgb_framebuffer();
            dirty_rows = DISP_HEIGHT;
        }
        const int64_t render_end = esp_timer_get_time();

        if (dirty_rows > 0) {
            // Direct PSRAM framebuffer writes bypass esp_lcd_panel_draw_bitmap(),
            // so explicitly write back CPU cache before RGB DMA scans it.
            esp_cache_msync(rgb_framebuf, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        const int64_t cache_end = esp_timer_get_time();

        display_render_total_us += render_end - render_start;
        display_cache_total_us += cache_end - render_end;
        display_dirty_total += dirty_rows;
        display_profile_frames++;
        if (display_profile_frames >= DISPLAY_PROFILE_INTERVAL) {
            ESP_LOGI("DISPLAY", "dirty avg: %d rows, render avg: %lld us, cache_sync avg: %lld us, total: %lld us",
                     display_dirty_total / display_profile_frames,
                     display_render_total_us / display_profile_frames,
                     display_cache_total_us / display_profile_frames,
                     (display_render_total_us + display_cache_total_us) / display_profile_frames);
            display_render_total_us = 0;
            display_cache_total_us = 0;
            display_dirty_total = 0;
            display_profile_frames = 0;
        }
#else
        const int64_t render_start = esp_timer_get_time();
        int dirty_rows = render_dirty_to_spi_panel();
        if (dirty_rows < 0) {
            // SPI strip-based fallback.
            for (int x = 0; x < LCD_WIDTH; x += RGB_BUF_LINES) {
                int lines = (x + RGB_BUF_LINES > LCD_WIDTH) ? (LCD_WIDTH - x) : RGB_BUF_LINES;

                for (int line = 0; line < lines; line++) {
                    int lcd_x = x + line;
                    for (int lcd_y = 0; lcd_y < LCD_HEIGHT; lcd_y++) {
                        rgb_buf[lcd_y * lines + line] = lcd_pixel_to_rgb565(lcd_x, lcd_y);
                    }
                }

                lcd_draw_bitmap(x, 0, lines, LCD_HEIGHT, (const uint8_t *)rgb_buf);
                lcd_wait_trans_complete();
            }
            dirty_rows = DISP_HEIGHT;
        }
        const int64_t render_end = esp_timer_get_time();

        display_render_total_us += render_end - render_start;
        display_dirty_total += dirty_rows;
        display_profile_frames++;
        if (display_profile_frames >= DISPLAY_PROFILE_INTERVAL) {
            ESP_LOGI("DISPLAY", "dirty avg: %d rows, render avg: %lld us",
                     display_dirty_total / display_profile_frames,
                     display_render_total_us / display_profile_frames);
            display_render_total_us = 0;
            display_dirty_total = 0;
            display_profile_frames = 0;
        }
#endif
    }
}

void display_task_start(int core, int priority) {
    if (display_task_handle != NULL) {
        ESP_LOGW(TAG, "Display task already running");
        return;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(display_task, "display_task", 4096, NULL, priority,
                                             &display_task_handle, core);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
    } else {
        ESP_LOGI(TAG, "Display task created on Core %d with priority %d", core, priority);
    }
}
