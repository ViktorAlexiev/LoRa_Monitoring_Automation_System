#ifndef FAKE_ESP_SLEEP_H
#define FAKE_ESP_SLEEP_H

#include <stdint.h>
#include "esp_err.h"

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t time_in_us);
esp_err_t esp_sleep_enable_gpio_wakeup(void);
void esp_deep_sleep_start(void) __attribute__((noreturn));
esp_err_t esp_light_sleep_start(void);   // прошивките НЕ го викат (автоматичен лек сън) - тестът го брои

#endif
