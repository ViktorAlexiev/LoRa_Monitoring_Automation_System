// Тестове на LoRa executor-а (РЕАЛНИТЕ app_main, exec_radio, exec_cmd, exec_consumers) срещу симулатора.
#include "test_support.h"
#include <algorithm>

#include "board_pins.h"
#include "crypto_common.h"
#include "esp_system.h"
#include "protocol.h"
#include "radio_timing.h"
#include "agro_radio.h"
#include "test_radio_support.h"

extern "C" void app_main(void);

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);

static const uint32_t FREQ = 433000000;
static const int PIN_V01 = 4, PIN_P01 = 5;

// ---------------------------------------------------------------- помощници
static void setup(const char *id = "E007", const char *consumers = "V01:4,P01:5", uint8_t sf = 7, uint32_t bw = 125000) {
  nvsRadioConfig(id, FREQ, sf, bw);
  int n = 0;
  std::string list = consumers;
  size_t pos = 0;
  while (pos < list.size()) {
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
}

static void bootExec() { sim::boot(app_main); }

static uint32_t g_dlCtr = 1000;
static std::vector<uint8_t> downlink(const char *target, uint8_t type, const std::vector<uint8_t> &pt = {},
                                     const uint8_t *key = TEST_KEY) {
  uint8_t frame[6 + CRYPTO_OVERHEAD + CRYPTO_MAX_PT];
  memset(frame, 0, sizeof(frame));
  memcpy(frame, target, strlen(target));
  const uint8_t gw[6] = {'G', '0', '0', '1', 0, 0};
  uint8_t n = cryptoBuildWirePacket(frame + 6, key, type, gw, g_dlCtr++, pt.data(), (uint8_t)pt.size());
  return std::vector<uint8_t>(frame, frame + 6 + n);
}
static std::vector<uint8_t> cmdFrame(const char *target, const char *cid, uint8_t com) {
  std::vector<uint8_t> pt(6, 0);
  memcpy(pt.data(), cid, strlen(cid));
  pt[4] = com;
  pt[5] = 0xFF;
  return downlink(target, CRYPTO_TYPE_GW_CMD, pt);
}

static void injectAfter(uint32_t ms, const std::vector<uint8_t> &f) {
  sim::at_after_ms(ms, [f] { sim::radio.deliver(f, FREQ, -70, 6.0f); }, true);
}

struct Tx {
  uint64_t tStart;
  std::string kind;
  std::string sender;
  std::vector<uint8_t> pt;
  long freq;
  size_t frames;
};
static std::vector<Tx> txs(size_t from = 0) {
  std::vector<Tx> r;
  struct { const char *name; uint8_t type; } types[4] = {{"ACK", CRYPTO_TYPE_EXEC_ACK}, {"HB", CRYPTO_TYPE_EXEC_HB},
                                                         {"STATE", CRYPTO_TYPE_STATE_RESP}, {"RESTART", CRYPTO_TYPE_STATE_RESP_RESTART}};
  for (size_t i = from; i < sim::airLog.size(); i++) {
    const sim::RadioFrame &f = sim::airLog[i];
    Tx t;
    t.tStart = f.tStart;
    t.kind = "?";
    t.freq = f.freqHz;
    t.frames = i + 1;
    for (auto &ty : types) {
      uint8_t sender[6], pt[CRYPTO_MAX_PT], len = 0;
      if (cryptoParseWirePacket(f.data.data(), (uint8_t)f.data.size(), TEST_KEY, ty.type, sender, pt, &len)) {
        t.kind = ty.name;
        for (int k = 0; k < 6 && sender[k]; k++) t.sender += (char)sender[k];
        t.pt.assign(pt, pt + len);
        break;
      }
    }
    r.push_back(t);
  }
  return r;
}
static std::vector<Tx> txKind(const char *kind) {
  std::vector<Tx> r;
  for (auto &t : txs()) if (t.kind == kind) r.push_back(t);
  return r;
}
static long msSince(uint64_t t0, uint64_t t) { return (long)((t - t0) / 1000); }

static bool pinHigh(int pin) { return sim::gpio.read(pin) == 1; }

// ============================================================================ старт
STEST(boot_makes_all_consumers_off_creates_tasks_and_enables_automatic_light_sleep) {
  setup();
  bootExec();
  sim::run_for_ms(500);
  for (int pin : {PIN_V01, PIN_P01}) {
    ASSERT_EQ(sim::gpio.pins[pin].mode, 3);                      // INPUT_OUTPUT: състоянието се чете от пина
    ASSERT_FALSE(pinHigh(pin));
    ASSERT_EQ(sim::gpio.pins[pin].history.front(), 0);           // никога не е бил HIGH (без късо включване при boot)
  }
  std::vector<sim::TaskInfo> t = sim::tasks();
  const sim::TaskInfo *radio = sim::find_task(t, "exec_radio");
  const sim::TaskInfo *cmd = sim::find_task(t, "exec_cmd");
  ASSERT_TRUE(radio && cmd);
  if (radio && cmd) {
    ASSERT_EQ(radio->core, 1);                                   // ядро 1 (APP); ядро 0 остава свободно - няма Wi-Fi
    ASSERT_EQ(cmd->core, 1);
    ASSERT_TRUE(radio->prio > cmd->prio);                        // радиото е с по-висок приоритет от изпълнението
    ASSERT_GE((long)radio->stack, 4096L);
  }
  ASSERT_FALSE(sim::find_task(t, "mqtt_task") != nullptr);       // няма мрежова част
  sim::PmStats pm = sim::pm_stats();
  ASSERT_TRUE(pm.configured);                                     // esp_pm_configure(light_sleep_enable)
  ASSERT_TRUE(pm.gpio_wake_enabled);                              // събуждане от DIO0
  ASSERT_EQ(sim::gpio.pins[LORA_DIO1].wakeEnabled, true);
  ASSERT_EQ(sim::gpio.pins[LORA_DIO1].wakeType, 5);               // HIGH ниво
  ASSERT_EQ(sim::gpio.pins[LORA_DIO1].intrEnabled, true);
  ASSERT_EQ(pm.manual_light_sleep_calls, 0);                      // никога esp_light_sleep_start() ръчно
  ASSERT_TRUE(sim::radio.listening());
  ASSERT_LE(std::labs(sim::radio.freqHz() - (long)FREQ), 61L);
}

STEST(boot_sends_restart_state_response_with_all_consumers_off) {
  setup();
  bootExec();
  sim::run_for_ms(1000);
  std::vector<Tx> r = txKind("RESTART");
  ASSERT_GE((int)r.size(), 1);
  if (!r.empty()) {
    ASSERT_STREQ(r[0].sender, "E007");
    ASSERT_EQ((int)r[0].pt.size(), 10);                           // 2 консуматора x 5 B
    ASSERT_STREQ(std::string((const char *)r[0].pt.data(), 3), "V01");
    ASSERT_EQ(r[0].pt[4], 0);                                     // OFF
    ASSERT_STREQ(std::string((const char *)r[0].pt.data() + 5, 3), "P01");
    ASSERT_EQ(r[0].pt[9], 0);
    ASSERT_LE(std::labs(r[0].freq - (long)FREQ), 61L);
    ASSERT_EQ((int)sim::airLog[0].data.size(), 24);               // 14 + 10
  }
}

STEST(restart_response_is_retried_up_to_three_times_without_ack_then_gives_up) {
  setup();
  bootExec();
  sim::run_for_ms(60000);
  std::vector<Tx> r = txKind("RESTART");
  ASSERT_EQ((int)r.size(), 3);                                    // 1 + 2 retry
  if (r.size() == 3) {
    long gap = msSince(r[0].tStart, r[1].tStart);
    ASSERT_GE(gap, (long)radioAckTimeoutRestartMs() - 5);         // интервалът е от SF/BW (ACK timeout), не константа (началото на TX е след проверка на канала: +-няколко ms)
    ASSERT_LE(gap, (long)radioAckTimeoutRestartMs() + 100);
    ASSERT_EQ(msSince(r[1].tStart, r[2].tStart) / 10, gap / 10);
  }
  ASSERT_TRUE(sim::log_contains("няма ACK след 3 опита"));
}

STEST(ack_from_gateway_stops_the_restart_retries) {
  setup();
  bootExec();
  injectAfter(300, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(60000);
  ASSERT_EQ((int)txKind("RESTART").size(), 1);
  ASSERT_TRUE(sim::log_contains("потвърдено от Gateway"));
}

STEST(ack_for_another_module_or_with_bad_mac_does_not_stop_retries) {
  setup();
  bootExec();
  injectAfter(300, downlink("E999", CRYPTO_TYPE_STATE_RESP_ACK));
  uint8_t other[16];
  memcpy(other, TEST_KEY, 16);
  other[0] ^= 0xFF;
  injectAfter(400, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK, {}, other));
  sim::run_for_ms(60000);
  ASSERT_EQ((int)txKind("RESTART").size(), 3);
}

// ============================================================================ команди
STEST(on_command_switches_the_pin_immediately_and_acks_after_the_half_duplex_pause) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));      // restart отговорът е потвърден: без повторения
  sim::run_for_ms(1500);
  size_t before = sim::airLog.size();
  uint64_t t0 = sim::now_us();
  injectAfter(10, cmdFrame("E007", "V01", CMD_ON));
  sim::run_for_ms(300);
  ASSERT_TRUE(pinHigh(PIN_V01));                                  // изпълнено веднага (преди ACK)
  ASSERT_FALSE(pinHigh(PIN_P01));
  ASSERT_EQ((int)sim::airLog.size(), (int)before);                // още няма ACK (пауза 500 ms)
  sim::run_for_ms(1500);
  std::vector<Tx> acks = txKind("ACK");
  ASSERT_EQ((int)acks.size(), 1);
  if (!acks.empty()) {
    long dt = msSince(t0 + 10000, acks[0].tStart);
    ASSERT_GE(dt, (long)RADIO_EXEC_ACK_DELAY_MS);                 // не по-рано от 500 ms
    ASSERT_LE(dt, (long)RADIO_EXEC_ACK_DELAY_MS + 40);            // и без излишно забавяне
    ASSERT_STREQ(acks[0].sender, "E007");
    ASSERT_EQ((int)acks[0].pt.size(), 6);
    ASSERT_STREQ(std::string((const char *)acks[0].pt.data(), 3), "V01");
    ASSERT_EQ(acks[0].pt[4], CMD_ON);                             // com се ехоира: gateway-ят сравнява com
    ASSERT_EQ(acks[0].pt[5], STATUS_ACK);
  }
}

STEST(off_command_switches_the_pin_back) {
  setup();
  bootExec();
  injectAfter(1000, cmdFrame("E007", "P01", CMD_ON));
  injectAfter(3000, cmdFrame("E007", "P01", CMD_OFF));
  sim::run_for_ms(2000);
  ASSERT_TRUE(pinHigh(PIN_P01));
  sim::run_for_ms(3000);
  ASSERT_FALSE(pinHigh(PIN_P01));
  std::vector<Tx> acks = txKind("ACK");
  ASSERT_EQ((int)acks.size(), 2);
  if (acks.size() == 2) { ASSERT_EQ(acks[1].pt[4], CMD_OFF); ASSERT_EQ(acks[1].pt[5], STATUS_ACK); }
}

STEST(invalid_command_code_and_unknown_consumer_are_nacked_and_change_nothing) {
  setup();
  bootExec();
  injectAfter(1000, cmdFrame("E007", "V01", 0x55));               // непозната команда
  injectAfter(3000, cmdFrame("E007", "XXX", CMD_ON));             // няма такъв консуматор
  sim::run_for_ms(6000);
  ASSERT_FALSE(pinHigh(PIN_V01));
  ASSERT_FALSE(pinHigh(PIN_P01));
  std::vector<Tx> acks = txKind("ACK");
  ASSERT_EQ((int)acks.size(), 2);
  for (auto &a : acks) ASSERT_EQ(a.pt[5], STATUS_NACK);
}

STEST(commands_for_other_modules_bad_mac_or_wrong_length_are_silently_ignored) {
  setup();
  bootExec();
  injectAfter(1000, cmdFrame("E008", "V01", CMD_ON));             // чужд модул
  uint8_t other[16];
  memcpy(other, TEST_KEY, 16);
  other[5] ^= 0x10;
  {
    std::vector<uint8_t> pt = {'V', '0', '1', 0, CMD_ON, 0xFF};
    uint8_t frame[6 + CRYPTO_OVERHEAD + 6];
    memset(frame, 0, sizeof(frame));
    memcpy(frame, "E007", 4);
    const uint8_t gw[6] = {'G', '0', '0', '1', 0, 0};
    cryptoBuildWirePacket(frame + 6, other, CRYPTO_TYPE_GW_CMD, gw, 5000, pt.data(), 6);
    injectAfter(2000, std::vector<uint8_t>(frame, frame + sizeof(frame)));          // чужд ключ
  }
  std::vector<uint8_t> tampered = cmdFrame("E007", "V01", CMD_ON);
  tampered[tampered.size() - 1] ^= 1;
  injectAfter(3000, tampered);                                                       // повреден tag
  injectAfter(4000, std::vector<uint8_t>(27, 0x42));                                 // непозната дължина
  injectAfter(5000, std::vector<uint8_t>(3, 0x42));
  sim::run_for_ms(8000);
  ASSERT_FALSE(pinHigh(PIN_V01));
  ASSERT_EQ((int)txKind("ACK").size(), 0);
  ASSERT_TRUE(sim::log_contains("не е за мен"));
  ASSERT_TRUE(sim::log_contains("невалиден MAC"));
  ASSERT_TRUE(sim::log_contains("непознат размер"));
}

STEST(burst_of_commands_all_execute_in_order_and_acks_are_sent_one_at_a_time) {
  setup("E007", "V01:4,P01:5,V02:6,V03:7");
  bootExec();
  sim::run_for_ms(1000);
  const char *ids[4] = {"V01", "P01", "V02", "V03"};
  for (int i = 0; i < 4; i++) injectAfter(10 + 3 * i, cmdFrame("E007", ids[i], CMD_ON));
  sim::run_for_ms(8000);
  for (int pin : {4, 5, 6, 7}) ASSERT_TRUE(pinHigh(pin));
  std::vector<Tx> acks = txKind("ACK");
  ASSERT_EQ((int)acks.size(), 4);
  // реда на потвържденията е реда на командите; всеки ACK е със собствен nonce
  for (int i = 0; i < (int)acks.size() && i < 4; i++) ASSERT_STREQ(std::string((const char *)acks[i].pt.data(), 3), ids[i]);
  uint64_t prevEnd = 0;
  for (size_t i = 0; i < sim::airLog.size(); i++) {
    ASSERT_TRUE(sim::airLog[i].tStart >= prevEnd);                // предаванията не се застъпват
    prevEnd = sim::airLog[i].tEnd;
  }
}

STEST(command_flood_never_crashes_and_leaves_consistent_pin_state) {
  setup();
  bootExec();
  sim::run_for_ms(1000);
  for (int i = 0; i < 60; i++) injectAfter(10 + 2 * i, cmdFrame("E007", "V01", (i % 2) ? CMD_OFF : CMD_ON));
  sim::run_for_ms(120000);
  ASSERT_FALSE(sim::restarted());
  // последната команда (i=59 -> OFF) е изпълнена след всички предходни
  ASSERT_FALSE(pinHigh(PIN_V01));
  ASSERT_GE((int)txKind("ACK").size(), 8);
}

// ============================================================================ state request
STEST(state_request_returns_the_real_pin_states) {
  setup();
  bootExec();
  injectAfter(1000, cmdFrame("E007", "P01", CMD_ON));
  sim::run_for_ms(4000);
  size_t before = sim::airLog.size();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_REQ));
  sim::run_for_ms(1500);
  std::vector<Tx> all = txs(before);
  ASSERT_EQ((int)all.size(), 1);
  if (!all.empty()) {
    ASSERT_STREQ(all[0].kind, "STATE");
    ASSERT_EQ((int)all[0].pt.size(), 10);
    ASSERT_EQ(all[0].pt[4], 0);                                   // V01 изключен
    ASSERT_EQ(all[0].pt[9], 1);                                   // P01 включен (прочетено от реалния пин)
  }
  injectAfter(100, downlink("E123", CRYPTO_TYPE_STATE_REQ));      // за друг модул
  size_t b2 = sim::airLog.size();
  sim::run_for_ms(1500);
  ASSERT_EQ((int)sim::airLog.size(), (int)b2);
}

STEST(external_change_of_a_pin_is_visible_in_the_next_state_response) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(1000);
  sim::gpio.pins[PIN_V01].level = 1;                              // нещо друго е вдигнало пина (напр. ръчно/къс)
  size_t before = sim::airLog.size();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_REQ));
  sim::run_for_ms(1500);
  std::vector<Tx> all = txs(before);
  ASSERT_EQ((int)all.size(), 1);
  if (!all.empty()) ASSERT_EQ(all[0].pt[4], 1);
}

// ============================================================================ heartbeat
// Първият heartbeat е на СЛУЧАЙНА фаза в (1 s, 304 s] (устройства, пуснати едновременно, не бива да остават синхронизирани);
// симулаторът е детерминиран, затова времето му се намира с пробно изпълнение и се ползва във второ.
static long firstHbMs() {
  sim::reset();
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(400000);
  std::vector<Tx> hb = txKind("HB");
  long t = hb.empty() ? -1 : msSince(1000000ULL, hb[0].tStart);
  sim::reset();
  return t;
}

STEST(heartbeat_first_at_a_random_phase_then_every_five_minutes_with_unique_nonce) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));   // без повторения на boot отговора
  sim::run_for_ms(1000000);                                          // ~16.6 минути
  std::vector<Tx> hb = txKind("HB");
  ASSERT_TRUE(hb.size() == 3 || hb.size() == 4);
  if (hb.size() >= 3) {
    long first = msSince(1000000ULL, hb[0].tStart);
    ASSERT_GE(first, (long)HB_FIRST_MIN_MS);
    ASSERT_LE(first, (long)HB_INTERVAL_MS + 1100);                    // най-късно 38 x 8 s + jitter
    for (size_t i = 1; i < hb.size(); i++) {
      long gap = msSince(hb[i - 1].tStart, hb[i].tStart);
      ASSERT_GE(gap, (long)HB_INTERVAL_MS - 1100);
      ASSERT_LE(gap, (long)HB_INTERVAL_MS + 1100);
    }
    ASSERT_EQ((int)sim::airLog[1].data.size(), 14);                   // HB: 14 B (без данни)
    ASSERT_STREQ(hb[0].sender, "E007");
    ASSERT_MEM_NE(sim::airLog[1].data.data(), sim::airLog[2].data.data(), 14);
  }
}

STEST(different_boots_give_different_heartbeat_phases) {
  std::vector<long> firsts;
  for (uint32_t seed = 1; seed <= 6; seed++) {
    sim::reset();
    sim::seed_random(seed);
    setup();
    bootExec();
    injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
    sim::run_for_ms(400000);
    std::vector<Tx> hb = txKind("HB");
    ASSERT_GE((int)hb.size(), 1);
    if (!hb.empty()) firsts.push_back(msSince(1000000ULL, hb[0].tStart));
  }
  std::sort(firsts.begin(), firsts.end());
  ASSERT_TRUE(firsts.back() - firsts.front() > 30000);               // разпределени, не на един и същ момент
}

STEST(heartbeat_is_skipped_on_a_busy_channel_and_comes_back_next_period) {
  long t1 = firstHbMs();
  ASSERT_GE(t1, 1000L);
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms((uint32_t)(t1 - 2000));
  sim::radio.busyUntil = sim::now_us() + 15000000ULL;                // зает точно в момента на HB
  sim::run_for_ms(20000);
  ASSERT_EQ((int)txKind("HB").size(), 0);                            // пропуснат (телеметрия/SKIP: два CAD-а, без предаване)
  ASSERT_TRUE(sim::log_contains("канала остана зает"));
  sim::run_for_ms(320000);
  ASSERT_EQ((int)txKind("HB").size(), 1);                            // следващият по график
}

// ============================================================================ достъп до канала
STEST(ack_waits_for_a_busy_channel_then_goes_out) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(1000);
  injectAfter(10, cmdFrame("E007", "V01", CMD_ON));
  sim::run_for_ms(300);
  sim::radio.busyUntil = sim::now_us() + 400000ULL;                  // чужд предавател до ~ACK + 300 ms
  sim::run_for_ms(5000);
  std::vector<Tx> acks = txKind("ACK");
  ASSERT_EQ((int)acks.size(), 1);
  if (!acks.empty()) ASSERT_GE(msSince(1000000ULL + 1000000ULL, acks[0].tStart), 600L);   // след края на заетостта
  ASSERT_TRUE(sim::radio.cadCount >= 2);
}

STEST(ack_gives_up_when_channel_stays_busy_longer_than_the_gateway_waits) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(1000);
  sim::radio.busyUntil = sim::now_us() + 60000000ULL;
  injectAfter(10, cmdFrame("E007", "V01", CMD_ON));
  sim::run_for_ms(15000);
  ASSERT_TRUE(pinHigh(PIN_V01));                                     // командата е изпълнена независимо от канала
  ASSERT_EQ((int)txKind("ACK").size(), 0);                           // ACK-ът не стига до ефира (gateway-ят ще направи retry/TIMEOUT)
  ASSERT_TRUE(sim::log_contains("отказвам се"));
  ASSERT_FALSE(sim::restarted());
  // каналът се освобождава: следващата команда се потвърждава нормално
  sim::radio.busyUntil = 0;
  injectAfter(10, cmdFrame("E007", "V01", CMD_OFF));
  sim::run_for_ms(3000);
  ASSERT_EQ((int)txKind("ACK").size(), 1);
}

STEST(works_on_every_sf_and_bw_with_timings_taken_from_the_settings) {
  for (int sf : {7, 9, 12}) {
    for (uint32_t bw : {62500u, 125000u, 250000u}) {
      sim::reset();
      setup("E007", "V01:4,P01:5", (uint8_t)sf, bw);
      bootExec();
      sim::run_for_ms(6000);                                             // първото предаване (до ~3 s при SF12/62.5k) е свършило
      sim::radio.deliver(downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK), FREQ);
      sim::run_for_ms(500);
      uint32_t ackDelay = 600;
      uint64_t t0 = sim::now_us();
      sim::at_after_ms(10, [] { sim::radio.deliver(cmdFrame("E007", "V01", CMD_ON), FREQ); }, true);
      sim::run_for_ms(ackDelay + 6000);
      ASSERT_TRUE(pinHigh(PIN_V01));
      std::vector<Tx> acks = txKind("ACK");
      ASSERT_EQ((int)acks.size(), 1);
      ASSERT_EQ(sim::airLog.back().sf, sf);
      ASSERT_EQ((int)sim::airLog.back().bwHz, (int)bw);
      (void)t0;
    }
  }
}

// ============================================================================ енергия
STEST(idle_executor_sleeps_nearly_all_the_time_and_does_not_poll) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(5000);
  sim::pm_reset_counters();
  sim::run_for_ms(3600000);                                          // 1 час без трафик
  sim::PmStats pm = sim::pm_stats();
  double idleFrac = (double)pm.idle_us / (double)pm.total_us;
  ASSERT_TRUE(idleFrac > 0.999);                                     // чипът е в сън >99.9 % от времето
  // реалните събуждания са ~2 на heartbeat (срок за jitter и край на таймера); превключванията в задачата
  // по време на самото предаване (опрос на TxDone през 1 ms) не са сън и не се броят тук
  ASSERT_LE((int)pm.sleeps, 60);                                      // не опресняване на всеки ms
  ASSERT_GE((int)pm.sleeps, 10);
  ASSERT_EQ(pm.manual_light_sleep_calls, 0);
  ASSERT_EQ((int)pm.gpio_wakes, 0);
}

STEST(a_packet_wakes_the_chip_from_light_sleep_through_dio0) {
  setup();
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(5000);
  sim::pm_reset_counters();
  sim::at_after_ms(20000, [] { sim::radio.deliver(cmdFrame("E007", "V01", CMD_ON), FREQ); }, true);
  sim::run_for_ms(30000);
  sim::PmStats pm = sim::pm_stats();
  ASSERT_GE((int)pm.gpio_wakes, 1);                                  // събуждане по пина, не по таймер
  ASSERT_TRUE(pinHigh(PIN_V01));
  ASSERT_EQ((int)txKind("ACK").size(), 1);
}

// ============================================================================ nonce
STEST(every_transmission_uses_a_fresh_counter_and_a_reboot_never_goes_backwards) {
  setup();
  bootExec();
  injectAfter(1000, cmdFrame("E007", "V01", CMD_ON));
  injectAfter(2500, cmdFrame("E007", "V01", CMD_OFF));
  injectAfter(4000, downlink("E007", CRYPTO_TYPE_STATE_REQ));
  sim::run_for_ms(8000);
  auto ctrOf = [](const sim::RadioFrame &f) {
    size_t n = f.data.size();
    return ((uint32_t)f.data[n - 8] << 24) | ((uint32_t)f.data[n - 7] << 16) | ((uint32_t)f.data[n - 6] << 8) | f.data[n - 5];
  };
  uint32_t last = 0;
  bool first = true;
  uint32_t maxSeen = 0;
  for (auto &f : sim::airLog) {
    uint32_t c = ctrOf(f);
    if (!first) ASSERT_TRUE(c > last);
    first = false;
    last = c;
    maxSeen = c;
  }
  ASSERT_GE((int)sim::airLog.size(), 5);
  ASSERT_EQ(sim::nvs.writes("ceil"), 1);                              // 100 стойности на запис във flash
  // power-on reset (RAM загубена, NVS не): продължава НАД използваното
  sim::reboot();
  sim::set_reset_reason(ESP_RST_POWERON);
  size_t base = sim::airLog.size();
  setup();
  bootExec();
  sim::run_for_ms(1000);
  ASSERT_GE((int)sim::airLog.size(), (int)base + 1);
  ASSERT_TRUE(ctrOf(sim::airLog[base]) > maxSeen);
}

// ============================================================================ конфигурация и повреди
STEST(unconfigured_executor_keeps_consumers_off_and_starts_no_radio_tasks) {
  bootExec();
  sim::run_for_ms(2000);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_TRUE(sim::find_task(sim::tasks(), "exec_radio") == nullptr);
  ASSERT_TRUE(sim::log_contains("няма id/ключ"));
}

STEST(consumer_on_an_unusable_gpio_is_never_driven_and_commands_to_it_are_nacked) {
  setup("E007", "V01:4,BAD:30");                                       // GPIO30 е SPI flash на ESP32-S3
  bootExec();
  injectAfter(1000, cmdFrame("E007", "BAD", CMD_ON));
  sim::run_for_ms(4000);
  ASSERT_EQ(sim::gpio.pins[30].outputWrites, 0);                        // никога не е пипан
  std::vector<Tx> acks = txKind("ACK");
  ASSERT_EQ((int)acks.size(), 1);
  if (!acks.empty()) ASSERT_EQ(acks[0].pt[5], STATUS_NACK);
  ASSERT_TRUE(sim::log_contains("не е използваем"));
}

STEST(radio_chip_appearing_late_is_picked_up_without_a_reboot) {
  setup();
  sim::radio.present = false;
  bootExec();
  sim::run_for_ms(3000);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_TRUE(sim::log_contains("радиото не стартира"));
  sim::radio.present = true;
  sim::run_for_ms(2000);
  ASSERT_GE((int)txKind("RESTART").size(), 1);
  ASSERT_TRUE(sim::radio.listening());
}

STEST(zero_consumers_gives_a_14_byte_state_response) {
  setup("E007", "");
  sim::nvs.setU8("nc", 0);
  bootExec();
  injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
  sim::run_for_ms(1000);
  ASSERT_EQ((int)sim::airLog.size(), 1);
  ASSERT_EQ((int)sim::airLog[0].data.size(), 14);
  std::vector<Tx> r = txKind("RESTART");
  ASSERT_EQ((int)r.size(), 1);
}

// ============================================================================ паралелизъм
STEST(chaos_interleavings_give_the_same_observable_behaviour) {
  for (uint32_t seed = 1; seed <= 25; seed++) {
    sim::reset();
    setup();
    sim::chaos(seed, 250);
    bootExec();
    injectAfter(100, downlink("E007", CRYPTO_TYPE_STATE_RESP_ACK));
    injectAfter(1000, cmdFrame("E007", "V01", CMD_ON));
    injectAfter(1010, cmdFrame("E007", "P01", CMD_ON));
    injectAfter(3000, downlink("E007", CRYPTO_TYPE_STATE_REQ));
    sim::run_for_ms(8000);
    ASSERT_TRUE(pinHigh(PIN_V01));
    ASSERT_TRUE(pinHigh(PIN_P01));
    ASSERT_EQ((int)txKind("ACK").size(), 2);
    ASSERT_EQ((int)txKind("STATE").size(), 1);
    ASSERT_EQ((int)txKind("RESTART").size(), 1);
    ASSERT_EQ(sim::radio.protocolErrors, 0);
    ASSERT_EQ(sim::radio.modeViolations, 0);
    ASSERT_EQ(sim::radio.rxOverrun, 0);
  }
}

STEST(tx_power_comes_from_nvs_with_a_default_of_17_dbm_and_is_limited_to_the_module_range) {
  struct { bool set; int dbm; int expectDbm; } cases[] = {
      {false, 0, 17}, {true, 10, 10}, {true, -9, -9}, {true, 22, 22}, {true, 30, 22}, {true, -20, -9}, {true, 0, 0}, {true, 14, 14}};
  for (auto &c : cases) {
    int want = refPowerSetting(c.expectDbm);
    setup();
    if (c.set) nvsTxPower(c.dbm);
    bootExec();
    sim::run_for_ms(3000);                                           // restart state response при старт
    ASSERT_GE((int)sim::airLog.size(), 1);
    if (!sim::airLog.empty()) ASSERT_EQ(sim::airLog[0].txPowerDbm, want);
  }
}

int main() { return runAllTests(); }
