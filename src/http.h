// Minimal HTTP server: status page, firmware upload (OTA) and Wi-Fi setup.

#pragma once

#include <stdbool.h>
#include <stddef.h>

void http_start(void);

// The /status JSON (also pushed over the WebSocket).
void http_status_json(char *body, size_t size);
// The /debug/memory report: clients, lwIP pools and heap, FreeRTOS heap, RAM (also pushed to Debug tabs).
void http_debug_memory(char *out, size_t size);
bool http_listening(void);   // true once the server socket is open
