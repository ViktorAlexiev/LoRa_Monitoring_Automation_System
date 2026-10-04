#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <Arduino.h>

// HB на всеки HB_INTERVAL_CYCLES watchdog цикъла (~8s всеки) - 4 × 8s ≈ 32s (вместо 30s точно,
// не е критично - heartbeat е "жив съм" сигнал, не се нуждае от прецизен timing).
#define HB_INTERVAL_CYCLES  4

extern volatile uint8_t wdt_ticks;

void wdt_arm_8s_continuous();
void enablePinChangeWake();

// Дълбок сън - буди се от watchdog (~8s, HB разписание) ИЛИ мигновено от PCINT (DIO0,
// входящ пакет). Извиква се само когато няма чакащи forward-и/HB.
void deep_sleep_until_event();

#endif
