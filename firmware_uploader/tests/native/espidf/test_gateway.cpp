// Тестове на gateway-я (РЕАЛНИТЕ app_main, gw_radio, gw_logic, gw_config + wifi_link/mqtt_link) срещу симулатора.
#include "test_support.h"

#include <algorithm>

#include "board_pins.h"
#include "crypto_common.h"
#include "esp_system.h"
#include "protocol.h"
#include "radio_timing.h"
#include "agro_radio.h"
#include "test_radio_support.h"
#include "esp_task_wdt.h"

extern "C" void app_main(void);

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);

static const uint32_t FREQ = 433000000;

// ---------------------------------------------------------------- помощници
static void setup(uint8_t sf = 7, uint32_t bw = 125000) {
  nvsRadioConfig("G001", FREQ, sf, bw);
  sim::nvs.setStr("wssid", "FarmNet");
  sim::nvs.setStr("wpass", "pw");
  sim::nvs.setStr("mqip", "192.168.1.10");
  sim::nvs.setStr("mqport", "1883");
}
static void bootGw() {
  sim::boot(app_main);
  sim::run_for_ms(3000);                      // Wi-Fi ~0.3 s, MQTT ~+0.05 s, абонаменти
}

static uint8_t pad6(uint8_t *d, const char *s) { memset(d, 0, 6); memcpy(d, s, strlen(s) < 6 ? strlen(s) : 6); return 6; }

static uint32_t g_ctr = 100;
static std::vector<uint8_t> wire(uint8_t type, const char *sender, const std::vector<uint8_t> &pt) {
  uint8_t s6[6], w[CRYPTO_OVERHEAD + CRYPTO_MAX_PT];
  pad6(s6, sender);
  uint8_t n = cryptoBuildWirePacket(w, TEST_KEY, type, s6, g_ctr++, pt.data(), (uint8_t)pt.size());
  return std::vector<uint8_t>(w, w + n);
}
static std::vector<uint8_t> sensorFrame(const char *id, int16_t a, int16_t b, int16_t c, int16_t d) {
  std::vector<uint8_t> pt(8);
  int16_t v[4] = {a, b, c, d};
  for (int i = 0; i < 4; i++) { pt[2 * i] = v[i] & 0xff; pt[2 * i + 1] = (uint8_t)(v[i] >> 8); }
  return wire(CRYPTO_TYPE_SENSOR, id, pt);
}
static std::vector<uint8_t> ackFrame(const char *m, const char *cid, uint8_t com, uint8_t status) {
  std::vector<uint8_t> pt(6, 0);
  memcpy(pt.data(), cid, strlen(cid));
  pt[4] = com;
  pt[5] = status;
  return wire(CRYPTO_TYPE_EXEC_ACK, m, pt);
}
static std::vector<uint8_t> stateFrame(const char *m, bool restart, const std::vector<std::pair<std::string, int>> &e) {
  std::vector<uint8_t> pt;
  for (auto &x : e) {
    uint8_t c[4] = {0};
    memcpy(c, x.first.data(), x.first.size() < 4 ? x.first.size() : 4);
    pt.insert(pt.end(), c, c + 4);
    pt.push_back((uint8_t)x.second);
  }
  return wire(restart ? CRYPTO_TYPE_STATE_RESP_RESTART : CRYPTO_TYPE_STATE_RESP, m, pt);
}
static std::vector<uint8_t> hbFrame(const char *m, bool repeater) {
  return wire(repeater ? CRYPTO_TYPE_REPEATER_HB : CRYPTO_TYPE_EXEC_HB, m, {});
}

static void inject(uint32_t afterMs, const std::vector<uint8_t> &f, long freq = FREQ) {
  sim::at_after_ms(afterMs, [f, freq] { sim::radio.deliver(f, freq, -70, 6.25f); }, true);
}
static void mqttIn(uint32_t afterMs, const char *topic, const std::string &payload) {
  sim::at_after_ms(afterMs, [topic, payload] { sim::broker.deliver(topic, payload); });
}

static std::vector<sim::MqttPub> pubs(const char *topic) { return sim::broker.on(topic); }

static double jnum(const std::string &json, const char *key) { return atof(jraw(json, key).c_str()); }

struct TxFrame { uint64_t t; std::string kind; std::string target, sender; std::vector<uint8_t> pt; size_t len; long freq; };
static std::vector<TxFrame> gwTx() {
  std::vector<TxFrame> r;
  struct { const char *name; uint8_t type; } types[3] = {{"CMD", CRYPTO_TYPE_GW_CMD}, {"STATE_REQ", CRYPTO_TYPE_STATE_REQ}, {"STATE_ACK", CRYPTO_TYPE_STATE_RESP_ACK}};
  for (auto &f : sim::airLog) {
    TxFrame t;
    t.t = f.tStart; t.kind = "?"; t.len = f.data.size(); t.freq = f.freqHz;
    if (f.data.size() > 6) {
      for (int i = 0; i < 6 && f.data[i]; i++) t.target += (char)f.data[i];
      for (auto &ty : types) {
        uint8_t sender[6], pt[CRYPTO_MAX_PT], len = 0;
        if (cryptoParseWirePacket(f.data.data() + 6, (uint8_t)(f.data.size() - 6), TEST_KEY, ty.type, sender, pt, &len)) {
          t.kind = ty.name;
          for (int k = 0; k < 6 && sender[k]; k++) t.sender += (char)sender[k];
          t.pt.assign(pt, pt + len);
          break;
        }
      }
    }
    r.push_back(t);
  }
  return r;
}
static std::vector<TxFrame> gwTxKind(const char *k) {
  std::vector<TxFrame> r;
  for (auto &t : gwTx()) if (t.kind == k) r.push_back(t);
  return r;
}
static const std::string CMD_ON_E007 = "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}";

// ============================================================================ старт и топология на задачите
STEST(boot_distributes_tasks_over_both_cores_and_connects) {
  setup();
  bootGw();
  std::vector<sim::TaskInfo> t = sim::tasks();
  const sim::TaskInfo *radio = sim::find_task(t, "gw_radio");
  const sim::TaskInfo *cmd = sim::find_task(t, "gw_cmd");
  const sim::TaskInfo *mq = sim::find_task(t, "mqtt_task");
  ASSERT_TRUE(radio && cmd && mq);
  ASSERT_TRUE(sim::find_task(t, "watchdog") == nullptr);          // собствената watchdog задача я няма: Task WDT на IDF
  if (radio && cmd && mq) {
    ASSERT_EQ(radio->core, 1);                                   // ядро 1: LoRa радио
    ASSERT_EQ(cmd->core, 0);                                     // ядро 0: логика + MQTT/Wi-Fi
    ASSERT_EQ(mq->core, 0);
    ASSERT_TRUE(radio->prio > cmd->prio);                        // приемането е най-приоритетно
    ASSERT_TRUE(radio->prio > mq->prio);
  }
  ASSERT_TRUE(sim::wifi.connected);
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_EQ((int)sim::broker.conns.size(), 1);
  ASSERT_STREQ(sim::broker.conns[0].clientId, "G001");
  ASSERT_TRUE(sim::broker.conns[0].cleanSession);
  ASSERT_STREQ(sim::broker.conns[0].lwtTopic, "");
  ASSERT_TRUE(sim::broker.subscribedTo("commands", 1));
  ASSERT_TRUE(sim::broker.subscribedTo("module_states_requests", 1));
  ASSERT_FALSE(sim::broker.subscribedTo("wifi_commands"));       // Wi-Fi executor-ите са отделен канал
  ASSERT_TRUE(sim::radio.listening());
  ASSERT_LE(std::labs(sim::radio.freqHz() - (long)FREQ), 61L);
  ASSERT_EQ(sim::wifi.psMode, 0);
  ASSERT_FALSE(sim::pm_stats().configured);                      // мрежово захранване: без лек сън
  ASSERT_TRUE(sim::gpio.pins[LORA_DIO1].intrEnabled);
}

STEST(incomplete_configuration_starts_nothing) {
  nvsRadioConfig("G001");                                         // няма Wi-Fi/MQTT
  sim::boot(app_main);
  sim::run_for_ms(2000);
  ASSERT_TRUE(sim::find_task(sim::tasks(), "gw_radio") == nullptr);
  ASSERT_TRUE(sim::log_contains("непълна конфигурация"));
}

STEST(mqtt_port_is_read_from_the_string_stored_by_the_config_step_and_defaults_to_1883) {
  setup();
  sim::nvs.setStr("mqport", "8883");
  bootGw();
  ASSERT_EQ((int)sim::broker.port, 8883);
  sim::reset();
  setup();
  sim::nvs.setStr("mqport", "0");
  bootGw();
  ASSERT_EQ((int)sim::broker.port, 1883);
}

// ============================================================================ sensor -> MQTT
STEST(sensor_frame_becomes_the_expected_json_on_the_sensors_topic) {
  setup();
  bootGw();
  inject(10, sensorFrame("S001", 2257, 5000, -430, 8870));
  sim::run_for_ms(500);
  std::vector<sim::MqttPub> p = pubs("sensors");
  ASSERT_EQ((int)p.size(), 1);
  if (!p.empty()) {
    ASSERT_EQ(p[0].qos, 0);
    ASSERT_FALSE(p[0].retain);                                    // КЛЮЧОВО: нищо не е retained (старият gateway ги retain-ваше)
    const std::string &j = p[0].payload;
    ASSERT_STREQ(jraw(j, "id"), "\"S001\"");
    ASSERT_STREQ(jraw(j, "soil_t"), "22.57");
    ASSERT_STREQ(jraw(j, "soil_h"), "50.00");
    ASSERT_STREQ(jraw(j, "air_t"), "-4.30");
    ASSERT_STREQ(jraw(j, "air_h"), "88.70");
    ASSERT_STREQ(jraw(j, "rssi"), "-70");
    ASSERT_NEAR(jnum(j, "snr"), 6.25, 0.001);
    ASSERT_GE((long)jnum(j, "ts"), 3L);
    ASSERT_LE((long)jnum(j, "ts"), 10L);                          // секунди от старта (не милисекунди)
    // редът на ключовете е като в Arduino версията
    size_t a = j.find("\"id\""), b = j.find("\"soil_t\""), c = j.find("\"soil_h\""), d = j.find("\"air_t\""), e = j.find("\"air_h\""),
           f = j.find("\"rssi\""), g = j.find("\"snr\""), h = j.find("\"ts\"");
    ASSERT_TRUE(a < b && b < c && c < d && d < e && e < f && f < g && g < h);
  }
}

STEST(no_data_marker_is_published_as_minus_32768_for_that_value_only) {
  setup();
  bootGw();
  inject(10, sensorFrame("S002", SENSOR_RAW_NODATA, SENSOR_RAW_NODATA, 2000, SENSOR_RAW_NODATA));
  sim::run_for_ms(500);
  std::vector<sim::MqttPub> p = pubs("sensors");
  ASSERT_EQ((int)p.size(), 1);
  if (!p.empty()) {
    ASSERT_STREQ(jraw(p[0].payload, "soil_t"), "-32768");
    ASSERT_STREQ(jraw(p[0].payload, "soil_h"), "-32768");
    ASSERT_STREQ(jraw(p[0].payload, "air_t"), "20.00");
    ASSERT_STREQ(jraw(p[0].payload, "air_h"), "-32768");
  }
}

STEST(value_range_incl_negatives_and_extremes_survives_the_trip) {
  setup();
  bootGw();
  const int16_t vals[] = {-4000, -1, 0, 1, 5, 99, 10000, 32767, -32767};
  const char *want[] = {"-40.00", "-0.01", "0.00", "0.01", "0.05", "0.99", "100.00", "327.67", "-327.67"};
  for (int i = 0; i < 9; i++) inject(10 + 600 * i, sensorFrame("S003", vals[i], vals[i], vals[i], vals[i]));
  sim::run_for_ms(8000);
  std::vector<sim::MqttPub> p = pubs("sensors");
  ASSERT_EQ((int)p.size(), 9);
  for (int i = 0; i < (int)p.size() && i < 9; i++) ASSERT_STREQ(jraw(p[i].payload, "soil_t"), want[i]);
}

STEST(duplicates_from_two_repeaters_are_published_once) {
  setup();
  bootGw();
  std::vector<uint8_t> f = sensorFrame("S001", 2000, 4000, 2100, 5000);
  inject(10, f);
  inject(400, f);       // препратен от repeater 1
  inject(900, f);       // препратен от repeater 2
  sim::run_for_ms(3000);
  ASSERT_EQ((int)pubs("sensors").size(), 1);
}

STEST(dedup_remembers_eight_frames) {
  setup();
  bootGw();
  std::vector<std::vector<uint8_t>> fr;
  for (int i = 0; i < 9; i++) fr.push_back(sensorFrame("S004", (int16_t)(100 + i), 0, 0, 0));
  for (int i = 0; i < 9; i++) inject(10 + 300 * i, fr[i]);
  inject(10 + 300 * 9, fr[8]);                                    // още в буфера
  inject(10 + 300 * 10, fr[0]);                                   // изпаднал (8 записа) -> публикува се пак
  sim::run_for_ms(8000);
  ASSERT_EQ((int)pubs("sensors").size(), 10);
}

STEST(tampered_foreign_key_and_old_30_byte_frames_are_rejected) {
  setup();
  bootGw();
  std::vector<uint8_t> f = sensorFrame("S001", 1, 2, 3, 4);
  std::vector<uint8_t> t = f;
  t[10] ^= 0x04;
  inject(10, t);
  uint8_t other[16];
  memcpy(other, TEST_KEY, 16);
  other[2] ^= 1;
  uint8_t s6[6] = {'S', '0', '0', '1', 0, 0}, w[22], pt[8] = {1, 0, 2, 0, 3, 0, 4, 0};
  cryptoBuildWirePacket(w, other, CRYPTO_TYPE_SENSOR, s6, 5, pt, 8);
  inject(500, std::vector<uint8_t>(w, w + 22));
  inject(1000, std::vector<uint8_t>(30, 0x33));                   // старият 30-байтов формат вече не се приема
  inject(1500, std::vector<uint8_t>(3, 0x33));
  sim::run_for_ms(3000);
  ASSERT_EQ((int)pubs("sensors").size(), 0);
  ASSERT_TRUE(sim::log_contains("невалиден MAC"));
  ASSERT_TRUE(sim::log_contains("UNKNOWN length"));
  // след отхвърлените пакети истинският минава (отхвърленият не е попаднал в dedup)
  inject(10, f);
  sim::run_for_ms(500);
  ASSERT_EQ((int)pubs("sensors").size(), 1);
}

STEST(crc_error_packets_are_dropped_by_the_radio) {
  setup();
  bootGw();
  sim::at_after_ms(10, [] { sim::radio.deliver(sensorFrame("S001", 1, 2, 3, 4), FREQ, -90, 1, false); }, true);
  sim::run_for_ms(500);
  ASSERT_EQ((int)pubs("sensors").size(), 0);
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
}

// ============================================================================ heartbeat
STEST(repeater_and_executor_heartbeats_are_published_with_their_fields) {
  setup();
  bootGw();
  inject(10, hbFrame("R001", true));
  inject(500, hbFrame("E007", false));
  sim::run_for_ms(1500);
  std::vector<sim::MqttPub> hb = pubs("heartbeat");
  ASSERT_EQ((int)hb.size(), 2);
  if (hb.size() == 2) {
    ASSERT_STREQ(jraw(hb[0].payload, "id"), "\"R001\"");
    ASSERT_STREQ(jraw(hb[0].payload, "rssi"), "-70");
    ASSERT_STREQ(jraw(hb[0].payload, "snr"), "6.25");
    ASSERT_STREQ(jraw(hb[0].payload, "ts") == "<няма>" ? "no" : "yes", "yes");
    ASSERT_STREQ(jraw(hb[1].payload, "id"), "\"E007\"");
    ASSERT_STREQ(jraw(hb[1].payload, "rssi"), "<няма>");          // executor HB е без rssi/snr (като преди)
    ASSERT_FALSE(hb[0].retain || hb[1].retain);
    ASSERT_EQ(hb[0].qos, 0);
  }
}

STEST(gateway_heartbeat_every_30_seconds_only_while_connected) {
  setup();
  bootGw();
  sim::broker.published.clear();
  sim::run_for_ms(95000);
  std::vector<sim::MqttPub> hb = pubs("heartbeat");
  ASSERT_EQ((int)hb.size(), 3);
  if (!hb.empty()) { ASSERT_STREQ(jraw(hb[0].payload, "id"), "\"G001\""); ASSERT_FALSE(hb[0].retain); }
  sim::broker.setOnline(false);
  sim::broker.published.clear();
  sim::run_for_ms(60000);
  ASSERT_EQ((int)pubs("heartbeat").size(), 0);                    // без връзка не се публикува
  ASSERT_EQ(sim::broker.outboxQueued, 0);                         // и нищо не се натрупва за "късна" доставка
}

// ============================================================================ state response
STEST(state_response_normal_has_no_restart_key_and_restart_has_it_and_is_acked_over_radio) {
  setup();
  bootGw();
  inject(10, stateFrame("E007", false, {{"V01", 1}, {"P01", 0}}));
  inject(1500, stateFrame("E007", true, {{"V01", 0}, {"P01", 0}}));
  sim::run_for_ms(4000);
  std::vector<sim::MqttPub> r = pubs("module_states_response");
  ASSERT_EQ((int)r.size(), 2);
  if (r.size() == 2) {
    ASSERT_STREQ(r[0].payload, "{\"id\":\"E007\",\"states\":[{\"id\":\"V01\",\"state\":\"ON\"},{\"id\":\"P01\",\"state\":\"OFF\"}]}");
    ASSERT_STREQ(r[1].payload, "{\"id\":\"E007\",\"restart\":true,\"states\":[{\"id\":\"V01\",\"state\":\"OFF\"},{\"id\":\"P01\",\"state\":\"OFF\"}]}");
    ASSERT_EQ(r[0].qos, 1);                                       // QoS 1: състоянията не бива да се губят
    ASSERT_FALSE(r[0].retain);
  }
  std::vector<TxFrame> acks = gwTxKind("STATE_ACK");              // само за RESTART варианта
  ASSERT_EQ((int)acks.size(), 1);
  if (!acks.empty()) {
    ASSERT_STREQ(acks[0].target, "E007");
    ASSERT_STREQ(acks[0].sender, "G001");
    ASSERT_EQ((int)acks[0].len, 20);
    ASSERT_EQ((int)acks[0].pt.size(), 0);
    ASSERT_LE(std::labs(acks[0].freq - (long)FREQ), 61L);
  }
  ASSERT_EQ((int)sim::airLog.size(), 1);                          // нормалният отговор не се потвърждава
}

STEST(state_response_with_zero_and_ten_consumers) {
  setup();
  bootGw();
  inject(10, stateFrame("E001", false, {}));                      // 14 B: еднаква дължина с heartbeat
  std::vector<std::pair<std::string, int>> ten;
  for (int i = 0; i < 10; i++) ten.push_back({"C" + std::to_string(i), i % 2});
  inject(800, stateFrame("E002", false, ten));                    // 64 B - най-големият пакет
  sim::run_for_ms(3000);
  std::vector<sim::MqttPub> r = pubs("module_states_response");
  ASSERT_EQ((int)r.size(), 2);
  if (r.size() == 2) {
    ASSERT_STREQ(r[0].payload, "{\"id\":\"E001\",\"states\":[]}");
    ASSERT_STREQ(jraw(r[1].payload, "id"), "\"E002\"");
    ASSERT_TRUE(r[1].payload.find("\"C9\",\"state\":\"ON\"") != std::string::npos);
    ASSERT_TRUE(r[1].payload.size() < 400);
  }
  ASSERT_EQ((int)pubs("heartbeat").size(), 0);                    // не е объркан с heartbeat
}

// ============================================================================ команди
STEST(command_goes_out_as_a_decodable_frame_and_ack_becomes_a_status) {
  setup();
  bootGw();
  uint64_t t0 = sim::now_us();
  mqttIn(100, "commands", CMD_ON_E007);
  inject(900, ackFrame("E007", "V01", 0xA1, 0));
  sim::run_for_ms(2000);
  std::vector<TxFrame> c = gwTxKind("CMD");
  ASSERT_EQ((int)c.size(), 1);
  if (!c.empty()) {
    ASSERT_EQ((int)c[0].len, 26);
    ASSERT_STREQ(c[0].target, "E007");
    ASSERT_STREQ(c[0].sender, "G001");
    ASSERT_EQ((int)c[0].pt.size(), 6);
    ASSERT_STREQ(std::string((const char *)c[0].pt.data(), 3), "V01");
    ASSERT_EQ(c[0].pt[4], 0xA1);
    ASSERT_EQ(c[0].pt[5], 0xFF);
    ASSERT_LE((long)((c[0].t - t0) / 1000), 100L + 3000L / 10);   // веднага след MQTT съобщението
    ASSERT_LE(std::labs(c[0].freq - (long)FREQ), 61L);
  }
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) {
    ASSERT_STREQ(st[0].payload, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"status\":0}");
    ASSERT_EQ(st[0].qos, 1);
    ASSERT_FALSE(st[0].retain);
  }
  ASSERT_FALSE(sim::log_contains("RETRY"));
}

STEST(nack_from_the_executor_becomes_status_2) {
  setup();
  bootGw();
  mqttIn(100, "commands", "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\"}");
  inject(900, ackFrame("E007", "V01", 0xB2, 2));
  sim::run_for_ms(2000);
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) ASSERT_STREQ(st[0].payload, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"B2\",\"status\":2}");
}

STEST(no_ack_two_retries_then_timeout_status_with_intervals_from_sf_bw) {
  setup();
  bootGw();
  mqttIn(100, "commands", CMD_ON_E007);
  sim::run_for_ms(30000);
  std::vector<TxFrame> c = gwTxKind("CMD");
  ASSERT_EQ((int)c.size(), 3);                                    // 1 + 2 повторения
  if (c.size() == 3) {
    long g1 = (long)((c[1].t - c[0].t) / 1000), g2 = (long)((c[2].t - c[1].t) / 1000);
    ASSERT_GE(g1, (long)radioAckTimeoutCmdMs());
    ASSERT_LE(g1, (long)radioAckTimeoutCmdMs() + 60);
    ASSERT_GE(g2, (long)radioAckTimeoutCmdMs());
    ASSERT_LE(g2, (long)radioAckTimeoutCmdMs() + 60);
    ASSERT_MEM_NE(sim::airLog[0].data.data() + 6, sim::airLog[1].data.data() + 6, 14);   // всяко повторение е с нов nonce
  }
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) ASSERT_STREQ(st[0].payload, "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"status\":1}");   // TIMEOUT
}

STEST(second_command_while_one_is_pending_is_ignored_and_the_next_works_after_completion) {
  setup();
  bootGw();
  mqttIn(100, "commands", CMD_ON_E007);
  mqttIn(300, "commands", "{\"M_ID\":\"E008\",\"C_ID\":\"V09\",\"com\":\"A1\"}");
  inject(900, ackFrame("E007", "V01", 0xA1, 0));
  sim::run_for_ms(2500);
  ASSERT_EQ((int)gwTxKind("CMD").size(), 1);                      // втората не е излъчена
  ASSERT_TRUE(sim::log_contains("команда вече чака"));
  mqttIn(100, "commands", "{\"M_ID\":\"E008\",\"C_ID\":\"V09\",\"com\":\"B2\"}");
  inject(900, ackFrame("E008", "V09", 0xB2, 0));
  sim::run_for_ms(2500);
  ASSERT_EQ((int)gwTxKind("CMD").size(), 2);
  ASSERT_EQ((int)pubs("commands_status").size(), 2);
}

STEST(wrong_ack_is_ignored_and_the_command_times_out) {
  setup();
  bootGw();
  mqttIn(100, "commands", CMD_ON_E007);
  inject(700, ackFrame("E999", "V01", 0xA1, 0));                  // друг модул
  inject(900, ackFrame("E007", "V02", 0xA1, 0));                  // друг консуматор
  inject(1100, ackFrame("E007", "V01", 0xB2, 0));                 // друга команда
  std::vector<uint8_t> bad = ackFrame("E007", "V01", 0xA1, 0);
  bad[8] ^= 1;
  inject(1300, bad);                                              // повреден MAC
  sim::run_for_ms(30000);
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) ASSERT_EQ((int)jnum(st[0].payload, "status"), 1);
}

STEST(invalid_mqtt_commands_are_dropped_without_transmission) {
  setup();
  bootGw();
  const char *bad[] = {"{}", "not json", "{\"M_ID\":\"E007\"}", "{\"M_ID\":\"\",\"C_ID\":\"V01\",\"com\":\"A1\"}",
                       "{\"M_ID\":\"E0077777\",\"C_ID\":\"V01\",\"com\":\"A1\"}",     // отрязан id би съвпаднал с друг модул
                       "{\"M_ID\":\"E007\",\"C_ID\":\"V0123\",\"com\":\"A1\"}",
                       "{\"M_ID\":7,\"C_ID\":\"V01\",\"com\":\"A1\"}"};
  for (int i = 0; i < 7; i++) mqttIn(100 + 100 * i, "commands", bad[i]);
  std::string huge = "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"x\":\"" + std::string(300, 'x') + "\"}";
  mqttIn(1000, "commands", huge);
  sim::run_for_ms(3000);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_FALSE(sim::restarted());
  mqttIn(100, "commands", CMD_ON_E007);                           // нормална команда след боклука
  inject(900, ackFrame("E007", "V01", 0xA1, 0));
  sim::run_for_ms(2000);
  ASSERT_EQ((int)pubs("commands_status").size(), 1);
}

STEST(command_with_extra_fields_and_whitespace_is_accepted_com_is_hex) {
  setup();
  bootGw();
  mqttIn(100, "commands", "  {\"extra\": [1,2,{\"M_ID\":\"NO\"}],\n \"com\" : \"a1\" , \"C_ID\":\"V01\",\"M_ID\":\"E007\"}  ");
  inject(700, ackFrame("E007", "V01", 0xA1, 0));
  sim::run_for_ms(1500);
  std::vector<TxFrame> c = gwTxKind("CMD");
  ASSERT_EQ((int)c.size(), 1);
  if (!c.empty()) { ASSERT_STREQ(c[0].target, "E007"); ASSERT_EQ(c[0].pt[4], 0xA1); }
}

STEST(hostile_json_nesting_in_an_mqtt_payload_is_dropped_without_touching_the_stack) {
  setup();
  bootGw();
  // cJSON е рекурсивна: дълбоко вложен обект (в рамките на допустимия размер на съобщението) се отхвърля преди разбора
  std::string deep = "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"x\":" + std::string(150, '[') + std::string(150, ']') + "}";
  mqttIn(100, "commands", deep);
  mqttIn(200, "module_states_requests", "{\"M_ID\":\"E007\",\"x\":" + std::string(150, '{') + "}");
  sim::run_for_ms(2000);
  ASSERT_EQ((int)sim::airLog.size(), 0);                          // нищо не е предадено
  ASSERT_FALSE(sim::restarted());
  mqttIn(100, "commands", CMD_ON_E007);                           // и gateway-ят работи нормално след това
  inject(900, ackFrame("E007", "V01", 0xA1, 0));
  sim::run_for_ms(2000);
  ASSERT_EQ((int)pubs("commands_status").size(), 1);
}

STEST(json_memory_exhaustion_publishes_nothing_corrupt_and_the_gateway_recovers) {
  setup();
  bootGw();
  {
    CjHooks fail(0, true);                                         // всяко заделяне на cJSON се проваля
    inject(10, sensorFrame("S001", 2257, 5000, -430, 8870));
    inject(60, hbFrame("E007", false));
    mqttIn(200, "commands", CMD_ON_E007);                          // и разборът на команда не успява
    mqttIn(300, "module_states_requests", "{\"M_ID\":\"E007\"}");
    sim::run_for_ms(1500);
    ASSERT_EQ((int)pubs("sensors").size(), 0);                    // нищо отрязано/повредено в MQTT
    ASSERT_EQ((int)pubs("heartbeat").size(), 0);
    ASSERT_TRUE(sim::log_contains("JSON не е изграден"));
    ASSERT_EQ((int)sim::airLog.size(), 0);                         // команда без валиден разбор не се предава
    ASSERT_FALSE(sim::restarted());
  }
  inject(10, sensorFrame("S001", 2257, 5000, -430, 8870));         // паметта се е върнала -> работи
  sim::run_for_ms(500);
  ASSERT_EQ((int)pubs("sensors").size(), 1);
}

STEST(every_single_json_allocation_failure_while_publishing_is_survived) {
  // за ВСЯКА възможна липса на памет по пътя на публикуването: или пълен точен документ, или нищо; без срив
  for (int k = 0; k < 12; k++) {
    sim::reset();
    setup();
    bootGw();
    {
      CjHooks hooks(k);
      inject(10, sensorFrame("S001", 2257, 5000, -430, 8870));
      sim::run_for_ms(400);
      std::vector<sim::MqttPub> p = pubs("sensors");
      ASSERT_TRUE(p.size() <= 1);
      if (p.size() == 1) {
        ASSERT_STREQ(jraw(p[0].payload, "id"), "\"S001\"");
        ASSERT_STREQ(jraw(p[0].payload, "air_h"), "88.70");
      }
      ASSERT_FALSE(sim::restarted());
      ASSERT_TRUE(hooks.balanced());                                // без теч
    }
  }
}

// ============================================================================ state request
STEST(state_request_roundtrip) {
  setup();
  bootGw();
  mqttIn(100, "module_states_requests", "{\"M_ID\":\"E007\"}");
  inject(900, stateFrame("E007", false, {{"V01", 1}}));
  sim::run_for_ms(3000);
  std::vector<TxFrame> q = gwTxKind("STATE_REQ");
  ASSERT_EQ((int)q.size(), 1);
  if (!q.empty()) { ASSERT_EQ((int)q[0].len, 20); ASSERT_STREQ(q[0].target, "E007"); ASSERT_STREQ(q[0].sender, "G001"); }
  std::vector<sim::MqttPub> r = pubs("module_states_response");
  ASSERT_EQ((int)r.size(), 1);
  ASSERT_FALSE(sim::log_contains("RETRY STATE_REQ"));
}

STEST(second_state_request_while_one_is_pending_is_ignored) {
  setup();
  bootGw();
  mqttIn(100, "module_states_requests", "{\"M_ID\":\"E007\"}");
  mqttIn(300, "module_states_requests", "{\"M_ID\":\"E008\"}");
  inject(700, stateFrame("E007", false, {{"V01", 1}}));
  sim::run_for_ms(2000);
  std::vector<TxFrame> q = gwTxKind("STATE_REQ");
  ASSERT_EQ((int)q.size(), 1);
  if (!q.empty()) ASSERT_STREQ(q[0].target, "E007");
  ASSERT_TRUE(sim::log_contains("state request вече чака"));
}

STEST(state_request_without_answer_retries_then_publishes_timeout) {
  setup();
  bootGw();
  mqttIn(100, "module_states_requests", "{\"M_ID\":\"E007\"}");
  sim::run_for_ms(30000);
  ASSERT_EQ((int)gwTxKind("STATE_REQ").size(), 3);
  std::vector<sim::MqttPub> r = pubs("module_states_response");
  ASSERT_EQ((int)r.size(), 1);
  if (!r.empty()) ASSERT_STREQ(r[0].payload, "{\"id\":\"E007\",\"status\":\"timeout\"}");
}

STEST(command_and_state_request_in_flight_together_are_sent_one_after_another) {
  setup();
  bootGw();
  mqttIn(100, "commands", CMD_ON_E007);
  mqttIn(101, "module_states_requests", "{\"M_ID\":\"E007\"}");
  inject(700, ackFrame("E007", "V01", 0xA1, 0));
  inject(900, stateFrame("E007", false, {{"V01", 1}}));
  sim::run_for_ms(4000);
  ASSERT_EQ((int)gwTxKind("CMD").size(), 1);
  ASSERT_EQ((int)gwTxKind("STATE_REQ").size(), 1);
  uint64_t prevEnd = 0;
  for (auto &f : sim::airLog) { ASSERT_TRUE(f.tStart >= prevEnd); prevEnd = f.tEnd; }
  ASSERT_EQ((int)pubs("commands_status").size(), 1);
  ASSERT_EQ((int)pubs("module_states_response").size(), 1);
}

// ============================================================================ достъп до канала
STEST(busy_channel_delays_the_command_and_a_permanently_busy_one_ends_in_timeout) {
  setup();
  bootGw();
  uint64_t t0 = sim::now_us();
  sim::radio.busyUntil = t0 + 60000ULL;                           // по-кратко от прозореца за сондиране (~100 ms при SF7)
  mqttIn(10, "commands", CMD_ON_E007);
  inject(900, ackFrame("E007", "V01", 0xA1, 0));                  // преди изтичането на ACK таймаута (~1.1 s от заявката)
  sim::run_for_ms(3000);
  std::vector<TxFrame> c = gwTxKind("CMD");
  ASSERT_EQ((int)c.size(), 1);
  if (!c.empty()) ASSERT_GE((long)((c[0].t - t0) / 1000), 60L);    // след края на заетостта
  ASSERT_GE(sim::radio.cadCount, 2);

  sim::reset();
  setup();
  bootGw();
  sim::radio.busyUntil = sim::now_us() + 600000000ULL;
  mqttIn(100, "commands", CMD_ON_E007);
  sim::run_for_ms(40000);
  ASSERT_EQ((int)gwTxKind("CMD").size(), 0);                      // каналът не се освободи нито веднъж
  std::vector<sim::MqttPub> st = pubs("commands_status");
  ASSERT_EQ((int)st.size(), 1);
  if (!st.empty()) ASSERT_EQ((int)jnum(st[0].payload, "status"), 1);
  ASSERT_TRUE(sim::log_contains("канала остана зает"));
}

STEST(reception_continues_between_channel_checks_while_a_command_waits) {
  setup(9, 125000);
  bootGw();
  sim::radio.busyUntil = sim::now_us() + 500000ULL;
  mqttIn(100, "commands", CMD_ON_E007);
  // идва, докато командата още сондира канала (CAD заема радиото: кадърът идва в първия момент, в който то слуша)
  ASSERT_TRUE(deliverWhenListening(sensorFrame("S001", 1000, 2000, 3000, 4000), FREQ, 300));
  sim::run_for_ms(4000);
  ASSERT_EQ((int)pubs("sensors").size(), 1);
  ASSERT_EQ(sim::radio.missedRx, 0);
}

STEST(while_a_command_senses_a_busy_channel_the_radio_still_listens_a_large_part_of_the_time) {
  for (uint8_t sf : {7, 9, 12}) {
    sim::reset();
    setup(sf, 125000);
    bootGw();
    sim::radio.busyUntil = sim::now_us() + 3000000ULL;
    mqttIn(100, "commands", CMD_ON_E007);
    sim::run_for_ms(300);
    int pct = listeningPercent(1200);
    printf("    SF%u: радиото слуша %d%% от времето при сондиране\n", (unsigned)sf, pct);
    ASSERT_GE(pct, 25);
  }
}

// ============================================================================ nonce
STEST(command_counters_are_unique_across_commands_state_requests_and_acks_and_survive_a_reset) {
  setup();
  bootGw();
  mqttIn(100, "commands", CMD_ON_E007);
  inject(900, ackFrame("E007", "V01", 0xA1, 0));
  mqttIn(1500, "module_states_requests", "{\"M_ID\":\"E007\"}");
  inject(2300, stateFrame("E007", false, {{"V01", 1}}));
  inject(3000, stateFrame("E007", true, {{"V01", 1}}));
  sim::run_for_ms(6000);
  auto ctrOf = [](const sim::RadioFrame &f) {
    size_t n = f.data.size();
    return ((uint32_t)f.data[n - 8] << 24) | ((uint32_t)f.data[n - 7] << 16) | ((uint32_t)f.data[n - 6] << 8) | f.data[n - 5];
  };
  ASSERT_EQ((int)sim::airLog.size(), 3);
  uint32_t last = 0, maxSeen = 0;
  for (size_t i = 0; i < sim::airLog.size(); i++) {
    uint32_t c = ctrOf(sim::airLog[i]);
    if (i > 0) ASSERT_TRUE(c > last);
    last = maxSeen = c;
  }
  ASSERT_EQ(sim::nvs.writes("cmdceil"), 1);
  sim::reboot();
  sim::set_reset_reason(ESP_RST_POWERON);
  size_t base = sim::airLog.size();
  setup();
  bootGw();
  mqttIn(100, "commands", CMD_ON_E007);
  sim::run_for_ms(1500);
  ASSERT_GE((int)sim::airLog.size(), (int)base + 1);
  ASSERT_TRUE(ctrOf(sim::airLog[base]) > maxSeen);
}

// ============================================================================ мрежа и възстановяване
STEST(mqtt_commands_arriving_during_an_outage_are_never_delivered_after_reconnect) {
  setup();
  bootGw();
  sim::broker.setOnline(false);
  sim::run_for_ms(2000);
  ASSERT_FALSE(sim::broker.deliver("commands", CMD_ON_E007));    // няма връзка: брокерът не може да я достави
  sim::broker.setOnline(true);
  sim::run_for_ms(10000);
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_EQ((int)gwTxKind("CMD").size(), 0);                      // clean session: старата команда не се изпълнява със закъснение
  ASSERT_TRUE(sim::broker.subscribedTo("commands", 1));          // абонаментите са подновени
  mqttIn(100, "commands", CMD_ON_E007);
  inject(700, ackFrame("E007", "V01", 0xA1, 0));
  sim::run_for_ms(1500);
  ASSERT_EQ((int)gwTxKind("CMD").size(), 1);
}

STEST(radio_keeps_receiving_while_wifi_and_mqtt_are_down) {
  setup();
  bootGw();
  sim::wifi.apUp = false;
  sim::wifi.dropLink();
  inject(1000, sensorFrame("S001", 1, 2, 3, 4));
  inject(2000, hbFrame("R001", true));
  sim::run_for_ms(20000);
  ASSERT_EQ(sim::radio.missedRx, 0);                              // радиото слуша независимо от мрежата
  ASSERT_EQ(sim::radio.rxDelivered, 2);
  ASSERT_EQ((int)pubs("sensors").size(), 0);                      // но не публикува без връзка
  ASSERT_EQ(sim::broker.outboxQueued, 0);
  sim::wifi.apUp = true;
  sim::run_for_ms(20000);
  ASSERT_TRUE(sim::broker.connected);
  ASSERT_EQ((int)pubs("sensors").size(), 0);                      // стари показания не се доставят "на куп"
  inject(100, sensorFrame("S001", 5, 6, 7, 8));
  sim::run_for_ms(1000);
  ASSERT_EQ((int)pubs("sensors").size(), 1);
}

// ============================================================================ watchdog
STEST(dead_radio_chip_leads_to_a_watchdog_restart_but_a_late_chip_does_not) {
  setup();
  sim::radio.present = false;
  sim::boot(app_main);
  sim::run_for_ms(60000);
  ASSERT_TRUE(sim::restarted());
  ASSERT_TRUE(sim::log_contains("gw_radio"));

  sim::reset();
  setup();
  sim::radio.present = false;
  sim::boot(app_main);
  sim::run_for_ms(12000);
  sim::radio.present = true;                                      // чипът се "събужда" преди изтичането на 30 s
  sim::run_for_ms(120000);
  ASSERT_FALSE(sim::restarted());
  ASSERT_TRUE(sim::radio.listening());
}

STEST(idle_gateway_does_not_trip_its_own_watchdog) {
  setup();
  bootGw();
  sim::run_for_ms(3600000);
  ASSERT_FALSE(sim::restarted());
  sim::TwdtInfo w = sim::twdt_info();
  ASSERT_EQ(w.expiries, 0);
  ASSERT_GE(w.resets, 3600 / 5 * 2 - 10);                         // и двете задачи се обаждат поне на 5 s, цял час
}

STEST(task_watchdog_is_configured_for_30_seconds_with_restart_and_both_tasks_are_subscribed) {
  setup();
  sim::boot(app_main);
  sim::run_for_ms(200);
  sim::TwdtInfo w = sim::twdt_info();
  ASSERT_EQ((int)w.timeoutMs, 30000);                              // по подразбиране в IDF е 5 s
  ASSERT_TRUE(w.panic);                                            // по подразбиране в IDF НЕ рестартира, само пише
  ASSERT_EQ(w.subscribed, 2);                                      // gw_radio и gw_cmd
}

STEST(a_task_that_stops_feeding_the_watchdog_restarts_the_gateway_after_30_seconds_with_the_right_reason) {
  setup();
  sim::radio.present = false;                                      // радио чипът не отговаря: gw_radio остава в инициализацията и не храни watchdog-а
  sim::boot(app_main);
  sim::run_for_ms(25000);
  ASSERT_FALSE(sim::restarted());                                  // още не е изтекъл срокът
  sim::run_for_ms(10000);
  ASSERT_TRUE(sim::restarted());
  sim::TwdtInfo w = sim::twdt_info();
  ASSERT_EQ(w.expiries, 1);
  ASSERT_STREQ(w.lastExpired, "gw_radio");                         // виновната е посочена; gw_cmd (която се обажда) - не
  ASSERT_TRUE(sim::log_contains("Task watchdog got triggered"));
  sim::reboot(false);
  ASSERT_EQ((int)esp_reset_reason(), (int)ESP_RST_TASK_WDT);       // след рестарта системата знае, че е от watchdog
  ASSERT_EQ(sim::twdt_info().subscribed, 0);                       // новото зареждане започва чисто
}

// ============================================================================ натоварване и паралелизъм
STEST(burst_of_sensor_frames_is_all_published_or_counted_never_crashes) {
  setup();
  bootGw();
  for (int i = 0; i < 40; i++) inject(10 + 5 * i, sensorFrame("S0BB", (int16_t)i, 0, 0, 0));
  sim::run_for_ms(5000);
  ASSERT_FALSE(sim::restarted());
  ASSERT_EQ((int)pubs("sensors").size(), 40);
  ASSERT_EQ(sim::radio.rxOverrun, 0);
}

STEST(chaos_interleavings_give_the_same_observable_behaviour) {
  for (uint32_t seed = 1; seed <= 20; seed++) {
    sim::reset();
    g_ctr = 100;
    setup();
    sim::chaos(seed, 250);
    bootGw();
    inject(10, sensorFrame("S001", 100, 200, 300, 400));
    mqttIn(200, "commands", CMD_ON_E007);
    inject(1000, ackFrame("E007", "V01", 0xA1, 0));
    inject(1400, stateFrame("E007", true, {{"V01", 1}}));
    inject(2000, sensorFrame("S002", 1, 2, 3, 4));
    sim::run_for_ms(5000);
    ASSERT_EQ((int)pubs("sensors").size(), 2);
    ASSERT_EQ((int)pubs("commands_status").size(), 1);
    ASSERT_EQ((int)pubs("module_states_response").size(), 1);
    ASSERT_EQ((int)gwTxKind("CMD").size(), 1);
    ASSERT_EQ((int)gwTxKind("STATE_ACK").size(), 1);
    ASSERT_EQ(sim::radio.protocolErrors, 0);
    ASSERT_EQ(sim::radio.modeViolations, 0);
    ASSERT_FALSE(sim::restarted());
  }
}

STEST(tx_power_comes_from_nvs_with_a_default_of_17_dbm_and_is_limited_to_the_module_range) {
  struct { bool set; int dbm; int expectDbm; } cases[] = {
      {false, 0, 17}, {true, 10, 10}, {true, -9, -9}, {true, 22, 22}, {true, 30, 22}, {true, -20, -9}, {true, 0, 0}, {true, 14, 14}};
  for (auto &c : cases) {
    int want = refPowerSetting(c.expectDbm);
    setup();
    if (c.set) nvsTxPower(c.dbm);
    bootGw();
    mqttIn(100, "commands", CMD_ON_E007);
    sim::run_for_ms(1500);
    ASSERT_GE((int)sim::airLog.size(), 1);
    if (!sim::airLog.empty()) ASSERT_EQ(sim::airLog[0].txPowerDbm, want);
  }
}

int main() { return runAllTests(); }
