cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    COMPILE_DEFINITIONS
    "ESP_IDF_LWIP_HOOK_FILENAME=\"${CMAKE_CURRENT_LIST_DIR}/src/services/solar_os_lwip_hooks.h\""
    APPEND
)
project(solar_os)
include(patches/nimble/overlay.cmake)
