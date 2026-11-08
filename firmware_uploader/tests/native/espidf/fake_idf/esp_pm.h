#ifndef FAKE_ESP_PM_H
#define FAKE_ESP_PM_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
  int max_freq_mhz;
  int min_freq_mhz;
  bool light_sleep_enable;
} esp_pm_config_t;

esp_err_t esp_pm_configure(const void *config);

#endif
