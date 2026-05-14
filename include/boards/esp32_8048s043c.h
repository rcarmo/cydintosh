#ifndef BOARD_ESP32_8048S043C_H
#define BOARD_ESP32_8048S043C_H

// Sunton ESP32-8048S043C: ESP32-S3 N16R8, 800x480 RGB LCD, GT911 touch.
#define BOARD_NAME "ESP32-8048S043C"

#define BOARD_HAS_RGB_LED 0
#define BOARD_HAS_TOUCH 1
#define TOUCH_CONTROLLER_GT911 1
#define LCD_PANEL_RGB 1

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 480
#define LCD_WIDTH SCREEN_WIDTH
#define LCD_HEIGHT SCREEN_HEIGHT

// Native portrait mode at 1x: 480x800 Mac framebuffer rotated clockwise
// onto the physical 800x480 RGB panel. DISP_WIDTH=480, DISP_HEIGHT=800
// set in platformio.ini.

#define LCD_RENDER_SCALE 1
#define LCD_RENDER_ROTATE_CW 1
#define LCD_RENDER_FLIP_X 0
#define LCD_RENDER_FLIP_Y 0
#define LCD_RENDER_INVERT_MONO 1
#define LCD_RENDER_OFFSET_X 0
#define LCD_RENDER_OFFSET_Y 0

// RGB panels are memory-mapped; wider PSRAM-backed strips reduce draw call overhead.
#define LCD_TRANSFER_STRIP_WIDTH 40
#define LCD_TRANSFER_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

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
#define TOUCH_MIRROR_X 1   // GT911 x=0 is at the right edge in landscape; negate dx before delta rotation
#define TOUCH_MIRROR_Y 0

#define TOUCH_DELTA_ROTATE_CW 1
#define TOUCH_DELTA_SCALE 1  // 1:1 GT911->Mac pixel for native 480x800 portrait
#define MOUSE_DELTA_CAP 3    // avoid Mac acceleration spikes on GT911
#define TOUCH_FILTER_SHIFT 0 // no absolute-coordinate smoothing for capacitive touch
#define DEADZONE_PX 3        // GT911 capacitive touch jitter is low
#define MOVEMENT_THRESHOLD_PX 6

#define LED_R_PIN GPIO_UNUSED
#define LED_G_PIN GPIO_UNUSED
#define LED_B_PIN GPIO_UNUSED
#define GPIO_LED_PIN GPIO_UNUSED

// Keep the emulator task stack smaller on S3 so it fits internal heap after WiFi/LCD setup.
#define UMAC_TASK_STACK_SIZE 16384

// Desktop metadata is pre-seeded in the System 6 image; keep flash disk read-only
// to avoid guest crashes in the still-minimal Sony write path.
#define DISK_IMAGE_READ_ONLY 1

#endif
