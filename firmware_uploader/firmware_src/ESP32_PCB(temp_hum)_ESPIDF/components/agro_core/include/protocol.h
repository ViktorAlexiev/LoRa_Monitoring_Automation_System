/*
  protocol.h - константи на радио/командния протокол, общи за всички устройства.
  Стойностите са ИДЕНТИЧНИ със старата Arduino версия (LoRa_radio_node+TTGO / ESP32_PCB(temp_hum)).
*/
#ifndef AGRO_PROTOCOL_H
#define AGRO_PROTOCOL_H

#include <stdint.h>
#include "crypto_common.h"

#define MODULE_ID_LEN      6
#define CONSUMER_ID_LEN    4
#define MAX_CONSUMERS      10

// ---------- LoRa параметри (SF и BW идват от NVS) ----------
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   12      // по подразбиране (433 MHz: 10 mW ERP); настройка "txp" в NVS, ограничена до LORA_TX_POWER_LIMIT_DBM (board_pins.h)
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD      0x12
#define LORA_FREQ_DEFAULT_HZ UINT32_C(433000000)

// ---------- Sensor пакет: 4 x int16 little-endian (S_T,S_H,A_T,A_H), стойност = x * 100 ----------
// SENSOR_RAW_NODATA (-32768) е единственото число за "няма данни" (грешка при четене или липсващ сензор).
// Gateway го публикува в MQTT като -32768 (без деление на 100).
#define SENSOR_PT_LEN        8
#define SENSOR_WIRE_LEN      (CRYPTO_OVERHEAD + SENSOR_PT_LEN)      // 22
#define SENSOR_RAW_NODATA    (-32768)
#define SENSOR_NODATA_JSON   (-32768)

// float -> int16 x100 със закръгляне (както в Arduino версията)
static inline int16_t sensorEncodeCompact(float v) {
  return (int16_t)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
}

// ---------- Батерия (по желание, само ESP32 PCB с делител): mV като int16 little-endian ----------
// BATTERY_RAW_NODATA (-32768) = няма измерване (същото число като SENSOR_RAW_NODATA). Устройства БЕЗ батерия (всички AVR и
// ESP32 без опцията) пращат СТАРИТЕ пакети; gateway-ят разпознава и двата формата по дължина и за старите слага
// BATTERY_RAW_NODATA в полето "battery_mv" на JSON-а.
#define BATTERY_RAW_NODATA   (-32768)
#define BATTERY_NODATA_JSON  (-32768)
#define HB_BAT_PT_LEN        2                                          // heartbeat с батерия: само mV
#define SENSOR_BAT_PT_LEN    (SENSOR_PT_LEN + 2)                        // sensor с батерия: 4 x int16 + mV = 10

// ---------- Дължини на пакетите по въздуха (wire) ----------
#define HB_WIRE_LEN              (CRYPTO_OVERHEAD)                  // 14 (repeater/executor heartbeat, state response с 0 консуматора)
#define ACK_PT_LEN               6                                  // C_ID(4) + com(1) + status(1)
#define ACK_WIRE_LEN             (CRYPTO_OVERHEAD + ACK_PT_LEN)     // 20
#define CMD_PLAINTEXT_LEN        6
#define CMD_WIRE_LEN             (MODULE_ID_LEN + CRYPTO_OVERHEAD + CMD_PLAINTEXT_LEN)   // 26 (target M_ID + wire)
#define STATE_REQ_WIRE_LEN       (MODULE_ID_LEN + CRYPTO_OVERHEAD)  // 20 (state request и state-resp ACK; разграничават се по type-id)
#define STATE_ENTRY_LEN          5                                  // C_ID(4) + state(1) на консуматор
#define HB_BAT_WIRE_LEN          (CRYPTO_OVERHEAD + HB_BAT_PT_LEN)          // 16 (repeater/executor heartbeat с батерия)
#define SENSOR_BAT_WIRE_LEN      (CRYPTO_OVERHEAD + SENSOR_BAT_PT_LEN)      // 24 (sensor с батерия)

// Gateway-ят разпознава пакетите по ДЪЛЖИНА (type-id не пътува по въздуха). Дължини в uplink-а:
//   14 heartbeat (и state response с 0 консуматора) | 16 heartbeat с батерия | 19,24,29..64 state response (14 + 5n)
//   20 ACK | 22 sensor | 24 sensor с батерия.
// ЕДИНСТВЕНОТО съвпадение е 24 = sensor с батерия = state response от 2 консуматора: разграничава се с MAC (type-id е в
// nonce-а, чужд тип не се проверява успешно - вероятност 2^-32). 14 е същото съвпадение като досега (heartbeat/state(0)).
static_assert(HB_BAT_WIRE_LEN == 16 && SENSOR_BAT_WIRE_LEN == 24, "дължините на пакетите с батерия");
static_assert(HB_BAT_WIRE_LEN != ACK_WIRE_LEN && HB_BAT_WIRE_LEN != SENSOR_WIRE_LEN && HB_BAT_WIRE_LEN != HB_WIRE_LEN,
              "heartbeat с батерия не съвпада с друг uplink пакет");
static_assert(HB_BAT_WIRE_LEN != CRYPTO_OVERHEAD + STATE_ENTRY_LEN * 1 && HB_BAT_WIRE_LEN != CRYPTO_OVERHEAD + STATE_ENTRY_LEN * 2,
              "heartbeat с батерия не е state response");
static_assert(SENSOR_BAT_WIRE_LEN == CRYPTO_OVERHEAD + STATE_ENTRY_LEN * 2,
              "съвпада със state response от 2 консуматора - разграничава се по MAC (виж коментара)");
static_assert(SENSOR_BAT_WIRE_LEN != ACK_WIRE_LEN && SENSOR_BAT_WIRE_LEN != SENSOR_WIRE_LEN, "sensor с батерия не съвпада с ACK/стария sensor");

// ---------- Команди и статуси ----------
#define CMD_ON         0xA1
#define CMD_OFF        0xB2
#define CMD_STATE_REQ  0xC3        // само в wifi_commands (без C_ID)
#define STATUS_ACK     0
#define STATUS_TIMEOUT 1
#define STATUS_NACK    2

// Точно две шестнадесетични цифри ("A1", "b2") -> байт. false при друга дължина или знак: без каст от по-голямо число
// ("1A1" не става 0xA1) и без мълчаливо игнориране на остатъка ("A1xyz").
static inline bool parseHexByte(const char *s, uint8_t *out) {
  uint8_t v = 0;
  for (uint8_t i = 0; i < 2; i++) {
    char c = s[i];
    uint8_t d;
    if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
    else return false;
    v = (uint8_t)((v << 4) | d);
  }
  if (s[2] != 0) return false;
  *out = v;
  return true;
}

// ---------- Времена ----------
// HB на всеки HB_INTERVAL_CYCLES "тика" по 8 s = 304 s (~5 мин; "жив съм" сигнал, не е нужна точност)
#define TICK_MS             UINT32_C(8000)
#define HB_INTERVAL_CYCLES  38
#define HB_INTERVAL_MS      (HB_INTERVAL_CYCLES * TICK_MS)
// Първият heartbeat е на СЛУЧАЙНО време в (HB_FIRST_MIN_MS, HB_INTERVAL_MS], после на всеки HB_INTERVAL_MS: устройства, пуснати
// едновременно (напр. след спиране на тока), иначе остават синхронизирани завинаги (кварцовите часовници не се
// разминават) и heartbeat-ите им се сблъскват/пропускат на всяка стъпка. Всеки има и jitter до JITTER_MAX_MS.
#define HB_FIRST_MIN_MS     UINT32_C(1000)
#define JITTER_MAX_MS       1000   // случайно закъснение преди предаване - разминава колизии между устройства

#endif
