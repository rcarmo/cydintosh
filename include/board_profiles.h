#ifndef BOARD_PROFILES_H
#define BOARD_PROFILES_H

// Board selection. PlatformIO environments define exactly one of these.
#if !defined(CYD_BOARD_ESP32_2432S028) && \
    !defined(CYD_BOARD_ESP32_2432S028_MAC512X384_ROTFIT) && \
    !defined(CYD_BOARD_ESP32_8048S043C)
#define CYD_BOARD_ESP32_2432S028 1
#endif

#define GPIO_UNUSED -1

// Include board-specific header first; boards may override DISP_WIDTH/DISP_HEIGHT.
#if defined(CYD_BOARD_ESP32_8048S043C)
#include "boards/esp32_8048s043c.h"
#elif defined(CYD_BOARD_ESP32_2432S028_MAC512X384_ROTFIT)
#include "boards/esp32_2432s028_mac512x384_rotfit.h"
#elif defined(CYD_BOARD_ESP32_2432S028)
#include "boards/esp32_2432s028.h"
#else
#error "Unsupported Cydintosh board profile"
#endif

// Emulated Macintosh framebuffer dimensions.
// Boards may define these to override the defaults below.
#ifndef DISP_WIDTH
#define DISP_WIDTH 240
#endif
#ifndef DISP_HEIGHT
#define DISP_HEIGHT 320
#endif

#ifndef BOARD_NAME
#error "Board profile must define BOARD_NAME"
#endif
#ifndef LCD_WIDTH
#error "Board profile must define LCD_WIDTH"
#endif
#ifndef LCD_HEIGHT
#error "Board profile must define LCD_HEIGHT"
#endif
#ifndef LCD_RENDER_SCALE
#error "Board profile must define LCD_RENDER_SCALE"
#endif
#ifndef TFT_BL
#error "Board profile must define TFT_BL (or GPIO_UNUSED)"
#endif
#ifndef TOUCH_DELTA_SCALE
#error "Board profile must define TOUCH_DELTA_SCALE"
#endif
#ifndef UMAC_TASK_STACK_SIZE
#error "Board profile must define UMAC_TASK_STACK_SIZE"
#endif
#ifndef LCD_RENDER_FLIP_X
#error "Board profile must define LCD_RENDER_FLIP_X"
#endif
#ifndef LCD_RENDER_FLIP_Y
#error "Board profile must define LCD_RENDER_FLIP_Y"
#endif
#ifndef LCD_RENDER_INVERT_MONO
#error "Board profile must define LCD_RENDER_INVERT_MONO"
#endif
#ifndef LCD_TRANSFER_STRIP_WIDTH
#error "Board profile must define LCD_TRANSFER_STRIP_WIDTH"
#endif
#ifndef LCD_TRANSFER_BUFFER_CAPS
#error "Board profile must define LCD_TRANSFER_BUFFER_CAPS"
#endif
#ifndef MOUSE_DELTA_CAP
#error "Board profile must define MOUSE_DELTA_CAP"
#endif
#ifndef TOUCH_FILTER_SHIFT
#error "Board profile must define TOUCH_FILTER_SHIFT"
#endif
#ifndef LCD_RENDER_FIT_TO_PANEL
#define LCD_RENDER_FIT_TO_PANEL 0
#endif
#ifndef LCD_RENDER_FIT_GRAYSCALE
#define LCD_RENDER_FIT_GRAYSCALE 0
#endif
#ifndef LCD_PANEL_RGB565_BYTE_SWAP
#define LCD_PANEL_RGB565_BYTE_SWAP 0
#endif
#ifndef TOUCH_DELTA_INVERT_X
#define TOUCH_DELTA_INVERT_X 0
#endif
#ifndef TOUCH_DELTA_INVERT_Y
#define TOUCH_DELTA_INVERT_Y 0
#endif
#ifndef TOUCH_DELTA_GAIN_NUM
#define TOUCH_DELTA_GAIN_NUM 1
#endif
#ifndef TOUCH_DELTA_GAIN_DEN
#define TOUCH_DELTA_GAIN_DEN 1
#endif
#ifndef TOUCH_DELTA_SOFT_CURVE
#define TOUCH_DELTA_SOFT_CURVE 0
#endif
#ifndef TOUCH_RAW_DEADZONE_PX
#define TOUCH_RAW_DEADZONE_PX 0
#endif
#ifndef TOUCH_ADAPTIVE_FILTER
#define TOUCH_ADAPTIVE_FILTER 0
#endif
#ifndef TOUCH_FILTER_FAST_THRESHOLD_PX
#define TOUCH_FILTER_FAST_THRESHOLD_PX 6
#endif
#ifndef DISK_IMAGE_READ_ONLY
#define DISK_IMAGE_READ_ONLY 1
#endif

#if TOUCH_DELTA_GAIN_DEN < 1
#error "TOUCH_DELTA_GAIN_DEN must be >= 1"
#endif

#if LCD_RENDER_SCALE < 1
#error "LCD_RENDER_SCALE must be >= 1"
#endif
#if LCD_TRANSFER_STRIP_WIDTH < 1
#error "LCD_TRANSFER_STRIP_WIDTH must be >= 1"
#endif

#if (defined(LCD_PANEL_RGB) && defined(LCD_PANEL_ILI9341_SPI)) || \
    (!defined(LCD_PANEL_RGB) && !defined(LCD_PANEL_ILI9341_SPI))
#error "Board profile must select exactly one LCD panel backend"
#endif

#if (defined(TOUCH_CONTROLLER_GT911) && defined(TOUCH_CONTROLLER_XPT2046))
#error "Board profile must select at most one touch controller backend"
#endif

#if BOARD_HAS_TOUCH && !defined(TOUCH_CONTROLLER_GT911) && !defined(TOUCH_CONTROLLER_XPT2046)
#error "Board profile declares touch support but selects no touch controller backend"
#endif

#if !LCD_RENDER_FIT_TO_PANEL
#if LCD_RENDER_ROTATE_CW
#if (LCD_WIDTH < (DISP_HEIGHT * LCD_RENDER_SCALE)) || \
    (LCD_HEIGHT < (DISP_WIDTH * LCD_RENDER_SCALE))
#error "Rotated render area does not fit LCD dimensions"
#endif
#else
#if (LCD_WIDTH < (DISP_WIDTH * LCD_RENDER_SCALE)) || \
    (LCD_HEIGHT < (DISP_HEIGHT * LCD_RENDER_SCALE))
#error "Render area does not fit LCD dimensions"
#endif
#endif
#endif

// Boot button (GPIO0) - used as mouse click when available.
#define BOOT_BUTTON_PIN 0

// LEDC PWM configuration for RGB LED/backlight.
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL_R LEDC_CHANNEL_0
#define LEDC_CHANNEL_G LEDC_CHANNEL_1
#define LEDC_CHANNEL_B LEDC_CHANNEL_2
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY 5000

#endif
