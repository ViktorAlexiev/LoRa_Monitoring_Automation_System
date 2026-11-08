// Заместител на esp_sleep.h. Симулира лек/дълбок сън върху контролируемия системен часовник:
//  - esp_light_sleep_start() напредва времето до таймера, ИЛИ по-рано, ако тестът е задал
//    g_gpioWakeAfterUs (имитира входящ LoRa пакет на DIO0);
//  - esp_deep_sleep_start() само записва заявката (на реален чип не връща).
#ifndef FAKE_ESP_SLEEP_H
#define FAKE_ESP_SLEEP_H

#include <cstdint>

extern uint64_t g_fakeTimeUs;           // системен часовник (gettimeofday)
extern uint64_t g_timerWakeUs;          // последно зададен таймер
extern uint64_t g_gpioWakeAfterUs;      // 0 = няма събитие на DIO0
extern bool g_gpioWakeEnabled;
extern int g_lightSleepCount;
extern int g_lightSleepGpioWakes;
extern bool g_deepSleepRequested;
extern uint64_t g_deepSleepUs;

inline void esp_sleep_enable_timer_wakeup(uint64_t us) { g_timerWakeUs = us; }
inline void esp_sleep_enable_gpio_wakeup() { g_gpioWakeEnabled = true; }
void esp_light_sleep_start();
inline void esp_deep_sleep_start() { g_deepSleepRequested = true; g_deepSleepUs = g_timerWakeUs; }

#endif
