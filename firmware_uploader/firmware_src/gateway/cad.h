#ifndef CAD_H
#define CAD_H

#include <Arduino.h>

// ---------------- CAD (Channel Activity Detection) + LDRO - SX127x register-level ----------------
// Изисква readRegister/writeRegister да са public в LoRa.h (патчнато ръчно).
// Идентично копие във всяко устройство (sensor/executor/repeater/gateway).
//
// CAD timeout НЕ е константа - зависи от SF/BW (radioCadTimeoutMs() в radio_timing.h).
#define REG_OP_MODE        0x01
#define REG_IRQ_FLAGS      0x12
#define REG_MODEM_CONFIG_3 0x26
#define OP_MODE_LORA_CAD   0x87   // LongRangeMode бит + CAD mode (0b111)
#define IRQ_CAD_DONE       0x04
#define IRQ_CAD_DETECTED   0x01
#define MODEM_CONFIG_3_LDRO 0x08  // бит 3 - LowDataRateOptimize

// true = каналът е зает точно сега (не предавай)
bool channelActive();

// Задава/маха Low Data Rate Optimization директно в RegModemConfig3.
void radioSetLowDataRateOptimize(bool on);

// Прилага SF/BW/CR/преамбюл от radio_timing (radioTimingInit трябва да е викнат преди това)
// и изрично задава LDRO по РЕАЛНОТО символно време (библиотеката греши на SF11/BW125).
// Вика се СЛЕД LoRa.begin().
void radioApplyModemSettings();

#endif
