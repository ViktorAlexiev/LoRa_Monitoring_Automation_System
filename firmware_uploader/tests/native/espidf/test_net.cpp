// Тестове на agro_net: wifi_link (esp_wifi/event loop) и mqtt_link (esp-mqtt) срещу симулирания Wi-Fi/брокер.
#include "test_support.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "agro_hal.h"
#include "mqtt_link.h"
#include "wifi_link.h"

static int g_gotIp, g_connCb, g_discCb, g_msgs;
static std::vector<std::pair<std::string, std::string>> g_rx;

static void resetCounters() { g_gotIp = g_connCb = g_discCb = g_msgs = 0; g_rx.clear(); }

static void onGotIp(void *) { g_gotIp++; mqtt_link_network_up(); }
static void onConn(bool c, void *) { if (c) g_connCb++; else g_discCb++; }
static void onMsg(const char *topic, const char *data, size_t len, void *) {
  g_msgs++;
  g_rx.push_back({topic, std::string(data, len)});
}

static const char *SUBS[2] = {"commands", "module_states_requests"};

static MqttLinkConfig mcfg(bool lwt = false, const char *user = "") {
  MqttLinkConfig c = {};
  c.host = "192.168.1.10";
  c.port = 1883;
  c.user = user;
  c.password = "secret";
  c.clientId = "G001";
  if (lwt) { c.lwtTopic = "executors/E007/lwt"; c.lwtMessage = "{\"id\":\"E007\",\"online\":false}"; }
  c.subTopics = SUBS;
  c.subCount = 2;
  c.subQos = 1;
  c.onMessage = onMsg;
  c.onConnection = onConn;
  return c;
}

static void netApp() {
  resetCounters();
  MqttLinkConfig c = mcfg();
  mqtt_link_init(&c);
  wifi_link_start("FarmNet", "pw", onGotIp, nullptr);
  vTaskDelay(portMAX_DELAY);
}
static void netAppLwt() {
  resetCounters();
  MqttLinkConfig c = mcfg(true, "user1");
  mqtt_link_init(&c);
  wifi_link_start("FarmNet", "pw", onGotIp, nullptr);
  vTaskDelay(portMAX_DELAY);
}

// ============================================================================ Wi-Fi
STEST(wifi_connects_with_configured_credentials_and_disables_power_save) {
  sim::boot(netApp);
  sim::run_for_ms(2000);
  ASSERT_STREQ(sim::wifi.cfgSsid, "FarmNet");
  ASSERT_STREQ(sim::wifi.cfgPass, "pw");
  ASSERT_EQ(sim::wifi.psMode, 0);                    // WIFI_PS_NONE
  ASSERT_EQ(sim::wifi.connectCalls, 1);
  ASSERT_EQ(g_gotIp, 1);
  ASSERT_TRUE(wifi_link_connected());
  ASSERT_EQ((int)wifi_link_down_ms(), 0);
}

STEST(wifi_wrong_password_retries_every_5_s_then_restarts_after_2_minutes) {
  sim::wifi.pass = "other";
  sim::boot(netApp);
  sim::run_for_ms(60000);
  ASSERT_GE(sim::wifi.connectCalls, 8);              // опит ~на всеки 6.5 s (1.5 s отказ + 5 s пауза)
  ASSERT_LE(sim::wifi.connectCalls, 11);
  ASSERT_FALSE(sim::restarted());
  ASSERT_GE((long)wifi_link_down_ms(), 59000L);
  sim::run_for_ms(70000);
  ASSERT_TRUE(sim::restarted());
  ASSERT_TRUE(sim::log_contains("2 мин"));
}

STEST(wifi_recovers_after_access_point_returns_and_resets_the_down_timer) {
  sim::boot(netApp);
  sim::run_for_ms(10000);
  ASSERT_TRUE(wifi_link_connected());
  sim::wifi.apUp = false;
  sim::wifi.dropLink();
  sim::run_for_ms(30000);
  ASSERT_FALSE(wifi_link_connected());
  ASSERT_GE((long)wifi_link_down_ms(), 29000L);
  ASSERT_FALSE(sim::restarted());
  sim::wifi.apUp = true;
  sim::run_for_ms(15000);
  ASSERT_TRUE(wifi_link_connected());
  ASSERT_EQ(g_gotIp, 2);
  ASSERT_EQ((int)wifi_link_down_ms(), 0);
  sim::run_for_ms(300000);                           // и повече никакъв рестарт при работеща връзка
  ASSERT_FALSE(sim::restarted());
}

// ============================================================================ MQTT
STEST(mqtt_starts_only_after_network_and_applies_the_session_policy) {
  sim::boot(netApp);
  sim::run_for_ms(100);
  ASSERT_FALSE(sim::broker.started);                 // още няма IP -> клиентът не е стартиран
  sim::run_for_ms(2000);
  ASSERT_TRUE(sim::broker.started);
  ASSERT_EQ((int)sim::broker.conns.size(), 1);
  const sim::MqttConn &c = sim::broker.conns[0];
  ASSERT_STREQ(c.clientId, "G001");
  ASSERT_TRUE(c.cleanSession);                       // clean session = true: без стари команди
  ASSERT_EQ(c.keepalive, MQTT_KEEPALIVE_S);
  ASSERT_STREQ(c.user, "");                          // без потребител -> без автентикация
  ASSERT_STREQ(sim::broker.host, "192.168.1.10");
  ASSERT_EQ((int)sim::broker.port, 1883);
  ASSERT_EQ(sim::broker.reconnectMs, 5000);
  ASSERT_GE(sim::broker.bufferSize, 1024);
  ASSERT_STREQ(c.lwtTopic, "");
  ASSERT_EQ(g_connCb, 1);
}

STEST(mqtt_credentials_and_last_will_are_passed_to_the_client) {
  sim::boot(netAppLwt);
  sim::run_for_ms(2000);
  const sim::MqttConn &c = sim::broker.conns[0];
  ASSERT_STREQ(c.user, "user1");
  ASSERT_STREQ(c.pass, "secret");
  ASSERT_STREQ(c.lwtTopic, "executors/E007/lwt");
  ASSERT_STREQ(c.lwtMsg, "{\"id\":\"E007\",\"online\":false}");
  ASSERT_EQ(c.lwtQos, 1);
  ASSERT_EQ(c.lwtRetain, 1);
  ASSERT_TRUE(c.cleanSession);
}

STEST(mqtt_subscribes_with_qos1_on_every_connection_not_only_the_first) {
  sim::boot(netApp);
  sim::run_for_ms(2000);
  ASSERT_TRUE(sim::broker.subscribedTo("commands", 1));
  ASSERT_TRUE(sim::broker.subscribedTo("module_states_requests", 1));
  // брокерът затваря връзката; clean session -> абонаментите са загубени и трябва да се повторят
  sim::broker.dropConnection();
  sim::run_for_ms(500);
  ASSERT_FALSE(sim::broker.connected);
  ASSERT_EQ(g_discCb, 1);
  ASSERT_FALSE(sim::broker.subscribedTo("commands"));
  sim::run_for_ms(6000);                              // esp-mqtt се свързва сам след 5 s
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_EQ((int)sim::broker.conns.size(), 2);
  ASSERT_TRUE(sim::broker.subscribedTo("commands", 1));
  ASSERT_TRUE(sim::broker.subscribedTo("module_states_requests", 1));
  ASSERT_EQ(g_connCb, 2);
}

STEST(mqtt_delivers_messages_with_a_proper_nul_terminated_topic_and_exact_payload) {
  sim::boot(netApp);
  sim::run_for_ms(2000);
  ASSERT_TRUE(sim::broker.deliver("commands", "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}"));
  ASSERT_FALSE(sim::broker.deliver("not_subscribed", "x"));
  ASSERT_TRUE(sim::broker.deliver("module_states_requests", ""));
  sim::run_for_ms(100);
  ASSERT_EQ(g_msgs, 2);
  ASSERT_STREQ(g_rx[0].first, "commands");           // темата няма JUNK след края (esp-mqtt я дава без NUL)
  ASSERT_STREQ(g_rx[0].second, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  ASSERT_STREQ(g_rx[1].first, "module_states_requests");
  ASSERT_STREQ(g_rx[1].second, "");
  ASSERT_EQ(sim::broker.droppedNotSubscribed, 1);
}

STEST(mqtt_publish_policy_qos_retain_and_never_while_offline) {
  static bool r0, r1, r2, offline;
  r0 = r1 = r2 = offline = true;
  sim::boot([] {
    netApp();
  });
  // netApp блокира; публикуването се прави от втора задача
  sim::at_after_ms(1, [] {});
  sim::run_for_ms(2000);
  static const char *msg = "{\"id\":\"S001\"}";
  runAsTask([] {
    r0 = mqtt_link_publish("sensors", msg, strlen(msg), 0, false);
    r1 = mqtt_link_publish("commands_status", msg, strlen(msg), 1, false);
    r2 = mqtt_link_publish("lwt", msg, strlen(msg), 1, true);
  }, 100);
  ASSERT_TRUE(r0 && r1 && r2);
  ASSERT_EQ((int)sim::broker.published.size(), 3);
  ASSERT_EQ(sim::broker.published[0].qos, 0);
  ASSERT_EQ(sim::broker.published[1].qos, 1);
  ASSERT_FALSE(sim::broker.published[0].retain);
  ASSERT_FALSE(sim::broker.published[1].retain);
  ASSERT_TRUE(sim::broker.published[2].retain);
  ASSERT_STREQ(sim::broker.published[0].payload, "{\"id\":\"S001\"}");
  // без връзка: връща false и НИЩО не отива в outbox на esp-mqtt (иначе стари показания идват след свързване)
  sim::broker.setOnline(false);
  sim::run_for_ms(500);
  runAsTask([] {
    offline = mqtt_link_publish("commands_status", msg, strlen(msg), 1, false);
  }, 100);
  ASSERT_FALSE(offline);
  ASSERT_EQ(sim::broker.outboxQueued, 0);
  ASSERT_EQ(sim::broker.rejectedOffline, 0);          // и не стигна до клиента
  sim::broker.setOnline(true);
  sim::run_for_ms(10000);
  ASSERT_EQ((int)sim::broker.published.size(), 3);    // нищо "от outbox" след повторно свързване
}

STEST(mqtt_down_for_2_minutes_with_wifi_up_restarts_but_wifi_outage_alone_does_not_count_as_mqtt_down) {
  sim::boot(netApp);
  sim::run_for_ms(2000);
  sim::broker.setOnline(false);
  sim::run_for_ms(60000);
  ASSERT_FALSE(sim::restarted());
  ASSERT_GE((long)mqtt_link_down_ms(), 55000L);
  sim::run_for_ms(70000);
  ASSERT_TRUE(sim::restarted());
  ASSERT_TRUE(sim::log_contains("MQTT 2 мин"));
}

STEST(mqtt_recovers_when_broker_returns_and_clears_the_down_timer) {
  sim::boot(netApp);
  sim::run_for_ms(2000);
  sim::broker.setOnline(false);
  sim::run_for_ms(40000);
  sim::broker.setOnline(true);
  sim::run_for_ms(10000);
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_EQ((int)mqtt_link_down_ms(), 0);
  sim::run_for_ms(300000);
  ASSERT_FALSE(sim::restarted());
}

STEST(wifi_loss_drops_mqtt_and_both_come_back_without_restart) {
  sim::boot(netApp);
  sim::run_for_ms(2000);
  sim::wifi.apUp = false;
  sim::wifi.dropLink();
  sim::run_for_ms(20000);
  ASSERT_FALSE(sim::broker.connected);
  ASSERT_EQ((int)mqtt_link_down_ms(), 0);             // Wi-Fi е причината: не се брои като MQTT авария
  sim::wifi.apUp = true;
  sim::run_for_ms(30000);
  ASSERT_TRUE(wifi_link_connected());
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_TRUE(sim::broker.subscribedTo("commands", 1));
  ASSERT_FALSE(sim::restarted());
}

int main() { return runAllTests(); }
