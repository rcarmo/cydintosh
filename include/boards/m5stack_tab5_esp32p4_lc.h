#ifndef BOARD_M5STACK_TAB5_ESP32P4_LC_H
#define BOARD_M5STACK_TAB5_ESP32P4_LC_H

// M5Stack Tab5 / ESP32-P4 experimental target for Macintosh LC color work.
// This header is deliberately LC/P4-only and is not used by the existing
// Mac Plus ESP32 / ESP32-S3 board profiles.
#define BOARD_NAME "M5Stack Tab5 ESP32-P4 LC color"

#define BOARD_HAS_RGB_LED 0
#define BOARD_HAS_TOUCH 1
#define LCD_PANEL_TAB5 1

// Hardware details still need confirmation from M5Stack examples/schematics.
// Keep these as conservative metadata for compile-time diagnostics until the
// real display/touch driver is integrated.
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define LCD_WIDTH SCREEN_WIDTH
#define LCD_HEIGHT SCREEN_HEIGHT

// Initial LC guest-video target; not yet wired to a real color renderer.
#define DISP_WIDTH 512
#define DISP_HEIGHT 384
#define LC_GUEST_COLOR_DEPTH_BITS 8
#define LC_GUEST_RAM_SIZE (4 * 1024 * 1024)
#ifndef LC_ROM_EXPECTED_SIZE
#define LC_ROM_EXPECTED_SIZE 0x80000
#endif
#ifndef LC_ROM_EXPECTED_FIRST_LONG
#define LC_ROM_EXPECTED_FIRST_LONG 0x350EACF0u
#endif

#define TAB5_USB_SERIAL_JTAG_PORT "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D1:46:0D-if00"

// Placeholder render/touch values so board profile sanity checks remain possible
// once the LC path starts including board_profiles.h.
#define LCD_RENDER_SCALE 1
#define LCD_RENDER_ROTATE_CW 0
#define LCD_RENDER_FLIP_X 0
#define LCD_RENDER_FLIP_Y 0
#define LCD_RENDER_INVERT_MONO 0
#define LCD_TRANSFER_STRIP_WIDTH 40
#define LCD_TRANSFER_BUFFER_CAPS MALLOC_CAP_8BIT
#define TOUCH_DELTA_SCALE 1
#define MOUSE_DELTA_CAP 8
#define TOUCH_FILTER_SHIFT 0
#define TFT_BL GPIO_UNUSED
#define UMAC_TASK_STACK_SIZE 16384
#define DISK_IMAGE_READ_ONLY 1

#endif
