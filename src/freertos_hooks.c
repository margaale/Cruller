// FreeRTOS failure hooks: fail loudly instead of silently corrupting memory.

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "pico/stdlib.h"

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    panic("stack overflow in task %s", name);
}

void vApplicationMallocFailedHook(void) {
    panic("FreeRTOS heap exhausted");
}
