#ifndef BOARD_ESP32_2432S028_MAC512X384_ROTFIT_H
#define BOARD_ESP32_2432S028_MAC512X384_ROTFIT_H

// ESP32-2432S028 / CYD2USB test profile:
// - physical panel: 240x320 ILI9341 SPI LCD + XPT2046 touch
// - emulated Mac framebuffer: 512x384 square-pixel test resolution
// - render: rotate clockwise, then grayscale box-filter downscale to 240x320 portrait LCD
#define BOARD_NAME "ESP32-2432S028 CYD2USB Mac512x384 rotated-fit"

#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_TOUCH 1
#define TOUCH_CONTROLLER_XPT2046 1
#define LCD_PANEL_ILI9341_SPI 1

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define LCD_WIDTH SCREEN_WIDTH
#define LCD_HEIGHT SCREEN_HEIGHT
#define LCD_RENDER_SCALE 1
#define LCD_RENDER_ROTATE_CW 1
#define LCD_RENDER_FLIP_X 0
#define LCD_RENDER_FLIP_Y 0
#define LCD_RENDER_INVERT_MONO 0
#define LCD_RENDER_OFFSET_X 0
#define LCD_RENDER_OFFSET_Y 0
#define LCD_RENDER_FIT_TO_PANEL 1
#define LCD_RENDER_FIT_GRAYSCALE 1
// The esp_lcd ILI9341 SPI path expects color bytes MSB-first on the wire.
// Black/white are byte-symmetric; grayscale is not, so pre-swap RGB565 words.
#define LCD_PANEL_RGB565_BYTE_SWAP 1
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
// Match the clockwise-rotated framebuffer: physical down -> Mac right,
// physical right -> Mac up.
#define TOUCH_DELTA_ROTATE_CW 1
#define TOUCH_DELTA_INVERT_X 1
#define TOUCH_DELTA_INVERT_Y 1
// The 512x384 Mac framebuffer is rotated to 384x512 and downsampled to
// 240x320, so one LCD/touch pixel represents 8/5 Mac pixels geometrically.
// Use a stable linear gain above the geometric value (19/10 = 1.9) with
// fractional accumulation in touch_cyd.c; avoid nonlinear boosts.
#define TOUCH_DELTA_GAIN_NUM 19
#define TOUCH_DELTA_GAIN_DEN 10
#define TOUCH_DELTA_SOFT_CURVE 0
#define TOUCH_DELTA_SCALE 1
#define MOUSE_DELTA_CAP 32
#define TOUCH_FILTER_SHIFT 2             // smooth tiny resistive jitter: 1/4 new, 3/4 previous
#define TOUCH_ADAPTIVE_FILTER 1          // bypass most smoothing during real finger movement
#define TOUCH_FILTER_FAST_THRESHOLD_PX 5 // >=5px raw movement uses fast tracking
#define TOUCH_RAW_DEADZONE_PX 1          // suppress only true 1px resistive jitter before gain
#define DEADZONE_PX 1                    // minimal post-scale deadzone; preserve slow finger tracking
#define MOVEMENT_THRESHOLD_PX 8          // tap-vs-motion threshold; first motion sends catch-up delta

// Touch calibration values (from CYD reference project).
#define TOUCH_MIN_X 200
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 250
#define TOUCH_MAX_Y 3850

#endif
