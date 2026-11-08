// executor/power_mgmt.cpp (идентичен в repeater/) - лек сън и броене на 8-секундните тикове.
// Часовникът (gettimeofday) и сънят са симулирани: виж fakelibs_esp32/esp_sleep.h.
#include "../test_framework.h"
#include "esp32_test_helpers.h"
#include "power_mgmt.h"
#include "board_pins.h"

static const uint64_t SEC = 1000000ULL;

TEST(wake_is_armed_on_lora_dio0_high) {
  resetFakes();
  enablePinChangeWake();
  ASSERT_EQ(g_wakeGpio, (int)LORA_DIO0);
  ASSERT_EQ(g_wakeGpioLevel, (int)GPIO_INTR_HIGH_LEVEL);
  ASSERT_TRUE(g_gpioWakeEnabled);
}

TEST(ticks_are_counted_from_elapsed_time) {
  resetFakes();
  wdt_arm_8s_continuous();
  powerTicksPoll();
  ASSERT_EQ((int)wdt_ticks, 0);
  g_fakeTimeUs += 7 * SEC;  powerTicksPoll();
  ASSERT_EQ((int)wdt_ticks, 0);          // още не са станали 8 s
  g_fakeTimeUs += 1 * SEC;  powerTicksPoll();
  ASSERT_EQ((int)wdt_ticks, 1);
  g_fakeTimeUs += 24 * SEC; powerTicksPoll();
  ASSERT_EQ((int)wdt_ticks, 4);
}

TEST(idle_sleep_wakes_on_next_tick_boundary) {
  resetFakes();
  enablePinChangeWake();
  wdt_arm_8s_continuous();
  for (int i = 1; i <= 5; i++) {
    deep_sleep_until_event();            // няма събитие на DIO0 -> спи до следващия тик
    ASSERT_EQ((int)wdt_ticks, i);
  }
  ASSERT_EQ(g_lightSleepCount, 5);
  ASSERT_EQ(g_lightSleepGpioWakes, 0);
}

TEST(heartbeat_interval_is_38_ticks_about_5_minutes) {
  resetFakes();
  enablePinChangeWake();
  wdt_arm_8s_continuous();
  int sleeps = 0;
  while ((int)wdt_ticks < HB_INTERVAL_CYCLES) { deep_sleep_until_event(); sleeps++; ASSERT_TRUE(sleeps < 100); }
  ASSERT_EQ(sleeps, 38);
  uint64_t elapsedSec = (g_fakeTimeUs - 1000000ULL) / SEC;
  ASSERT_EQ((long)elapsedSec, 304L);     // 38 x 8 s
}

TEST(frequent_radio_wakeups_do_not_starve_the_heartbeat_ticks) {
  // repeater в натоварена мрежа: пакет на всяка секунда. Тиковете трябва да се броят по реално
  // време, а не само при пълно изтичане на таймера.
  resetFakes();
  enablePinChangeWake();
  wdt_arm_8s_continuous();
  for (int i = 0; i < 100; i++) {
    g_gpioWakeAfterUs = 1 * SEC;         // входящ пакет след 1 s
    deep_sleep_until_event();
  }
  // близо до границата на тика таймерът (<1 s) се задейства преди пакета - нормално
  ASSERT_TRUE(g_lightSleepGpioWakes >= 80 && g_lightSleepGpioWakes <= 100);
  // инвариантата: тиковете винаги следват реално изминалото време, независимо колко пъти е будено
  long elapsedSec = (long)((g_fakeTimeUs - 1000000ULL) / SEC);
  ASSERT_TRUE(elapsedSec >= 90);
  ASSERT_EQ((int)wdt_ticks, (int)(elapsedSec / 8));
}

TEST(ticks_keep_counting_while_awake_between_sleeps) {
  resetFakes();
  enablePinChangeWake();
  wdt_arm_8s_continuous();
  g_fakeTimeUs += 20 * SEC;              // 20 s будно (обработка на пакети, TX и т.н.)
  deep_sleep_until_event();              // вътре първо наваксва пропуснатите тикове
  ASSERT_TRUE((int)wdt_ticks >= 2);
}

TEST(sleep_never_requests_zero_or_negative_timer) {
  resetFakes();
  enablePinChangeWake();
  wdt_arm_8s_continuous();
  g_fakeTimeUs += 8 * SEC - 1;           // 1 µs преди тика
  deep_sleep_until_event();
  ASSERT_TRUE(g_timerWakeUs > 0);
  ASSERT_TRUE(g_timerWakeUs <= 8 * SEC);
}

int main() { return runAllTests(); }
