#ifndef FAKE_DRIVER_GPIO_H
#define FAKE_DRIVER_GPIO_H

#include <stdint.h>
#include "esp_err.h"

// enum, както в реалния IDF: int не се преобразува неявно (нужен е (gpio_num_t) каст)
typedef enum : int { GPIO_NUM_NC = -1, GPIO_NUM_0 = 0, GPIO_NUM_1 = 1 } gpio_num_t;

typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2, GPIO_MODE_INPUT_OUTPUT = 3 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum {
  GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE = 1, GPIO_INTR_NEGEDGE = 2, GPIO_INTR_ANYEDGE = 3,
  GPIO_INTR_LOW_LEVEL = 4, GPIO_INTR_HIGH_LEVEL = 5
} gpio_int_type_t;

typedef struct {
  uint64_t pin_bit_mask;
  gpio_mode_t mode;
  gpio_pullup_t pull_up_en;
  gpio_pulldown_t pull_down_en;
  gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *arg);

typedef enum { GPIO_PULLUP_ONLY = 0, GPIO_PULLDOWN_ONLY = 1, GPIO_PULLUP_PULLDOWN = 2, GPIO_FLOATING = 3 } gpio_pull_mode_t;
#define ESP_INTR_FLAG_IRAM (1 << 10)

esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t mode);

#endif
