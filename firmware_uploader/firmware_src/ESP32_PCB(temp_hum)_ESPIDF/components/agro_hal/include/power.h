/*
  power.h - управление на захранването.

  Sensor:           дълбок сън (esp_deep_sleep_start) - чипът се рестартира при събуждане.
  Executor/Repeater: АВТОМАТИЧЕН лек сън. Няма ръчно извикване на esp_light_sleep_start(): когато
                     всички задачи са блокирани (чакат опашка/notify/таймер), FreeRTOS tickless idle
                     + Power Management приспиват чипа и го будят от (а) DIO1 на радиото (пин, ниво
                     HIGH), (б) най-близкия таймер/таймаут на задача. RAM, GPIO и радиото запазват
                     състоянието си. Изисква CONFIG_PM_ENABLE=y и CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
                     (виж sdkconfig.defaults на приложението).
*/
#ifndef AGRO_POWER_H
#define AGRO_POWER_H

#include <stdint.h>
#include <stdbool.h>

// Включва автоматичния лек сън и регистрира wake_gpio като източник за събуждане (HIGH ниво).
// Връща false, ако Power Management не е компилиран в прошивката (тогава системата просто не спи).
bool power_enable_auto_light_sleep(int32_t wake_gpio);

// Дълбок сън за us микросекунди (събуждане по RTC таймер). НЕ ВРЪЩА - при събуждане чипът
// стартира от app_main() наново.
void power_deep_sleep_us(uint64_t us) __attribute__((noreturn));

#endif
