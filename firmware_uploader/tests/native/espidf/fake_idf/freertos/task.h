#ifndef FAKE_FREERTOS_TASK_H
#define FAKE_FREERTOS_TASK_H

#include "FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stackBytes, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stackBytes, void *arg,
                       UBaseType_t prio, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t t);
void taskYIELD(void);

uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticksToWait);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *higherPriorityTaskWoken);

#endif
