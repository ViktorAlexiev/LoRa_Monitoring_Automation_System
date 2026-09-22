#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <Arduino.h>

// ~15 минути между измерванията: watchdog цикъл е ~8s, 113 × 8s ≈ 904s ≈ 15.07 мин
// (не е критично да е точно 15 - heartbeat-подобен интервал, не изисква прецизен timing).
#define WDT_CYCLES_BETWEEN_SEND  113

// Дълбок сън (PWR_DOWN) за посочения брой ~8s watchdog цикъла. Изключва ADC/USART
// през времетраенето, връща ги при събуждане.
void deep_sleep(uint8_t cycles);

#endif
