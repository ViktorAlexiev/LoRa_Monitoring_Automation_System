// Тества РЕАЛНИЯ firmware_src/sensor/radio_timing.cpp (идентичен в 4-те устройства):
// Time-on-Air по Semtech формулата (сравнен с независима double реализация върху цялата
// решетка SF7..12 x BW 62.5/125/250 x дължини 1..64 x CRC), LDRO прага (вкл. SF11/BW125,
// където библиотеката LoRa греши), CAD timeout, стъпка и ACK timeout-и.
#include "test_framework.h"
#include "radio_timing.h"
#include <cmath>

// ---- независима референтна реализация (double, директно по AN1200.13) ----
static double refToaMs(int PL, int SF, double BW, int CRC, int crDenom, int pre) {
  double Ts = std::pow(2.0, SF) / BW * 1000.0;
  int DE = Ts > 16.0 ? 1 : 0;
  double num = 8.0 * PL - 4.0 * SF + 28 + 16 * CRC;
  double den = 4.0 * (SF - 2 * DE);
  double blocks = std::ceil(num / den);
  if (blocks < 0) blocks = 0;
  double nSym = 8 + blocks * (crDenom);   // (CR+4) == знаменателя
  return (pre + 4.25) * Ts + nSym * Ts;
}

TEST(toa_matches_reference_over_full_grid) {
  const uint32_t bws[] = {62500, 125000, 250000};
  int checked = 0, bad = 0;
  for (int sf = 7; sf <= 12; sf++) {
    for (uint32_t bw : bws) {
      for (int cr = 5; cr <= 8; cr++) {
        for (int crc = 0; crc <= 1; crc++) {
          radioTimingInit((uint8_t)sf, bw, (uint8_t)cr, 8, crc == 1);
          for (int pl = 1; pl <= 64; pl++) {
            double ref = refToaMs(pl, sf, (double)bw, crc, cr, 8);
            uint32_t got = radioToaMs((uint8_t)pl);
            // реализацията закръгля нагоре до цял ms
            if (std::fabs((double)got - std::ceil(ref - 1e-9)) > 0.5) {
              if (bad < 5) printf("  [MISMATCH] SF%d BW%u CR%d CRC%d PL%d got=%u ref=%.3f\n", sf, (unsigned)bw, cr, crc, pl, (unsigned)got, ref);
              bad++;
            }
            checked++;
          }
        }
      }
    }
  }
  ASSERT_EQ(checked, 9216);   // 6 SF x 3 BW x 4 CR x 2 CRC x 64 дължини
  ASSERT_EQ(bad, 0);
}

TEST(toa_known_values_sf7_bw125_crc_on) {
  radioTimingInit(7, 125000, 5, 8, true);
  ASSERT_EQ(radioToaMs(14), 47UL);    // 46.336 ms
  ASSERT_EQ(radioToaMs(20), 57UL);    // 56.576 ms
  ASSERT_EQ(radioToaMs(64), 119UL);   // 118.016 ms - worst case пакет на SF7
}

TEST(toa_known_values_sf12_bw125_crc_on) {
  radioTimingInit(12, 125000, 5, 8, true);
  ASSERT_EQ(radioToaMs(64), 2794UL);  // 2793.472 ms
  ASSERT_EQ(radioToaMs(20), 1319UL);  // 1318.9 ms
}

TEST(ldro_threshold_uses_real_symbol_time_not_truncated) {
  // Ts > 16 ms е прагът. SF11/BW125 = 16.384 ms -> ТРЯБВА да е включена (библиотеката
  // LoRa я оставя изключена заради целочислено деление: 1000/(125000/2048) = 16, не > 16).
  radioTimingInit(11, 125000, 5, 8, true);
  ASSERT_TRUE(radioLdroNeeded());
  ASSERT_EQ(radioSymbolUs(), 16384UL);

  radioTimingInit(12, 125000, 5, 8, true);  ASSERT_TRUE(radioLdroNeeded());
  radioTimingInit(10, 125000, 5, 8, true);  ASSERT_FALSE(radioLdroNeeded());
  radioTimingInit(7, 125000, 5, 8, true);   ASSERT_FALSE(radioLdroNeeded());
  // при по-широка лента символът е 2x по-къс
  radioTimingInit(11, 250000, 5, 8, true);  ASSERT_FALSE(radioLdroNeeded());   // 8.192 ms
  radioTimingInit(12, 250000, 5, 8, true);  ASSERT_TRUE(radioLdroNeeded());    // 16.384 ms
  // при по-тясна лента - 2x по-дълъг
  radioTimingInit(10, 62500, 5, 8, true);   ASSERT_TRUE(radioLdroNeeded());    // 16.384 ms
  radioTimingInit(9, 62500, 5, 8, true);    ASSERT_FALSE(radioLdroNeeded());   // 8.192 ms
}

TEST(cad_timeout_and_step_scale_with_sf) {
  const uint32_t expectedTimeout[] = {8, 10, 14, 22, 39, 72};   // SF7..SF12 @ BW125
  for (int sf = 7; sf <= 12; sf++) {
    radioTimingInit((uint8_t)sf, 125000, 5, 8, true);
    ASSERT_EQ(radioCadTimeoutMs(), expectedTimeout[sf - 7]);
    // стъпката никога не е по-малка от timeout-а и от 4 символа
    ASSERT_TRUE(radioCadStepMs() >= radioCadTimeoutMs());
    ASSERT_TRUE(radioCadStepMs() * 1000UL >= 4UL * radioSymbolUs());
  }
  radioTimingInit(7, 125000, 5, 8, true);
  ASSERT_EQ(radioCadStepMs(), 8UL);
  radioTimingInit(12, 125000, 5, 8, true);
  ASSERT_EQ(radioCadStepMs(), 132UL);   // max(72, ceil(4 x 32.768 = 131.07))
}

TEST(cad_timeout_doubles_when_bandwidth_halves) {
  radioTimingInit(9, 125000, 5, 8, true);
  uint32_t t125 = radioCadTimeoutMs();
  radioTimingInit(9, 62500, 5, 8, true);
  uint32_t t62 = radioCadTimeoutMs();
  ASSERT_TRUE(t62 > t125);
}

TEST(sensing_window_and_telemetry_wait_are_airtimes) {
  radioTimingInit(7, 125000, 5, 8, true);
  ASSERT_EQ(radioSensingWindowMs(), radioToaMs(64));
  ASSERT_EQ(radioTelemetryWaitMs(), radioToaMs(30));
  ASSERT_EQ(radioSlotMs(), radioCadStepMs());
}

TEST(ack_timeouts_match_formula_and_no_hidden_floor) {
  for (int sf = 7; sf <= 12; sf++) {
    radioTimingInit((uint8_t)sf, 125000, 5, 8, true);
    uint32_t w   = radioToaMs(64);
    uint32_t cmd = (3UL * (radioToaMs(26) + 500 + radioToaMs(20)) + 1) / 2 + 2 * w;
    uint32_t rst = (3UL * (radioToaMs(64) + 200 + radioToaMs(20)) + 1) / 2 + 2 * w;
    uint32_t req = (3UL * (radioToaMs(20) + 200 + radioToaMs(64)) + 1) / 2 + 2 * w;
    ASSERT_EQ(radioAckTimeoutCmdMs(), cmd);
    ASSERT_EQ(radioAckTimeoutRestartMs(), rst);
    ASSERT_EQ(radioStateReqTimeoutMs(), req);
  }
  // порядъци (от разговора): SF7 ~1.2 s (НЕ 4 s - няма долна граница), SF12 ~10.8 s
  radioTimingInit(7, 125000, 5, 8, true);
  ASSERT_TRUE(radioAckTimeoutCmdMs() > 1000 && radioAckTimeoutCmdMs() < 1400);
  ASSERT_TRUE(radioAckTimeoutCmdMs() < 4000);
  radioTimingInit(12, 125000, 5, 8, true);
  ASSERT_TRUE(radioAckTimeoutCmdMs() > 10000 && radioAckTimeoutCmdMs() < 11500);
}

TEST(ack_timeouts_monotonic_in_sf) {
  uint32_t prevCmd = 0, prevRst = 0;
  for (int sf = 7; sf <= 12; sf++) {
    radioTimingInit((uint8_t)sf, 125000, 5, 8, true);
    ASSERT_TRUE(radioAckTimeoutCmdMs() > prevCmd);
    ASSERT_TRUE(radioAckTimeoutRestartMs() > prevRst);
    prevCmd = radioAckTimeoutCmdMs();
    prevRst = radioAckTimeoutRestartMs();
  }
}

TEST(bw_index_mapping_and_validation) {
  ASSERT_EQ(radioBwFromIndex(0), 62500UL);
  ASSERT_EQ(radioBwFromIndex(1), 125000UL);
  ASSERT_EQ(radioBwFromIndex(2), 250000UL);
  ASSERT_EQ(radioBwFromIndex(3), 0UL);
  ASSERT_EQ(radioBwFromIndex(0xFF), 0UL);   // изтрит EEPROM
  ASSERT_EQ(radioBwToIndex(62500), (uint8_t)0);
  ASSERT_EQ(radioBwToIndex(125000), (uint8_t)1);
  ASSERT_EQ(radioBwToIndex(250000), (uint8_t)2);
  ASSERT_EQ(radioBwToIndex(500000), (uint8_t)0xFF);
  ASSERT_EQ(radioBwToIndex(0), (uint8_t)0xFF);
}

TEST(sf_validation_and_init_falls_back_to_defaults) {
  ASSERT_FALSE(radioSfValid(6));
  ASSERT_TRUE(radioSfValid(7));
  ASSERT_TRUE(radioSfValid(12));
  ASSERT_FALSE(radioSfValid(13));
  ASSERT_FALSE(radioSfValid(0xFF));

  radioTimingInit(99, 12345, 9, 8, true);   // всичко невалидно
  ASSERT_EQ(radioSf(), (uint8_t)RADIO_SF_DEFAULT);
  ASSERT_EQ(radioBw(), (uint32_t)RADIO_BW_DEFAULT_HZ);
  ASSERT_EQ(radioCrDenom(), (uint8_t)5);
}

int main() {
  return runAllTests();
}
