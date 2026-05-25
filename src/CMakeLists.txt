set(UMAC_DIR "${CMAKE_SOURCE_DIR}/external/umac")
set(MUSASHI_DIR "${UMAC_DIR}/external/Musashi")

if(CYD_MACHINE_MAC_LC OR "${IDF_TARGET}" STREQUAL "esp32p4")
    idf_component_register(
        SRCS "tab5_lc_smoke.c" "machine_lc/lc_rom.c"
        INCLUDE_DIRS "." "machine_lc" "${CMAKE_SOURCE_DIR}/include"
        REQUIRES esp_psram esp_partition esp_hw_support heap
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CYD_MACHINE_MAC_LC=1)
else()
    set(UMAC_SOURCES
        ${UMAC_DIR}/src/disc.c
        ${UMAC_DIR}/src/main.c
        ${UMAC_DIR}/src/rom.c
        ${UMAC_DIR}/src/scc.c
        ${UMAC_DIR}/src/via.c
    )

    set(MUSASHI_SOURCES
        ${MUSASHI_DIR}/m68kcpu.c
        ${MUSASHI_DIR}/m68kdasm.c
        ${MUSASHI_DIR}/m68kops.c
        ${MUSASHI_DIR}/softfloat/softfloat.c
    )

    idf_component_register(
        SRCS "main.c" "video.c" "display.c" "disc_lfs.c" "touch_cyd.c" "umac_ipc.c" "wifi_info.c" "mqtt_home.c" "hw_control.c" ${UMAC_SOURCES} ${MUSASHI_SOURCES}
        INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include" "${UMAC_DIR}/include" "${MUSASHI_DIR}"
        REQUIRES driver esp_timer esp_lcd joltwallet__littlefs lcd_cyd esp_wifi esp_netif mqtt
    )

    # PlatformIO build_flags (-D) only reach src/ files via platformio's own mechanism.
    # Explicitly forward critical defines so external/umac and Musashi sources compile correctly.
    if(CONFIG_IDF_TARGET_ESP32S3)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CYD_MACHINE_MAC_PLUS=1 DISP_HEIGHT=800 DISP_WIDTH=480 UMAC_MEMSIZE=1024 MOUSE_MAX_PENDING_PIX=60)
    else()
        target_compile_definitions(${COMPONENT_LIB} PRIVATE CYD_MACHINE_MAC_PLUS=1 DISP_HEIGHT=320 DISP_WIDTH=240 UMAC_MEMSIZE=128 MOUSE_MAX_PENDING_PIX=120)
    endif()
endif()
