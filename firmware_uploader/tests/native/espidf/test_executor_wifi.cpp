// Тестове на Wi-Fi executor-а (РЕАЛНИТЕ app_main, ew_logic, ew_config + wifi_link/mqtt_link) срещу симулатора.
#include "test_support.h"

#include "board_pins.h"
#include "protocol.h"

extern "C" void app_main(void);

static const int PIN_V01 = 4, PIN_P01 = 5;

static void setup(const char *id = "E007", const char *consumers = "V01:4,P01:5", const char *user = "") {
  sim::nvs.setStr("id", id);
  int n = 0;
  std::string list = consumers;
  size_t pos = 0;
  while (!list.empty() && pos < list.size()) {
    size_t comma = list.find(',', pos);
    std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    size_t colon = item.find(':');
    char ki[8], kp[8];
    snprintf(ki, sizeof(ki), "c%di", n);
    snprintf(kp, sizeof(kp), "c%dp", n);
    sim::nvs.setStr(ki, item.substr(0, colon));
    sim::nvs.setU8(kp, (uint8_t)atoi(item.substr(colon + 1).c_str()));
    n++;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  sim::nvs.setU8("nc", (uint8_t)n);
  sim::nvs.setStr("wssid", "FarmNet");
  sim::nvs.setStr("wpass", "pw");
  sim::nvs.setStr("mqip", "192.168.1.10");
  sim::nvs.setStr("mqport", "1883");
  sim::nvs.setStr("mquser", user);
  sim::nvs.setStr("mqpass", user[0] ? "secret" : "");
}
static void bootEw() {
  sim::boot(app_main);
  sim::run_for_ms(3000);
}
static void toModule(uint32_t afterMs, const std::string &json) {
  sim::at_after_ms(afterMs, [json] { sim::broker.deliver("wifi_commands", json); });
}
static std::string cmd(const char *m, const char *c, const char *com) {
  return std::string("{\"M_ID\":\"") + m + "\",\"C_ID\":\"" + c + "\",\"com\":\"" + com + "\"}";
}
static std::vector<sim::MqttPub> pubs(const char *topic) { return sim::broker.on(topic); }
static bool pinHigh(int pin) { return sim::gpio.read(pin) == 1; }

// ============================================================================ старт
STEST(boot_pins_off_tasks_split_over_cores_and_session_policy) {
  setup("E007", "V01:4,P01:5", "mqttuser");
  sim::boot(app_main);
  sim::run_for_ms(100);
  for (int pin : {PIN_V01, PIN_P01}) {
    ASSERT_FALSE(pinHigh(pin));
    ASSERT_EQ(sim::gpio.pins[pin].mode, 3);
    ASSERT_EQ(sim::gpio.pins[pin].history.front(), 0);
  }
  sim::run_for_ms(2900);
  std::vector<sim::TaskInfo> t = sim::tasks();
  const sim::TaskInfo *ex = sim::find_task(t, "ew_exec");
  const sim::TaskInfo *mq = sim::find_task(t, "mqtt_task");
  ASSERT_TRUE(ex && mq);
  if (ex && mq) { ASSERT_EQ(ex->core, 1); ASSERT_EQ(mq->core, 0); }
  ASSERT_TRUE(sim::find_task(t, "gw_radio") == nullptr);          // няма LoRa
  ASSERT_EQ((int)sim::broker.conns.size(), 1);
  const sim::MqttConn &c = sim::broker.conns[0];
  ASSERT_STREQ(c.clientId, "E007");
  ASSERT_STREQ(c.user, "mqttuser");
  ASSERT_TRUE(c.cleanSession);                                    // clean session: стари команди не се доставят
  ASSERT_STREQ(c.lwtTopic, "executors/E007/lwt");
  ASSERT_STREQ(c.lwtMsg, "{\"id\":\"E007\",\"online\":false}");
  ASSERT_EQ(c.lwtRetain, 1);
  ASSERT_EQ(c.lwtQos, 1);
  ASSERT_TRUE(sim::broker.subscribedTo("wifi_commands", 1));      // QoS 1 абониране, единствената нова тема
  ASSERT_FALSE(sim::broker.subscribedTo("commands"));
  ASSERT_FALSE(sim::broker.subscribedTo("module_states_requests"));
  ASSERT_EQ(sim::wifi.psMode, 0);
  ASSERT_FALSE(sim::pm_stats().configured);
}

STEST(on_connect_publishes_online_status_and_one_restart_state_response) {
  setup();
  bootEw();
  std::vector<sim::MqttPub> l = pubs("executors/E007/lwt");
  ASSERT_EQ((int)l.size(), 1);
  if (!l.empty()) { ASSERT_STREQ(l[0].payload, "{\"id\":\"E007\",\"online\":true}"); ASSERT_TRUE(l[0].retain); ASSERT_EQ(l[0].qos, 1); }
  std::vector<sim::MqttPub> s = pubs("module_states_response");
  ASSERT_EQ((int)s.size(), 1);
  if (!s.empty()) {
    ASSERT_STREQ(s[0].payload, "{\"id\":\"E007\",\"restart\":true,\"states\":[{\"id\":\"V01\",\"state\":\"OFF\"},{\"id\":\"P01\",\"state\":\"OFF\"}]}");
    ASSERT_EQ(s[0].qos, 1);
    ASSERT_FALSE(s[0].retain);
  }
  // връзката пада и се връща: "online" се повтаря, "restart" - не (това е веднъж на boot)
  sim::broker.dropConnection();
  sim::run_for_ms(8000);
  ASSERT_EQ((int)pubs("executors/E007/lwt").size(), 2);
  ASSERT_EQ((int)pubs("module_states_response").size(), 1);
  ASSERT_TRUE(sim::broker.subscribedTo("wifi_commands", 1));       // абонаментът е подновен
}

STEST(incomplete_configuration_leaves_consumers_off_and_starts_no_network) {
  sim::nvs.setStr("id", "E007");
  sim::nvs.setU8("nc", 1);
  sim::nvs.setStr("c0i", "V01");
  sim::nvs.setU8("c0p", 4);                                        // няма Wi-Fi/MQTT
  sim::boot(app_main);
  sim::run_for_ms(2000);
  ASSERT_FALSE(pinHigh(4));
  ASSERT_EQ(sim::wifi.initCalls, 0);
  ASSERT_TRUE(sim::find_task(sim::tasks(), "ew_exec") == nullptr);
  ASSERT_TRUE(sim::log_contains("непълна конфигурация"));
}

// ============================================================================ команди
STEST(on_and_off_commands_switch_the_pin_and_report_status_qos1) {
  setup();
  bootEw();
  sim::broker.published.clear();
  toModule(10, cmd("E007", "V01", "A1"));
  sim::run_for_ms(200);
  ASSERT_TRUE(pinHigh(PIN_V01));
  ASSERT_FALSE(pinHigh(PIN_P01));
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) {
    ASSERT_STREQ(st[0].payload, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"status\":0}");
    ASSERT_EQ(st[0].qos, 1);
    ASSERT_FALSE(st[0].retain);
  }
  toModule(10, cmd("E007", "V01", "B2"));
  sim::run_for_ms(200);
  ASSERT_FALSE(pinHigh(PIN_V01));
  st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 2);
  if (st.size() == 2) ASSERT_STREQ(st[1].payload, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\",\"status\":0}");
}

STEST(invalid_command_and_unknown_consumer_are_nacked_and_change_nothing) {
  setup();
  bootEw();
  sim::broker.published.clear();
  toModule(10, cmd("E007", "V01", "55"));
  toModule(100, cmd("E007", "ZZZ", "A1"));
  sim::run_for_ms(500);
  ASSERT_FALSE(pinHigh(PIN_V01));
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 2);
  for (auto &p : st) ASSERT_STREQ(jraw(p.payload, "status"), "2");
}

STEST(commands_for_other_modules_and_malformed_json_are_ignored_silently) {
  setup();
  bootEw();
  sim::broker.published.clear();
  toModule(10, cmd("E008", "V01", "A1"));                          // всички Wi-Fi модули получават темата
  toModule(20, "not json");
  toModule(30, "{\"C_ID\":\"V01\",\"com\":\"A1\"}");
  toModule(40, "{\"M_ID\":7,\"C_ID\":\"V01\",\"com\":\"A1\"}");
  toModule(50, "{}");
  sim::run_for_ms(500);
  ASSERT_FALSE(pinHigh(PIN_V01));
  ASSERT_EQ((int)pubs("commands_status").size(), 0);              // чужда команда: без статус, без NACK
  ASSERT_FALSE(sim::restarted());
  toModule(10, cmd("E007", "V01", "A1"));
  sim::run_for_ms(200);
  ASSERT_TRUE(pinHigh(PIN_V01));                                   // нормална команда след боклука
}

STEST(hostile_json_nesting_and_memory_exhaustion_are_survived) {
  setup();
  bootEw();
  sim::broker.published.clear();
  std::string deep = "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"x\":" + std::string(300, '[') + std::string(300, ']') + "}";
  toModule(10, deep);                                              // твърде дълбоко: отхвърля се преди разбора
  sim::run_for_ms(300);
  ASSERT_FALSE(pinHigh(PIN_V01));
  ASSERT_EQ((int)pubs("commands_status").size(), 0);
  {
    CjHooks fail(0, true);                                         // няма памет за cJSON: командата не се изпълнява, нищо не се публикува повредено
    toModule(10, cmd("E007", "V01", "A1"));
    sim::run_for_ms(300);
    ASSERT_FALSE(pinHigh(PIN_V01));
    ASSERT_EQ((int)pubs("commands_status").size(), 0);
    ASSERT_FALSE(sim::restarted());
  }
  toModule(10, cmd("E007", "V01", "A1"));                          // паметта се е върнала
  sim::run_for_ms(300);
  ASSERT_TRUE(pinHigh(PIN_V01));
  ASSERT_EQ((int)pubs("commands_status").size(), 1);
  // липса на памет точно при печата на статуса: или точен документ, или нищо
  for (int k = 0; k < 8; k++) {
    CjHooks hooks(k);
    sim::broker.published.clear();
    toModule(10, cmd("E007", "P01", "A1"));
    sim::run_for_ms(300);
    std::vector<sim::MqttPub> st = pubs("commands_status");
    ASSERT_TRUE(st.size() <= 1);
    if (st.size() == 1) ASSERT_STREQ(jraw(st[0].payload, "C_ID"), "\"P01\"");
    ASSERT_TRUE(hooks.balanced());
    toModule(10, cmd("E007", "P01", "55"));
    sim::run_for_ms(200);
  }
}

STEST(extra_fields_whitespace_and_lowercase_hex_are_accepted) {
  setup();
  bootEw();
  toModule(10, "  {\"x\":{\"M_ID\":\"NO\"},\n\"com\" : \"a1\" , \"C_ID\":\"P01\" ,\"M_ID\":\"E007\"}  ");
  sim::run_for_ms(300);
  ASSERT_TRUE(pinHigh(PIN_P01));
}

STEST(state_request_c3_returns_real_states_without_restart_key) {
  setup();
  bootEw();
  toModule(10, cmd("E007", "P01", "A1"));
  sim::run_for_ms(200);
  sim::broker.published.clear();
  toModule(10, "{\"M_ID\":\"E007\",\"com\":\"C3\"}");
  sim::run_for_ms(200);
  std::vector<sim::MqttPub> s = pubs("module_states_response");
  ASSERT_EQ((int)s.size(), 1);
  if (!s.empty()) ASSERT_STREQ(s[0].payload, "{\"id\":\"E007\",\"states\":[{\"id\":\"V01\",\"state\":\"OFF\"},{\"id\":\"P01\",\"state\":\"ON\"}]}");
  sim::gpio.pins[PIN_V01].level = 1;                               // външна промяна на пина се вижда (чете се реалният пин)
  toModule(10, "{\"M_ID\":\"E007\",\"com\":\"C3\"}");
  sim::run_for_ms(200);
  s = pubs("module_states_response");
  ASSERT_EQ((int)s.size(), 2);
  if (s.size() == 2) ASSERT_TRUE(s[1].payload.find("\"V01\",\"state\":\"ON\"") != std::string::npos);
}

STEST(commands_are_executed_in_arrival_order) {
  setup();
  bootEw();
  sim::broker.published.clear();
  toModule(10, cmd("E007", "V01", "A1"));
  toModule(11, cmd("E007", "P01", "A1"));
  toModule(12, cmd("E007", "V01", "B2"));
  toModule(13, cmd("E007", "P01", "B2"));
  toModule(14, cmd("E007", "V01", "A1"));
  sim::run_for_ms(500);
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 5);
  const char *want[5] = {"V01\",\"com\":\"A1", "P01\",\"com\":\"A1", "V01\",\"com\":\"B2", "P01\",\"com\":\"B2", "V01\",\"com\":\"A1"};
  for (int i = 0; i < (int)st.size() && i < 5; i++) ASSERT_TRUE(st[i].payload.find(want[i]) != std::string::npos);
  ASSERT_TRUE(pinHigh(PIN_V01));
  ASSERT_FALSE(pinHigh(PIN_P01));
}

STEST(flood_over_the_queue_size_never_crashes_and_keeps_pin_state_consistent) {
  setup();
  bootEw();
  sim::broker.published.clear();
  for (int i = 0; i < 40; i++) toModule(10, cmd("E007", "V01", (i % 2) ? "B2" : "A1"));   // всичките в един и същ момент
  sim::run_for_ms(3000);
  ASSERT_FALSE(sim::restarted());
  int n = (int)pubs("commands_status").size();
  ASSERT_TRUE(n >= 8 && n <= 40);                                   // най-много опашката се губи, нищо не се чупи
  ASSERT_TRUE(sim::gpio.pins[PIN_V01].outputWrites >= 8);
  toModule(10, cmd("E007", "V01", "B2"));
  sim::run_for_ms(300);
  ASSERT_FALSE(pinHigh(PIN_V01));
}

STEST(consumer_on_an_unusable_gpio_is_never_driven) {
  setup("E007", "V01:4,BAD:30");
  bootEw();
  sim::broker.published.clear();
  toModule(10, cmd("E007", "BAD", "A1"));
  sim::run_for_ms(300);
  ASSERT_EQ(sim::gpio.pins[30].outputWrites, 0);
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) ASSERT_STREQ(jraw(st[0].payload, "status"), "2");
}

// ============================================================================ мрежа
STEST(no_status_is_published_into_the_void_while_offline_and_nothing_is_queued) {
  setup();
  bootEw();
  sim::broker.published.clear();
  toModule(10, cmd("E007", "V01", "A1"));
  sim::at_after_ms(10, [] { sim::broker.dropConnection(); });      // връзката пада точно след доставката на командата
  sim::run_for_ms(300);
  ASSERT_TRUE(pinHigh(PIN_V01));                                    // командата е изпълнена
  ASSERT_EQ(sim::broker.outboxQueued, 0);                           // статусът не отива в outbox за "късна" доставка
  sim::run_for_ms(15000);
  ASSERT_EQ((int)pubs("commands_status").size(), 0);
}

STEST(commands_during_an_outage_are_never_delivered_after_reconnect) {
  setup();
  bootEw();
  sim::broker.setOnline(false);
  sim::run_for_ms(1000);
  ASSERT_FALSE(sim::broker.deliver("wifi_commands", cmd("E007", "V01", "A1")));
  sim::broker.setOnline(true);
  sim::run_for_ms(10000);
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_FALSE(pinHigh(PIN_V01));                                   // стара команда не се изпълнява със закъснение
  toModule(10, cmd("E007", "V01", "A1"));
  sim::run_for_ms(300);
  ASSERT_TRUE(pinHigh(PIN_V01));
}

STEST(wifi_outage_then_recovery_restores_service_without_restart) {
  setup();
  bootEw();
  sim::wifi.apUp = false;
  sim::wifi.dropLink();
  sim::run_for_ms(30000);
  ASSERT_FALSE(sim::broker.connected);
  sim::wifi.apUp = true;
  sim::run_for_ms(30000);
  ASSERT_TRUE(sim::broker.connected);
  toModule(10, cmd("E007", "P01", "A1"));
  sim::run_for_ms(300);
  ASSERT_TRUE(pinHigh(PIN_P01));
  ASSERT_FALSE(sim::restarted());
}

STEST(heartbeat_every_30_seconds_only_while_connected) {
  setup();
  bootEw();
  sim::broker.published.clear();
  sim::run_for_ms(95000);
  std::vector<sim::MqttPub> hb = pubs("heartbeat");
  ASSERT_EQ((int)hb.size(), 3);
  if (!hb.empty()) { ASSERT_STREQ(jraw(hb[0].payload, "id"), "\"E007\""); ASSERT_EQ(hb[0].qos, 0); ASSERT_FALSE(hb[0].retain); }
  sim::broker.setOnline(false);
  sim::broker.published.clear();
  sim::run_for_ms(60000);
  ASSERT_EQ((int)pubs("heartbeat").size(), 0);
  ASSERT_EQ(sim::broker.outboxQueued, 0);
}

STEST(nothing_but_the_last_will_is_ever_retained) {
  setup();
  bootEw();
  toModule(10, cmd("E007", "V01", "A1"));
  toModule(500, "{\"M_ID\":\"E007\",\"com\":\"C3\"}");
  sim::run_for_ms(40000);
  for (auto &p : sim::broker.published) {
    if (p.topic == "executors/E007/lwt") ASSERT_TRUE(p.retain);
    else ASSERT_FALSE(p.retain);
  }
}

STEST(idle_for_an_hour_does_not_restart_or_leak_wakeups) {
  setup();
  bootEw();
  sim::run_for_ms(3600000);
  ASSERT_FALSE(sim::restarted());
  ASSERT_TRUE(sim::broker.connected);
}

STEST(chaos_interleavings_give_the_same_result) {
  for (uint32_t seed = 1; seed <= 20; seed++) {
    sim::reset();
    setup();
    sim::chaos(seed, 250);
    bootEw();
    sim::broker.published.clear();
    toModule(10, cmd("E007", "V01", "A1"));
    toModule(11, cmd("E007", "P01", "A1"));
    toModule(300, "{\"M_ID\":\"E007\",\"com\":\"C3\"}");
    sim::run_for_ms(2000);
    ASSERT_TRUE(pinHigh(PIN_V01));
    ASSERT_TRUE(pinHigh(PIN_P01));
    ASSERT_EQ((int)pubs("commands_status").size(), 2);
    ASSERT_EQ((int)pubs("module_states_response").size(), 1);
    ASSERT_FALSE(sim::restarted());
  }
}

int main() { return runAllTests(); }
