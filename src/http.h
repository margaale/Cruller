// Minimal HTTP server: status page, firmware upload (OTA) and Wi-Fi setup.

#pragma once

#include <stdbool.h>

void http_start(void);
bool http_listening(void);   // true once the server socket is open
