#ifndef BOARD_PROFILES_H
#define BOARD_PROFILES_H

// Board selection. PlatformIO environments define exactly one of these.
#if !defined(CYD_BOARD_ESP32_2432S028) && !defined(CYD_BOARD_ESP32_8048S043C)
#define CYD_BOARD_ESP32_2432S028 1
#endif

// Emulated Macintosh framebuffer dimensions. These must match the patched ROM.
#ifndef DISP_WIDTH
#define DISP_WIDTH 240
#endif
#ifndef DISP_HEIGHT
#define DISP_HEIGHT 320
#endif

#define GPIO_UNUSED -1

#if defined(CYD_BOARD_ESP32_8048S043C)
#include "boards/esp32_8048s043c.h"
#elif defined(CYD_BOARD_ESP32_2432S028)
#include "boards/esp32_2432s028.h"
#else
#error "Unsupported Cydintosh board profile"
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

#if LCD_RENDER_SCALE < 1
#error "LCD_RENDER_SCALE must be >= 1"
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
