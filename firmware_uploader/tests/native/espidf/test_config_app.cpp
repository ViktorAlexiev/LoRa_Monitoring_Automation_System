// Тестове на config приложението (РЕАЛНИЯТ config_app + cfg_parse) с подменена конзола.
#include "test_support.h"

#include <deque>
#include "cfg_parse.h"
#include "config_app.h"
#include "agro_hal.h"

// конзола: символите пристигат в зададени моменти (виртуално време)
struct Rx { uint64_t at; int c; };
static std::deque<Rx> g_in;
static std::vector<std::pair<uint64_t, std::string>> g_out;

static int getChar() {
  if (!g_in.empty() && g_in.front().at <= sim::now_us()) { int c = g_in.front().c; g_in.pop_front(); return c; }
  return -1;
}
static void print(const char *l) { g_out.push_back({sim::now_us(), l}); }

static void feed(const std::string &s, uint32_t atMs, uint32_t gapMs = 0) {
  uint64_t t = sim::now_us() + (uint64_t)atMs * 1000;
  for (char ch : s) { g_in.push_back({t, (unsigned char)ch}); t += (uint64_t)gapMs * 1000; }
}
static ConfigResult g_result;
static bool g_done;
static void run(uint32_t ms) {
  g_done = false;
  runAsTask([] {
    ConfigIo io = {getChar, print};
    g_result = config_run(&io, CONFIG_TIMEOUT_MS, CONFIG_READY_MS);
    g_done = true;
  }, ms);
}
static void fresh() { g_in.clear(); g_out.clear(); }
static int count(const char *s) { int n = 0; for (auto &o : g_out) if (o.second == s) n++; return n; }
static std::string last() { return g_out.empty() ? "" : g_out.back().second; }

static const std::string GOOD = "CFG:{\"id\":\"E007\",\"freq\":\"433000000\",\"sf\":\"9\"}\n";

STEST(ready_every_500ms_while_waiting_then_timeout_nack) {
  fresh();
  run(65000);
  ASSERT_TRUE(g_done);
  ASSERT_EQ((int)g_result, (int)CONFIG_TIMED_OUT);
  ASSERT_GE(count("READY"), 119);                   // 60 s / 0.5 s
  ASSERT_LE(count("READY"), 121);
  ASSERT_TRUE(g_out.back().second.find("NACK:Timeout") == 0);
  ASSERT_LE((long)((g_out.back().first - 1000000ULL) / 1000), 60100L);
}

STEST(valid_line_is_acked_stored_and_ready_stops) {
  fresh();
  feed(GOOD, 1200);
  run(5000);
  ASSERT_EQ((int)g_result, (int)CONFIG_ACKED);
  ASSERT_STREQ(last(), "ACK");
  ASSERT_GE(count("READY"), 2);
  ASSERT_LE(count("READY"), 4);
  ASSERT_STREQ(sim::nvs.str("id"), "E007");
  ASSERT_EQ(sim::nvs.num("sf"), 9);
  ASSERT_EQ(sim::nvs.num("freq"), 433000000);
}

STEST(characters_arriving_slowly_and_crlf_endings_are_handled) {
  fresh();
  std::string line = GOOD;
  line.insert(line.size() - 1, "\r");
  feed(line, 100, 5);                                // 5 ms между символите
  run(5000);
  ASSERT_EQ((int)g_result, (int)CONFIG_ACKED);
  ASSERT_STREQ(sim::nvs.str("id"), "E007");
}

STEST(boot_log_noise_before_the_cfg_line_is_ignored) {
  fresh();
  feed("ets Jun  8 2016\nI (123) boot: ESP-IDF v5\nrst:0x1 (POWERON)\n", 50, 1);
  feed(GOOD, 800);
  run(5000);
  ASSERT_EQ((int)g_result, (int)CONFIG_ACKED);
  ASSERT_STREQ(sim::nvs.str("id"), "E007");
}

STEST(invalid_line_is_nacked_without_writing_and_a_corrected_one_still_works) {
  fresh();
  feed("CFG:{\"id\":\"E007\",\"sf\":\"99\"}\n", 100);
  feed(GOOD, 2000);
  run(8000);
  ASSERT_EQ((int)g_result, (int)CONFIG_ACKED);
  bool nack = false;
  for (auto &o : g_out) if (o.second.find("NACK:невалиден sf") == 0) nack = true;
  ASSERT_TRUE(nack);
  ASSERT_EQ(sim::nvs.num("sf"), 9);                  // записва се само валидният ред
}

STEST(truncated_line_is_nacked_not_partially_stored) {
  fresh();
  feed("CFG:{\"id\":\"E007\",\"freq\":\"4330\n", 100);
  run(3000);
  bool nack = false;
  for (auto &o : g_out) if (o.second.find("NACK:") == 0) nack = true;
  ASSERT_TRUE(nack);
  ASSERT_FALSE(sim::nvs.has("id"));
  ASSERT_FALSE(sim::nvs.has("freq"));
}

STEST(overlong_line_is_reported_and_the_next_one_is_accepted) {
  fresh();
  feed(std::string(CFG_LINE_MAX + 500, 'x') + "\n", 100);
  feed(GOOD, 3000);
  run(10000);
  bool over = false;
  for (auto &o : g_out) if (o.second.find("NACK:ред твърде дълъг") == 0) over = true;
  ASSERT_TRUE(over);
  ASSERT_EQ((int)g_result, (int)CONFIG_ACKED);
}

STEST(the_full_gateway_line_from_the_uploader_fits_and_is_stored) {
  fresh();
  std::string l = "CFG:{\"id\":\"G001\",\"freq\":\"433000000\",\"freq_tx\":\"433500000\",\"sf\":\"9\",\"bw\":\"125000\","
                  "\"key\":\"00112233445566778899aabbccddeeff\",\"net\":\"1\",\"wifi_ssid\":\"Моята мрежа\",\"wifi_password\":\"тайна парола 123\","
                  "\"mqtt_ip\":\"192.168.1.10\",\"mqtt_port\":\"1883\",\"mqtt_user\":\"\",\"mqtt_password\":\"\"}\n";
  feed(l, 100);
  run(4000);
  ASSERT_EQ((int)g_result, (int)CONFIG_ACKED);
  ASSERT_STREQ(sim::nvs.str("wssid"), "Моята мрежа");         // UTF-8 в SSID/паролата минава непроменено
  ASSERT_STREQ(sim::nvs.str("wpass"), "тайна парола 123");
  ASSERT_EQ(sim::nvs.num("freqtx"), 433500000);
}

int main() { return runAllTests(); }
