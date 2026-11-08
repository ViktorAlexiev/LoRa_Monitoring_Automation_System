#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>

// Сериен дебъг. AGRO_SERIAL=1 (по подразбиране, както досега) = всички съобщения; 0 = продукционен билд без Serial:
// спестява flash, ~170 B RAM (обектът и двата 64-B буфера на HardwareSerial) и блокирането при пълен TX буфер
// (115200 baud = ~87 us на байт, ред от 100 B = ~9 ms). Задава се с -DAGRO_SERIAL=0.
#ifndef AGRO_SERIAL
#define AGRO_SERIAL 1
#endif

#if AGRO_SERIAL
#define DBG_BEGIN(baud) Serial.begin(baud)
#define DBG(...)        Serial.print(__VA_ARGS__)
#define DBGLN(...)      Serial.println(__VA_ARGS__)
#define DBG_FLUSH()     Serial.flush()
#else
#define DBG_BEGIN(baud) ((void)0)
#define DBG(...)        ((void)0)
#define DBGLN(...)      ((void)0)
#define DBG_FLUSH()     ((void)0)
#endif

#endif
