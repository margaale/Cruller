# Cruller for the Raspberry Pi Pico 2 W (Pico SDK): the common code in src/core plus this target.
#   cmake -S src/platform/rp2 -B build/rp2 -G Ninja   (scripts/build.sh does it)
cmake_minimum_required(VERSION 3.13)

get_filename_component(CRULLER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(cmake/pico_sdk_import.cmake)
include(cmake/FreeRTOS_Kernel_import.cmake)

project(cruller C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(${CRULLER_ROOT}/src/version.cmake) # (CRULLER_VERSION, CRULLER_DEBUG)
include(${CRULLER_ROOT}/src/web/web.cmake)
include(${CRULLER_ROOT}/src/core/core.cmake) # (CRULLER_CORE_SOURCES)
set(CRULLER_BUILD 0 CACHE STRING "Build number: CI's run number (github.run_number), 0 for a local build")

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
    ${CRULLER_CORE_SOURCES}
    main.c
    platform.c
    platform_reboot.c
    freertos_hooks.c
    flash_ops.c
    store_flash.c
    cfgfs_flash.c
    ota.c
    tls.c
    tls_roots.c
    net_port.c
    health.c
    freeze.c
    log.c
    status_led.c
    rt4k.c # (the native USB port is the RT4K's host; logs go to the in-memory log, /log)
    ${CRULLER_ROOT}/third_party/picow_ap/dhcpserver.c
    ${CRULLER_ROOT}/third_party/picow_ap/dnsserver.c
)
target_include_directories(cruller_app INTERFACE
    ${CRULLER_ROOT}/src/core ${CRULLER_ROOT}/src/platform ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_host ${CRULLER_ROOT}/third_party/picow_ap)
cruller_web(cruller_app INTERFACE)
cruller_defs(cruller_app INTERFACE)

target_compile_definitions(cruller_app INTERFACE
    CYW43_HOST_NAME="cruller"
)
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
    tinyusb_host
)

# The image's version (CRULLER_BIN_MAJOR/MINOR, above) goes in crt0's metadata block, and only crt0 is
# built with it: pico_set_binary_version() would define it for every file, and the build number changes
# with every CI run, so the CI's ccache would find none of them again. crt0 as the SDK's pico_crt0 names it.
get_target_property(crt0 pico_crt0 INTERFACE_SOURCES)
if (NOT crt0)
    message(FATAL_ERROR "pico_crt0 has no sources: the image's version has nowhere to go")
endif()
set_source_files_properties(${crt0} PROPERTIES COMPILE_DEFINITIONS
    "PICO_CRT0_VERSION_MAJOR=${CRULLER_BIN_MAJOR};PICO_CRT0_VERSION_MINOR=${CRULLER_BIN_MINOR}")

function(cruller_image TARGET)
    add_executable(${TARGET})
    target_link_libraries(${TARGET} PRIVATE cruller_app)
    # The RT4K receive path runs from RAM (see cmake/linker/default_text_excludes.incl).
    pico_add_linker_script_override_path(${TARGET} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/linker
        FILES default_text_excludes.incl)
    pico_enable_stdio_uart(${TARGET} 0)
    pico_add_extra_outputs(${TARGET})
endfunction()

# OTA image (Cruller -> Cruller): "try before you buy", confirmed once healthy.
cruller_image(cruller)
target_compile_definitions(cruller PRIVATE PICO_CRT0_IMAGE_TYPE_TBYB=1)

# First image on a new board (the factory image, below), started with a normal boot: must not be TBYB.
cruller_image(cruller_factory_app)

# --- factory image ----------------------------------------------------------------------------

# A new board over USB: the flash contents from offset 0 (partition table, slot A) as a UF2, to drop on
# the RP2350 drive in BOOTSEL mode. Family "absolute": each block goes to its own address.
# cruller.uf2 alone brings no partition table, and without one OTA has no partition to update.
set(PT_JSON ${CMAKE_CURRENT_SOURCE_DIR}/pt.json)
set(PT_BIN ${CMAKE_CURRENT_BINARY_DIR}/pt.bin)
set(FACTORY_BIN ${CMAKE_CURRENT_BINARY_DIR}/cruller_factory.bin)
set(PARTITION_A_OFFSET 0x2000)    # pt.json: slot A, after the table
set(PARTITION_B_OFFSET 0x1D2000)  # pt.json: slot B, where slot A ends

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${PT_BIN}
    COMMAND picotool partition create ${PT_JSON} ${PT_BIN}
    DEPENDS ${PT_JSON}
    COMMENT "Partition table"
)
add_custom_command(
    OUTPUT ${FACTORY_BIN}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_factory.py
        ${PT_BIN} ${CMAKE_CURRENT_BINARY_DIR}/cruller_factory_app.bin ${FACTORY_BIN}
        ${PARTITION_A_OFFSET} ${PARTITION_B_OFFSET}
    DEPENDS ${PT_BIN} cruller_factory_app ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_factory.py
    COMMENT "Factory image (partition table + slot A)"
)
set(FACTORY_UF2 ${CMAKE_CURRENT_BINARY_DIR}/cruller-factory.uf2)
add_custom_command(
    OUTPUT ${FACTORY_UF2}
    COMMAND picotool uf2 convert ${FACTORY_BIN} -t bin ${FACTORY_UF2} -o 0x10000000 --family absolute
    DEPENDS ${FACTORY_BIN}
    COMMENT "Factory UF2 (partition table + slot A)"
)
add_custom_target(cruller_factory ALL DEPENDS ${FACTORY_UF2})
