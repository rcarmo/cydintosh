#include "display.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lcd_cyd.h"
#include "hw.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "display";

static uint16_t *rgb_buf = NULL; // Single RGB buffer for narrow vertical-strip transfer
static uint8_t *fb_copy = NULL;  // Framebuffer copy for safe transfer

#define RGB_BUF_LINES 4
#define FB_COPY_SIZE (DISP_WIDTH * DISP_HEIGHT / 8)

static TaskHandle_t display_task_handle = NULL;
static const uint8_t *current_framebuffer = NULL;
static volatile bool frame_pending = false;

static inline uint16_t mac_pixel_to_rgb565(int mac_col, int mac_row) {
    if (mac_col < 0 || mac_col >= DISP_WIDTH || mac_row < 0 || mac_row >= DISP_HEIGHT) {
        return 0x0000;
    }

    const int mac_bytes_per_row = DISP_WIDTH / 8;
    int byte_offset = mac_row * mac_bytes_per_row + (mac_col / 8);
    int bit_offset = 7 - (mac_col % 8);
    uint8_t pixel = (fb_copy[byte_offset] >> bit_offset) & 1;
    return pixel ? 0xFFFF : 0x0000;
}

static inline uint16_t lcd_pixel_to_rgb565(int lcd_x, int lcd_y) {
    int local_x = lcd_x - LCD_RENDER_OFFSET_X;
    int local_y = lcd_y - LCD_RENDER_OFFSET_Y;

#if LCD_RENDER_ROTATE_CW
    const int render_width = DISP_HEIGHT * LCD_RENDER_SCALE;
    const int render_height = DISP_WIDTH * LCD_RENDER_SCALE;
#else
    const int render_width = DISP_WIDTH * LCD_RENDER_SCALE;
    const int render_height = DISP_HEIGHT * LCD_RENDER_SCALE;
#endif

    if (local_x < 0 || local_y < 0 || local_x >= render_width || local_y >= render_height) {
        return 0x0000;
    }

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

void display_init(void) {
    // Allocate RGB buffer for batch vertical line transfer.
    rgb_buf = heap_caps_malloc(LCD_HEIGHT * RGB_BUF_LINES * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (rgb_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer");
        return;
    }
    ESP_LOGI(TAG, "Allocated RGB buffer: %d bytes for %d lines on %dx%d panel",
             LCD_HEIGHT * RGB_BUF_LINES * 2, RGB_BUF_LINES, LCD_WIDTH, LCD_HEIGHT);

    fb_copy = heap_caps_malloc(FB_COPY_SIZE, MALLOC_CAP_8BIT);
    if (fb_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer copy");
        return;
    }
    ESP_LOGI(TAG, "Allocated framebuffer copy: %d bytes (%dx%d Mac FB)", FB_COPY_SIZE,
             DISP_WIDTH, DISP_HEIGHT);
}

void display_notify_update(void) {
    if (display_task_handle) {
        frame_pending = true;
        xTaskNotifyGive(display_task_handle);
    }
}

static void display_task(void *arg) {
    ESP_LOGI(TAG, "Display task started on Core %d", xPortGetCoreID());

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (frame_pending && current_framebuffer) {
            // Copy framebuffer at the start of transfer. This copy is held until
            // the full LCD update completes, so umac can continue writing.
            memcpy(fb_copy, current_framebuffer, FB_COPY_SIZE);
            frame_pending = false;

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
        }
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

void display_set_framebuffer(const uint8_t *fb) {
    current_framebuffer = fb;
}
