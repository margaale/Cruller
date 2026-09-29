get_filename_component(CRULLER_ROOT ${COMPONENT_DIR}/../../../.. ABSOLUTE)
set(CORE ${CRULLER_ROOT}/src/core)

idf_component_register(
    SRCS
        ${CORE}/clients.c
        ${CORE}/console.c
        ${CORE}/console_core.c
        ${CORE}/creds.c
        ${CORE}/http.c
        ${CORE}/power.c
        ${CORE}/power_core.c
        ${CORE}/rfc2217.c
        ${CORE}/rfc2217_proto.c
        ${CORE}/rt4k_stub.c # the RT4K's USB host comes next
        ${CORE}/rtl1.c
        ${CORE}/rtl1_core.c
        ${CORE}/settings.c
        ${CORE}/svs.c
        ${CORE}/svs_proto.c
        ${CORE}/ws.c
        ${CORE}/ws_proto.c
        ${CRULLER_ROOT}/third_party/picow_ap/dnsserver.c
        main.c
        platform.c
        store_nvs.c
        ota.c
        net.c
        health.c
        freeze.c
        log.c
        status_led.c
    INCLUDE_DIRS . ${CORE} ${CRULLER_ROOT}/src/platform ${CRULLER_ROOT}/third_party/picow_ap
    REQUIRES
        app_update esp_driver_gpio esp_event esp_netif esp_system esp_timer esp_wifi freertos
        lwip mbedtls nvs_flash vfs
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE CRULLER_VERSION="${CRULLER_VERSION}")

# src/core includes the kernel's headers as "FreeRTOS.h", "task.h"... (the Pico SDK's way); ESP-IDF
# puts them under freertos/.
idf_component_get_property(freertos_dir freertos COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE ${freertos_dir}/FreeRTOS-Kernel/include/freertos)

# The web page, embedded as C arrays (the same as rp2, see src/web/embed.cmake).
foreach(asset sha256.js:web_sha256_js fw.js:web_fw_js sd.js:web_sd_js app.js:web_app_js index.html:web_index_html)
    string(REPLACE ":" ";" asset ${asset})
    list(GET asset 0 file)
    list(GET asset 1 name)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}.c)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -DIN=${CRULLER_ROOT}/src/web/${file} -DOUT=${out} -DNAME=${name}
            -P ${CRULLER_ROOT}/src/web/embed.cmake
        DEPENDS ${CRULLER_ROOT}/src/web/${file} ${CRULLER_ROOT}/src/web/embed.cmake
        VERBATIM)
    target_sources(${COMPONENT_LIB} PRIVATE ${out})
endforeach()
