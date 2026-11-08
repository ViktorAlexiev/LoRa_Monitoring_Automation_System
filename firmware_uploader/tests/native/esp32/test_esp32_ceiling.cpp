// ceiling_counter.cpp (ESP32: NVS watermark + RTC памет). Реалният файл от sensor/ (идентичен в
// executor/ и repeater/ - това се проверява от run_esp32.sh).
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "ceiling_counter.h"
#include <set>

TEST(fresh_power_on_starts_at_zero_and_commits_100_ahead) {
  resetFakes();
  CeilingCounter c;
  c.begin("ceil");
  ASSERT_EQ((long)c.next(), 0L);
  ASSERT_EQ((long)nvsGetNum("ceil"), 100L);   // watermark се записва ПРЕДИ да се ползва 0
  ASSERT_EQ((long)c.next(), 1L);
  ASSERT_EQ((long)c.next(), 2L);
  ASSERT_EQ((long)nvsGetNum("ceil"), 100L);   // докато не стигне 100 - без нов запис
  ASSERT_EQ(g_nvsWrites, 1);
}

TEST(commits_only_once_per_100_values) {
  resetFakes();
  CeilingCounter c;
  c.begin("ceil");
  for (int i = 0; i < 250; i++) c.next();     // стойности 0..249 -> записи при 0, 100, 200
  ASSERT_EQ(g_nvsWrites, 3);
  ASSERT_EQ((long)nvsGetNum("ceil"), 300L);
}

TEST(power_on_reset_resumes_at_committed_never_backwards) {
  resetFakes();
  CeilingCounter a;
  a.begin("ceil");
  uint32_t last = 0;
  for (int i = 0; i < 250; i++) last = a.next();   // последна ползвана = 249
  ASSERT_EQ((long)last, 249L);

  g_resetReason = ESP_RST_POWERON;                 // unclean reset: RAM е загубена
  CeilingCounter b;
  b.begin("ceil");
  uint32_t first = b.next();
  ASSERT_EQ((long)first, 300L);                    // скача напред до committed, не назад
  ASSERT_TRUE(first > last);
}

TEST(deep_sleep_wake_keeps_counter_without_flash_write) {
  resetFakes();
  CeilingCounter a;
  a.begin("ceil");
  for (int i = 0; i < 5; i++) a.next();            // ползвани 0..4, RTC памет = 5
  int writesBefore = g_nvsWrites;

  g_resetReason = ESP_RST_DEEPSLEEP;               // събуждане от дълбок сън (sensor)
  CeilingCounter b;
  b.begin("ceil");
  ASSERT_EQ((long)b.next(), 5L);                   // продължава плавно, без скок до 100
  ASSERT_EQ((long)b.next(), 6L);
  ASSERT_EQ(g_nvsWrites, writesBefore);            // без нов запис във flash
}

TEST(sensor_wakeups_all_day_write_flash_rarely) {
  // сензор: 96 събуждания/ден x 365 дни = 35 040 съобщения -> записи във flash ~ 351, не 35 040
  resetFakes();
  CeilingCounter c;
  g_resetReason = ESP_RST_POWERON;
  c.begin("ceil");
  for (int n = 0; n < 35040; n++) {
    c.next();
    g_resetReason = ESP_RST_DEEPSLEEP;
    CeilingCounter fresh;                          // новият "бут" след всеки сън
    fresh.begin("ceil");
    c = fresh;
  }
  ASSERT_TRUE(g_nvsWrites <= 360);
  ASSERT_TRUE(g_nvsWrites >= 340);
}

TEST(values_are_unique_and_increasing_over_many_unclean_resets) {
  resetFakes();
  std::set<uint32_t> seen;
  uint32_t maxSeen = 0;
  bool first = true;
  bool monotonic = true, unique = true;
  for (int boot = 0; boot < 40; boot++) {
    g_resetReason = ESP_RST_POWERON;
    CeilingCounter c;
    c.begin("ceil");
    int uses = (boot * 37) % 130 + 1;              // различен брой ползвания преди всеки "reset"
    for (int i = 0; i < uses; i++) {
      uint32_t v = c.next();
      if (!seen.insert(v).second) unique = false;
      if (!first && v <= maxSeen) monotonic = false;
      maxSeen = v;
      first = false;
    }
  }
  ASSERT_TRUE(unique);
  ASSERT_TRUE(monotonic);
}

TEST(never_writes_nvs_in_read_only_mode) {
  resetFakes();
  CeilingCounter c;
  c.begin("ceil");
  for (int i = 0; i < 300; i++) c.next();
  ASSERT_EQ(g_nvsReadOnlyWriteAttempts, 0);
}

TEST(two_counters_with_different_keys_do_not_interfere) {
  resetFakes();
  CeilingCounter a, b;
  a.begin("ceilA");
  g_resetReason = ESP_RST_POWERON;
  b.begin("ceilB");
  a.next(); a.next();
  ASSERT_TRUE(nvsHas("ceilA"));
  ASSERT_TRUE(!nvsHas("ceilB"));
  ASSERT_EQ((long)nvsGetNum("ceilA"), 100L);
}

int main() { return runAllTests(); }
