#ifndef FAKE_ESP_SYSTEM_H
#define FAKE_ESP_SYSTEM_H

#include "esp_err.h"

typedef enum {
  ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO
} esp_reset_reason_t;

void esp_restart(void) __attribute__((noreturn));
esp_reset_reason_t esp_reset_reason(void);

#endif
