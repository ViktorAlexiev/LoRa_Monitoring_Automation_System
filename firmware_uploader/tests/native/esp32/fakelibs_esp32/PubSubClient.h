// Заместител на PubSubClient - прихваща connect/subscribe/publish и позволява на теста да "достави"
// входящо съобщение към callback-а на скеча. Състоянието е глобално (g_mqtt) - в скечовете има само
// един MQTT клиент.
#ifndef FAKE_PUBSUBCLIENT_H
#define FAKE_PUBSUBCLIENT_H

#include "Arduino.h"
#include "WiFi.h"
#include <vector>

typedef void (*MQTT_CALLBACK_SIGNATURE)(char *, uint8_t *, unsigned int);

struct MqttPublished {
  std::string topic;
  std::string payload;
  bool retained;
};
struct MqttSubscription {
  std::string topic;
  int qos;
};
struct MqttConnectArgs {
  bool called = false;
  int calls = 0;
  std::string id, user, pass, willTopic, willMessage;
  bool hasUser = false;
  int willQos = -1;
  bool willRetain = false;
  bool cleanSession = true;   // стойността по подразбиране в PubSubClient (прави се изрична от скеча)
  bool cleanSessionExplicit = false;
};
struct MqttFake {
  bool connected = false;
  bool connectResult = true;
  int stateCode = 0;
  std::string server;
  uint16_t port = 0;
  unsigned bufferSize = 256;
  MQTT_CALLBACK_SIGNATURE callback = nullptr;
  MqttConnectArgs connectArgs;
  std::vector<MqttSubscription> subs;
  std::vector<MqttPublished> published;
  bool publishResult = true;
  void reset() { *this = MqttFake(); }
};
extern MqttFake g_mqtt;

class PubSubClient {
public:
  PubSubClient() {}
  PubSubClient(WiFiClient &) {}
  bool setBufferSize(unsigned size) { g_mqtt.bufferSize = size; return true; }
  PubSubClient &setServer(const char *host, uint16_t port) { g_mqtt.server = host ? host : ""; g_mqtt.port = port; return *this; }
  PubSubClient &setCallback(MQTT_CALLBACK_SIGNATURE cb) { g_mqtt.callback = cb; return *this; }

  bool connect(const char *id) {
    return doConnect(id, nullptr, nullptr, nullptr, 0, false, nullptr, true, false);
  }
  bool connect(const char *id, const char *user, const char *pass) {
    return doConnect(id, user, pass, nullptr, 0, false, nullptr, true, false);
  }
  bool connect(const char *id, const char *user, const char *pass, const char *willTopic, uint8_t willQos,
               bool willRetain, const char *willMessage, bool cleanSession = true) {
    return doConnect(id, user, pass, willTopic, willQos, willRetain, willMessage, cleanSession, true);
  }
  bool connected() { return g_mqtt.connected; }
  int state() { return g_mqtt.stateCode; }
  bool loop() { return g_mqtt.connected; }

  bool subscribe(const char *topic, uint8_t qos = 0) {
    g_mqtt.subs.push_back({topic, (int)qos});
    return true;
  }
  bool publish(const char *topic, const char *payload) { return doPublish(topic, payload, strlen(payload), false); }
  bool publish(const char *topic, const char *payload, bool retained) {
    return doPublish(topic, payload, strlen(payload), retained);
  }
  bool publish(const char *topic, const uint8_t *payload, unsigned int len, bool retained = false) {
    return doPublish(topic, (const char *)payload, len, retained);
  }

private:
  bool doConnect(const char *id, const char *user, const char *pass, const char *wt, uint8_t wq, bool wr,
                 const char *wm, bool clean, bool explicitClean) {
    MqttConnectArgs &a = g_mqtt.connectArgs;
    a.called = true; a.calls++;
    a.id = id ? id : "";
    a.hasUser = user != nullptr;
    a.user = user ? user : ""; a.pass = pass ? pass : "";
    a.willTopic = wt ? wt : ""; a.willQos = wt ? (int)wq : -1; a.willRetain = wr; a.willMessage = wm ? wm : "";
    a.cleanSession = clean; a.cleanSessionExplicit = explicitClean;
    g_mqtt.connected = g_mqtt.connectResult;
    g_mqtt.stateCode = g_mqtt.connectResult ? 0 : -2;
    return g_mqtt.connectResult;
  }
  bool doPublish(const char *topic, const char *payload, size_t len, bool retained) {
    if (!g_mqtt.publishResult) return false;
    g_mqtt.published.push_back({topic, std::string(payload, len), retained});
    return true;
  }
};

// помощна функция за теста: "доставя" съобщение към callback-а, както би направил брокерът
inline void mqttInject(const std::string &topic, const std::string &payload) {
  if (!g_mqtt.callback) return;
  std::vector<uint8_t> buf(payload.begin(), payload.end());
  std::vector<char> t(topic.begin(), topic.end());
  t.push_back(0);
  g_mqtt.callback(t.data(), buf.data(), (unsigned int)buf.size());
}

#endif
