// executor_wifi - РЕАЛНИТЕ executor_wifi.ino (setup/loop), mqtt_exec.cpp и config_storage.cpp, срещу
// фалшиви WiFi/PubSubClient/Preferences и РЕАЛНАТА библиотека ArduinoJson. Тестовете са подредени и
// споделят състояние (статичните променливи на mqtt_exec.cpp), като един реален сеанс на модула.
#include "../test_framework.h"
#include "esp32_test_helpers.h"

#include "executor_wifi/executor_wifi.ino"   // -I <папката с firmware> (виж run_esp32.sh)

static const char *CMD_TOPIC = "wifi_commands";

static void configure() {
  nvsSetStr("id", "E007");
  nvsSetU8("nc", 2);
  nvsSetStr("c0i", "V01"); nvsSetU8("c0p", 25);
  nvsSetStr("c1i", "P01"); nvsSetU8("c1p", 26);
  nvsSetStr("wssid", "FarmNet");
  nvsSetStr("wpass", "secret123");
  nvsSetStr("mqip", "192.168.1.10");
  nvsSetStr("mqport", "1883");
  nvsSetStr("mquser", "agro");
  nvsSetStr("mqpass", "pw");
}

static std::vector<MqttPublished> pubsOn(const std::string &topic) {
  std::vector<MqttPublished> r;
  for (auto &p : g_mqtt.published) if (p.topic == topic) r.push_back(p);
  return r;
}
static void clearPubs() { g_mqtt.published.clear(); }
static void send(const std::string &json) { mqttInject(CMD_TOPIC, json); loop(); }

TEST(boot_connects_with_clean_session_will_and_subscribes_qos1) {
  resetFakes();
  configure();
  g_fakeMillis = 10000;
  setup();
  loop();

  ASSERT_TRUE(g_wifiSsid == "FarmNet");
  ASSERT_TRUE(g_wifiPass == "secret123");
  ASSERT_TRUE(g_mqtt.server == "192.168.1.10");
  ASSERT_EQ((int)g_mqtt.port, 1883);
  ASSERT_EQ(g_mqtt.connectArgs.calls, 1);
  ASSERT_TRUE(g_mqtt.connectArgs.id == "E007");
  ASSERT_TRUE(g_mqtt.connectArgs.hasUser);
  ASSERT_TRUE(g_mqtt.connectArgs.user == "agro");
  ASSERT_TRUE(g_mqtt.connectArgs.pass == "pw");
  // clean session изрично включена -> стари пропуснати команди НЕ се доставят
  ASSERT_TRUE(g_mqtt.connectArgs.cleanSessionExplicit);
  ASSERT_TRUE(g_mqtt.connectArgs.cleanSession);
  // Last Will: retained, QoS 1, "offline"
  ASSERT_TRUE(g_mqtt.connectArgs.willTopic == "executors/E007/lwt");
  ASSERT_EQ(g_mqtt.connectArgs.willQos, 1);
  ASSERT_TRUE(g_mqtt.connectArgs.willRetain);
  ASSERT_TRUE(g_mqtt.connectArgs.willMessage == "{\"id\":\"E007\",\"online\":false}");
  // абониран само за wifi_commands, с QoS 1
  ASSERT_EQ((int)g_mqtt.subs.size(), 1);
  ASSERT_TRUE(g_mqtt.subs[0].topic == "wifi_commands");
  ASSERT_EQ(g_mqtt.subs[0].qos, 1);
  ASSERT_TRUE(g_mqtt.bufferSize >= 512);
}

TEST(boot_forces_all_consumers_off) {
  ASSERT_EQ(g_pinMode[25], OUTPUT);
  ASSERT_EQ(g_pinMode[26], OUTPUT);
  ASSERT_EQ((int)g_pinState[25], 0);
  ASSERT_EQ((int)g_pinState[26], 0);
}

TEST(boot_publishes_birth_and_restart_state_response) {
  std::vector<MqttPublished> lwt = pubsOn("executors/E007/lwt");
  ASSERT_EQ((int)lwt.size(), 1);
  ASSERT_TRUE(lwt[0].payload == "{\"id\":\"E007\",\"online\":true}");
  ASSERT_TRUE(lwt[0].retained);

  std::vector<MqttPublished> st = pubsOn("module_states_response");
  ASSERT_EQ((int)st.size(), 1);
  ASSERT_TRUE(st[0].payload ==
              "{\"id\":\"E007\",\"restart\":true,\"states\":[{\"id\":\"V01\",\"state\":\"OFF\"},{\"id\":\"P01\",\"state\":\"OFF\"}]}");
  ASSERT_TRUE(!st[0].retained);
}

TEST(on_command_sets_pin_and_acks_with_same_json_as_the_gateway) {
  clearPubs();
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  ASSERT_EQ((int)g_pinState[25], 1);
  ASSERT_EQ((int)g_pinState[26], 0);
  std::vector<MqttPublished> s = pubsOn("commands_status");
  ASSERT_EQ((int)s.size(), 1);
  ASSERT_TRUE(s[0].payload == "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"status\":0}");
  ASSERT_TRUE(!s[0].retained);
}

TEST(off_command_clears_pin_and_acks) {
  clearPubs();
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\"}");
  ASSERT_EQ((int)g_pinState[25], 0);
  std::vector<MqttPublished> s = pubsOn("commands_status");
  ASSERT_EQ((int)s.size(), 1);
  ASSERT_TRUE(s[0].payload == "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\",\"status\":0}");
}

TEST(repeated_on_command_is_idempotent_and_acked_each_time) {
  clearPubs();
  send("{\"M_ID\":\"E007\",\"C_ID\":\"P01\",\"com\":\"A1\"}");
  send("{\"M_ID\":\"E007\",\"C_ID\":\"P01\",\"com\":\"A1\"}");   // QoS 1 дубликат
  ASSERT_EQ((int)g_pinState[26], 1);
  ASSERT_EQ((int)pubsOn("commands_status").size(), 2);
  send("{\"M_ID\":\"E007\",\"C_ID\":\"P01\",\"com\":\"B2\"}");
  ASSERT_EQ((int)g_pinState[26], 0);
}

TEST(command_for_another_module_is_ignored_silently) {
  clearPubs();
  uint8_t before25 = g_pinState[25];
  send("{\"M_ID\":\"E008\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  send("{\"M_ID\":\"e007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");   // друг регистър = друг модул
  send("{\"M_ID\":\"E0070\",\"C_ID\":\"V01\",\"com\":\"A1\"}");  // префикс на моя ID
  send("{\"C_ID\":\"V01\",\"com\":\"A1\"}");                      // без M_ID
  ASSERT_EQ((int)g_pinState[25], (int)before25);
  ASSERT_EQ((int)g_mqtt.published.size(), 0);
}

TEST(unknown_consumer_or_invalid_command_is_nacked_without_touching_pins) {
  clearPubs();
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V99\",\"com\":\"A1\"}");   // непознат консуматор
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"ZZ\"}");   // невалиден команден код
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\"}");                   // липсва com
  send("{\"M_ID\":\"E007\",\"com\":\"A1\"}");                     // липсва C_ID
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"C4\"}");   // непознат код
  std::vector<MqttPublished> s = pubsOn("commands_status");
  ASSERT_EQ((int)s.size(), 5);
  for (auto &p : s) ASSERT_TRUE(contains(p.payload, "\"status\":2"));
  ASSERT_EQ((int)g_pinState[25], 0);
  ASSERT_EQ((int)g_pinState[26], 0);
  ASSERT_TRUE(contains(g_serialOut, "[REJECT]"));
}

TEST(state_request_c3_reports_real_pin_states_without_restart_flag) {
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  clearPubs();
  send("{\"M_ID\":\"E007\",\"C_ID\":\"\",\"com\":\"C3\"}");
  std::vector<MqttPublished> st = pubsOn("module_states_response");
  ASSERT_EQ((int)st.size(), 1);
  ASSERT_TRUE(st[0].payload ==
              "{\"id\":\"E007\",\"states\":[{\"id\":\"V01\",\"state\":\"ON\"},{\"id\":\"P01\",\"state\":\"OFF\"}]}");
  ASSERT_EQ((int)pubsOn("commands_status").size(), 0);           // заявката няма commands_status
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\"}");
}

TEST(state_request_for_other_module_is_ignored) {
  clearPubs();
  send("{\"M_ID\":\"E009\",\"com\":\"C3\"}");
  ASSERT_EQ((int)g_mqtt.published.size(), 0);
}

TEST(state_request_without_c_id_field_also_works) {
  clearPubs();
  send("{\"M_ID\":\"E007\",\"com\":\"C3\"}");
  ASSERT_EQ((int)pubsOn("module_states_response").size(), 1);
}

TEST(bad_json_and_foreign_topics_are_ignored) {
  clearPubs();
  g_serialOut.clear();
  send("{bad json");
  send("");
  mqttInject("some_other_topic", "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  loop();
  ASSERT_EQ((int)g_pinState[25], 0);
  ASSERT_EQ((int)g_mqtt.published.size(), 0);
  ASSERT_TRUE(contains(g_serialOut, "[ERR]"));
}

TEST(messages_are_executed_in_arrival_order) {
  clearPubs();
  mqttInject(CMD_TOPIC, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  mqttInject(CMD_TOPIC, "{\"M_ID\":\"E007\",\"C_ID\":\"P01\",\"com\":\"A1\"}");
  mqttInject(CMD_TOPIC, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\"}");
  loop();                                    // една итерация обработва всичко наредено
  std::vector<MqttPublished> s = pubsOn("commands_status");
  ASSERT_EQ((int)s.size(), 3);
  ASSERT_TRUE(contains(s[0].payload, "\"C_ID\":\"V01\",\"com\":\"A1\""));
  ASSERT_TRUE(contains(s[1].payload, "\"C_ID\":\"P01\",\"com\":\"A1\""));
  ASSERT_TRUE(contains(s[2].payload, "\"C_ID\":\"V01\",\"com\":\"B2\""));
  ASSERT_EQ((int)g_pinState[25], 0);
  ASSERT_EQ((int)g_pinState[26], 1);
  send("{\"M_ID\":\"E007\",\"C_ID\":\"P01\",\"com\":\"B2\"}");
}

TEST(queue_holds_8_messages_extra_ones_are_dropped_with_a_warning) {
  clearPubs();
  g_serialOut.clear();
  for (int i = 0; i < 10; i++) mqttInject(CMD_TOPIC, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  loop();
  ASSERT_EQ((int)pubsOn("commands_status").size(), 8);
  ASSERT_TRUE(contains(g_serialOut, "пълна"));
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\"}");
}

TEST(only_the_lwt_messages_are_ever_retained) {
  // във всичко публикувано до тук: retained само на executors/E007/lwt
  clearPubs();
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}");
  send("{\"M_ID\":\"E007\",\"com\":\"C3\"}");
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V99\",\"com\":\"A1\"}");
  for (auto &p : g_mqtt.published) ASSERT_TRUE(!p.retained);
  send("{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\"}");
}

TEST(heartbeat_is_sent_every_30_seconds_while_connected) {
  clearPubs();
  unsigned long t0 = g_fakeMillis;
  g_fakeMillis = t0 + 10000; loop();
  ASSERT_EQ((int)pubsOn("heartbeat").size(), 0);          // още няма 30 s от последния
  // първият heartbeat се очаква 30 s след setup (lastHeartbeat се зарежда при mqtt_wifi_setup)
  g_fakeMillis = t0 + 40000; loop();
  std::vector<MqttPublished> hb = pubsOn("heartbeat");
  ASSERT_EQ((int)hb.size(), 1);
  ASSERT_TRUE(contains(hb[0].payload, "\"id\":\"E007\""));
  ASSERT_TRUE(contains(hb[0].payload, "\"ts\":"));
  g_fakeMillis += 29000; loop();
  ASSERT_EQ((int)pubsOn("heartbeat").size(), 1);
  g_fakeMillis += 2000; loop();
  ASSERT_EQ((int)pubsOn("heartbeat").size(), 2);
}

TEST(no_heartbeat_while_mqtt_is_down) {
  clearPubs();
  g_mqtt.connected = false;
  g_mqtt.connectResult = false;
  g_fakeMillis += 31000;
  loop();
  ASSERT_EQ((int)pubsOn("heartbeat").size(), 0);
  g_mqtt.connectResult = true;      // връща се за следващите тестове
}

TEST(reconnect_resubscribes_and_republishes_birth_but_not_the_restart_response) {
  clearPubs();
  int callsBefore = g_mqtt.connectArgs.calls;
  size_t subsBefore = g_mqtt.subs.size();
  g_mqtt.connected = false;                 // брокерът е паднал
  g_fakeMillis += 6000;                     // > RECONNECT_ATTEMPT_MS
  loop();
  ASSERT_EQ(g_mqtt.connectArgs.calls, callsBefore + 1);
  ASSERT_EQ((int)g_mqtt.subs.size(), (int)subsBefore + 1);   // абониране наново
  ASSERT_TRUE(g_mqtt.subs.back().topic == "wifi_commands");
  ASSERT_EQ(g_mqtt.subs.back().qos, 1);
  std::vector<MqttPublished> lwt = pubsOn("executors/E007/lwt");
  ASSERT_EQ((int)lwt.size(), 1);
  ASSERT_TRUE(contains(lwt[0].payload, "\"online\":true"));
  ASSERT_EQ((int)pubsOn("module_states_response").size(), 0);  // restart:true е САМО след boot
}

TEST(mqtt_connect_failures_retry_every_5s_and_restart_after_2_minutes) {
  g_mqtt.connected = false;
  g_mqtt.connectResult = false;
  g_serialOut.clear();
  g_espRestarted = false;
  int calls0 = g_mqtt.connectArgs.calls;
  g_fakeMillis += 6000; loop();             // 1-ви опит (стартира таймера на падането)
  ASSERT_EQ(g_mqtt.connectArgs.calls, calls0 + 1);
  g_fakeMillis += 2000; loop();             // твърде рано за нов опит
  ASSERT_EQ(g_mqtt.connectArgs.calls, calls0 + 1);
  g_fakeMillis += 4000; loop();
  ASSERT_EQ(g_mqtt.connectArgs.calls, calls0 + 2);
  ASSERT_TRUE(contains(g_serialOut, "FAILED"));
  ASSERT_TRUE(!g_espRestarted);
  g_fakeMillis += 130000; loop();           // над 2 минути без връзка с брокера
  ASSERT_TRUE(g_espRestarted);
  g_espRestarted = false;
  g_mqtt.connectResult = true;
}

TEST(wifi_loss_triggers_reconnect_attempts_and_restart_after_2_minutes) {
  g_mqtt.connected = true;
  g_wifiStatus = WL_DISCONNECTED;
  g_espRestarted = false;
  int begins0 = g_wifiBeginCalls;
  g_fakeMillis += 1000; loop();
  g_fakeMillis += 6000; loop();
  ASSERT_TRUE(g_wifiBeginCalls > begins0);
  int begins1 = g_wifiBeginCalls;
  g_fakeMillis += 2000; loop();
  ASSERT_EQ(g_wifiBeginCalls, begins1);     // не по-често от 5 s
  ASSERT_TRUE(!g_espRestarted);
  for (int i = 0; i < 30; i++) { g_fakeMillis += 5000; loop(); }   // 150 s без Wi-Fi
  ASSERT_TRUE(g_espRestarted);
  g_wifiStatus = WL_CONNECTED;
}

TEST(wifi_loss_blocks_mqtt_attempts_until_wifi_returns) {
  g_wifiStatus = WL_DISCONNECTED;
  g_mqtt.connected = false;
  int calls0 = g_mqtt.connectArgs.calls;
  g_fakeMillis += 6000; loop();
  ASSERT_EQ(g_mqtt.connectArgs.calls, calls0);   // без Wi-Fi не се опитва MQTT
  g_wifiStatus = WL_CONNECTED;
  g_fakeMillis += 6000; loop();
  ASSERT_EQ(g_mqtt.connectArgs.calls, calls0 + 1);
}

TEST(the_module_never_publishes_to_the_gateway_command_topics) {
  // командите в wifi_commands НЕ бива да се връщат в commands (иначе gateway-ят би ги препратил по LoRa)
  for (auto &p : g_mqtt.published) {
    ASSERT_TRUE(p.topic != "commands");
    ASSERT_TRUE(p.topic != "wifi_commands");
    ASSERT_TRUE(p.topic != "module_states_requests");
  }
}

int main() { return runAllTests(); }
