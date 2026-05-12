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

#define BOARD_NAME "ESP32-8048S043C"

// Sunton ESP32-8048S043C: ESP32-S3 N16R8, 800x480 RGB LCD, GT911 touch.
#define BOARD_HAS_RGB_LED 0
#define BOARD_HAS_TOUCH 1
#define TOUCH_CONTROLLER_GT911 1
#define LCD_PANEL_RGB 1

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480
#define LCD_WIDTH SCREEN_WIDTH
#define LCD_HEIGHT SCREEN_HEIGHT

// Render the 240x320 Mac framebuffer as 640x480, rotated clockwise and centered.
#define LCD_RENDER_SCALE 2
#define LCD_RENDER_ROTATE_CW 1
#define LCD_RENDER_OFFSET_X ((LCD_WIDTH - (DISP_HEIGHT * LCD_RENDER_SCALE)) / 2)
#define LCD_RENDER_OFFSET_Y ((LCD_HEIGHT - (DISP_WIDTH * LCD_RENDER_SCALE)) / 2)

// RGB LCD timing and pins from ESPHome's Sunton ESP32-8048S043C profile.
#define LCD_RGB_PCLK_HZ (16 * 1000 * 1000)
#define LCD_RGB_HSYNC_GPIO 39
#define LCD_RGB_VSYNC_GPIO 41
#define LCD_RGB_DE_GPIO 40
#define LCD_RGB_PCLK_GPIO 42
#define LCD_RGB_PCLK_ACTIVE_NEG 1
#define LCD_RGB_HSYNC_FRONT_PORCH 8
#define LCD_RGB_HSYNC_PULSE_WIDTH 4
#define LCD_RGB_HSYNC_BACK_PORCH 8
#define LCD_RGB_VSYNC_FRONT_PORCH 8
#define LCD_RGB_VSYNC_PULSE_WIDTH 4
#define LCD_RGB_VSYNC_BACK_PORCH 8

// RGB565 data bus order expected by esp_lcd RGB panel: B0..B4, G0..G5, R0..R4.
#define LCD_RGB_DATA0_GPIO 8
#define LCD_RGB_DATA1_GPIO 3
#define LCD_RGB_DATA2_GPIO 46
#define LCD_RGB_DATA3_GPIO 9
#define LCD_RGB_DATA4_GPIO 1
#define LCD_RGB_DATA5_GPIO 5
#define LCD_RGB_DATA6_GPIO 6
#define LCD_RGB_DATA7_GPIO 7
#define LCD_RGB_DATA8_GPIO 15
#define LCD_RGB_DATA9_GPIO 16
#define LCD_RGB_DATA10_GPIO 4
#define LCD_RGB_DATA11_GPIO 45
#define LCD_RGB_DATA12_GPIO 48
#define LCD_RGB_DATA13_GPIO 47
#define LCD_RGB_DATA14_GPIO 21
#define LCD_RGB_DATA15_GPIO 14

#define TFT_BL 2
#define TFT_RESET GPIO_UNUSED

#define TOUCH_I2C_PORT 0
#define TOUCH_I2C_SDA 19
#define TOUCH_I2C_SCL 20
#define TOUCH_I2C_FREQ_HZ 400000
#define TOUCH_GT911_ADDR1 0x5D
#define TOUCH_GT911_ADDR2 0x14
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0

#define TOUCH_DELTA_ROTATE_CW 1
#define TOUCH_DELTA_SCALE LCD_RENDER_SCALE

#define LED_R_PIN GPIO_UNUSED
#define LED_G_PIN GPIO_UNUSED
#define LED_B_PIN GPIO_UNUSED
#define GPIO_LED_PIN GPIO_UNUSED

#elif defined(CYD_BOARD_ESP32_2432S028)

#define BOARD_NAME "ESP32-2432S028 CYD2USB"

#define BOARD_HAS_RGB_LED 1
#define BOARD_HAS_TOUCH 1
#define TOUCH_CONTROLLER_XPT2046 1
#define LCD_PANEL_ILI9341_SPI 1

#define SCREEN_WIDTH DISP_WIDTH
#define SCREEN_HEIGHT DISP_HEIGHT
#define LCD_WIDTH SCREEN_WIDTH
#define LCD_HEIGHT SCREEN_HEIGHT
#define LCD_RENDER_SCALE 1
#define LCD_RENDER_ROTATE_CW 0
#define LCD_RENDER_OFFSET_X 0
#define LCD_RENDER_OFFSET_Y 0

// RGB LED pins are active low on CYD.
#define LED_R_PIN 4
#define LED_G_PIN 16
#define LED_B_PIN 17
#define GPIO_LED_PIN LED_G_PIN

// TFT LCD (ILI9341) - SPI
#define TFT_SPI_MOSI 13
#define TFT_SPI_CLK 14
#define TFT_SPI_CS 15
#define TFT_SPI_MISO 12
#define TFT_DC 2
#define TFT_RESET GPIO_UNUSED
#define TFT_BL 21

// Touch Screen (XPT2046) - separate SPI pins
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK 25
#define TOUCH_CS 33
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 1
#define TOUCH_MIRROR_Y 1
#define TOUCH_DELTA_ROTATE_CW 0
#define TOUCH_DELTA_SCALE 1

// Touch calibration values (from CYD reference project)
#define TOUCH_MIN_X 200
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 250
#define TOUCH_MAX_Y 3850

#else
#error "Unsupported Cydintosh board profile"
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
