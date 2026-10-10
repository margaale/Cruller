# Cruller for the Raspberry Pi Pico 2 W (Pico SDK): the common code in src/core plus this target.
#   cmake -S src/platform/rp2 -B build/rp2 -G Ninja   (scripts/build.sh does it)
cmake_minimum_required(VERSION 3.13)

get_filename_component(CRULLER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
set(CRULLER_WEB ${CRULLER_ROOT}/src/web)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(cmake/pico_sdk_import.cmake)
include(cmake/FreeRTOS_Kernel_import.cmake)

project(cruller C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(${CRULLER_ROOT}/src/version.cmake)
set(CRULLER_VERSION "${CRULLER_VERSION_DEFAULT}" CACHE STRING "Firmware version (X.Y.Z or X.Y.Z-label.N)")
set(CRULLER_BUILD 0 CACHE STRING "Build number: CI's run number (github.run_number), 0 for a local build")
# The developer tools (src/platform/platform.h): the CI builds them into pre-releases and pull requests, not
# into releases.
option(CRULLER_DEBUG "The developer tools (src/platform/platform.h): 16 KB of RAM" ON)

pico_sdk_init()

# The boot ROM picks between partitions A and B by image version (two 16-bit numbers): the higher
# one runs after a reset, so the newest build must be the higher. MAJOR = X*100+Y, MINOR = 10000 +
# the build number: every CI build (release, develop, pull request; squash merges or not) gets a
# higher one than the builds before it. The 10000 puts them above every image of the earlier
# schemes (MINOR = Z, then Z*1000+N). A local build (0) sorts below any CI build of its X.Y.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z]+\\.[0-9]+)?$" _v "${CRULLER_VERSION}")
if (NOT _v)
    message(FATAL_ERROR "CRULLER_VERSION must be X.Y.Z or X.Y.Z-label.N, got '${CRULLER_VERSION}'")
endif()
math(EXPR CRULLER_BIN_MAJOR "${CMAKE_MATCH_1} * 100 + ${CMAKE_MATCH_2}")
if (NOT CRULLER_BUILD MATCHES "^[0-9]+$" OR CRULLER_BUILD GREATER 55535)
    message(FATAL_ERROR "CRULLER_BUILD must be 0..55535, got '${CRULLER_BUILD}'")
endif()
math(EXPR CRULLER_BIN_MINOR "10000 + ${CRULLER_BUILD}")

# --- third-party ------------------------------------------------------------------------------

add_library(littlefs STATIC ${CRULLER_ROOT}/third_party/littlefs/lfs.c ${CRULLER_ROOT}/third_party/littlefs/lfs_util.c)
target_include_directories(littlefs PUBLIC ${CRULLER_ROOT}/third_party/littlefs)
target_compile_definitions(littlefs PUBLIC LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

# --- application ------------------------------------------------------------------------------

# Common code (src/core) and the Pico 2 W target (src/platform/rp2, see src/platform/platform.h).
add_library(cruller_app INTERFACE)
target_sources(cruller_app INTERFACE
    ${CRULLER_ROOT}/src/core/buttons.c
    ${CRULLER_ROOT}/src/core/cfgfs.c
    ${CRULLER_ROOT}/src/core/clients.c
    ${CRULLER_ROOT}/src/core/console.c
    ${CRULLER_ROOT}/src/core/console_core.c
    ${CRULLER_ROOT}/src/core/creds.c
    ${CRULLER_ROOT}/src/core/gameid.c
    ${CRULLER_ROOT}/src/core/gameid_core.c
    ${CRULLER_ROOT}/src/core/gameid_run.c
    ${CRULLER_ROOT}/src/core/http.c
    ${CRULLER_ROOT}/src/core/json.c
    ${CRULLER_ROOT}/src/core/ota_fetch.c
    ${CRULLER_ROOT}/src/core/ota_fetch_proto.c
    ${CRULLER_ROOT}/src/core/power.c
    ${CRULLER_ROOT}/src/core/power_core.c
    ${CRULLER_ROOT}/src/core/rfc2217.c
    ${CRULLER_ROOT}/src/core/rfc2217_proto.c
    ${CRULLER_ROOT}/src/core/rt4k_info.c
    ${CRULLER_ROOT}/src/core/rt4k_info_core.c
    ${CRULLER_ROOT}/src/core/rtl1.c
    ${CRULLER_ROOT}/src/core/rtl1_core.c
    ${CRULLER_ROOT}/src/core/settings.c
    ${CRULLER_ROOT}/src/core/svs.c
    ${CRULLER_ROOT}/src/core/svs_proto.c
    ${CRULLER_ROOT}/src/core/version.c
    ${CRULLER_ROOT}/src/core/ws.c
    ${CRULLER_ROOT}/src/core/ws_proto.c

    main.c
    platform.c
    platform_reboot.c
    freertos_hooks.c
    flash_ops.c
    store_flash.c
    cfgfs_flash.c
    donutshop.c
    ota.c
    tls.c
    tls_roots.c
    net.c
    health.c
    freeze.c
    log.c
    status_led.c
    ${CRULLER_ROOT}/third_party/picow_ap/dhcpserver.c
    ${CRULLER_ROOT}/third_party/picow_ap/dnsserver.c
)
target_include_directories(cruller_app INTERFACE
    ${CRULLER_ROOT}/src/core ${CRULLER_ROOT}/src/platform ${CMAKE_CURRENT_SOURCE_DIR}
    ${CRULLER_ROOT}/third_party/picow_ap)

# Web assets kept as real files (src/web), embedded as C arrays (see src/web/embed.cmake).
foreach(asset sha256.js:web_sha256_js fw.js:web_fw_js sd.js:web_sd_js profiles.js:web_profiles_js mapper.js:web_mapper_js editor.js:web_editor_js gameid.js:web_gameid_js rt4k_settings.json:web_rt4k_settings_json app.js:web_app_js index.html:web_index_html)
    string(REPLACE ":" ";" asset ${asset})
    list(GET asset 0 file)
    list(GET asset 1 name)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}.c)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -DIN=${CRULLER_WEB}/${file} -DOUT=${out} -DNAME=${name}
            -P ${CRULLER_WEB}/embed.cmake
        DEPENDS ${CRULLER_WEB}/${file} ${CRULLER_WEB}/embed.cmake
        VERBATIM)
    target_sources(cruller_app INTERFACE ${out})
endforeach()
target_compile_definitions(cruller_app INTERFACE
    CYW43_HOST_NAME="cruller"
)
# The version for version.c alone: a new one compiles that file again, no other (ccache keeps the rest).
set_source_files_properties(${CRULLER_ROOT}/src/core/version.c PROPERTIES COMPILE_DEFINITIONS CRULLER_VERSION="${CRULLER_VERSION}")
if (NOT CRULLER_DEBUG)
    target_compile_definitions(cruller_app INTERFACE CRULLER_DEBUG=0)
endif()
target_link_libraries(cruller_app INTERFACE
    pico_stdlib
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mdns
    pico_flash
    pico_bootrom
    pico_sha256
    pico_mbedtls # tls.c (mbedtls_config.h)
    pico_rand # main.c: seeded before the scheduler
    pico_unique_id
    boot_uf2_headers
    hardware_adc # health.c: the supply and the chip's temperature
    hardware_exception # freeze.c: the sampler's NMI handler
    hardware_flash
    littlefs
    FreeRTOS-Kernel-Heap4
)

# The image's version (CRULLER_BIN_MAJOR/MINOR, above) goes in crt0's metadata block, and only crt0 is
# built with it: pico_set_binary_version() would define it for every file, and the build number changes
# with every CI run, so the CI's ccache would find none of them again.
set_source_files_properties(${PICO_SDK_PATH}/src/rp2_common/pico_crt0/crt0.S PROPERTIES COMPILE_DEFINITIONS
    "PICO_CRT0_VERSION_MAJOR=${CRULLER_BIN_MAJOR};PICO_CRT0_VERSION_MINOR=${CRULLER_BIN_MINOR}")

function(cruller_image TARGET)
    add_executable(${TARGET})
    target_link_libraries(${TARGET} PRIVATE cruller_app)
    if ("${TARGET}" MATCHES "_bench$")
        # Bench board on a PC: USB is a serial console (the log, live) and there is no RT4K link.
        target_sources(${TARGET} PRIVATE ${CRULLER_ROOT}/src/core/rt4k_stub.c)
        target_compile_definitions(${TARGET} PRIVATE CRULLER_BENCH=1)
        pico_enable_stdio_usb(${TARGET} 1)
    else()
        # The native USB port is the RT4K's host; logs go to the in-memory log (/log) instead.
        target_sources(${TARGET} PRIVATE rt4k.c)
        target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/usb_host)
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

# --- factory image ----------------------------------------------------------------------------

# A new board over USB: the same flash contents from offset 0 (partition table and slot A) as a UF2,
# to drop on the RP2350 drive in BOOTSEL mode. Family "absolute": each block goes to its own address.
# cruller.uf2 alone brings no partition table, and without one OTA has no partition to update.
set(FACTORY_UF2 ${CMAKE_CURRENT_BINARY_DIR}/cruller-factory.uf2)
add_custom_command(
    OUTPUT ${FACTORY_UF2}
    COMMAND picotool uf2 convert ${MIGRATION_BIN} -t bin ${FACTORY_UF2} -o 0x10000000 --family absolute
    DEPENDS ${MIGRATION_BIN}
    COMMENT "Factory UF2 (partition table + slot A)"
)
add_custom_target(cruller_factory ALL DEPENDS ${FACTORY_UF2})
