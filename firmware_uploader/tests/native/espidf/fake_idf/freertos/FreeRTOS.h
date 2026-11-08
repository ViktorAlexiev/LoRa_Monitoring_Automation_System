// Заместител на FreeRTOS.h за хост тестовете (виж sim_rtos.cpp - симулатор във виртуално време).
#ifndef FAKE_FREERTOS_H
#define FAKE_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0
#define portMAX_DELAY 0xFFFFFFFFu
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY 0x7FFFFFFF
#define configMAX_PRIORITIES 25
#define configMINIMAL_STACK_SIZE 768
#define portNUM_PROCESSORS 2

// ISR-контекстът в симулатора не превключва задачи
#define portYIELD_FROM_ISR(...) do {} while (0)

#endif
