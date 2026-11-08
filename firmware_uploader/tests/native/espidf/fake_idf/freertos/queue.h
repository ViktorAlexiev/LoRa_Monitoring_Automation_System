#ifndef FAKE_FREERTOS_QUEUE_H
#define FAKE_FREERTOS_QUEUE_H

#include "FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticksToWait);
BaseType_t xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t ticksToWait);
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *higherPriorityTaskWoken);
BaseType_t xQueueReceive(QueueHandle_t q, void *buf, TickType_t ticksToWait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
BaseType_t xQueueReset(QueueHandle_t q);

#endif
