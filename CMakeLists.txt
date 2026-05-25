cmake_minimum_required(VERSION 3.16.0)

if(CYD_MACHINE_MAC_LC)
    # LC/Tab5 smoke target intentionally avoids the current Mac Plus display/lib
    # components until Tab5 display support is selected explicitly.
    set(EXTRA_COMPONENT_DIRS "src" "components")
else()
    set(EXTRA_COMPONENT_DIRS "src" "components" "lib")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)

set(UMAC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/umac")
set(MUSASHI_DIR "${UMAC_DIR}/external/Musashi")

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

idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/include" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-I${UMAC_DIR}/include" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-I${MUSASHI_DIR}" APPEND)