/*
 * TTGO LoRa32 Gateway -> MQTT
 * Topics IN : sensors / heartbeat / commands_status / module_states_response
 * Topics OUT(sub): commands / module_states_requests
 */

#include <LoRa.h>
#include "crypto_common.h"
#include "config_storage.h"
#include "wifi_mqtt.h"
#include "lora_handlers.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  loadConfigFromNvs();

  lora_radio_setup();
  mqtt_wifi_setup();
}

void loop() {
  wifiMaintain();
  mqttMaintain();
  mqttClientLoop();

  int packetSize = LoRa.parsePacket();
  if (packetSize) lora_handle_incoming(packetSize);

  lora_managers_tick();
  gatewayHeartbeatTick();
}
