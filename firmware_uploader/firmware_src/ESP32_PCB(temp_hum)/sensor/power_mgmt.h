#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <Arduino.h>

// ~15 минути между измерванията: 113 x 8s = 904s = 15.07 мин (същата стойност като при ATmega;
// не е критично да е точно 15 - heartbeat-подобен интервал, не изисква прецизен timing).
#define WDT_CYCLES_BETWEEN_SEND  113

// Дълбок сън (esp_deep_sleep) за cycles x 8 секунди, събуждане по таймер (RTC). ФУНКЦИЯТА НЕ
// ВРЪЩА: при събуждане чипът стартира наново от setup() - затова броячът на nonce е в RTC
// паметта (виж ceiling_counter.cpp). Радиото трябва да е заспало преди повикването.
void deep_sleep(uint8_t cycles);

#endif
