# The common code (src/core), the same for every board: its CMakeLists.txt includes this and builds
# ${CRULLER_CORE_SOURCES} with its own files (the ESP32 adds rt4k_stub.c until it has a USB host).
set(CRULLER_CORE_SOURCES)
foreach(file
        app.c buttons.c cfgfs.c clients.c console.c console_core.c creds.c gameid.c gameid_core.c gameid_run.c
        health_net.c http.c json.c log_ring.c ota_fetch.c ota_fetch_proto.c power.c power_core.c rfc2217.c rfc2217_proto.c
        rt4k_info.c rt4k_info_core.c rtl1.c rtl1_core.c settings.c svs.c svs_proto.c version.c ws.c ws_proto.c)
    list(APPEND CRULLER_CORE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/${file})
endforeach()
