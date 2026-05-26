
idf_component_register(
    SRCS "m5stack_tab5.c" "esp_lcd_st7123.c"
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES
        driver
        esp_lcd
        esp_codec_dev
        esp_lcd_ili9881c
        esp_lcd_st7703
        esp_lcd_touch_gt911
        esp_lcd_touch_st7123
    PRIV_REQUIRES usb spiffs fatfs sdmmc esp_hw_support
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC BSP_CONFIG_NO_GRAPHIC_LIB=1)
