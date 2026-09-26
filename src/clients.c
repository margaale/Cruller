#include "clients.h"

#include "FreeRTOS.h"
#include "task.h"

static volatile int used;

bool clients_take(void) {
    taskENTER_CRITICAL();
    const bool ok = used < CLIENTS_MAX;
    if (ok) used++;
    taskEXIT_CRITICAL();
    return ok;
}

void clients_give(void) {
    taskENTER_CRITICAL();
    if (used > 0) used--;
    taskEXIT_CRITICAL();
}

int clients_used(void) {
    return used;
}
