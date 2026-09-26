cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(cmake/pico_sdk_import.cmake)
include(cmake/FreeRTOS_Kernel_import.cmake)

project(cruller C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(CRULLER_VERSION "0.0.4" CACHE STRING "Firmware version (X.Y.Z); must grow with every image, the boot ROM picks A/B by it")

pico_sdk_init()

# The boot ROM picks between partitions A and B by image version, so it must grow with every
# release: MAJOR = X*100+Y, MINOR = Z.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _v "${CRULLER_VERSION}")
if (NOT _v)
    message(FATAL_ERROR "CRULLER_VERSION must be X.Y.Z, got '${CRULLER_VERSION}'")
endif()
math(EXPR CRULLER_BIN_MAJOR "${CMAKE_MATCH_1} * 100 + ${CMAKE_MATCH_2}")
set(CRULLER_BIN_MINOR ${CMAKE_MATCH_3})

# --- third-party ------------------------------------------------------------------------------

add_library(littlefs STATIC third_party/littlefs/lfs.c third_party/littlefs/lfs_util.c)
target_include_directories(littlefs PUBLIC third_party/littlefs)
target_compile_definitions(littlefs PUBLIC LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

# --- application ------------------------------------------------------------------------------

add_library(cruller_app INTERFACE)
target_sources(cruller_app INTERFACE
    src/main.c
    src/freertos_hooks.c
    src/flash_ops.c
    src/creds.c
    src/ota.c
    src/platform_reboot.c
    src/net.c
    src/http.c
    src/status_led.c
    src/log.c
    src/freeze.c
    src/health.c
    src/clients.c
    src/console.c
    src/console_core.c
    src/power.c
    src/power_core.c
    src/rfc2217.c
    src/rfc2217_proto.c
    src/rtl1.c
    src/rtl1_core.c
    src/ws.c
    src/ws_proto.c
    third_party/picow_ap/dhcpserver.c
    third_party/picow_ap/dnsserver.c
)
target_include_directories(cruller_app INTERFACE src third_party/picow_ap)

# Web assets kept as real files (src/web), embedded as C arrays (see cmake/embed.cmake).
set(WEB_FW_JS_C ${CMAKE_CURRENT_BINARY_DIR}/web_fw_js.c)
add_custom_command(OUTPUT ${WEB_FW_JS_C}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/web/fw.js -DOUT=${WEB_FW_JS_C} -DNAME=web_fw_js
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/web/fw.js ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
set(WEB_APP_JS_C ${CMAKE_CURRENT_BINARY_DIR}/web_app_js.c)
add_custom_command(OUTPUT ${WEB_APP_JS_C}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/web/app.js -DOUT=${WEB_APP_JS_C} -DNAME=web_app_js
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/web/app.js ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
set(WEB_INDEX_C ${CMAKE_CURRENT_BINARY_DIR}/web_index_html.c)
add_custom_command(OUTPUT ${WEB_INDEX_C}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html -DOUT=${WEB_INDEX_C} -DNAME=web_index_html
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
target_sources(cruller_app INTERFACE ${WEB_FW_JS_C} ${WEB_APP_JS_C} ${WEB_INDEX_C})
target_compile_definitions(cruller_app INTERFACE
    CRULLER_VERSION="${CRULLER_VERSION}"
    CYW43_HOST_NAME="cruller"
)
target_link_libraries(cruller_app INTERFACE
    pico_stdlib
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mdns
    pico_flash
    pico_bootrom
    pico_sha256
    boot_uf2_headers
    hardware_flash
    littlefs
    FreeRTOS-Kernel-Heap4
)

function(cruller_image TARGET)
    add_executable(${TARGET})
    target_link_libraries(${TARGET} PRIVATE cruller_app)
    pico_set_binary_version(${TARGET} MAJOR ${CRULLER_BIN_MAJOR} MINOR ${CRULLER_BIN_MINOR})
    if ("${TARGET}" MATCHES "_bench$")
        # Bench board on a PC: USB is a serial console (the log, live) and there is no RT4K link.
        target_sources(${TARGET} PRIVATE src/rt4k_stub.c)
        target_compile_definitions(${TARGET} PRIVATE CRULLER_BENCH=1)
        pico_enable_stdio_usb(${TARGET} 1)
    else()
        # The native USB port is the RT4K's host; logs go to the in-memory log (/log) instead.
        target_sources(${TARGET} PRIVATE src/rt4k.c)
        target_include_directories(${TARGET} PRIVATE src/usb_host)
        target_link_libraries(${TARGET} PRIVATE tinyusb_host)
        # The RT4K receive path runs from RAM (see cmake/linker/default_text_excludes.incl).
        pico_add_linker_script_override_path(${TARGET} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/linker
            FILES default_text_excludes.incl)
        pico_enable_stdio_usb(${TARGET} 0)
    endif()
    pico_enable_stdio_uart(${TARGET} 0)
    pico_add_extra_outputs(${TARGET})
endfunction()

# OTA image (Cruller -> Cruller): "try before you buy", confirmed once healthy.
cruller_image(cruller)
target_compile_definitions(cruller PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1)

# Same OTA image for a board on the bench (see cruller_image). Not built by default.
cruller_image(cruller_bench)
target_compile_definitions(cruller_bench PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1)
set_target_properties(cruller_bench PROPERTIES EXCLUDE_FROM_ALL TRUE)

# First image, installed by DonutShop's OTA stage-3 with a normal boot: must not be TBYB.
cruller_image(cruller_migration_app)

# --- migration image --------------------------------------------------------------------------

set(PT_JSON ${CMAKE_CURRENT_SOURCE_DIR}/pt.json)
set(PT_BIN ${CMAKE_CURRENT_BINARY_DIR}/pt.bin)
set(MIGRATION_BIN ${CMAKE_CURRENT_BINARY_DIR}/cruller_migration.bin)
set(PARTITION_A_OFFSET 0x2000)      # pt.json: first partition after the table
set(DONUTSHOP_FS_OFFSET 0x1FF000)   # the migration image must end below DonutShop's LittleFS

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${PT_BIN}
    COMMAND picotool partition create ${PT_JSON} ${PT_BIN}
    DEPENDS ${PT_JSON}
    COMMENT "Partition table"
)
add_custom_command(
    OUTPUT ${MIGRATION_BIN}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_migration.py
        ${PT_BIN} ${CMAKE_CURRENT_BINARY_DIR}/cruller_migration_app.bin ${MIGRATION_BIN}
        ${PARTITION_A_OFFSET} ${DONUTSHOP_FS_OFFSET}
    DEPENDS ${PT_BIN} cruller_migration_app ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_migration.py
    COMMENT "DonutShop -> Cruller migration image"
)
add_custom_target(cruller_migration ALL DEPENDS ${MIGRATION_BIN})
