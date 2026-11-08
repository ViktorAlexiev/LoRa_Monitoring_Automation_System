#ifndef FAKE_ESP_EVENT_H
#define FAKE_ESP_EVENT_H

#include <stdint.h>
#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

#define ESP_EVENT_DECLARE_BASE(b) extern esp_event_base_t b
#define ESP_EVENT_ANY_ID (-1)

ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
ESP_EVENT_DECLARE_BASE(IP_EVENT);

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t h, void *arg);

#endif
