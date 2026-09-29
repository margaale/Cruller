// Minimal HTTP server: status page, firmware upload (OTA) and Wi-Fi setup.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The API's version (docs/API.md): routes under /api/v1, "api=1" in the _rt4k TXT, "api_version" in
// GET /api/v1/info. It moves on only for a change that breaks a v1 client (a route or field removed or
// renamed, a meaning changed); new routes and fields keep it.
#define HTTP_API_VERSION "1"

void http_start(void);

// A firmware upload (POST /update) in progress: bytes received of size. False when none.
bool http_update_progress(uint32_t *got, uint32_t *size);

// The /status JSON (also pushed over the WebSocket).
void http_status_json(char *body, size_t size);
// The /debug/memory report: clients, lwIP pools and heap, FreeRTOS heap, RAM (also pushed to Debug tabs).
void http_debug_memory(char *out, size_t size);
size_t http_debug_memory_json(char *out, size_t size); // the same as JSON; 0 if it didn't fit
bool http_listening(void);   // true once the server socket is open
