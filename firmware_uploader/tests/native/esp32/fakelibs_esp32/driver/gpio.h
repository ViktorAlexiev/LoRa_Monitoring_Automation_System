#ifndef FAKE_DRIVER_GPIO_H
#define FAKE_DRIVER_GPIO_H

typedef int gpio_num_t;
enum gpio_int_type_t { GPIO_INTR_DISABLE = 0, GPIO_INTR_LOW_LEVEL = 4, GPIO_INTR_HIGH_LEVEL = 5 };

extern int g_wakeGpio;
extern int g_wakeGpioLevel;
inline int gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t t) { g_wakeGpio = pin; g_wakeGpioLevel = (int)t; return 0; }

#endif
