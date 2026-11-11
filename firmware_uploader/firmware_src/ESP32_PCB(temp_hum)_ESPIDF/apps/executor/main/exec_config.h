#ifndef EXEC_CONFIG_H
#define EXEC_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "consumers.h"
#include "protocol.h"

struct ExecConfig {
  char id[MODULE_ID_LEN + 1];
  uint32_t freqHz;
  uint8_t sf;
  uint32_t bwHz;
  int8_t txPowerDbm;     // NVS "txp" (-9..12), по подразбиране LORA_TX_POWER_DBM; радиото го ограничава до модула и регулатора (12 dBm за 433 MHz)
  uint8_t key[CRYPTO_KEY_LEN];
  bool hasKey;
  bool hasId;
  bool hasBattery;       // NVS "bat": heartbeat-ът носи mV (16 B вместо 14 B)
  ConsumerSet consumers;
};

// NVS (namespace "cfg"): id, nc, c<i>i (ID), c<i>p (GPIO), freq, sf, bw, key
void exec_config_load(ExecConfig *c);

#endif
