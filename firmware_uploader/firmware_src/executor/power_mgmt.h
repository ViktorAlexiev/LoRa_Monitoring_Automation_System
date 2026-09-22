#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <Arduino.h>

// HB на всеки HB_INTERVAL_CYCLES watchdog цикъла (~8s всеки) - 38 × 8s ≈ 304s ≈ 5 мин
// (вместо точно 5 мин - не е критично, heartbeat е "жив съм" сигнал).
#define HB_INTERVAL_CYCLES  38

extern volatile uint8_t wdt_ticks;

void wdt_arm_8s_continuous();
void enablePinChangeWake();

// Дълбок сън - буди се от watchdog (~8s, HB разписание) ИЛИ мигновено от PCINT (DIO0,
// входяща команда/state request). Извиква се само когато няма чакащи ACK/RX/HB.
void deep_sleep_until_event();

#endif
