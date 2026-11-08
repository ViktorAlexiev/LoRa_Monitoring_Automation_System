// Заместител на esp_system.h - само esp_reset_reason(), контролируем от теста.
#ifndef FAKE_ESP_SYSTEM_H
#define FAKE_ESP_SYSTEM_H

enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_SW, ESP_RST_DEEPSLEEP };
extern esp_reset_reason_t g_resetReason;
inline esp_reset_reason_t esp_reset_reason() { return g_resetReason; }

#endif
