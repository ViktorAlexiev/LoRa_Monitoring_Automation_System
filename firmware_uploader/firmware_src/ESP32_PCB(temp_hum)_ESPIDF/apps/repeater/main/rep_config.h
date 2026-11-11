#ifndef REP_CONFIG_H
#define REP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// Default честоти - fallback ако NVS е празен
#define LORA_FREQ_RX_DEFAULT_HZ  UINT32_C(434000000)
#define LORA_FREQ_TX_DEFAULT_HZ  UINT32_C(433000000)

struct RepConfig {
  char id[MODULE_ID_LEN + 1];
  uint32_t freqRxHz;     // лентата на подателите (sensor-и / repeater-и от по-далечното ниво)
  uint32_t freqTxHz;     // лентата по-близо до Gateway
  uint8_t sf;
  uint32_t bwHz;
  int8_t txPowerDbm;     // NVS "txp" (-9..12), по подразбиране LORA_TX_POWER_DBM; радиото го ограничава до модула и регулатора (12 dBm за 433 MHz)
  uint8_t key[CRYPTO_KEY_LEN];
  bool hasKey;
  bool hasId;
  bool hasBattery;       // NVS "bat": heartbeat-ът носи mV (16 B вместо 14 B)
};

// NVS (namespace "cfg"): id, freq (RX), freqtx (TX), sf, bw, key
void rep_config_load(RepConfig *c);

#endif
