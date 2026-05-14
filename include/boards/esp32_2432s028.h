#ifndef BOARD_ESP32_2432S028_H
#define BOARD_ESP32_2432S028_H

// ESP32-2432S028 / CYD2USB: ESP32, 240x320 ILI9341 SPI LCD, XPT2046 touch.
#define BOARD_NAME "ESP32-2432S028 CYD2USB"

#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_TOUCH 1
#define TOUCH_CONTROLLER_XPT2046 1
#define LCD_PANEL_ILI9341_SPI 1

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define LCD_WIDTH SCREEN_WIDTH
#define LCD_HEIGHT SCREEN_HEIGHT
#define LCD_RENDER_SCALE 1
#define LCD_RENDER_ROTATE_CW 0
#define LCD_RENDER_FLIP_X 0
#define LCD_RENDER_FLIP_Y 0
#define LCD_RENDER_INVERT_MONO 0
#define LCD_RENDER_OFFSET_X 0
#define LCD_RENDER_OFFSET_Y 0
#ifndef LCD_RENDER_FIT_TO_PANEL
#define LCD_RENDER_FIT_TO_PANEL 0
#endif
#define LCD_TRANSFER_STRIP_WIDTH 4
#define LCD_TRANSFER_BUFFER_CAPS MALLOC_CAP_8BIT

// RGB LED pins are active low on CYD.
#define LED_R_PIN 4
#define LED_G_PIN 16
#define LED_B_PIN 17
#define GPIO_LED_PIN LED_G_PIN

#define UMAC_TASK_STACK_SIZE 32768

// TFT LCD (ILI9341) - SPI.
#define TFT_SPI_MOSI 13
#define TFT_SPI_CLK 14
#define TFT_SPI_CS 15
#define TFT_SPI_MISO 12
#define TFT_DC 2
#define TFT_RESET GPIO_UNUSED
#define TFT_BL 21

// Touch Screen (XPT2046) - separate SPI pins.
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 1
#define TOUCH_MIRROR_Y 1
#define TOUCH_DELTA_ROTATE_CW 0
#define TOUCH_DELTA_SCALE 1
#define MOUSE_DELTA_CAP 18
#define TOUCH_FILTER_SHIFT 1    // IIR low-pass: 1/2 new sample, 1/2 previous
#define DEADZONE_PX 3           // applied after low-pass filtering; suppress resistive jitter
#define MOVEMENT_THRESHOLD_PX 8 // tap-vs-motion threshold; first motion sends catch-up delta

// Touch calibration values (from CYD reference project).
#define TOUCH_MIN_X 200
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 250
#define TOUCH_MAX_Y 3850

#endif
