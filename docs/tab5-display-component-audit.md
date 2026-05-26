# Tab5 display/touch component audit

This is a hardware-independent audit for the M5Stack Tab5 display/touch bring-up.
No driver code is vendored or enabled yet.

## Source checked

- Repository: `https://github.com/m5stack/M5Tab5-UserDemo`
- Local audit clone: `/workspace/tmp/M5Tab5-UserDemo`
- Commit inspected: `611f2d6f447ed21a379fa7a6dcbc3ef19c07094a`
- Search query used: `M5Stack Tab5 ESP32-P4 ILI9881C ST7123 MIPI DSI ESP-IDF example`

The M5Stack demo README and third-party setup notes point at ESP-IDF 5.4.x. This
branch currently builds the Tab5 skeleton with the PlatformIO ESP-IDF 5.5.2
package, so display bring-up should treat ESP-IDF-version sensitivity as a risk.
A related Espressif issue reports Tab5 DSI regressions around ESP-IDF 5.5.x and
working behavior around Arduino/IDF 5.4.x.

## Local component files of interest

From `platforms/tab5/components/m5stack_tab5/`:

| File | Purpose |
|---|---|
| `m5stack_tab5.c` | BSP display, backlight, I2C, touch and LVGL integration |
| `esp_lcd_st7123.c` | ST7123 MIPI-DSI panel/touch helper driver |
| `priv_include/esp_lcd_st7123.h` | ST7123 private panel declarations |
| `include/bsp/display.h` | LCD resolution, DSI timing, lane count, bitrate, PHY LDO config |
| `include/bsp/m5stack_tab5.h` | Tab5 GPIO/I2C/audio/SD pin definitions |
| `idf_component.yml` | Component dependencies |

## Dependencies seen in `idf_component.yml`

```yaml
espressif/esp_lcd_st7703: ^1.0.1
espressif/esp_lcd_touch_st7123: ^1.0.0
espressif/esp_codec_dev: ^1.3.0
espressif/esp_lcd_touch_gt911: ^1.1.1~2
espressif/usb_host_hid: ^1.0.3
```

`CMakeLists.txt` additionally requires `esp_lcd_ili9881c` and `esp_lvgl_port` for
the full demo/BSP path.

## Display path findings

The official/demo component has two relevant paths:

### ILI9881C path

`bsp_display_new_with_handles()` uses:

- `esp_lcd_new_dsi_bus()`
- `esp_lcd_new_panel_io_dbi()`
- `esp_lcd_new_panel_ili9881c()`
- `tab5_lcd_ili9881c_specific_init_code_default` from `ili9881_init_data.c`
- `LCD_COLOR_PIXEL_FORMAT_RGB565`
- `bits_per_pixel = 16`
- 2 DSI lanes via `BSP_LCD_MIPI_DSI_LANE_NUM`
- lane bitrate `BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS`

Panel timing in the inspected code:

```text
h_size=720, v_size=1280
hsync_back_porch=140, hsync_pulse_width=40, hsync_front_porch=40
vsync_back_porch=20,  vsync_pulse_width=4,  vsync_front_porch=20
dpi_clock_freq_mhz=60
```

### ST7123 path

`bsp_display_new_with_handles_to_st7123()` uses:

- `esp_lcd_new_dsi_bus()`
- `esp_lcd_new_panel_io_dbi()`
- local `esp_lcd_new_panel_st7123()` from the demo component
- `st7123_vendor_specific_init_default`
- 2 DSI lanes
- lane bitrate `965 Mbps`
- DPI clock `70 MHz`
- RGB565 pixel format in `dpi_config`, but `lcd_dev_config.bits_per_pixel = 24`

Panel timing in the inspected code:

```text
h_size=720, v_size=1280
hsync_pulse_width=2, hsync_back_porch=40, hsync_front_porch=40
vsync_pulse_width=2, vsync_back_porch=8,  vsync_front_porch=220
```

This mixed RGB565/24-bit setting should be treated carefully during bring-up.
Start with a BSP-faithful smoke test before changing pixel format or timing.

## Common display constants

From `include/bsp/display.h`:

```text
BSP_LCD_H_RES = 720
BSP_LCD_V_RES = 1280
BSP_LCD_MIPI_DSI_LANE_NUM = 2
BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS = 730
BSP_MIPI_DSI_PHY_PWR_LDO_CHAN = 3
BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV = 2500
BSP_LCD_BACKLIGHT = GPIO22
```

The Cydintosh LC guest video target remains `512x384`; the physical panel path is
portrait `720x1280`, so the first display smoke test should render independent
solid colors/orientation markers before scaling the LC framebuffer.

## Touch path findings

The demo initializes the system I2C bus on:

```text
SDA = GPIO31
SCL = GPIO32
```

Touch paths seen:

- GT911 via `esp_lcd_touch_new_i2c_gt911()` with backup address from the
  Espressif GT911 component.
- ST7123 touch via I2C address `0x55` and `esp_lcd_touch_new_i2c_st7123()`.
- Touch interrupt is noted as GPIO23 in comments/config, but the inspected header
  has `BSP_LCD_TOUCH_INT = GPIO_NUM_NC` while ST7123/GT911 config comments still
  mention GPIO23.

The existing `include/boards/m5stack_tab5_esp32p4_lc.h` already matches the
important documented pins:

```text
TAB5_TOUCH_I2C_SDA = 31
TAB5_TOUCH_I2C_SCL = 32
TAB5_TOUCH_INT_GPIO = 23
TAB5_TOUCH_GT911_ADDR = 0x14
TAB5_TOUCH_ST7123_ADDR = 0x55
TAB5_LCD_BACKLIGHT_GPIO = 22
```

## Backlight scaffold status

The branch now has a Tab5-only LEDC backlight scaffold in
`src/machine_lc/tab5_backlight.c`:

```text
GPIO22 / LEDA
LEDC low-speed mode
timer 0, channel 1
5 kHz, 12-bit duty
TAB5_BACKLIGHT_BOOT_PERCENT = 20
```

This intentionally does not initialize DSI or draw to the panel. Standalone
GPIO22/PWM tests were insufficient on the actual unit because the firmware was
initially built with the wrong rev3 board/linker selection and trapped before
`app_main`.

For black-screen isolation, the branch also has a temporary
`esp32-p4-tab5-bootdiag` PlatformIO environment and `make flash-tab5-bootdiag`
target. That image disables PSRAM boot init, skips LC diagnostics, applies a
minimal M5Stack-BSP-faithful PI4IOE SYS-I2C expander init subset, and loops
forever toggling GPIO22 high/low.

## BSP display smoke status

The branch now vendors the M5Stack BSP component under
`components_tab5/m5stack_tab5` with `BSP_CONFIG_NO_GRAPHIC_LIB=1`, preserving the
Apache/MIT upstream files and avoiding the full demo app/LVGL layer. The separate
`esp32-p4-tab5-display-smoke` environment:

- uses PlatformIO board `m5stack-tab5-p4`, matching the connected ESP32-P4 rev v1.3;
- selects ESP32-P4 rev <3.0, min rev `100`, max rev `199`, and 360MHz CPU;
- initializes SYS-I2C and PI4IOE reset/power outputs;
- probes GT911 (`0x14`) vs ST7123 (`0x55`);
- initializes the ILI9881C/ST7703-compatible or ST7123 MIPI-DSI path;
- draws a real `720x1280` RGB565 stripe/orientation pattern, or the LC
  `512x384x8bpp` indexed debug pattern scaled to a `720x540` centered viewport;
- pulses backlight between 35% and 100%.

Serial capture from the flashed smoke image shows the app alive in the brightness
heartbeat loop. Camera/user confirmation verified that the panel visibly renders
the pattern and flashes. If the panel appears asleep after future flashes, press
the Tab5 power button once, as required by the patched official demo during
testing.

## Touch scaffold status

The branch now has a Tab5-only I2C/touch probe scaffold in
`src/machine_lc/tab5_touch.c`. It uses the ESP-IDF 5.5 I2C master API to:

- initialize I2C0 on GPIO31/GPIO32;
- probe GT911 at `0x14`;
- read GT911 product ID at register `0x8140` if present;
- probe ST7123 touch at `0x55`.

It does not read touch coordinates or emit ADB mouse packets yet.

## Bring-up recommendation

1. Keep the vendored BSP isolated under `components_tab5/` so Mac Plus builds do
   not pull Tab5/P4-only managed dependencies.
2. Route the LC guest framebuffer through the BSP panel handle, reusing the
   verified LC indexed-pattern scaling path.
3. Replace the debug CLUT/test pattern with ROM/System-driven VRAM updates once
   the LC memory map and video registers are implemented.

## Open risks

- ESP-IDF 5.5.2 DSI behavior may differ from M5Stack's 5.4.x demo baseline.
- The demo contains both ILI9881C and ST7123 paths; the correct path may need
  runtime detection or hardware revision confirmation.
- ST7123 code uses a local driver file and many vendor init commands; importing
  this requires license/header preservation and isolation from Mac Plus builds.
- Touch controller may be GT911 or ST7123 depending on the panel path.
