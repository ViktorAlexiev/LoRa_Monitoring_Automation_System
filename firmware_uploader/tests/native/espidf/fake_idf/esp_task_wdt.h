#ifndef FAKE_ESP_TASK_WDT_H
#define FAKE_ESP_TASK_WDT_H

// Заместител на Task Watchdog на ESP-IDF 5.x (esp_task_wdt.h). Симулаторът следи абонираните задачи във виртуално
// време; при изтичане: ред в лога, а с trigger_panic - рестарт на системата (причина ESP_RST_TASK_WDT).
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
  uint32_t timeout_ms;
  uint32_t idle_core_mask;
  bool trigger_panic;
} esp_task_wdt_config_t;

esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *config);          // ESP_ERR_INVALID_STATE ако вече е стартиран
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *config);   // ESP_ERR_INVALID_STATE ако не е стартиран
esp_err_t esp_task_wdt_add(TaskHandle_t task_handle);                      // NULL = текущата задача
esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_reset(void);                                        // ESP_ERR_NOT_FOUND, ако текущата задача не е абонирана

#endif
