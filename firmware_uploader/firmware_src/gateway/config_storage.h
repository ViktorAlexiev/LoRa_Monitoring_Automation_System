#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>
#include "crypto_common.h"

#define MODULE_ID_LEN       6
#define WIFI_SSID_LEN        32
#define WIFI_PASSWORD_LEN    63
#define MQTT_IP_LEN          64
#define MQTT_USER_LEN        32
#define MQTT_PASSWORD_LEN    63

// ---------- Конфигурация от NVS (записана от config_esp32.ino при setup) ----------
extern char GATEWAY_ID[MODULE_ID_LEN + 1];
extern char WIFI_SSID_BUF[WIFI_SSID_LEN + 1];
extern char WIFI_PASSWORD_BUF[WIFI_PASSWORD_LEN + 1];
extern char MQTT_IP_BUF[MQTT_IP_LEN + 1];
extern uint16_t MQTT_PORT_VAL;
extern char MQTT_USER_BUF[MQTT_USER_LEN + 1];
extern char MQTT_PASSWORD_BUF[MQTT_PASSWORD_LEN + 1];
extern uint32_t LORA_FREQ_HZ;
extern uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];

void loadConfigFromNvs();

// NVS ceiling watermark за изходящия nonce поток на Gateway - ЕДИН общ за команди, state
// request и state-resp ACK към ВСИЧКИ executor-и (type-id-то във всеки разграничава потока,
// значи споделен counter не създава nonce reuse между тях).
uint32_t cmdCeilingNext();

#endif
