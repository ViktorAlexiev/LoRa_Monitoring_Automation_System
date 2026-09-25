// Тества РЕАЛНАТА firmware_src/sensor/channel_access.cpp (state machine на достъпа до
// канала) с подменени CAD и random функции и контролирано време - точно както работи в
// устройствата, но детерминистично: проверява КОГА и КОЛКО ПЪТИ се прави CAD, какъв е
// backoff-ът и кога се отказва.
#include "test_framework.h"
#include "channel_access.h"
#include <vector>

// ---- подменен CAD: следва зададена последователност (true = зает) ----
static std::vector<bool> g_seq;
static size_t g_seqPos = 0;
static bool g_defaultBusy = false;
static int g_cadCalls = 0;
static bool fakeBusy() {
  g_cadCalls++;
  if (g_seqPos < g_seq.size()) return g_seq[g_seqPos++];
  return g_defaultBusy;
}
// ---- подменен random: връща фиксирана стойност (ограничена до maxExcl-1) ----
static uint32_t g_rndValue = 0;
static uint32_t g_lastRndMax = 0;
static uint32_t fakeRandom(uint32_t maxExcl) {
  g_lastRndMax = maxExcl;
  return g_rndValue < maxExcl ? g_rndValue : (maxExcl ? maxExcl - 1 : 0);
}
static void setup(const std::vector<bool>& seq, bool defBusy = false, uint32_t rnd = 0, int sf = 7) {
  g_seq = seq; g_seqPos = 0; g_defaultBusy = defBusy; g_cadCalls = 0; g_rndValue = rnd;
  radioTimingInit((uint8_t)sf, 125000, 5, 8, true);
}
static uint8_t poll(ChannelAccess &ca, uint32_t now) {
  return channelAccessPoll(&ca, now, fakeBusy, fakeRandom);
}

// ============================== COMMAND ==============================
TEST(command_clear_channel_transmits_immediately_with_one_cad) {
  setup({false});
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  ASSERT_EQ(poll(ca, 1000), (uint8_t)CH_CLEAR);
  ASSERT_EQ(g_cadCalls, 1);          // без сондиране, без backoff, без jitter
}

TEST(command_busy_then_clear_sensing_backoff_final_cad) {
  // SF7: step = 8 ms, slot = 8 ms. random(4) -> 2 слота = 16 ms
  setup({true, true, false, false}, false, 2);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  uint32_t t = 1000, step = radioCadStepMs();

  ASSERT_EQ(poll(ca, t), (uint8_t)CH_WAIT);            // CAD #1: зает -> сондиране
  ASSERT_EQ(g_cadCalls, 1);
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_WAIT);            // още не е дошло времето -> БЕЗ CAD
  ASSERT_EQ(poll(ca, t + step - 1), (uint8_t)CH_WAIT);
  ASSERT_EQ(g_cadCalls, 1);

  t += step;
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_WAIT);            // CAD #2: пак зает
  ASSERT_EQ(g_cadCalls, 2);

  t += step;
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_WAIT);            // CAD #3: свободно -> случаен слот backoff
  ASSERT_EQ(g_cadCalls, 3);
  ASSERT_EQ(g_lastRndMax, 4UL);                        // 0..3 слота

  ASSERT_EQ(poll(ca, t + 2 * radioSlotMs() - 1), (uint8_t)CH_WAIT);   // backoff още тече - без CAD
  ASSERT_EQ(g_cadCalls, 3);
  ASSERT_EQ(poll(ca, t + 2 * radioSlotMs()), (uint8_t)CH_CLEAR);      // последен CAD #4: свободно
  ASSERT_EQ(g_cadCalls, 4);
}

TEST(command_zero_slot_backoff_does_final_cad_on_next_poll) {
  setup({true, false, false}, false, 0);   // random -> 0 слота
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  uint32_t t = 500;
  poll(ca, t);                                  // #1 зает
  t += radioCadStepMs();
  poll(ca, t);                                  // #2 свободно -> backoff 0 слота
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_CLEAR);    // #3 веднага
  ASSERT_EQ(g_cadCalls, 3);
}

TEST(command_final_cad_busy_returns_to_sensing_keeping_same_deadline) {
  setup({true, false, true, false, false}, false, 1);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  uint32_t t0 = 2000, step = radioCadStepMs();
  poll(ca, t0);                                   // #1 зает, deadline = t0 + toa(64)
  uint32_t deadline = ca.deadline;
  ASSERT_EQ(deadline, t0 + radioSensingWindowMs());
  uint32_t t = t0 + step;
  poll(ca, t);                                    // #2 свободно -> backoff (1 слот)
  t += radioSlotMs();
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_WAIT);       // #3 последен CAD: пак зает -> обратно на сондиране
  ASSERT_EQ(ca.deadline, deadline);               // срокът НЕ е нулиран
  t += step;
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_WAIT);       // #4 свободно -> нов backoff
  t += radioSlotMs();
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_CLEAR);      // #5 свободно
}

TEST(command_always_busy_gives_up_after_one_airtime_window) {
  setup({}, true);   // винаги зает
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  uint32_t t0 = 10000, t = t0;
  uint8_t r = CH_WAIT;
  while (r == CH_WAIT && t < t0 + 20000) { r = poll(ca, t); t += 1; }
  ASSERT_EQ(r, (uint8_t)CH_GIVEUP);
  ASSERT_TRUE(t - 1 >= t0 + radioSensingWindowMs());        // не се отказва по-рано от срока
  ASSERT_TRUE(t - 1 <= t0 + radioSensingWindowMs() + radioCadStepMs() + 1);   // и не много след него
}

TEST(command_number_of_cads_is_about_same_at_every_sf) {
  // "~15-24 проверки на всеки SF" - броят се пази приблизително постоянен, защото стъпката
  // расте със Ts, докато срокът (airtime на 64 B) расте със същия порядък.
  for (int sf = 7; sf <= 12; sf++) {
    setup({}, true, 0, sf);
    ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
    uint32_t t = 0; uint8_t r = CH_WAIT;
    while (r == CH_WAIT && t < 60000) { r = poll(ca, t); t += 1; }
    ASSERT_EQ(r, (uint8_t)CH_GIVEUP);
    ASSERT_TRUE(g_cadCalls >= 10 && g_cadCalls <= 30);
  }
}

// ============================== TELEMETRY ==============================
TEST(telemetry_clear_channel_transmits_immediately) {
  setup({false});
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE);
  ASSERT_EQ(poll(ca, 0), (uint8_t)CH_CLEAR);
  ASSERT_EQ(g_cadCalls, 1);
}

TEST(telemetry_busy_waits_random_up_to_airtime_then_second_cad) {
  setup({true, false}, false, 50);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_TELEMETRY_SKIP);
  ASSERT_EQ(poll(ca, 1000), (uint8_t)CH_WAIT);               // CAD #1 зает
  ASSERT_EQ(g_lastRndMax, radioTelemetryWaitMs() + 1);       // изчакване в [0, toa(30 B)]
  ASSERT_EQ(poll(ca, 1049), (uint8_t)CH_WAIT);               // още чакаме - БЕЗ CAD
  ASSERT_EQ(g_cadCalls, 1);
  ASSERT_EQ(poll(ca, 1050), (uint8_t)CH_CLEAR);              // CAD #2 свободно
  ASSERT_EQ(g_cadCalls, 2);
}

TEST(telemetry_force_transmits_anyway_after_second_busy) {
  setup({true, true}, false, 10);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE);
  poll(ca, 0);
  ASSERT_EQ(poll(ca, 10), (uint8_t)CH_CLEAR);   // зает и втори път, но FORCE -> предай
  ASSERT_EQ(g_cadCalls, 2);                      // най-много 2 CAD-а
}

TEST(telemetry_skip_gives_up_after_second_busy) {
  setup({true, true}, false, 10);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_TELEMETRY_SKIP);
  poll(ca, 0);
  ASSERT_EQ(poll(ca, 10), (uint8_t)CH_GIVEUP);
  ASSERT_EQ(g_cadCalls, 2);
}

TEST(telemetry_never_does_more_than_two_cads) {
  for (int sf = 7; sf <= 12; sf++) {
    setup({}, true, 0, sf);
    ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_TELEMETRY_FORCE);
    uint32_t t = 0; uint8_t r = CH_WAIT;
    while (r == CH_WAIT && t < 20000) { r = poll(ca, t); t += 1; }
    ASSERT_EQ(r, (uint8_t)CH_CLEAR);
    ASSERT_EQ(g_cadCalls, 2);
  }
}

// ============================== millis() wraparound ==============================
TEST(command_works_across_millis_wraparound) {
  setup({true, true, false, false}, false, 1);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  uint32_t t = 0xFFFFFFFCUL, step = radioCadStepMs();
  poll(ca, t);            // зает; nextAt = t + step пресича 0
  t += step;              // -> малко число след пренасянето
  ASSERT_TRUE(t < 0x100000UL);
  poll(ca, t);            // #2 зает
  t += step;
  poll(ca, t);            // #3 свободно -> backoff 1 слот
  t += radioSlotMs();
  ASSERT_EQ(poll(ca, t), (uint8_t)CH_CLEAR);
}

TEST(each_poll_does_at_most_one_cad) {
  // неблокиращо: една проверка на извикване, независимо колко време е минало
  setup({true, true, true, true, true, true}, true);
  ChannelAccess ca; channelAccessStart(&ca, CH_POLICY_COMMAND);
  int before = 0;
  for (uint32_t t = 0; t < 500; t += 7) {
    before = g_cadCalls;
    poll(ca, t);
    ASSERT_TRUE(g_cadCalls - before <= 1);
  }
}

int main() {
  return runAllTests();
}
