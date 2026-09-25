/*
  radio_timing.h / radio_timing.cpp
  ---------------------------------
  Единственото място, откъдето се извеждат ВСИЧКИ времена на радио слоя от текущите
  SF/BW: символно време, Time-on-Air (Semtech AN1200.13 формула), CAD timeout, стъпка на
  сондиране, изчакване за телеметрия и ACK timeout-и. SF/BW идват от EEPROM (AVR) / NVS
  (Gateway) при старт - няма нищо хардкодвано в милисекунди.

  Чиста логика (без LoRa/SPI зависимости) - затова се тества нативно (tests/native).
  Идентично копие във всяка от папките sensor/, executor/, repeater/, gateway/.

  LDRO (Low Data Rate Optimization) е нужна при символно време Ts > 16 ms. Библиотеката LoRa
  смята Ts с целочислено деление и на SF11/BW125 (Ts=16.384 ms) греши -> LDRO остава изключена.
  Затова ние смятаме сами (radioLdroNeeded) и задаваме бита изрично (виж cad.cpp).
*/
#ifndef RADIO_TIMING_H
#define RADIO_TIMING_H

#include <Arduino.h>

#define RADIO_SF_MIN        7
#define RADIO_SF_MAX        12
#define RADIO_SF_DEFAULT    7
#define RADIO_BW_DEFAULT_HZ 125000UL

// BW индекс, както се пази в EEPROM (адрес 96): 0=62.5k, 1=125k, 2=250k.
// 0xFF (изтрит EEPROM) или друга стойност -> default (125k).
#define RADIO_BW_INDEX_COUNT 3

// Размери на пакетите, върху които се смятат времената (wire байтове, вкл. CRYPTO_OVERHEAD)
#define RADIO_MAX_PAYLOAD_LEN   64   // state response с 10 консуматора - най-дългият пакет
#define RADIO_TELEMETRY_LEN     30   // sensor данни
#define RADIO_CMD_WIRE_LEN      26   // команда Gateway -> Executor
#define RADIO_ACK_WIRE_LEN      20   // ACK/NACK, state request, state-resp ACK

// Пауза на Executor преди ACK (half-duplex: Gateway трябва да излезе от TX и да влезе в RX)
#define RADIO_EXEC_ACK_DELAY_MS 500
// Груба обработка на отсрещната страна (декриптиране, публикуване в MQTT и т.н.)
#define RADIO_PROCESSING_MS     200

bool     radioSfValid(uint8_t sf);
uint32_t radioBwFromIndex(uint8_t idx);      // 0 при невалиден индекс
uint8_t  radioBwToIndex(uint32_t bwHz);      // 0xFF при неподдържан BW

// Задава текущите параметри; невалидни sf/bw се заменят с default.
// crcOn: дали радио-ниво CRC е включен (в системата - винаги true).
void radioTimingInit(uint8_t sf, uint32_t bwHz, uint8_t crDenom, uint8_t preambleLen, bool crcOn);

uint8_t  radioSf();
uint32_t radioBw();
uint8_t  radioCrDenom();
uint8_t  radioPreambleLen();

uint32_t radioSymbolUs();                    // Ts в микросекунди
bool     radioLdroNeeded();                  // Ts > 16 ms
uint32_t radioToaMs(uint8_t payloadLen);     // Time-on-Air, закръглен НАГОРЕ до ms

uint32_t radioCadTimeoutMs();                // 2 x (2^SF+32)/BW + 5 ms
uint32_t radioCadStepMs();                   // max(CAD timeout, 4 x Ts) - стъпка на сондиране/слот
uint32_t radioSensingWindowMs();             // toa(64 B) - най-дългото възможно чуждо предаване
uint32_t radioTelemetryWaitMs();             // toa(30 B)
uint32_t radioSlotMs();                      // слот на случайния backoff (= стъпката)

// ACK timeout-и (без долна граница - на ниско SF са къси, на високо SF растат)
uint32_t radioAckTimeoutCmdMs();             // команда -> ACK
uint32_t radioAckTimeoutRestartMs();         // restart state response -> ACK
uint32_t radioStateReqTimeoutMs();           // state request -> state response

#endif
