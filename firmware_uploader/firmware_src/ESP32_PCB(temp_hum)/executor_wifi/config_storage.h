#ifndef CONFIG_STORAGE_H
#define CONFIG_STORAGE_H

#include <Arduino.h>

#define MODULE_ID_LEN        6
#define CONSUMER_ID_LEN      4
#define MAX_CONSUMERS        10
#define WIFI_SSID_LEN        32
#define WIFI_PASSWORD_LEN    63
#define MQTT_IP_LEN          64
#define MQTT_USER_LEN        32
#define MQTT_PASSWORD_LEN    63

// ---------- Конфигурация от NVS (записана от config_esp32 при stage 1) ----------
extern char MY_M_ID[MODULE_ID_LEN + 1];
extern char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
extern uint8_t consumerPins[MAX_CONSUMERS];
extern uint8_t NUM_CONSUMERS;
extern char WIFI_SSID_BUF[WIFI_SSID_LEN + 1];
extern char WIFI_PASSWORD_BUF[WIFI_PASSWORD_LEN + 1];
extern char MQTT_IP_BUF[MQTT_IP_LEN + 1];
extern uint16_t MQTT_PORT_VAL;
extern char MQTT_USER_BUF[MQTT_USER_LEN + 1];
extern char MQTT_PASSWORD_BUF[MQTT_PASSWORD_LEN + 1];

// NVS ключове (namespace "cfg"): id, nc, c<i>i, c<i>p, wssid, wpass, mqip, mqport, mquser, mqpass
void loadConfigFromNvs();

// Индекс на консуматор по ID (4 символа) или -1
int consumerIndex(const char* c_id);

#endif
