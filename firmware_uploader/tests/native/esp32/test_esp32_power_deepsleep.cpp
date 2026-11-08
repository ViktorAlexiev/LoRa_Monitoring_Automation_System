// sensor/power_mgmt.cpp - дълбок сън на сензора (таймер, ~15 минути).
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "power_mgmt.h"

TEST(sensor_sleeps_113_cycles_of_8s_about_15_minutes) {
  resetFakes();
  deep_sleep(WDT_CYCLES_BETWEEN_SEND);
  ASSERT_TRUE(g_deepSleepRequested);
  ASSERT_EQ((long)(g_deepSleepUs / 1000000ULL), 904L);     // 113 x 8 s
  ASSERT_TRUE(g_deepSleepUs / 1000000ULL / 60 >= 15);
}

TEST(deep_sleep_timer_scales_with_cycles) {
  resetFakes();
  deep_sleep(1);
  ASSERT_EQ((long)(g_deepSleepUs / 1000000ULL), 8L);
  resetFakes();
  deep_sleep(255);
  ASSERT_EQ((long)(g_deepSleepUs / 1000000ULL), 2040L);    // без препълване на 32 бита (255*8e6 < 2^32 но >uint16)
}

int main() { return runAllTests(); }
