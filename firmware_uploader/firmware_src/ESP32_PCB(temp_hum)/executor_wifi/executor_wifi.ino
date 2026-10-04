/*
 * executor_wifi (ESP32_PCB) - изпълнителен модул, който приема команди директно по MQTT (Wi-Fi),
 * без LoRa. Захранва се постоянно (Wi-Fi е винаги включен), затова няма сън.
 *
 * Тема КЪМ модула (единствената нова): wifi_commands
 *   команда:           {"M_ID":"E007","C_ID":"V01","com":"A1"}   (A1 = включи, B2 = изключи)
 *   заявка за състояние: {"M_ID":"E007","com":"C3"}
 * Теми ОТ модула (същите като при gateway, формат непроменен): commands_status,
 *   module_states_response (след boot с "restart":true), heartbeat.
 * Командите са без retain и clean session - стари пропуснати команди не се доставят.
 * Опашката на командите е в бекенда: следващата тръгва след отговора на предишната.
 */

#include "config_storage.h"
#include "mqtt_exec.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  loadConfigFromNvs();

  // Всички консуматори са изключени при (ре)стартиране - съзнателно безопасно състояние;
  // бекендът го вижда чрез автоматичния state response с restart:true.
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    pinMode(consumerPins[i], OUTPUT);
    digitalWrite(consumerPins[i], LOW);
  }

  mqtt_wifi_setup();
  Serial.print(F("Executor (Wi-Fi/MQTT) готов, M_ID=")); Serial.println(MY_M_ID);
}

void loop() {
  wifiMaintain();
  mqttMaintain();
  mqttClientLoop();
  processMessages();
  heartbeatTick();
}
