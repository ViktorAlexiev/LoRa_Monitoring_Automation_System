#ifndef GW_CONFIG_H
#define GW_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "cfg_limits.h"
#include "protocol.h"

struct GwConfig {
  char id[MODULE_ID_LEN + 1];
  char wifiSsid[CFG_WIFI_SSID_LEN + 1];
  char wifiPass[CFG_WIFI_PASSWORD_LEN + 1];
  char mqttHost[CFG_MQTT_IP_LEN + 1];
  uint16_t mqttPort;
  char mqttUser[CFG_MQTT_USER_LEN + 1];
  char mqttPass[CFG_MQTT_PASSWORD_LEN + 1];
  uint32_t freqHz;
  uint8_t sf;
  uint32_t bwHz;
  int8_t txPowerDbm;     // NVS "txp" (-9..12), по подразбиране LORA_TX_POWER_DBM; радиото го ограничава до модула и регулатора (12 dBm за 433 MHz)
  uint8_t key[CRYPTO_KEY_LEN];
  bool hasKey;
  bool hasId;
  bool hasNet;           // има wifi ssid и mqtt адрес
};

// NVS (namespace "cfg"): id, wssid, wpass, mqip, mqport (низ!), mquser, mqpass, freq, sf, bw, key; cmdceil (nonce watermark)
void gw_config_load(GwConfig *c);

#endif
