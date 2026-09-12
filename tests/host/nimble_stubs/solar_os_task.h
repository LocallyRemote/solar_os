#pragma once
#include "freertos/FreeRTOS.h"
#include <assert.h>
#define pdPASS 1
#define configASSERT(x) assert(x)
#define tskNO_AFFINITY 0
#define SOLAR_OS_TASK_ROLE_SYSTEM 0
typedef void *TaskHandle_t;
static inline int solar_os_task_create_pinned_internal(void (*fn)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *handle, int core, int role)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)core; (void)role;
    *handle = (void *)1;
    return pdPASS;
}
