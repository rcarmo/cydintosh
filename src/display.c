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
static inline uint16_t mac_pixel_to_rgb565(int mac_col, int mac_row) {
    const int mac_bytes_per_row = DISP_WIDTH / 8;
    int byte_offset = mac_row * mac_bytes_per_row + (mac_col / 8);
    int bit_offset  = 7 - (mac_col % 8);
    uint8_t pixel   = (fb_copy[byte_offset] >> bit_offset) & 1;
#if LCD_RENDER_INVERT_MONO
    pixel ^= 1;
#endif
    return pixel ? 0xFFFF : 0x0000;
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

static int render_dirty_to_rgb_framebuffer(void) {
#if LCD_RENDER_ROTATE_CW && LCD_RENDER_SCALE == 2
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

#if defined(LCD_PANEL_RGB)
    fb_prev = heap_caps_malloc(FB_COPY_SIZE, MALLOC_CAP_8BIT);
    if (fb_prev == NULL) {
        ESP_LOGE(TAG, "Failed to allocate previous framebuffer copy");
        return;
    }
    memset(fb_prev, 0xFF, FB_COPY_SIZE); // force first frame to redraw all rows
    ESP_LOGI(TAG, "Allocated previous framebuffer copy: %d bytes", FB_COPY_SIZE);
#endif
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
        // SPI strip-based rendering (ILI9341 / CYD2USB path — unchanged).
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
