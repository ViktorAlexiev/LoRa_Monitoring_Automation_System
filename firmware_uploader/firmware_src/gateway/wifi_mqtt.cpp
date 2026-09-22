#include "wifi_mqtt.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "config_storage.h"

const char* TOPIC_SENSORS              = "sensors";
const char* TOPIC_HEARTBEAT            = "heartbeat";
const char* TOPIC_COMMANDS             = "commands";
const char* TOPIC_COMMANDS_STATUS      = "commands_status";
const char* TOPIC_STATE_REQ            = "module_states_requests";
const char* TOPIC_STATE_RESP           = "module_states_response";

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static unsigned long wifiDownSince = 0;
static unsigned long wifiLastAttempt = 0;
static unsigned long mqttDownSince = 0;
static unsigned long mqttLastAttempt = 0;
static unsigned long lastGwHeartbeat = 0;

void mqtt_wifi_setup() {
  // По подразбиране PubSubClient има вътрешен буфер от само 256 B за целия MQTT пакет
  // (топик+payload+overhead) в много версии на библиотеката - state response JSON с 10
  // консуматора може да го надхвърли. 512 B покрива най-големия ни реален payload
  // (module_states_response с 10 записа е ~190-200 B, плюс overhead и топик с достатъчен запас).
  mqttClient.setBufferSize(512);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID_BUF, WIFI_PASSWORD_BUF);
  lastGwHeartbeat = millis();
}

// ---------------- WiFi maintain (non-blocking) ----------------
void wifiMaintain() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSince = 0;
    return;
  }

  if (wifiDownSince == 0) {
    wifiDownSince = millis();
    Serial.println("[WiFi] изгубена връзка");
  }

  if (millis() - wifiLastAttempt >= RECONNECT_ATTEMPT_MS) {
    wifiLastAttempt = millis();
    Serial.println("[WiFi] опит за връзка...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID_BUF, WIFI_PASSWORD_BUF);
  }

  if (millis() - wifiDownSince >= WIFI_RETRY_TIMEOUT_MS) {
    Serial.println("[WiFi] timeout 2 min - RESTART");
    delay(200);
    ESP.restart();
  }
}

// ---------------- MQTT maintain (non-blocking) ----------------
void mqttMaintain() {
  if (WiFi.status() != WL_CONNECTED) { mqttDownSince = 0; return; }

  if (mqttClient.connected()) {
    mqttDownSince = 0;
    return;
  }

  if (mqttDownSince == 0) {
    mqttDownSince = millis();
    Serial.println("[MQTT] изгубена връзка");
  }

  if (millis() - mqttLastAttempt >= RECONNECT_ATTEMPT_MS) {
    mqttLastAttempt = millis();
    Serial.println("[MQTT] опит за връзка...");
    mqttClient.setServer(MQTT_IP_BUF, MQTT_PORT_VAL);
    mqttClient.setCallback(mqttCallback);

    bool connected;
    if (strlen(MQTT_USER_BUF) > 0) {
      connected = mqttClient.connect(GATEWAY_ID, MQTT_USER_BUF, MQTT_PASSWORD_BUF);
    } else {
      connected = mqttClient.connect(GATEWAY_ID);
    }

    if (connected) {
      mqttClient.subscribe(TOPIC_COMMANDS);
      mqttClient.subscribe(TOPIC_STATE_REQ);
      Serial.println("[MQTT] OK");
    } else {
      Serial.print("[MQTT] FAILED state=");
      Serial.println(mqttClient.state());
    }
  }

  if (millis() - mqttDownSince >= MQTT_RETRY_TIMEOUT_MS) {
    Serial.println("[MQTT] timeout 2 min - RESTART");
    delay(200);
    ESP.restart();
  }
}

void mqttClientLoop() {
  mqttClient.loop();
}

bool mqttIsConnected() {
  return mqttClient.connected();
}

void publishJson(const char* topic, JsonDocument& doc) {
  char buf[512];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (!mqttClient.publish(topic, buf, n)) {
    Serial.print("[MQTT] publish ПРОВАЛ, topic="); Serial.print(topic);
    Serial.print(" len="); Serial.println(n);
  }
}

// ---------------- Heartbeat на гейтуея ----------------
void sendGatewayHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["id"] = GATEWAY_ID;
  doc["ts"] = (uint32_t)(millis() / 1000);
  publishJson(TOPIC_HEARTBEAT, doc);
  Serial.print("[HEARTBEAT] "); Serial.println(GATEWAY_ID);
}

void gatewayHeartbeatTick() {
  if (millis() - lastGwHeartbeat >= GW_HEARTBEAT_INTERVAL_MS) {
    lastGwHeartbeat = millis();
    if (mqttIsConnected()) sendGatewayHeartbeat();
  }
}
