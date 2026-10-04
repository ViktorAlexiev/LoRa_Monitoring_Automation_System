#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <Arduino.h>

// HB на всеки HB_INTERVAL_CYCLES "тика" (~8s всеки) - 38 x 8s = 304s = ~5 мин
// (вместо точно 5 мин - не е критично, heartbeat е "жив съм" сигнал).
#define HB_INTERVAL_CYCLES  38

// Брой изминали 8-секундни тика. Поддържа се от powerTicksPoll() по реално изминало време
// (включително времето, прекарано в лек сън), за да не "гладува" при чести събуждания от радиото.
extern volatile uint8_t wdt_ticks;

void wdt_arm_8s_continuous();   // стартира отброяването на тиковете
void enablePinChangeWake();     // позволява събуждане от лек сън по DIO0 на LoRa радиото
void powerTicksPoll();          // обновява wdt_ticks от изминалото време - вика се преди четене

// Лек сън (esp_light_sleep) - буди се от DIO0 (входящ пакет) ИЛИ при следващия 8-секунден тик
// (HB разписание). RAM, GPIO и радиото запазват състоянието си. Извиква се само когато няма
// чакащи ACK/RX/HB/TX.
void deep_sleep_until_event();

#endif
