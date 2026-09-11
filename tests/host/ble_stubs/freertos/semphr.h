#pragma once

#include "freertos/FreeRTOS.h"

typedef struct ble_test_semaphore *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
