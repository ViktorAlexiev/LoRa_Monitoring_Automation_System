#include "mqtt_exec.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config_storage.h"

const char* TOPIC_WIFI_COMMANDS   = "wifi_commands";
const char* TOPIC_COMMANDS_STATUS = "commands_status";
const char* TOPIC_STATE_RESP      = "module_states_response";
const char* TOPIC_HEARTBEAT       = "heartbeat";

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static unsigned long wifiDownSince = 0;
static unsigned long wifiLastAttempt = 0;
static unsigned long mqttDownSince = 0;
static unsigned long mqttLastAttempt = 0;
static unsigned long lastHeartbeat = 0;
static bool restartStateSent = false;   // автоматичният state response след boot - веднъж на boot

// ---------------- Малка опашка от входящи съобщения ----------------
// Callback-ът само слага в опашката; изпълнението е в processMessages() от loop().
#define MSG_FIFO_SIZE 8
struct Msg {
  char c_id[CONSUMER_ID_LEN + 1];
  uint8_t com;
};
static Msg msgFifo[MSG_FIFO_SIZE];
static uint8_t msgHead = 0;
static uint8_t msgCount = 0;

static void msgPush(const char* c_id, uint8_t com) {
  if (msgCount >= MSG_FIFO_SIZE) {
    Serial.println(F("[WARN] опашката е пълна - съобщението е изхвърлено"));
    return;
  }
  uint8_t tail = (msgHead + msgCount) % MSG_FIFO_SIZE;
  memset(msgFifo[tail].c_id, 0, sizeof(msgFifo[tail].c_id));
  strncpy(msgFifo[tail].c_id, c_id, CONSUMER_ID_LEN);
  msgFifo[tail].com = com;
  msgCount++;
}

static bool msgPop(Msg* out) {
  if (msgCount == 0) return false;
  *out = msgFifo[msgHead];
  msgHead = (msgHead + 1) % MSG_FIFO_SIZE;
  msgCount--;
  return true;
}

// ---------------- Публикуване ----------------
static void publishJson(const char* topic, JsonDocument& doc) {
  char buf[512];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (!mqttClient.publish(topic, (const uint8_t*)buf, n, false)) {   // retain = false
    Serial.print(F("[MQTT] publish ПРОВАЛ, topic=")); Serial.print(topic);
    Serial.print(F(" len=")); Serial.println(n);
  }
}

static void publishCommandStatus(const char* c_id, uint8_t com, uint8_t status) {
  StaticJsonDocument<160> doc;
  doc["M_ID"] = MY_M_ID;
  doc["C_ID"] = c_id;
  char comHex[3];
  snprintf(comHex, sizeof(comHex), "%02X", com);
  doc["com"] = comHex;
  doc["status"] = status;
  publishJson(TOPIC_COMMANDS_STATUS, doc);
  Serial.print(F("[STATUS] ")); Serial.print(c_id);
  Serial.print(F(" com=")); Serial.print(comHex);
  Serial.print(F(" status=")); Serial.println(status);
}

// Състоянието се чете от реалния пин (както при LoRa executor-а), не от запомнена стойност.
static void publishStates(bool isRestart) {
  StaticJsonDocument<1024> doc;
  doc["id"] = MY_M_ID;
  // само за автоматичния отговор след boot; изпуснато (не false) при нормален отговор
  if (isRestart) doc["restart"] = true;
  JsonArray states = doc.createNestedArray("states");
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    JsonObject o = states.createNestedObject();
    o["id"] = consumerList[i];
    o["state"] = digitalRead(consumerPins[i]) ? "ON" : "OFF";
  }
  publishJson(TOPIC_STATE_RESP, doc);
  Serial.print(isRestart ? F("[STATE_RESP RESTART] ") : F("[STATE_RESP] "));
  Serial.println(NUM_CONSUMERS);
}

// ---------------- Входящи съобщения ----------------
static void onMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_WIFI_COMMANDS) != 0) return;

  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, payload, length)) { Serial.println(F("[ERR] Bad JSON wifi_commands")); return; }

  const char* m_id = doc["M_ID"] | "";
  if (strcmp(m_id, MY_M_ID) != 0) return;     // чужда команда - всички Wi-Fi модули получават темата

  const char* c_id = doc["C_ID"] | "";
  const char* comStr = doc["com"] | "";
  uint8_t com = (uint8_t)strtol(comStr, nullptr, 16);
  msgPush(c_id, com);
}

void processMessages() {
  Msg m;
  while (msgPop(&m)) {
    if (m.com == CMD_STATE_REQ) {
      publishStates(false);
      continue;
    }
    int idx = consumerIndex(m.c_id);
    bool cmdOk = (m.com == CMD_ON || m.com == CMD_OFF);
    if (idx >= 0 && cmdOk) {
      digitalWrite(consumerPins[idx], m.com == CMD_ON ? HIGH : LOW);
      Serial.print(F("[EXEC] ")); Serial.print(m.c_id);
      Serial.print(F(" (pin ")); Serial.print(consumerPins[idx]);
      Serial.println(m.com == CMD_ON ? F(") -> ON") : F(") -> LOW"));
      publishCommandStatus(m.c_id, m.com, STATUS_ACK);
    } else {
      Serial.print(F("[REJECT] ")); Serial.print(m.c_id);
      if (idx < 0) Serial.print(F(" - консуматор не съществува"));
      if (!cmdOk)  Serial.print(F(" - невалидна команда"));
      Serial.println();
      publishCommandStatus(m.c_id, m.com, STATUS_NACK);
    }
  }
}

// ---------------- WiFi / MQTT ----------------
void mqtt_wifi_setup() {
  // стандартният буфер на PubSubClient е 256 B - module_states_response с 10 консуматора е ~200 B
  mqttClient.setBufferSize(512);
  mqttClient.setServer(MQTT_IP_BUF, MQTT_PORT_VAL);
  mqttClient.setCallback(onMessage);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID_BUF, WIFI_PASSWORD_BUF);
  lastHeartbeat = millis();
}

void wifiMaintain() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
    return;
  }
  if (wifiDownSince == 0) {
    wifiDownSince = millis();
    Serial.println(F("[WiFi] изгубена връзка"));
  }
  if (millis() - wifiLastAttempt >= RECONNECT_ATTEMPT_MS) {
    wifiLastAttempt = millis();
    Serial.println(F("[WiFi] опит за връзка..."));
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID_BUF, WIFI_PASSWORD_BUF);
  }
  if (millis() - wifiDownSince >= WIFI_RETRY_TIMEOUT_MS) {
    Serial.println(F("[WiFi] timeout 2 min - RESTART"));
    delay(200);
    ESP.restart();
  }
}

void mqttMaintain() {
  if (WiFi.status() != WL_CONNECTED) { mqttDownSince = 0; return; }

  if (mqttClient.connected()) {
    mqttDownSince = 0;
    return;
  }

  if (mqttDownSince == 0) {
    mqttDownSince = millis();
    Serial.println(F("[MQTT] изгубена връзка"));
  }

  if (millis() - mqttLastAttempt >= RECONNECT_ATTEMPT_MS) {
    mqttLastAttempt = millis();
    Serial.println(F("[MQTT] опит за връзка..."));

    // Last Will (retained): брокерът публикува "offline", ако връзката падне без DISCONNECT.
    char willTopic[24];
    char willMsgOff[48];
    char willMsgOn[48];
    snprintf(willTopic, sizeof(willTopic), "executors/%s/lwt", MY_M_ID);
    snprintf(willMsgOff, sizeof(willMsgOff), "{\"id\":\"%s\",\"online\":false}", MY_M_ID);
    snprintf(willMsgOn, sizeof(willMsgOn), "{\"id\":\"%s\",\"online\":true}", MY_M_ID);

    const char* user = (strlen(MQTT_USER_BUF) > 0) ? MQTT_USER_BUF : NULL;
    const char* pass = (strlen(MQTT_USER_BUF) > 0) ? MQTT_PASSWORD_BUF : NULL;
    // cleanSession = true (изрично): при нова връзка брокерът НЕ доставя команди, пропуснати
    // докато модулът е бил офлайн - стара команда не бива да се изпълни със закъснение.
    bool connected = mqttClient.connect(MY_M_ID, user, pass, willTopic, 1, true, willMsgOff, true);

    if (connected) {
      mqttClient.subscribe(TOPIC_WIFI_COMMANDS, 1);   // QoS 1; команди без retain
      mqttClient.publish(willTopic, willMsgOn, true);
      Serial.println(F("[MQTT] OK"));
      if (!restartStateSent) {
        publishStates(true);       // автоматичен state response веднага след boot (restart:true)
        restartStateSent = true;
      }
    } else {
      Serial.print(F("[MQTT] FAILED state="));
      Serial.println(mqttClient.state());
    }
  }

  if (millis() - mqttDownSince >= MQTT_RETRY_TIMEOUT_MS) {
    Serial.println(F("[MQTT] timeout 2 min - RESTART"));
    delay(200);
    ESP.restart();
  }
}

void mqttClientLoop() {
  mqttClient.loop();
}

// ---------------- Heartbeat ----------------
void heartbeatTick() {
  if (millis() - lastHeartbeat >= EXEC_HEARTBEAT_INTERVAL_MS) {
    lastHeartbeat = millis();
    if (mqttClient.connected()) {
      StaticJsonDocument<96> doc;
      doc["id"] = MY_M_ID;
      doc["ts"] = (uint32_t)(millis() / 1000);
      publishJson(TOPIC_HEARTBEAT, doc);
      Serial.print(F("[HEARTBEAT] ")); Serial.println(MY_M_ID);
    }
  }
}
