get_filename_component(CRULLER_ROOT ${COMPONENT_DIR}/../../../.. ABSOLUTE)
set(CORE ${CRULLER_ROOT}/src/core)

idf_component_register(
    SRCS
        ${CORE}/buttons.c
        ${CORE}/cfgfs.c
        ${CORE}/clients.c
        ${CORE}/console.c
        ${CORE}/console_core.c
        ${CORE}/creds.c
        ${CORE}/gameid.c
        ${CORE}/gameid_core.c
        ${CORE}/gameid_run.c
        ${CORE}/http.c
        ${CORE}/json.c
        ${CORE}/ota_fetch.c
        ${CORE}/ota_fetch_proto.c
        ${CORE}/power.c
        ${CORE}/power_core.c
        ${CORE}/rfc2217.c
        ${CORE}/rfc2217_proto.c
        ${CORE}/rt4k_info.c
        ${CORE}/rt4k_info_core.c
        ${CORE}/rt4k_stub.c # the RT4K's USB host comes next
        ${CORE}/rtl1.c
        ${CORE}/rtl1_core.c
        ${CORE}/settings.c
        ${CORE}/svs.c
        ${CORE}/svs_proto.c
        ${CORE}/version.c
        ${CORE}/ws.c
        ${CORE}/ws_proto.c
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
