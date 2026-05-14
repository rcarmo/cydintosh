#include "touch_cyd.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "hw.h"

#if defined(TOUCH_CONTROLLER_XPT2046)
#include "esp_lcd_touch_xpt2046.h"
#include "driver/spi_master.h"
#endif

#if defined(TOUCH_CONTROLLER_GT911)
#include "driver/i2c.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "touch";

#if defined(TOUCH_CONTROLLER_XPT2046)
static esp_lcd_touch_handle_t touch_handle = NULL;
#endif

#if defined(TOUCH_CONTROLLER_GT911)
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINTS 0x814F
#define GT911_STATUS_BUFFER_READY 0x80
#define GT911_STATUS_POINT_MASK 0x0F

static uint8_t gt911_addr = 0;
#endif

static touch_event_t current_event = {0};
static touch_event_t last_event = {0};
static uint8_t has_pending_event = 0;

#if TOUCH_FILTER_SHIFT > 0
static int32_t touch_filter_x = 0;
static int32_t touch_filter_y = 0;
#endif

static QueueHandle_t mouse_queue = NULL;
static TaskHandle_t touch_task_handle = NULL;

// Double-tap state machine
typedef enum {
    TAP_STATE_IDLE,
    TAP_STATE_FIRST_DOWN,
    TAP_STATE_FIRST_UP,
    TAP_STATE_CLICK_HOLD,
    TAP_STATE_DOUBLE_DOWN,
    TAP_STATE_DOUBLE_DRAGGING,
    TAP_STATE_DRAG_RELEASE_PENDING
} tap_state_t;

static tap_state_t tap_state = TAP_STATE_IDLE;
static uint32_t first_tap_start_time = 0;   // When first tap started
static uint32_t first_tap_release_time = 0; // When first tap released
static int16_t first_tap_x = 0, first_tap_y = 0;
static int16_t double_tap_x = 0, double_tap_y = 0;
static uint8_t first_tap_moved = 0; // Flag: significant movement during first tap
static uint32_t drag_release_start_time = 0;

static uint32_t get_tick_ms(void) {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

#if defined(TOUCH_CONTROLLER_GT911)
static esp_err_t gt911_write(uint16_t reg, const uint8_t *data, size_t len) {
    uint8_t buf[2 + len];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xff);
    if (len > 0 && data != NULL) {
        memcpy(&buf[2], data, len);
    }
    return i2c_master_write_to_device((i2c_port_t)TOUCH_I2C_PORT, gt911_addr, buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
}

static esp_err_t gt911_read_from(uint8_t addr, uint16_t reg, uint8_t *data, size_t len) {
    uint8_t reg_buf[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xff)};
    return i2c_master_write_read_device((i2c_port_t)TOUCH_I2C_PORT, addr, reg_buf, sizeof(reg_buf),
                                        data, len, pdMS_TO_TICKS(100));
}

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len) {
    return gt911_read_from(gt911_addr, reg, data, len);
}

static esp_err_t gt911_clear_status(void) {
    uint8_t zero = 0;
    return gt911_write(GT911_REG_STATUS, &zero, 1);
}

static esp_err_t gt911_probe(void) {
    const uint8_t candidates[] = {TOUCH_GT911_ADDR1, TOUCH_GT911_ADDR2};
    uint8_t id[4] = {0};

    for (size_t i = 0; i < sizeof(candidates); i++) {
        esp_err_t ret = gt911_read_from(candidates[i], GT911_REG_PRODUCT_ID, id, sizeof(id));
        if (ret == ESP_OK) {
            gt911_addr = candidates[i];
            ESP_LOGI(TAG, "GT911 found at 0x%02x, product id: %c%c%c%c", gt911_addr, id[0], id[1],
                     id[2], id[3]);
            gt911_clear_status();
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}
#endif

void touch_init(void) {
#if defined(TOUCH_CONTROLLER_XPT2046)
    ESP_LOGI(TAG, "Initializing XPT2046 touch controller on VSPI");

    spi_bus_config_t buscfg = {
        .mosi_io_num = TOUCH_MOSI,
        .miso_io_num = TOUCH_MISO,
        .sclk_io_num = TOUCH_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize VSPI bus: %s", esp_err_to_name(ret));
        return;
    }

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_config = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(TOUCH_CS);
    ret =
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return;
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = SCREEN_WIDTH,
        .y_max = SCREEN_HEIGHT,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags =
            {
                .swap_xy = TOUCH_SWAP_XY,
                .mirror_x = TOUCH_MIRROR_X,
                .mirror_y = TOUCH_MIRROR_Y,
            },
    };

    ESP_LOGI(TAG, "Initialize touch controller XPT2046");
    ret = esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create touch handle: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Touch controller initialized");
#elif defined(TOUCH_CONTROLLER_GT911)
    ESP_LOGI(TAG, "Initializing GT911 touch controller on I2C SDA=%d SCL=%d", TOUCH_I2C_SDA,
             TOUCH_I2C_SCL);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config((i2c_port_t)TOUCH_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2C: %s", esp_err_to_name(ret));
        return;
    }

    ret = i2c_driver_install((i2c_port_t)TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install I2C driver: %s", esp_err_to_name(ret));
        return;
    }

    ret = gt911_probe();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 probe failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Touch controller initialized");
#else
    ESP_LOGW(TAG, "No touch controller selected");
#endif
}

int touch_update(void) {
#if defined(TOUCH_CONTROLLER_XPT2046)
    if (touch_handle == NULL) {
        return 0;
    }

    esp_err_t ret = esp_lcd_touch_read_data(touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Touch read failed: %s", esp_err_to_name(ret));
        return 0;
    }

    esp_lcd_touch_point_data_t touch_data[1];
    uint8_t count = 0;

    esp_err_t ret_get = esp_lcd_touch_get_data(touch_handle, touch_data, &count, 1);

    if (ret_get == ESP_OK && count > 0) {
        int16_t raw_x = touch_data[0].x;
        int16_t raw_y = touch_data[0].y;
#if TOUCH_FILTER_SHIFT > 0
        if (!last_event.pressed) {
            touch_filter_x = ((int32_t)raw_x) << TOUCH_FILTER_SHIFT;
            touch_filter_y = ((int32_t)raw_y) << TOUCH_FILTER_SHIFT;
        } else {
            int shift_x = TOUCH_FILTER_SHIFT;
            int shift_y = TOUCH_FILTER_SHIFT;
#if TOUCH_ADAPTIVE_FILTER
            int16_t filtered_x = (touch_filter_x + (1 << (TOUCH_FILTER_SHIFT - 1))) >> TOUCH_FILTER_SHIFT;
            int16_t filtered_y = (touch_filter_y + (1 << (TOUCH_FILTER_SHIFT - 1))) >> TOUCH_FILTER_SHIFT;
            if (abs(raw_x - filtered_x) >= TOUCH_FILTER_FAST_THRESHOLD_PX) shift_x = 1;
            if (abs(raw_y - filtered_y) >= TOUCH_FILTER_FAST_THRESHOLD_PX) shift_y = 1;
#endif
            touch_filter_x += (((int32_t)raw_x << TOUCH_FILTER_SHIFT) - touch_filter_x) >> shift_x;
            touch_filter_y += (((int32_t)raw_y << TOUCH_FILTER_SHIFT) - touch_filter_y) >> shift_y;
        }
        current_event.x = (touch_filter_x + (1 << (TOUCH_FILTER_SHIFT - 1))) >> TOUCH_FILTER_SHIFT;
        current_event.y = (touch_filter_y + (1 << (TOUCH_FILTER_SHIFT - 1))) >> TOUCH_FILTER_SHIFT;
#else
        current_event.x = raw_x;
        current_event.y = raw_y;
#endif
#elif defined(TOUCH_CONTROLLER_GT911)
    if (gt911_addr == 0) {
        return 0;
    }

    uint8_t status = 0;
    esp_err_t ret = gt911_read(GT911_REG_STATUS, &status, 1);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "GT911 status read failed: %s", esp_err_to_name(ret));
        return 0;
    }

    if ((status & GT911_STATUS_BUFFER_READY) && (status & GT911_STATUS_POINT_MASK)) {
        uint8_t point[8] = {0};
        ret = gt911_read(GT911_REG_POINTS, point, sizeof(point));
        gt911_clear_status();
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "GT911 point read failed: %s", esp_err_to_name(ret));
            return 0;
        }

        current_event.x = point[1] | (point[2] << 8);
        current_event.y = point[3] | (point[4] << 8);

        if (TOUCH_MIRROR_X) {
            current_event.x = SCREEN_WIDTH - 1 - current_event.x;
        }
        if (TOUCH_MIRROR_Y) {
            current_event.y = SCREEN_HEIGHT - 1 - current_event.y;
        }
        if (TOUCH_SWAP_XY) {
            int16_t tmp = current_event.x;
            current_event.x = current_event.y;
            current_event.y = tmp;
        }
#else
    if (0) {
#endif
        if (last_event.pressed) {
            current_event.dx = current_event.x - last_event.x;
            current_event.dy = current_event.y - last_event.y;

            if (abs(current_event.dx) < DEADZONE_PX)
                current_event.dx = 0;
            if (abs(current_event.dy) < DEADZONE_PX)
                current_event.dy = 0;
            current_event.clicked = 0;
        } else {
            current_event.dx = 0;
            current_event.dy = 0;
            current_event.clicked = 1;
        }

        current_event.pressed = 1;
        has_pending_event = 1;
        last_event = current_event;

        ESP_LOGD(TAG, "Touch: x=%d y=%d", current_event.x, current_event.y);

        return 1;
    } else {
#if defined(TOUCH_CONTROLLER_GT911)
        if (status & GT911_STATUS_BUFFER_READY) {
            gt911_clear_status();
        }
#endif
        if (last_event.pressed) {
            current_event.pressed = 0;
            current_event.dx = 0;
            current_event.dy = 0;
            current_event.clicked = 0;
            has_pending_event = 1;
            last_event.pressed = 0;
            return 1;
        }
    }

    return 0;
}

const touch_event_t *touch_get_event(void) {
    return &current_event;
}

void touch_create_queue(void) {
    if (mouse_queue == NULL) {
        mouse_queue = xQueueCreate(8, sizeof(mouse_delta_t));
        if (mouse_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create mouse queue");
        } else {
            ESP_LOGI(TAG, "Mouse queue created");
        }
    }
}

QueueHandle_t touch_get_mouse_queue(void) {
    return mouse_queue;
}

static int16_t scale_mouse_delta_axis(int16_t delta, int32_t *gain_remainder) {
#if TOUCH_RAW_DEADZONE_PX > 0
    if (abs(delta) <= TOUCH_RAW_DEADZONE_PX) {
        return 0;
    }
#endif
#if TOUCH_DELTA_SCALE > 1
    delta /= TOUCH_DELTA_SCALE;
#endif
#if TOUCH_DELTA_GAIN_NUM != TOUCH_DELTA_GAIN_DEN
    // Keep fractional gain across samples instead of rounding every tiny jitter
    // sample upward. This preserves average speed without turning 1px noise into
    // repeated 2px cursor movement.
    int32_t scaled = (int32_t)delta * TOUCH_DELTA_GAIN_NUM + *gain_remainder;
    int16_t out = (int16_t)(scaled / TOUCH_DELTA_GAIN_DEN);
    *gain_remainder = scaled - ((int32_t)out * TOUCH_DELTA_GAIN_DEN);
    delta = out;
#endif
#if TOUCH_DELTA_SOFT_CURVE
    int sign = (delta < 0) ? -1 : 1;
    int mag = abs(delta);
    if (mag <= 6) {
        // Fine motion: slightly stronger than the geometric 8/5 scale so
        // one-pixel touch movement survives the deadzone and tracks the finger.
        mag = (mag * 5 + 2) / 4;
    } else if (mag <= 16) {
        // Medium motion: modest boost, but less than the rejected +30% linear gain.
        mag = (mag * 9 + 4) / 8;
    } else {
        // Fast swipes: compress so the Mac's own acceleration does not overshoot.
        mag = 18 + ((mag - 16) / 2);
    }
    delta = (int16_t)(sign * mag);
#endif
    return delta;
}

static void transform_mouse_delta(int16_t *dx, int16_t *dy) {
    static int32_t gain_remainder_x = 0;
    static int32_t gain_remainder_y = 0;
    *dx = scale_mouse_delta_axis(*dx, &gain_remainder_x);
    *dy = scale_mouse_delta_axis(*dy, &gain_remainder_y);
#if TOUCH_DELTA_ROTATE_CW
    int16_t old_dx = *dx;
    *dx = *dy;
    *dy = -old_dx;
#endif
#if TOUCH_DELTA_INVERT_X
    *dx = -*dx;
#endif
#if TOUCH_DELTA_INVERT_Y
    *dy = -*dy;
#endif
}

static void touch_task(void *arg) {
    ESP_LOGI(TAG, "Touch task started on Core %d", xPortGetCoreID());

    uint8_t last_pressed = 0;
    int16_t last_x = 0, last_y = 0;

    while (1) {
        if (touch_update()) {
            const touch_event_t *ev = touch_get_event();
            uint32_t now = get_tick_ms();

            int16_t dx = 0, dy = 0;
            if (ev->pressed && last_pressed) {
                dx = ev->x - last_x;
                dy = ev->y - last_y;
                transform_mouse_delta(&dx, &dy);
                if (abs(dx) < DEADZONE_PX) dx = 0;
                if (abs(dy) < DEADZONE_PX) dy = 0;
            }
            if (ev->pressed) {
                last_x = ev->x;
                last_y = ev->y;
            }

            // State machine for double-tap detection
            mouse_delta_t mouse = {.dx = 0, .dy = 0, .pressed = 0};

            uint8_t send_event = 0;

            switch (tap_state) {
            case TAP_STATE_IDLE:
                if (ev->pressed && !last_pressed) {
                    // First touch detected - record start position and time
                    ESP_LOGI(TAG, "TAP: IDLE -> FIRST_DOWN (x=%d, y=%d)", ev->x, ev->y);
                    tap_state = TAP_STATE_FIRST_DOWN;
                    first_tap_start_time = now;
                    first_tap_x = ev->x;
                    first_tap_y = ev->y;
                    first_tap_moved = 0; // Reset movement flag
                }
                break;

            case TAP_STATE_FIRST_DOWN:
                if (!ev->pressed && last_pressed) {
                    // Released first touch - decide what happened
                    uint32_t tap_duration = now - first_tap_start_time;
                    ESP_LOGI(TAG, "TAP: FIRST_DOWN -> release, duration=%dms, moved=%d",
                             tap_duration, first_tap_moved);

                    if (first_tap_moved) {
                        // Was cursor movement - no action needed
                        ESP_LOGI(TAG, "TAP: Was cursor movement, no action");
                        tap_state = TAP_STATE_IDLE;
                    } else if (tap_duration < TAP_MIN_DURATION_MS) {
                        // Too short - ignore (accidental touch)
                        ESP_LOGI(TAG, "TAP: Too short (< %dms), ignoring", TAP_MIN_DURATION_MS);
                        tap_state = TAP_STATE_IDLE;
                    } else if (tap_duration > TAP_MAX_DURATION_MS) {
                        // Long press without movement - ignore
                        ESP_LOGI(TAG, "TAP: Too long (> %dms), ignoring", TAP_MAX_DURATION_MS);
                        tap_state = TAP_STATE_IDLE;
                    } else {
                        // Valid tap - wait for potential double-tap
                        ESP_LOGI(TAG, "TAP: Valid first tap, waiting for double...");
                        tap_state = TAP_STATE_FIRST_UP;
                        first_tap_release_time = now;
                    }
                    first_tap_moved = 0; // Reset for next time
                } else if (ev->pressed && last_pressed) {
                    // Continued touch - check for movement
                    // Calculate distance from initial touch position
                    int32_t dist_x = ev->x - first_tap_x;
                    int32_t dist_y = ev->y - first_tap_y;
                    int32_t dist_sq = dist_x * dist_x + dist_y * dist_y;

                    if (dist_sq > MOVEMENT_THRESHOLD_SQ) {
                        // Significant movement detected - this is cursor movement.
                        // The threshold prevents taps/jitter becoming cursor motion, but
                        // once motion is confirmed, emit the accumulated travel from the
                        // initial touch so the cursor does not lag behind the stylus.
                        uint8_t was_moved = first_tap_moved;
                        first_tap_moved = 1; // Mark as moved

                        // Send cursor movement (no button press)
                        mouse.pressed = 0;
                        if (!was_moved) {
                            mouse.dx = ev->x - first_tap_x;
                            mouse.dy = ev->y - first_tap_y;
                            transform_mouse_delta(&mouse.dx, &mouse.dy);
                        } else {
                            mouse.dx = dx;
                            mouse.dy = dy;
                        }
                        send_event = 1;
                        ESP_LOGD(TAG, "TAP: Cursor movement (dist=%d, dx=%d, dy=%d)",
                                 (int)sqrt(dist_sq), mouse.dx, mouse.dy);
                    }
                }
                break;

            case TAP_STATE_FIRST_UP: {
                uint32_t release_duration = now - first_tap_release_time;
                if (release_duration > DOUBLE_TAP_WINDOW_MS) {
                    // Time window expired - single tap confirmed. Emit a real click:
                    // mouse down now, release later in CLICK_HOLD so classic Mac code sees it.
                    ESP_LOGI(TAG, "TAP: FIRST_UP timeout (%dms > %dms), single tap click DOWN",
                             release_duration, DOUBLE_TAP_WINDOW_MS);
                    tap_state = TAP_STATE_CLICK_HOLD;
                    first_tap_release_time = now;
                    mouse.pressed = 1;
                    send_event = 1;
                } else if (ev->pressed && !last_pressed) {
                    // Second touch within window - check timing
                    ESP_LOGI(TAG, "TAP: Second touch detected, release_duration=%dms (limit=%dms)",
                             release_duration, DOUBLE_TAP_RELEASE_WINDOW_MS);
                    if (release_duration <= DOUBLE_TAP_RELEASE_WINDOW_MS) {
                        // Quick second touch - DOUBLE TAP! Mouse button DOWN
                        ESP_LOGI(TAG, "TAP: **** DOUBLE TAP DETECTED! Mouse DOWN ****");
                        tap_state = TAP_STATE_DOUBLE_DOWN;
                        double_tap_x = ev->x;
                        double_tap_y = ev->y;
                        mouse.pressed = 1; // Mouse button DOWN
                        send_event = 1;
                    } else {
                        // Too slow - treat as new tap
                        ESP_LOGI(TAG, "TAP: Too slow for double tap, treating as new tap");
                        tap_state = TAP_STATE_FIRST_DOWN;
                        first_tap_start_time = now;
                        first_tap_x = ev->x;
                        first_tap_y = ev->y;
                        first_tap_moved = 0;
                    }
                } else if (!ev->pressed && last_pressed) {
                    // Released during "too slow" second touch - reset
                    ESP_LOGI(TAG, "TAP: FIRST_UP -> release (too slow), resetting");
                    tap_state = TAP_STATE_IDLE;
                } else if (ev->pressed && last_pressed) {
                    // Holding during "too slow" second touch - treat as new tap
                    ESP_LOGI(TAG, "TAP: FIRST_UP -> holding, treating as new tap");
                    tap_state = TAP_STATE_FIRST_DOWN;
                    first_tap_start_time = now;
                    first_tap_x = ev->x;
                    first_tap_y = ev->y;
                    first_tap_moved = 0;
                }
            } break;

            case TAP_STATE_CLICK_HOLD: {
                uint32_t hold_duration = now - first_tap_release_time;
                if (hold_duration >= TAP_CLICK_HOLD_MS) {
                    ESP_LOGI(TAG, "TAP: CLICK_HOLD timeout (%dms), mouse UP", hold_duration);
                    tap_state = TAP_STATE_IDLE;
                    mouse.pressed = 0;
                    send_event = 1;
                }
            } break;

            case TAP_STATE_DOUBLE_DOWN:
                if (!ev->pressed && last_pressed) {
                    // Quick release after double-tap - click, but hold down long enough
                    // for classic Mac apps/Finder to see a stable button press.
                    ESP_LOGI(TAG, "TAP: DOUBLE_DOWN -> release, entering CLICK_HOLD");
                    tap_state = TAP_STATE_CLICK_HOLD;
                    first_tap_release_time = now;
                } else if (ev->pressed && last_pressed) {
                    // Holding after double-tap - start drag
                    ESP_LOGI(TAG, "TAP: DOUBLE_DOWN -> DRAGGING");
                    tap_state = TAP_STATE_DOUBLE_DRAGGING;
                    mouse.pressed = 1; // Keep button DOWN
                    mouse.dx = dx;
                    mouse.dy = dy;
                    send_event = 1;
                }
                break;

            case TAP_STATE_DOUBLE_DRAGGING:
                if (!ev->pressed && last_pressed) {
                    // Possible release after drag. Do not release immediately: GT911 can
                    // briefly drop contact during long drags/minor slips.
                    ESP_LOGI(TAG, "TAP: DRAGGING -> release pending");
                    tap_state = TAP_STATE_DRAG_RELEASE_PENDING;
                    drag_release_start_time = now;
                    mouse.pressed = 1; // Keep button DOWN during grace window
                    mouse.dx = 0;
                    mouse.dy = 0;
                    send_event = 1;
                } else if (ev->pressed && last_pressed) {
                    // Continue dragging - send movement with button held
                    mouse.pressed = 1; // Keep button DOWN
                    mouse.dx = dx;
                    mouse.dy = dy;
                    send_event = 1;
                }
                break;

            case TAP_STATE_DRAG_RELEASE_PENDING:
                if (ev->pressed && !last_pressed) {
                    // Contact resumed inside grace window: continue drag without a release.
                    ESP_LOGI(TAG, "TAP: release pending -> DRAGGING resumed");
                    tap_state = TAP_STATE_DOUBLE_DRAGGING;
                    mouse.pressed = 1;
                    send_event = 1;
                }
                break;
            }

            if (send_event) {
                xQueueSend(mouse_queue, &mouse, 0);
            }

            last_pressed = ev->pressed;
        } else {
            // Check timeouts when no touch event arrives.
            uint32_t now = get_tick_ms();
            if (tap_state == TAP_STATE_FIRST_UP) {
                uint32_t release_duration = now - first_tap_release_time;
                if (release_duration > DOUBLE_TAP_WINDOW_MS) {
                    ESP_LOGI(TAG, "TAP: FIRST_UP timeout (%dms > %dms), single tap click DOWN",
                             release_duration, DOUBLE_TAP_WINDOW_MS);
                    tap_state = TAP_STATE_CLICK_HOLD;
                    first_tap_release_time = now;
                    mouse_delta_t mouse = {.dx = 0, .dy = 0, .pressed = 1};
                    xQueueSend(mouse_queue, &mouse, 0);
                }
            } else if (tap_state == TAP_STATE_CLICK_HOLD) {
                uint32_t hold_duration = now - first_tap_release_time;
                if (hold_duration >= TAP_CLICK_HOLD_MS) {
                    ESP_LOGI(TAG, "TAP: CLICK_HOLD timeout (%dms), mouse UP", hold_duration);
                    tap_state = TAP_STATE_IDLE;
                    mouse_delta_t mouse = {.dx = 0, .dy = 0, .pressed = 0};
                    xQueueSend(mouse_queue, &mouse, 0);
                }
            } else if (tap_state == TAP_STATE_DRAG_RELEASE_PENDING) {
                uint32_t release_duration = now - drag_release_start_time;
                if (release_duration >= DRAG_RELEASE_GRACE_MS) {
                    ESP_LOGI(TAG, "TAP: release grace timeout (%dms), mouse UP", release_duration);
                    tap_state = TAP_STATE_IDLE;
                    mouse_delta_t mouse = {.dx = 0, .dy = 0, .pressed = 0};
                    xQueueSend(mouse_queue, &mouse, 0);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

void touch_start_task(int core, int priority) {
    if (mouse_queue == NULL) {
        ESP_LOGE(TAG, "Mouse queue not created. Call touch_create_queue() first.");
        return;
    }

    if (touch_task_handle != NULL) {
        ESP_LOGW(TAG, "Touch task already running");
        return;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(touch_task, "touch_task", 4096, NULL, priority,
                                             &touch_task_handle, core);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
    } else {
        ESP_LOGI(TAG, "Touch task created on Core %d", core);
    }
}
