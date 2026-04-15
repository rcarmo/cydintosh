cmake_minimum_required(VERSION 3.9)

project(OfficeLights)

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "CMAKE_TOOLCHAIN_FILE must be set to Retro68 toolchain")
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common common)

add_application(OfficeLights
    OfficeLightsApp.c
    Resources.r
    TYPE "APPL"
    CREATOR "OLIT"
)

target_link_libraries(OfficeLights esp_ipc)
