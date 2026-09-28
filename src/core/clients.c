#include "clients.h"

#include "platform.h"

static volatile int used;

bool clients_take(void) {
    plat_critical_enter();
    const bool ok = used < CLIENTS_MAX;
    if (ok) used++;
    plat_critical_exit();
    return ok;
}

void clients_give(void) {
    plat_critical_enter();
    if (used > 0) used--;
    plat_critical_exit();
}

int clients_used(void) {
    return used;
}
