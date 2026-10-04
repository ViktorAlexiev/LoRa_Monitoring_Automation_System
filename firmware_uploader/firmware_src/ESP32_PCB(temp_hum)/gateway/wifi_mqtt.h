#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include <Arduino.h>
#include <ArduinoJson.h>

#define WIFI_RETRY_TIMEOUT_MS   (2UL*60UL*1000UL)
#define MQTT_RETRY_TIMEOUT_MS   (2UL*60UL*1000UL)
#define RECONNECT_ATTEMPT_MS    5000UL
#define GW_HEARTBEAT_INTERVAL_MS (30UL*1000UL)

extern const char* TOPIC_SENSORS;
extern const char* TOPIC_HEARTBEAT;
extern const char* TOPIC_COMMANDS;
extern const char* TOPIC_COMMANDS_STATUS;
extern const char* TOPIC_STATE_REQ;
extern const char* TOPIC_STATE_RESP;

// Имплементирана в lora_handlers.cpp - реагира на MQTT съобщения по TOPIC_COMMANDS/TOPIC_STATE_REQ
// и праща съответните LoRa пакети. Регистрира се тук чрез mqttClient.setCallback().
void mqttCallback(char* topic, byte* payload, unsigned int length);

void mqtt_wifi_setup();       // WiFi.mode/begin
void wifiMaintain();
void mqttMaintain();
void mqttClientLoop();        // обвивка на mqttClient.loop()
bool mqttIsConnected();

void publishJson(const char* topic, JsonDocument& doc);
void sendGatewayHeartbeat();
void gatewayHeartbeatTick();  // вика sendGatewayHeartbeat() на GW_HEARTBEAT_INTERVAL_MS, ако MQTT е свързан

#endif
