#pragma once
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(size_t count, size_t size);
int xQueueSend(QueueHandle_t, const void *, unsigned);
int xQueueReceive(QueueHandle_t, void *, unsigned);
unsigned uxQueueSpacesAvailable(QueueHandle_t);
