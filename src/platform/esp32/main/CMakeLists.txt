get_filename_component(CRULLER_ROOT ${COMPONENT_DIR}/../../../.. ABSOLUTE)
set(CORE ${CRULLER_ROOT}/src/core)
include(${CORE}/core.cmake) # (CRULLER_CORE_SOURCES)

idf_component_register(
    SRCS
        ${CRULLER_CORE_SOURCES}
        ${CORE}/rt4k_stub.c # the RT4K's USB host comes next
        ${CRULLER_ROOT}/third_party/picow_ap/dnsserver.c
        ${CRULLER_ROOT}/third_party/littlefs/lfs.c
        ${CRULLER_ROOT}/third_party/littlefs/lfs_util.c
        main.c
        platform.c
        store_nvs.c
        cfgfs_part.c
        ota.c
        tls.c
        net.c
        health.c
        freeze.c
        log.c
        status_led.c
    INCLUDE_DIRS . ${CORE} ${CRULLER_ROOT}/src/platform ${CRULLER_ROOT}/third_party/picow_ap ${CRULLER_ROOT}/third_party/littlefs
    REQUIRES
        app_update esp-tls esp_driver_gpio esp_partition spi_flash esp_event esp_netif esp_system esp_timer esp_wifi freertos
        lwip mbedtls nvs_flash vfs
)

cruller_defs(${COMPONENT_LIB} PRIVATE)
cruller_web(${COMPONENT_LIB} PRIVATE)
# littlefs (cfgfs.h) as on rp2: static buffers, quiet.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_NO_MALLOC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)

# src/core includes the kernel's headers as "FreeRTOS.h", "task.h"... (the Pico SDK's way); ESP-IDF
# puts them under freertos/.
idf_component_get_property(freertos_dir freertos COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE ${freertos_dir}/FreeRTOS-Kernel/include/freertos)
