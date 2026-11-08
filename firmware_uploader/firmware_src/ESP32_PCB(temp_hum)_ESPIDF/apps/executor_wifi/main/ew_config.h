#ifndef EW_CONFIG_H
#define EW_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "cfg_limits.h"
#include "consumers.h"
#include "protocol.h"

struct EwConfig {
  char id[MODULE_ID_LEN + 1];
  ConsumerSet consumers;
  char wifiSsid[CFG_WIFI_SSID_LEN + 1];
  char wifiPass[CFG_WIFI_PASSWORD_LEN + 1];
  char mqttHost[CFG_MQTT_IP_LEN + 1];
  uint16_t mqttPort;
  char mqttUser[CFG_MQTT_USER_LEN + 1];
  char mqttPass[CFG_MQTT_PASSWORD_LEN + 1];
  bool hasId;
  bool hasNet;
};

// NVS (namespace "cfg"): id, nc, c<i>i, c<i>p, wssid, wpass, mqip, mqport (низ), mquser, mqpass
void ew_config_load(EwConfig *c);

#endif
