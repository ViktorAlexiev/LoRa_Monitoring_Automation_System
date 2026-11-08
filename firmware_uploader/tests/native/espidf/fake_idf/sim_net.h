// Мрежа на симулатора - тестовата страна: Wi-Fi + MQTT брокер (виж sim.h).
#ifndef SIM_NET_H
#define SIM_NET_H

#include <stdint.h>
#include <string>
#include <vector>

namespace sim {

struct WifiFake {
  bool apUp = true;
  std::string ssid = "FarmNet";        // очакваните от "точката за достъп"
  std::string pass = "pw";
  uint32_t gotIpDelayMs = 300;
  // състояние
  bool started = false;
  bool connected = false;
  int connectCalls = 0;
  int initCalls = 0;
  int psMode = -1;
  std::string cfgSsid, cfgPass;        // каквото прошивката е конфигурирала

  void dropLink();                     // връзката пада (AP изчезва/ресет): DISCONNECTED + падане на MQTT
};
extern WifiFake wifi;

struct MqttPub {
  std::string topic, payload;
  int qos = 0;
  bool retain = false;
  uint64_t tUs = 0;
  int conn = 0;
  bool fromOutbox = false;
};
struct MqttSub { std::string topic; int qos; int conn; };
struct MqttConn {
  int id = 0;
  std::string clientId, user, pass;
  bool cleanSession = true;
  int keepalive = 0;
  std::string lwtTopic, lwtMsg;
  int lwtQos = 0, lwtRetain = 0;
  uint64_t tUs = 0;
};

struct BrokerFake {
  bool online = true;
  bool initialized = false;
  bool started = false;
  bool connected = false;
  int connCount = 0;
  std::vector<MqttConn> conns;
  std::vector<MqttPub> published;
  std::vector<MqttSub> subs;                  // абонаментите на ТЕКУЩАТА връзка + стари (conn поле)
  int rejectedOffline = 0;                    // publish QoS0 без връзка
  int outboxQueued = 0;                       // publish QoS>0 без връзка (esp-mqtt го слага в outbox!)
  int droppedNotSubscribed = 0;
  int deliveredToDevice = 0;
  int bufferSize = 0;
  int reconnectMs = 0;
  std::string host;
  uint32_t port = 0;

  void dropConnection();                      // брокерът затваря връзката
  void setOnline(bool on);                    // false = недостъпен (и пада връзката)
  // съобщение от брокера към устройството; false, ако не е абонирано/няма връзка
  bool deliver(const std::string &topic, const std::string &payload);
  std::vector<MqttPub> on(const std::string &topic) const;   // публикуваното на тема
  bool subscribedTo(const std::string &topic, int qos = -1) const;   // в текущата връзка
};
extern BrokerFake broker;

}  // namespace sim

#endif
