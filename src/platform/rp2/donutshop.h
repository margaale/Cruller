// The DonutShop Pico 2 W firmware Cruller replaces (docs/DESIGN.md, "Migration from DonutShop").

#pragma once

#include <stdbool.h>

#include "creds.h"

// First boot after migrating: reads /wifi.json from DonutShop's LittleFS, if it's still there.
bool donutshop_import_creds(wifi_creds_t *out);
