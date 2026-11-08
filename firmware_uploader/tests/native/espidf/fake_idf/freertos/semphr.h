#ifndef FAKE_FREERTOS_SEMPHR_H
#define FAKE_FREERTOS_SEMPHR_H

#include "FreeRTOS.h"
#include "queue.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticksToWait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);

#endif
