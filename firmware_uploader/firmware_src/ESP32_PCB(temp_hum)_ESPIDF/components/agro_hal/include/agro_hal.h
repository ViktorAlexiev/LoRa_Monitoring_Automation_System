/*
  agro_hal.h - тънък слой над ESP-IDF (време, закъснения, случайни числа, GPIO, рестарт, NVS init,
  прекъсване на пин -> известяване на задача).
  ---------------------------------------------------------------------------------------
  Целта е останалата логика (radio_timing, channel_access, протоколите) да не знае за
  ESP-IDF API-то и да се тества на хост машина срещу заместители на IDF (виж tests/native/espidf).
  Няма Arduino: само ESP-IDF и FreeRTOS.
*/
#ifndef AGRO_HAL_H
#define AGRO_HAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------------- Време ----------------
// Милисекунди от старта (esp_timer; продължава да върви и през автоматичния лек сън).
uint32_t hal_millis(void);
uint64_t hal_micros(void);

// delay >= 1 ms отстъпва процесора на други задачи (vTaskDelay); по-кратките са активно изчакване
void hal_delay_ms(uint32_t ms);
void hal_delay_us(uint32_t us);

// ---------------- Случайни числа (хардуерен RNG) ----------------
uint32_t hal_random(uint32_t max_excl);                  // равномерно в [0, max_excl); 0 при max <= 1
uint32_t hal_random_range(uint32_t lo, uint32_t hi_excl);

// ---------------- GPIO (консуматори, RST на радиото) ----------------
// Изход, който може да се чете обратно (INPUT_OUTPUT) - състоянието на консуматора се чете от пина.
void hal_gpio_output(int32_t pin, bool level);
void hal_gpio_write(int32_t pin, bool level);
bool hal_gpio_read(int32_t pin);

// ---------------- Прекъсване на пин -> известяване на задача (DIO1 на радиото) ----------------
// Ниво HIGH, не фронт: ако пинът още е висок (непрочетен пакет), прекъсването се повтаря и няма
// изгубено събитие. ISR-ът само забранява прекъсването и праща notify на задачата; задачата
// извиква hal_irq_rearm() СЛЕД като е прочела радиото (DIO1 пада, когато IRQ флаговете се изчистят).
// Същият пин е и източник за събуждане от автоматичния лек сън (виж power.h).
bool hal_irq_attach(int32_t pin, TaskHandle_t task);
void hal_irq_rearm(int32_t pin);
// Временно спира прекъсването (напр. докато радиото предава или прави CAD и сами следим флаговете му); безопасно и
// за пин, който не е закачан. Връща се с hal_irq_rearm().
void hal_irq_disable(int32_t pin);

// ---------------- Проба на стека (диагностика) ----------------
// 1 = всяка задача вика HAL_STACK_PROBE() на обиколка; при нов най-нисък остатък на стека логва "стек <задача>: най-малко
// свободно N B". Пусни най-тежкия сценарий (state response с 10 консуматора, MQTT reconnect, запис в NVS) и прочети
// лога: под ~500 B свободно стекът на задачата трябва да се увеличи. Изключено (0) = нулева цена.
#ifndef AGRO_STACK_PROBE
#define AGRO_STACK_PROBE 0
#endif
#if AGRO_STACK_PROBE
void hal_stack_probe(void);
#define HAL_STACK_PROBE() hal_stack_probe()
#else
#define HAL_STACK_PROBE() ((void)0)
#endif

// ---------------- Система ----------------
bool hal_nvs_init(void);                    // nvs_flash_init (с изтриване при нова версия/пълно)
void hal_restart(void) __attribute__((noreturn));
// Невъзстановима грешка при старт (няма памет за опашка/задача, мрежата не тръгва, прекъсването не се закача):
// логва причината, изчаква 2 s (да излезе редът и да няма бърз цикъл на рестарти) и рестартира - вместо
// устройството да остане тихо "живо" без функция.
void hal_fatal_restart(const char *what) __attribute__((noreturn));
bool hal_woke_from_deep_sleep(void);        // причина на рестарта = събуждане от дълбок сън

#endif
