#include "app.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cfgfs.h"
#include "console.h"
#include "gameid_run.h"
#include "health.h"
#include "http.h"
#include "net.h"
#include "ota.h"
#include "platform.h"
#include "power.h"
#include "rfc2217.h"
#include "rt4k.h"
#include "ws.h"

void app_run(void) {
    health_start_net_probe();
    // The files (gameID's): before rt4k_start(), so a first format needn't hold the USB host off.
    if (!cfgfs_mount()) printf("cfgfs: no files this run\n");
    console_start(); // before rt4k_start(): the rt4k task feeds it text from the start
    power_start();
    rt4k_start();
    ws_start();
    http_start();
    gameid_run_start(); // asks the consoles once the network is up
    rfc2217_start();
    net_start();

    // An updated image must be confirmed soon or the board goes back to the previous one (the Pico 2
    // W's boot ROM gives it ~16.7 s). Healthy enough: the web server, which carries the updates,
    // listening. Joining the network is not required, so a network outage never rejects a good image.
    while (!http_listening()) vTaskDelay(pdMS_TO_TICKS(50));
    ota_confirm_if_trial();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        printf("alive %lu s, heap free %u (min %u)\n", (unsigned long)(plat_ms() / 1000),
            (unsigned)xPortGetFreeHeapSize(), (unsigned)xPortGetMinimumEverFreeHeapSize());
    }
}
