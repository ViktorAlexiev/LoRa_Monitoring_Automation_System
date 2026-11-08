#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

struct SensorConfig {
  char id[MODULE_ID_LEN + 1];
  uint32_t freqHz;
  uint8_t sf;
  uint32_t bwHz;
  int8_t txPowerDbm;     // NVS "txp" (-9..22), по подразбиране LORA_TX_POWER_DBM; радиото го ограничава до максимума на модула
  uint8_t key[CRYPTO_KEY_LEN];
  bool hasKey;
  bool hasId;
  bool hasBattery;       // NVS "bat" (1 = има делител за батерията): sensor пакетът носи mV
};

// Чете настройките от NVS (namespace "cfg"), записани от config приложението (stage 1):
//   id (string), freq (u32 Hz), sf (u8), bw (u32 Hz), key (blob 16 B)
// Липсващи/невалидни полета -> безопасни стойности по подразбиране.
void sensor_config_load(SensorConfig *c);

#endif
