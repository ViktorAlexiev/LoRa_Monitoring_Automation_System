// Wi-Fi, event loop и esp-mqtt на симулатора (виж sim_net.h).
#include "sim_net.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sim.h"

void sim_notify_from_isr(void *task);
void sim_preempt_point(void);

esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t IP_EVENT = "IP_EVENT";

namespace sim {
WifiFake wifi;
BrokerFake broker;
}  // namespace sim

namespace {

// ======================================================================= event loop
struct Handler { esp_event_base_t base; int32_t id; esp_event_handler_t fn; void *arg; };
struct Event { esp_event_base_t base; int32_t id; };
std::vector<Handler> g_handlers;
std::deque<Event> g_events;
TaskHandle_t g_evTask = nullptr;
bool g_loopCreated = false;

void eventTaskMain(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!g_events.empty()) {
      Event e = g_events.front();
      g_events.pop_front();
      std::vector<Handler> hs = g_handlers;
      for (auto &h : hs) {
        if (h.base == e.base && (h.id == ESP_EVENT_ANY_ID || h.id == e.id)) h.fn(h.arg, e.base, e.id, nullptr);
      }
    }
  }
}

void postEvent(esp_event_base_t base, int32_t id) {
  g_events.push_back({base, id});
  if (g_evTask) sim_notify_from_isr(g_evTask);
}

// ======================================================================= mqtt
struct MqttEv {
  int id;
  std::string topic, data;
};
struct MqttClient {
  esp_mqtt_client_config_t cfg;
  std::string host, clientId, user, pass, lwtTopic, lwtMsg;
  esp_event_handler_t handler = nullptr;
  void *handlerArg = nullptr;
  bool retryScheduled = false;
};
MqttClient *g_client = nullptr;
std::deque<MqttEv> g_mqttEvents;
TaskHandle_t g_mqttTask = nullptr;
int g_msgId = 1;
std::vector<sim::MqttPub> g_outbox;

void mqttPost(int id, const std::string &topic = "", const std::string &data = "") {
  g_mqttEvents.push_back({id, topic, data});
  if (g_mqttTask) sim_notify_from_isr(g_mqttTask);
}

void mqttTaskMain(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!g_mqttEvents.empty()) {
      MqttEv e = g_mqttEvents.front();
      g_mqttEvents.pop_front();
      if (!g_client || !g_client->handler) continue;
      esp_mqtt_event_t ev;
      memset(&ev, 0, sizeof(ev));
      ev.event_id = (esp_mqtt_event_id_t)e.id;
      ev.client = (esp_mqtt_client_handle_t)g_client;
      // topic НЕ е NUL-терминиран, точно като в esp-mqtt (подаваме указател в средата на по-дълъг низ)
      std::string tbuf = e.topic + "\x01JUNK";
      ev.topic = (char *)tbuf.data();
      ev.topic_len = (int)e.topic.size();
      ev.data = (char *)e.data.data();
      ev.data_len = (int)e.data.size();
      ev.total_data_len = ev.data_len;
      ev.current_data_offset = 0;
      g_client->handler(g_client->handlerArg, "MQTT_EVENTS", e.id, &ev);
    }
  }
}

void tryConnect();

void scheduleRetry() {
  if (!g_client || g_client->retryScheduled) return;
  g_client->retryScheduled = true;
  int ms = g_client->cfg.network.reconnect_timeout_ms > 0 ? g_client->cfg.network.reconnect_timeout_ms : 10000;
  sim::at_after_ms((uint32_t)ms, [] {
    if (g_client) g_client->retryScheduled = false;
    tryConnect();
  });
}

void tryConnect() {
  using namespace sim;
  if (!g_client || !broker.started || broker.connected) return;
  if (!wifi.connected || !broker.online) { scheduleRetry(); return; }
  broker.connected = true;
  broker.connCount++;
  MqttConn c;
  c.id = broker.connCount;
  c.clientId = g_client->clientId;
  c.user = g_client->user;
  c.pass = g_client->pass;
  c.cleanSession = !g_client->cfg.session.disable_clean_session;
  c.keepalive = g_client->cfg.session.keepalive;
  c.lwtTopic = g_client->lwtTopic;
  c.lwtMsg = g_client->lwtMsg;
  c.lwtQos = g_client->cfg.session.last_will.qos;
  c.lwtRetain = g_client->cfg.session.last_will.retain;
  c.tUs = now_us();
  broker.conns.push_back(c);
  if (c.cleanSession) broker.subs.erase(std::remove_if(broker.subs.begin(), broker.subs.end(),
      [&](const MqttSub &s) { return s.conn != c.id; }), broker.subs.end());
  // outbox (QoS>0, публикувани без връзка) се доставя при свързване - точно това не искаме да се случва
  for (auto p : g_outbox) { p.fromOutbox = true; p.conn = c.id; broker.published.push_back(p); }
  g_outbox.clear();
  mqttPost(MQTT_EVENT_CONNECTED);
}

}  // namespace

// ======================================================================= публичен API на тестовете
namespace sim {

void WifiFake::dropLink() {
  bool was = connected;
  connected = false;
  broker.dropConnection();
  if (was || started) postEvent(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
}

void BrokerFake::dropConnection() {
  if (!connected) return;
  connected = false;
  mqttPost(MQTT_EVENT_DISCONNECTED);
  scheduleRetry();
}

void BrokerFake::setOnline(bool on) {
  online = on;
  if (!on) dropConnection();
}

bool BrokerFake::deliver(const std::string &topic, const std::string &payload) {
  if (!connected) return false;
  if (!subscribedTo(topic)) { droppedNotSubscribed++; return false; }
  deliveredToDevice++;
  mqttPost(MQTT_EVENT_DATA, topic, payload);
  return true;
}

std::vector<MqttPub> BrokerFake::on(const std::string &topic) const {
  std::vector<MqttPub> r;
  for (auto &p : published) if (p.topic == topic) r.push_back(p);
  return r;
}

bool BrokerFake::subscribedTo(const std::string &topic, int qos) const {
  if (!connected) return false;
  for (auto &s : subs) {
    if (s.conn == connCount && s.topic == topic && (qos < 0 || s.qos == qos)) return true;
  }
  return false;
}

}  // namespace sim

void sim_net_reset(bool world) {
  g_handlers.clear();
  g_events.clear();
  g_evTask = nullptr;
  g_loopCreated = false;
  g_mqttEvents.clear();
  g_mqttTask = nullptr;
  g_outbox.clear();
  delete g_client;
  g_client = nullptr;
  g_msgId = 1;
  sim::WifiFake w;
  if (!world) { w.apUp = sim::wifi.apUp; w.ssid = sim::wifi.ssid; w.pass = sim::wifi.pass; }   // светът извън устройството оцелява при рестарт
  sim::wifi = w;
  bool online = sim::broker.online;
  sim::broker = sim::BrokerFake();
  if (!world) sim::broker.online = online;
}

// ======================================================================= IDF API
esp_err_t esp_event_loop_create_default(void) {
  if (g_loopCreated) return ESP_ERR_INVALID_STATE;
  g_loopCreated = true;
  xTaskCreatePinnedToCore(eventTaskMain, "sys_evt", 3072, nullptr, 20, &g_evTask, 0);
  return ESP_OK;
}

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t h, void *arg) {
  if (!g_loopCreated) return ESP_ERR_INVALID_STATE;
  g_handlers.push_back({base, id, h, arg});
  return ESP_OK;
}

esp_err_t esp_netif_init(void) { return ESP_OK; }
void *esp_netif_create_default_wifi_sta(void) { return (void *)1; }

esp_err_t esp_wifi_init(const wifi_init_config_t *) { sim::wifi.initCalls++; return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t m) { return m == WIFI_MODE_STA ? ESP_OK : ESP_ERR_INVALID_ARG; }
esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t *c) {
  sim::wifi.cfgSsid = (const char *)c->sta.ssid;
  sim::wifi.cfgPass = (const char *)c->sta.password;
  return ESP_OK;
}
esp_err_t esp_wifi_start(void) {
  sim::wifi.started = true;
  postEvent(WIFI_EVENT, WIFI_EVENT_STA_START);
  return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t t) { sim::wifi.psMode = (int)t; return ESP_OK; }

esp_err_t esp_wifi_connect(void) {
  using namespace sim;
  wifi.connectCalls++;
  if (!wifi.started) return ESP_ERR_INVALID_STATE;
  bool ok = wifi.apUp && wifi.cfgSsid == wifi.ssid && wifi.cfgPass == wifi.pass;
  if (ok) {
    at_after_ms(wifi.gotIpDelayMs, [] {
      if (sim::wifi.apUp) { sim::wifi.connected = true; postEvent(IP_EVENT, IP_EVENT_STA_GOT_IP); }
      else postEvent(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
    });
  } else {
    at_after_ms(1500, [] { postEvent(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED); });
  }
  return ESP_OK;
}

// ---- esp-mqtt ----
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg) {
  using namespace sim;
  delete g_client;
  g_client = new MqttClient();
  g_client->cfg = *cfg;
  g_client->host = cfg->broker.address.hostname ? cfg->broker.address.hostname : "";
  g_client->clientId = cfg->credentials.client_id ? cfg->credentials.client_id : "";
  g_client->user = cfg->credentials.username ? cfg->credentials.username : "";
  g_client->pass = cfg->credentials.authentication.password ? cfg->credentials.authentication.password : "";
  g_client->lwtTopic = cfg->session.last_will.topic ? cfg->session.last_will.topic : "";
  g_client->lwtMsg = cfg->session.last_will.msg ? std::string(cfg->session.last_will.msg, (size_t)cfg->session.last_will.msg_len) : "";
  broker.initialized = true;
  broker.host = g_client->host;
  broker.port = cfg->broker.address.port;
  broker.bufferSize = cfg->buffer.size;
  broker.reconnectMs = cfg->network.reconnect_timeout_ms;
  if (!g_mqttTask) xTaskCreatePinnedToCore(mqttTaskMain, "mqtt_task", 6144, nullptr, 5, &g_mqttTask, 0);
  return (esp_mqtt_client_handle_t)g_client;
}

esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, esp_mqtt_event_id_t, esp_event_handler_t h, void *arg) {
  if (!g_client) return ESP_ERR_INVALID_STATE;
  g_client->handler = h;
  g_client->handlerArg = arg;
  return ESP_OK;
}

esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t) {
  if (!g_client) return ESP_ERR_INVALID_STATE;
  if (sim::broker.started) return ESP_ERR_INVALID_STATE;
  sim::broker.started = true;
  sim::at_after_ms(50, [] { tryConnect(); });
  return ESP_OK;
}

int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t, const char *topic, int qos) {
  sim_preempt_point();
  if (!sim::broker.connected) return -1;
  sim::broker.subs.push_back({topic, qos, sim::broker.connCount});
  return g_msgId++;
}

int esp_mqtt_client_publish(esp_mqtt_client_handle_t, const char *topic, const char *data, int len, int qos, int retain) {
  using namespace sim;
  sim_preempt_point();
  MqttPub p;
  p.topic = topic;
  p.payload = std::string(data, (size_t)len);
  p.qos = qos;
  p.retain = retain != 0;
  p.tUs = now_us();
  p.conn = broker.connCount;
  if (!broker.connected) {
    if (qos == 0) { broker.rejectedOffline++; return -1; }
    broker.outboxQueued++;
    g_outbox.push_back(p);
    return g_msgId++;
  }
  broker.published.push_back(p);
  return g_msgId++;
}
