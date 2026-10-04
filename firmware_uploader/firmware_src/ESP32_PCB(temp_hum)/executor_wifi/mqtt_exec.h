#ifndef MQTT_EXEC_H
#define MQTT_EXEC_H

#include <Arduino.h>

#define WIFI_RETRY_TIMEOUT_MS    (2UL*60UL*1000UL)
#define MQTT_RETRY_TIMEOUT_MS    (2UL*60UL*1000UL)
#define RECONNECT_ATTEMPT_MS     5000UL
#define EXEC_HEARTBEAT_INTERVAL_MS (30UL*1000UL)

// Команден протокол - същите кодове като при LoRa executor-а
#define CMD_ON         0xA1
#define CMD_OFF        0xB2
#define CMD_STATE_REQ  0xC3   // заявка за състоянията (само в wifi_commands, без C_ID)
#define STATUS_ACK     0
#define STATUS_NACK    2

// ---------- MQTT теми ----------
// Към модула (единствената нова тема): команди и заявки за състояние от бекенда.
// Всички Wi-Fi executor-и са абонирани за нея и филтрират по M_ID. Gateway НЕ е абониран.
extern const char* TOPIC_WIFI_COMMANDS;      // "wifi_commands"
// От модула - същите общи теми и JSON като при gateway (бекендът не се променя):
extern const char* TOPIC_COMMANDS_STATUS;    // "commands_status"
extern const char* TOPIC_STATE_RESP;         // "module_states_response"
extern const char* TOPIC_HEARTBEAT;          // "heartbeat"

void mqtt_wifi_setup();       // WiFi.mode/begin + параметри на MQTT клиента
void wifiMaintain();          // неблокиращ reconnect; рестарт след WIFI_RETRY_TIMEOUT_MS
void mqttMaintain();          // неблокиращ reconnect (clean session, LWT); рестарт след MQTT_RETRY_TIMEOUT_MS
void mqttClientLoop();

// Изпълнява (по реда на пристигане) чакащите команди/заявки. Командите се изпълняват една по
// една; следващата тръгва от бекенда чак след отговора на предишната (опашката е в бекенда).
void processMessages();

void heartbeatTick();         // heartbeat на EXEC_HEARTBEAT_INTERVAL_MS, ако MQTT е свързан

#endif
