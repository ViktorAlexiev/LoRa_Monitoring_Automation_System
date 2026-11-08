// Тестове на repeater-а (РЕАЛНИТЕ app_main, rep_radio, rep_config) срещу симулатора.
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

static const uint32_t RX_HZ = 434000000, TX_HZ = 433000000;

static void setup(uint8_t sf = 7, uint32_t bw = 125000) {
  sim::nvs.setStr("id", "R001");
  sim::nvs.setBlob("key", TEST_KEY, 16);
  sim::nvs.setU32("freq", RX_HZ);
  sim::nvs.setU32("freqtx", TX_HZ);
  sim::nvs.setU8("sf", sf);
  sim::nvs.setU32("bw", bw);
}
static void bootRep() { sim::boot(app_main); }

static uint32_t g_ctr = 1;
static std::vector<uint8_t> sensorFrame(const char *id = "S001", int16_t t = 2250) {
  uint8_t pt[8];
  for (int i = 0; i < 4; i++) { int16_t v = (int16_t)(t + i * 10); pt[2 * i] = v & 0xff; pt[2 * i + 1] = (uint8_t)(v >> 8); }
  uint8_t sender[6] = {0};
  memcpy(sender, id, strlen(id));
  uint8_t w[22];
  uint8_t n = cryptoBuildWirePacket(w, TEST_KEY, CRYPTO_TYPE_SENSOR, sender, g_ctr++, pt, 8);
  return std::vector<uint8_t>(w, w + n);
}

static void injectAfter(uint32_t ms, const std::vector<uint8_t> &f, long freq = RX_HZ) {
  sim::at_after_ms(ms, [f, freq] { sim::radio.deliver(f, freq, -80, 5.0f); }, true);
}

static std::vector<const sim::RadioFrame *> framesWithLen(size_t len) {
  std::vector<const sim::RadioFrame *> r;
  for (auto &f : sim::airLog) if (f.data.size() == len) r.push_back(&f);
  return r;
}

static bool isRepeaterHb(const sim::RadioFrame &f) {
  uint8_t sender[6], pt[CRYPTO_MAX_PT], len = 0;
  return f.data.size() == 14 && cryptoParseWirePacket(f.data.data(), 14, TEST_KEY, CRYPTO_TYPE_REPEATER_HB, sender, pt, &len);
}
static std::vector<const sim::RadioFrame *> hbFrames() {
  std::vector<const sim::RadioFrame *> r;
  for (auto &f : sim::airLog) if (isRepeaterHb(f)) r.push_back(&f);
  return r;
}
static std::vector<const sim::RadioFrame *> forwarded() {
  std::vector<const sim::RadioFrame *> r;
  for (auto &f : sim::airLog) if (!isRepeaterHb(f)) r.push_back(&f);
  return r;
}

// ============================================================================ старт
STEST(boot_creates_two_tasks_listens_on_rx_frequency_and_enables_light_sleep) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<sim::TaskInfo> t = sim::tasks();
  const sim::TaskInfo *rx = sim::find_task(t, "rep_rx");
  const sim::TaskInfo *fwd = sim::find_task(t, "rep_fwd");
  ASSERT_TRUE(rx && fwd);
  if (rx && fwd) {
    ASSERT_EQ(rx->core, 1);
    ASSERT_EQ(fwd->core, 1);
    ASSERT_TRUE(rx->prio > fwd->prio);              // приемането е с по-висок приоритет от препращането
  }
  ASSERT_TRUE(sim::radio.listening());
  ASSERT_LE(std::labs(sim::radio.freqHz() - (long)RX_HZ), 61L);
  sim::PmStats pm = sim::pm_stats();
  ASSERT_TRUE(pm.configured);
  ASSERT_TRUE(pm.gpio_wake_enabled);
  ASSERT_EQ(pm.manual_light_sleep_calls, 0);
  ASSERT_TRUE(sim::gpio.pins[LORA_DIO1].intrEnabled);
  ASSERT_EQ((int)sim::airLog.size(), 0);             // repeater не праща нищо при boot (heartbeat е по график)
}

STEST(unconfigured_repeater_starts_nothing) {
  bootRep();
  sim::run_for_ms(1000);
  ASSERT_TRUE(sim::find_task(sim::tasks(), "rep_rx") == nullptr);
  ASSERT_TRUE(sim::log_contains("няма id/ключ"));
}

// ============================================================================ препращане
STEST(sensor_frame_is_forwarded_unchanged_on_the_tx_frequency_and_listening_resumes) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<uint8_t> f = sensorFrame();
  uint64_t t0 = sim::now_us() + 10000;
  injectAfter(10, f);
  sim::run_for_ms(500);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 1);
  if (!fw.empty()) {
    ASSERT_STREQ(toHex(fw[0]->data), toHex(f));                    // байт по байт същият (без маркер)
    ASSERT_LE(std::labs(fw[0]->freqHz - (long)TX_HZ), 61L);
    ASSERT_LE((long)(fw[0]->tStart - t0), 100000L);                // backoff <= 50 ms + CAD
  }
  ASSERT_TRUE(sim::radio.listening());
  ASSERT_LE(std::labs(sim::radio.freqHz() - (long)RX_HZ), 61L);    // обратно на RX честотата
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
  ASSERT_TRUE(sim::gpio.pins[LORA_DIO1].intrEnabled);              // прекъсването е върнато (rearm)
  ASSERT_EQ(sim::radio.protocolErrors, 0);
  ASSERT_EQ(sim::radio.modeViolations, 0);
}

STEST(duplicate_is_forwarded_once) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<uint8_t> f = sensorFrame();
  injectAfter(10, f);
  injectAfter(500, f);
  injectAfter(1000, f);
  sim::run_for_ms(3000);
  ASSERT_EQ((int)forwarded().size(), 1);
}

STEST(dedup_remembers_six_frames_then_forgets_the_oldest) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<std::vector<uint8_t>> fr;
  for (int i = 0; i < 8; i++) fr.push_back(sensorFrame("S001", (int16_t)(2000 + i)));
  for (int i = 0; i < 7; i++) injectAfter(10 + 400 * i, fr[i]);     // 7 различни: първият изпада (6 записа)
  injectAfter(10 + 400 * 7, fr[6]);                                  // още е в буфера -> дубликат
  injectAfter(10 + 400 * 8, fr[0]);                                  // изпаднал -> препраща се пак
  sim::run_for_ms(8000);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 8);
  if (fw.size() == 8) ASSERT_STREQ(toHex(fw[7]->data), toHex(fr[0]));
}

STEST(many_sensors_are_all_forwarded_in_order) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<std::vector<uint8_t>> fr;
  for (int i = 0; i < 6; i++) fr.push_back(sensorFrame("S00X", (int16_t)(100 * i)));
  for (int i = 0; i < 6; i++) injectAfter(10 + 300 * i, fr[i]);
  sim::run_for_ms(5000);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 6);
  for (size_t i = 0; i < fw.size() && i < 6; i++) ASSERT_STREQ(toHex(fw[i]->data), toHex(fr[i]));
}

STEST(burst_up_to_the_queue_size_loses_nothing_and_transmissions_never_overlap) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<std::vector<uint8_t>> fr;
  for (int i = 0; i < 4; i++) fr.push_back(sensorFrame("S00Y", (int16_t)(7 * i)));
  for (int i = 0; i < 4; i++) injectAfter(10 + i, fr[i]);            // 4 пакета за 3 ms
  sim::run_for_ms(4000);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 4);
  std::vector<std::string> got, want;
  for (auto *f : fw) got.push_back(toHex(f->data));
  for (auto &f : fr) want.push_back(toHex(f));
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  ASSERT_TRUE(got == want);
  uint64_t prevEnd = 0;
  for (auto &f : sim::airLog) { ASSERT_TRUE(f.tStart >= prevEnd); prevEnd = f.tEnd; }
}

STEST(burst_over_the_queue_size_does_not_crash) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  for (int i = 0; i < 20; i++) injectAfter(10 + i, sensorFrame("S00Z", (int16_t)(3 * i)));
  sim::run_for_ms(20000);
  ASSERT_FALSE(sim::restarted());
  int n = (int)forwarded().size();
  ASSERT_TRUE(n >= 4 && n <= 20);
}

STEST(forwards_without_validating_content_documented_and_truncates_to_32_bytes) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  std::vector<uint8_t> junk(22);
  for (int i = 0; i < 22; i++) junk[i] = (uint8_t)i;
  injectAfter(10, junk);
  std::vector<uint8_t> big(60);
  for (int i = 0; i < 60; i++) big[i] = (uint8_t)(100 + i);
  injectAfter(500, big);
  sim::run_for_ms(3000);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 2);
  if (fw.size() == 2) {
    ASSERT_STREQ(toHex(fw[0]->data), toHex(junk));
    ASSERT_EQ((int)fw[1]->data.size(), 32);                          // буферът е DEDUP_MAX_LEN
  }
}

STEST(packets_on_the_tx_frequency_are_not_heard) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  injectAfter(10, sensorFrame(), TX_HZ);                              // чужд трафик в лентата към gateway-я
  sim::run_for_ms(2000);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_EQ(sim::radio.missedRx, 1);
}

STEST(crc_error_frames_are_dropped_by_the_radio_and_never_forwarded) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  sim::at_after_ms(10, [] { sim::radio.deliver(sensorFrame(), RX_HZ, -80, 5, false); }, true);
  sim::run_for_ms(2000);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
}

// ============================================================================ достъп до канала
STEST(jammed_channel_is_sensed_for_the_whole_window_then_the_frame_is_forwarded_anyway) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  sim::radio.busyUntil = sim::now_us() + 600000000ULL;
  injectAfter(10, sensorFrame());
  sim::run_for_ms(3000);
  ASSERT_EQ((int)forwarded().size(), 1);                              // измерването е по-ценно от чистия ефир
  ASSERT_GE(sim::radio.cadCount, 3);                                  // изчаква паузата, не само 2 CAD-а
}

STEST(a_second_repeater_waits_for_the_first_one_instead_of_transmitting_over_it) {
  setup();
  bootRep();
  sim::run_for_ms(500);
  uint64_t t0 = sim::now_us();
  sim::radio.busyUntil = t0 + 30000ULL;                               // "друг repeater" предава на TX честотата първите 30 ms
  injectAfter(5, sensorFrame());
  sim::run_for_ms(3000);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 1);
  if (!fw.empty()) ASSERT_TRUE(fw[0]->tStart >= t0 + 30000ULL);       // след края му, не върху него
}

STEST(reception_continues_while_the_forwarder_waits_for_a_busy_channel) {
  setup(9, 125000);                                                    // SF9: по-дълги изчаквания
  bootRep();
  sim::run_for_ms(500);
  // чужд предавател на TX честотата 300 ms; пакет A задейства сондиране, пакет B идва по средата.
  // CAD заема радиото (докато трае, то не приема) - B идва в първия момент след 150 ms, в който радиото слуша.
  sim::radio.busyUntil = sim::now_us() + 300000ULL;
  std::vector<uint8_t> a = sensorFrame("S00A"), b = sensorFrame("S00B");
  injectAfter(10, a);
  ASSERT_TRUE(deliverWhenListening(b, RX_HZ, 150));                    // радиото се връща на слушане между проверките
  sim::run_for_ms(6000);
  std::vector<const sim::RadioFrame *> fw = forwarded();
  ASSERT_EQ((int)fw.size(), 2);                                       // B е получен, докато A чака канала
  ASSERT_EQ(sim::radio.missedRx, 0);
  std::vector<std::string> got;
  for (auto *f : fw) got.push_back(toHex(f->data));
  ASSERT_TRUE(std::find(got.begin(), got.end(), toHex(a)) != got.end());
  ASSERT_TRUE(std::find(got.begin(), got.end(), toHex(b)) != got.end());
}

STEST(while_sensing_a_busy_channel_the_radio_still_listens_a_large_part_of_the_time) {
  for (uint8_t sf : {7, 9, 12}) {
    sim::reset();
    setup(sf, 125000);
    bootRep();
    sim::run_for_ms(500);
    sim::radio.busyUntil = sim::now_us() + 3000000ULL;                // канала е зает 3 s
    injectAfter(10, sensorFrame("S00A"));
    sim::run_for_ms(100);
    int pct = listeningPercent(1200);                                  // докато сондира
    printf("    SF%u: радиото слуша %d%% от времето при сондиране\n", (unsigned)sf, pct);
    ASSERT_GE(pct, 25);                                                // не е "глухо" през почти цялото време
  }
}

STEST(works_on_every_sf_and_bw) {
  for (int sf : {7, 10, 12}) {
    for (uint32_t bw : {62500u, 125000u, 250000u}) {
      sim::reset();
      setup((uint8_t)sf, bw);
      bootRep();
      sim::run_for_ms(500);
      std::vector<uint8_t> f = sensorFrame();
      sim::at_after_ms(10, [f] { sim::radio.deliver(f, RX_HZ); }, true);
      sim::run_for_ms(8000);
      std::vector<const sim::RadioFrame *> fw = forwarded();
      ASSERT_EQ((int)fw.size(), 1);
      if (!fw.empty()) { ASSERT_EQ(fw[0]->sf, sf); ASSERT_EQ((int)fw[0]->bwHz, (int)bw); ASSERT_STREQ(toHex(fw[0]->data), toHex(f)); }
    }
  }
}

// ============================================================================ heartbeat
// Първият heartbeat е на СЛУЧАЙНА фаза в (1 s, 304 s]; симулаторът е детерминиран, затова времето му се намира
// с пробно изпълнение (без трафик) и се ползва във второ.
static long firstHbMs() {
  sim::reset();
  setup();
  bootRep();
  sim::run_for_ms(400000);
  std::vector<const sim::RadioFrame *> hb = hbFrames();
  long t = hb.empty() ? -1 : (long)((hb[0]->tStart - 1000000ULL) / 1000);
  sim::reset();
  return t;
}

STEST(heartbeat_first_at_a_random_phase_then_every_five_minutes_on_the_tx_frequency) {
  setup();
  bootRep();
  sim::run_for_ms(1000000);
  std::vector<const sim::RadioFrame *> hb = hbFrames();
  ASSERT_TRUE(hb.size() == 3 || hb.size() == 4);
  if (hb.size() >= 3) {
    long first = (long)((hb[0]->tStart - 1000000ULL) / 1000);
    ASSERT_GE(first, (long)HB_FIRST_MIN_MS);
    ASSERT_LE(first, (long)HB_INTERVAL_MS + 1100);
    for (auto *f : hb) ASSERT_LE(std::labs(f->freqHz - (long)TX_HZ), 61L);
    for (size_t i = 1; i < hb.size(); i++) {
      ASSERT_MEM_NE(hb[i - 1]->data.data(), hb[i]->data.data(), 14);
      long gap = (long)((hb[i]->tStart - hb[i - 1]->tStart) / 1000);
      ASSERT_GE(gap, (long)HB_INTERVAL_MS - 1100);
      ASSERT_LE(gap, (long)HB_INTERVAL_MS + 1100);
    }
  }
  ASSERT_TRUE(sim::radio.listening());
}

STEST(different_boots_give_different_heartbeat_phases) {
  std::vector<long> firsts;
  for (uint32_t seed = 1; seed <= 6; seed++) {
    sim::reset();
    sim::seed_random(seed);
    setup();
    bootRep();
    sim::run_for_ms(400000);
    std::vector<const sim::RadioFrame *> hb = hbFrames();
    ASSERT_GE((int)hb.size(), 1);
    if (!hb.empty()) firsts.push_back((long)((hb[0]->tStart - 1000000ULL) / 1000));
  }
  std::sort(firsts.begin(), firsts.end());
  ASSERT_TRUE(firsts.back() - firsts.front() > 30000);    // разпределени, не на един и същ момент
}

STEST(heartbeat_is_skipped_on_a_busy_channel) {
  long t1 = firstHbMs();
  ASSERT_GE(t1, 1000L);
  setup();
  bootRep();
  sim::run_for_ms((uint32_t)(t1 - 2000));
  sim::radio.busyUntil = sim::now_us() + 15000000ULL;
  sim::run_for_ms(20000);
  ASSERT_EQ((int)hbFrames().size(), 0);
  ASSERT_TRUE(sim::log_contains("пропускам този heartbeat"));
  sim::run_for_ms(320000);
  ASSERT_EQ((int)hbFrames().size(), 1);
}

STEST(heartbeat_and_forwarding_share_the_radio_without_corrupting_either) {
  long t1 = firstHbMs();
  ASSERT_GE(t1, 1000L);
  setup();
  bootRep();
  // sensor пакети непосредствено преди и след момента на heartbeat-а
  std::vector<std::vector<uint8_t>> fr;
  for (int i = 0; i < 4; i++) fr.push_back(sensorFrame("S00H", (int16_t)(11 * i)));
  for (int i = 0; i < 4; i++) injectAfter((uint32_t)(t1 - 1500 + 700 * i), fr[i]);
  sim::run_for_ms((uint32_t)(t1 + 20000));
  ASSERT_EQ((int)hbFrames().size(), 1);
  ASSERT_EQ((int)forwarded().size(), 4);
  uint64_t prevEnd = 0;
  for (auto &f : sim::airLog) { ASSERT_TRUE(f.tStart >= prevEnd); prevEnd = f.tEnd; }
  ASSERT_EQ(sim::radio.abortedTx, 0);
  ASSERT_EQ(sim::radio.protocolErrors, 0);
}

// ============================================================================ енергия
STEST(idle_repeater_sleeps_nearly_all_the_time) {
  setup();
  bootRep();
  sim::run_for_ms(5000);
  sim::pm_reset_counters();
  sim::run_for_ms(3600000);
  sim::PmStats pm = sim::pm_stats();
  ASSERT_TRUE((double)pm.idle_us / (double)pm.total_us > 0.999);
  ASSERT_LE((int)pm.sleeps, 60);
  ASSERT_GE((int)pm.sleeps, 10);
  ASSERT_EQ(pm.manual_light_sleep_calls, 0);
  ASSERT_TRUE(hbFrames().size() == 11 || hbFrames().size() == 12);     // 3605 s / 304 s, на случайна фаза
}

STEST(a_packet_wakes_the_chip_through_dio0) {
  setup();
  bootRep();
  sim::run_for_ms(5000);
  sim::pm_reset_counters();
  sim::at_after_ms(20000, [] { sim::radio.deliver(sensorFrame(), RX_HZ); }, true);
  sim::run_for_ms(30000);
  ASSERT_GE((int)sim::pm_stats().gpio_wakes, 1);
  ASSERT_EQ((int)forwarded().size(), 1);
}

// ============================================================================ nonce / паралелизъм
STEST(heartbeat_counter_never_goes_backwards_across_a_power_on_reset) {
  setup();
  bootRep();
  sim::run_for_ms(700000);
  std::vector<const sim::RadioFrame *> hb = hbFrames();
  ASSERT_TRUE(hb.size() == 2 || hb.size() == 3);
  auto ctrOf = [](const sim::RadioFrame &f) {
    return ((uint32_t)f.data[6] << 24) | ((uint32_t)f.data[7] << 16) | ((uint32_t)f.data[8] << 8) | f.data[9];
  };
  uint32_t last = hb.empty() ? 0 : ctrOf(*hb.back());
  sim::reboot();
  sim::set_reset_reason(ESP_RST_POWERON);
  size_t base = sim::airLog.size();
  setup();
  bootRep();
  sim::run_for_ms(400000);
  ASSERT_GE((int)sim::airLog.size(), (int)base + 1);
  ASSERT_TRUE(ctrOf(sim::airLog[base]) > last);
}

STEST(chaos_interleavings_give_the_same_result) {
  for (uint32_t seed = 1; seed <= 25; seed++) {
    sim::reset();
    g_ctr = 1;
    setup();
    sim::chaos(seed, 250);
    bootRep();
    sim::run_for_ms(500);
    std::vector<std::vector<uint8_t>> fr;
    for (int i = 0; i < 3; i++) fr.push_back(sensorFrame("S00C", (int16_t)(5 * i)));
    injectAfter(10, fr[0]);
    injectAfter(300, fr[1]);
    injectAfter(320, fr[0]);                                           // дубликат
    injectAfter(700, fr[2]);
    sim::run_for_ms(5000);
    ASSERT_EQ((int)forwarded().size(), 3);
    ASSERT_EQ(sim::radio.protocolErrors, 0);
    ASSERT_EQ(sim::radio.modeViolations, 0);
    ASSERT_EQ(sim::radio.abortedTx, 0);
    ASSERT_TRUE(sim::radio.listening());
  }
}

STEST(tx_power_comes_from_nvs_with_a_default_of_17_dbm_and_is_limited_to_the_module_range) {
  struct { bool set; int dbm; int expectDbm; } cases[] = {
      {false, 0, 17}, {true, 10, 10}, {true, -9, -9}, {true, 22, 22}, {true, 30, 22}, {true, -20, -9}, {true, 0, 0}, {true, 14, 14}};
  for (auto &c : cases) {
    int want = refPowerSetting(c.expectDbm);
    setup();
    if (c.set) nvsTxPower(c.dbm);
    bootRep();
    sim::run_for_ms(500);
    injectAfter(10, sensorFrame());
    sim::run_for_ms(3000);
    std::vector<const sim::RadioFrame *> fw = forwarded();
    ASSERT_EQ((int)fw.size(), 1);
    if (!fw.empty()) ASSERT_EQ(fw[0]->txPowerDbm, want);
  }
}

int main() { return runAllTests(); }
