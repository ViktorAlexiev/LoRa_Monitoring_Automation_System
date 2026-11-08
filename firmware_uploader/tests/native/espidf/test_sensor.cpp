// Тестове на приложението sensor (РЕАЛНИЯТ app_main + sensors_io + sensor_tx) срещу симулатора.
#include "test_support.h"

#include "board_pins.h"
#include "ceiling_counter.h"
#include "esp_system.h"
#include "crypto_common.h"
#include "protocol.h"
#include "radio_timing.h"
#include "sensor_config.h"
#include "sensor_tx.h"
#include "sensors_io.h"
#include "agro_radio.h"
#include "test_radio_support.h"

extern "C" void app_main(void);

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);

struct Cycle {
  bool slept = false;
  uint64_t sleepUs = 0;
  uint64_t awakeUs = 0;
};

// една "работа" на чипа: събуждане -> app_main -> дълбок сън
static Cycle runCycle(uint32_t maxMs = 30000) {
  Cycle c;
  uint64_t t0 = sim::now_us();
  sim::boot(app_main);
  sim::run_for_ms(maxMs);
  c.slept = sim::deep_slept();
  c.sleepUs = sim::deep_sleep_us();
  c.awakeUs = sim::now_us() - t0;
  return c;
}

struct Decoded { bool ok; std::string id; int16_t v[4]; };
static Decoded decode(const std::vector<uint8_t> &frame, const uint8_t *key = TEST_KEY) {
  Decoded d = {};
  if (frame.size() != SENSOR_WIRE_LEN) return d;
  uint8_t sender[6], pt[CRYPTO_MAX_PT], len = 0;
  d.ok = cryptoParseWirePacket(frame.data(), (uint8_t)frame.size(), key, CRYPTO_TYPE_SENSOR, sender, pt, &len) && len == SENSOR_PT_LEN;
  if (!d.ok) return d;
  for (int i = 0; i < 6 && sender[i]; i++) d.id += (char)sender[i];
  for (int i = 0; i < 4; i++) d.v[i] = (int16_t)(pt[2 * i] | (pt[2 * i + 1] << 8));
  return d;
}

static void configured(const char *id = "S001", uint32_t freq = 433000000, uint8_t sf = 7, uint32_t bw = 125000) {
  nvsRadioConfig(id, freq, sf, bw);
}

// ============================================================================ нормален цикъл
STEST(normal_cycle_sends_one_22_byte_frame_and_deep_sleeps_15_minutes) {
  configured();
  sim::i2c.soil().temp = 23.46; sim::i2c.soil().hum = 61.2;
  sim::i2c.air().temp = -4.30; sim::i2c.air().hum = 88.7;
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((long)c.sleepUs, 904000000L);                       // 113 x 8 s
  ASSERT_EQ((int)sim::airLog.size(), 1);
  const sim::RadioFrame &f = sim::airLog[0];
  ASSERT_EQ((int)f.data.size(), 22);
  ASSERT_LE(std::labs(f.freqHz - 433000000L), 61L);
  ASSERT_EQ(f.sf, 7);
  ASSERT_EQ((int)f.bwHz, 125000);
  ASSERT_TRUE(f.crc);
  Decoded d = decode(f.data);
  ASSERT_TRUE(d.ok);
  ASSERT_STREQ(d.id, "S001");
  ASSERT_NEAR(d.v[0], 2346, 2);                                  // SHT31 е 16-битов: ±0.01 °C
  ASSERT_NEAR(d.v[1], 6120, 5);
  ASSERT_NEAR(d.v[2], -430, 2);                                  // и двата сензора са SHT31 (16 бита): ±0.01 °C
  ASSERT_NEAR(d.v[3], 8870, 3);
  ASSERT_EQ(sim::radio.mode(), 0);                               // радиото е приспано преди дълбокия сън
  ASSERT_EQ(sim::radio.protocolErrors, 0);
  ASSERT_EQ(sim::radio.modeViolations, 0);
}

STEST(awake_time_is_short_and_no_extra_tasks_or_timers_exist) {
  configured();
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  // jitter <= 1 s + измерване ~0.15 s + airtime ~50 ms
  ASSERT_LE((long)c.awakeUs, 1500000L);
  ASSERT_GE((long)c.awakeUs, 100000L);
  std::vector<sim::TaskInfo> t = sim::tasks();
  ASSERT_EQ((int)t.size(), 1);                                    // само main: няма Wi-Fi, MQTT, допълнителни задачи
  ASSERT_STREQ(t[0].name, "main");
}

STEST(slow_radio_settings_still_fit_the_awake_budget) {
  configured("S001", 868100000, 12, 62500);                        // най-бавно: SF12/BW62.5 (airtime ~1.4 s за 22 B)
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  const sim::RadioFrame &f = sim::airLog[0];
  ASSERT_EQ(f.sf, 12);
  ASSERT_TRUE(f.ldro);
  ASSERT_LE((long)c.awakeUs, 4500000L);
  ASSERT_TRUE(decode(f.data).ok);
}

STEST(every_sf_bw_combination_produces_a_decodable_frame_on_the_configured_settings) {
  for (int sf = 7; sf <= 12; sf++) {
    for (uint32_t bw : {62500u, 125000u, 250000u}) {
      sim::reset();
      configured("S001", 433000000, (uint8_t)sf, bw);
      Cycle c = runCycle();
      ASSERT_TRUE(c.slept);
      ASSERT_EQ((int)sim::airLog.size(), 1);
      ASSERT_EQ(sim::airLog[0].sf, sf);
      ASSERT_EQ((int)sim::airLog[0].bwHz, (int)bw);
      ASSERT_TRUE(decode(sim::airLog[0].data).ok);
    }
  }
}

// ============================================================================ отказ на сензори
static Decoded oneCycleDecoded() {
  Cycle c = runCycle();
  Decoded d = {};
  if (c.slept && sim::airLog.size() == 1) d = decode(sim::airLog[0].data);
  return d;
}

STEST(missing_soil_sensor_gives_no_data_only_for_the_soil_values) {
  configured();
  sim::i2c.soil().present = false;
  sim::i2c.air().temp = 20.0; sim::i2c.air().hum = 50.0;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_EQ(d.v[0], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[1], SENSOR_RAW_NODATA);
  ASSERT_NEAR(d.v[2], 2000, 3);
  ASSERT_NEAR(d.v[3], 5000, 5);
}

STEST(missing_air_sensor_gives_no_data_only_for_the_air_values) {
  configured();
  sim::i2c.air().present = false;
  sim::i2c.soil().temp = 18.0; sim::i2c.soil().hum = 40.0;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_NEAR(d.v[0], 1800, 2);
  ASSERT_NEAR(d.v[1], 4000, 3);
  ASSERT_EQ(d.v[2], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[3], SENSOR_RAW_NODATA);
}

STEST(bad_crc_is_no_data_not_a_garbage_value) {
  configured();
  sim::i2c.soil().badCrc = true;
  sim::i2c.air().badCrc = true;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  for (int i = 0; i < 4; i++) ASSERT_EQ(d.v[i], SENSOR_RAW_NODATA);
}

STEST(sht31_values_outside_the_physical_range_are_no_data_for_that_sensor_only) {
  configured();
  sim::i2c.air().useRawOverride = true;
  sim::i2c.air().rawTemp = 0xFFFF;                                 // 130 °C - над обхвата на сензора
  sim::i2c.air().rawHum = 0x8000;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_EQ(d.v[2], SENSOR_RAW_NODATA);                            // и двете стойности на сензора са невалидни заедно
  ASSERT_EQ(d.v[3], SENSOR_RAW_NODATA);
  ASSERT_TRUE(d.v[0] != SENSOR_RAW_NODATA);                        // другият сензор не е засегнат
  ASSERT_TRUE(d.v[1] != SENSOR_RAW_NODATA);
  sim::reset();
  configured();
  sim::i2c.soil().useRawOverride = true;
  sim::i2c.soil().rawTemp = 0x0000;                                // -45 °C - под обхвата
  sim::i2c.soil().rawHum = 0x8000;
  d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_EQ(d.v[0], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[1], SENSOR_RAW_NODATA);
  ASSERT_TRUE(d.v[2] != SENSOR_RAW_NODATA);
}

STEST(i2c_bus_failure_still_sends_a_frame_with_all_no_data) {
  configured();
  sim::i2c.bus[0].failCreate = sim::i2c.bus[1].failCreate = true;  // и двете шини не се вдигат
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);                                               // живо устройство без данни, не мълчание
  for (int i = 0; i < 4; i++) ASSERT_EQ(d.v[i], SENSOR_RAW_NODATA);
  ASSERT_EQ(sim::i2c.usageErrors, 0);
}

STEST(one_bus_failing_only_loses_its_own_sensor) {
  configured();
  sim::i2c.bus[1].failCreate = true;                               // шината на въздушния сензор
  sim::i2c.soil().temp = 21.0; sim::i2c.soil().hum = 33.0;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_NEAR(d.v[0], 2100, 2);
  ASSERT_NEAR(d.v[1], 3300, 2);
  ASSERT_EQ(d.v[2], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[3], SENSOR_RAW_NODATA);
  sim::reset();
  configured();
  sim::i2c.bus[0].failAddDevice = true;                            // устройството на почвения сензор не се добавя
  sim::i2c.air().temp = 15.0; sim::i2c.air().hum = 70.0;
  d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_EQ(d.v[0], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[1], SENSOR_RAW_NODATA);
  ASSERT_NEAR(d.v[2], 1500, 2);
  ASSERT_NEAR(d.v[3], 7000, 2);
  ASSERT_EQ(sim::i2c.usageErrors, 0);
  ASSERT_EQ(sim::i2c.liveBuses(), 0);                              // и неуспешният канал не оставя шина
}

STEST(each_sensor_is_on_its_own_bus_with_the_pins_and_speed_from_board_pins) {
  configured();
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  const sim::I2cBusFake &soil = sim::i2c.bus[I2C_SOIL_PORT];
  const sim::I2cBusFake &air = sim::i2c.bus[I2C_AIR_PORT];
  ASSERT_TRUE(I2C_SOIL_PORT != I2C_AIR_PORT);                      // две различни шини
  ASSERT_EQ(soil.busesCreated, 1);
  ASSERT_EQ(air.busesCreated, 1);
  ASSERT_EQ(soil.sda, I2C_SOIL_SDA); ASSERT_EQ(soil.scl, I2C_SOIL_SCL);
  ASSERT_EQ(air.sda, I2C_AIR_SDA);   ASSERT_EQ(air.scl, I2C_AIR_SCL);
  ASSERT_TRUE(soil.sda != air.sda && soil.scl != air.scl);
  ASSERT_EQ((int)soil.hz, I2C_HZ); ASSERT_EQ((int)air.hz, I2C_HZ);
  ASSERT_TRUE(soil.pullup && air.pullup);
  ASSERT_EQ((int)soil.addr, SHT31_SOIL_ADDR); ASSERT_EQ((int)air.addr, SHT31_AIR_ADDR);
  // всеки сензор: един soft reset и ТОЧНО едно измерване
  ASSERT_EQ(soil.sht31.resets, 1); ASSERT_EQ(air.sht31.resets, 1);
  ASSERT_EQ(soil.sht31.measurements, 1); ASSERT_EQ(air.sht31.measurements, 1);
  ASSERT_EQ(soil.nackWrites, 0); ASSERT_EQ(air.nackWrites, 0);
  ASSERT_EQ(soil.nackReads, 0); ASSERT_EQ(air.nackReads, 0);
}

STEST(buses_and_devices_are_released_before_sleep_and_the_api_is_used_correctly) {
  configured();
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ(sim::i2c.liveBuses(), 0);
  ASSERT_EQ(sim::i2c.liveDevices(), 0);
  ASSERT_EQ(sim::i2c.usageErrors, 0);                              // без двойно създаване, без изтриване със заредено устройство
  // втори цикъл след събуждане: портовете са свободни и всичко работи пак
  sim::reboot();
  Cycle c2 = runCycle();
  ASSERT_TRUE(c2.slept);
  ASSERT_EQ((int)sim::airLog.size(), 2);
  ASSERT_EQ(sim::i2c.bus[0].busesCreated, 2);
  ASSERT_EQ(sim::i2c.usageErrors, 0);
}

STEST(a_sensor_at_the_wrong_address_is_no_data_not_a_crash) {
  configured();
  sim::i2c.bus[1].addr = 0x45;                                     // въздушният сензор е на друг адрес от конфигурирания
  sim::i2c.soil().temp = 19.0; sim::i2c.soil().hum = 44.0;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_NEAR(d.v[0], 1900, 2);
  ASSERT_EQ(d.v[2], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[3], SENSOR_RAW_NODATA);
  ASSERT_GE(sim::i2c.bus[1].nackWrites, 1);
}

STEST(a_sensor_that_fails_to_answer_does_not_change_the_other_sensors_reading) {
  configured();
  sim::i2c.soil().temp = 12.34; sim::i2c.soil().hum = 56.78;
  sim::i2c.air().failRead = true;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_NEAR(d.v[0], 1234, 2);
  ASSERT_NEAR(d.v[1], 5678, 2);
  ASSERT_EQ(d.v[2], SENSOR_RAW_NODATA);
  ASSERT_EQ(d.v[3], SENSOR_RAW_NODATA);
}

STEST(values_at_the_edges_of_the_ranges_encode_correctly) {
  struct { double st, sh, at, ah; } cases[] = {
      {-39.5, 1.0, -39.5, 1.0}, {84.0, 99.0, 84.0, 99.0}, {0.0, 0.5, 0.0, 99.9}, {-0.4, 10.0, 0.4, 20.0}};
  for (auto &k : cases) {
    sim::reset();
    configured();
    sim::i2c.soil().temp = k.st; sim::i2c.soil().hum = k.sh;
    sim::i2c.air().temp = k.at; sim::i2c.air().hum = k.ah;
    Decoded d = oneCycleDecoded();
    ASSERT_TRUE(d.ok);
    ASSERT_NEAR(d.v[0], k.st * 100, 3);
    ASSERT_NEAR(d.v[1], k.sh * 100, 3);
    ASSERT_NEAR(d.v[2], k.at * 100, 3);
    ASSERT_NEAR(d.v[3], k.ah * 100, 3);
    for (int i = 0; i < 4; i++) ASSERT_TRUE(d.v[i] != SENSOR_RAW_NODATA);   // реална стойност никога не е маркерът
  }
}

STEST(humidity_extremes_0_and_100_percent_are_valid_values_not_errors) {
  configured();
  sim::i2c.air().hum = 0.0;
  Decoded d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_EQ(d.v[3], 0);                                            // 0 % е валидна стойност (не маркерът -32768)
  sim::reset();
  configured();
  sim::i2c.air().hum = 100.0;
  sim::i2c.soil().hum = 100.0;
  d = oneCycleDecoded();
  ASSERT_TRUE(d.ok);
  ASSERT_EQ(d.v[3], 10000);
  ASSERT_EQ(d.v[1], 10000);
}

STEST(decode_functions_match_the_datasheet_examples) {
  // Sensirion SHT3x: T = -45 + 175 * raw/65535 ; RH = 100 * raw/65535
  uint8_t b[6] = {0x66, 0x66, 0, 0x80, 0x00, 0};
  b[2] = sim::sensirion_crc8(b, 2, 0xFF);
  b[5] = sim::sensirion_crc8(b + 3, 2, 0xFF);
  float t, h;
  ASSERT_TRUE(sht31_decode(b, &t, &h));
  ASSERT_NEAR(t, -45.0 + 175.0 * 0x6666 / 65535.0, 0.001);
  ASSERT_NEAR(h, 100.0 * 0x8000 / 65535.0, 0.001);
  // CRC пример от datasheet-а: 0xBEEF -> 0x92
  const uint8_t beef[2] = {0xBE, 0xEF};
  ASSERT_EQ(sim::sensirion_crc8(beef, 2, 0xFF), 0x92);
  // повреден CRC на температурата или на влажността
  uint8_t bad[6];
  memcpy(bad, b, 6); bad[2] ^= 1; ASSERT_FALSE(sht31_decode(bad, &t, &h));
  memcpy(bad, b, 6); bad[5] ^= 0x80; ASSERT_FALSE(sht31_decode(bad, &t, &h));
  memcpy(bad, b, 6); bad[1] ^= 4; ASSERT_FALSE(sht31_decode(bad, &t, &h));          // променени данни със стария CRC
  // шина без сензор: всички 0xFF и всички 0x00 се отхвърлят (CRC не излиза)
  uint8_t ff[6]; memset(ff, 0xFF, 6); ASSERT_FALSE(sht31_decode(ff, &t, &h));
  uint8_t zz[6]; memset(zz, 0x00, 6); ASSERT_FALSE(sht31_decode(zz, &t, &h));
  // обхват: -45 °C и 130 °C имат вярна CRC, но са извън обхвата на сензора
  for (uint16_t rt : {(uint16_t)0x0000, (uint16_t)0xFFFF}) {
    uint8_t r[6] = {(uint8_t)(rt >> 8), (uint8_t)(rt & 0xff), 0, 0x80, 0x00, 0};
    r[2] = sim::sensirion_crc8(r, 2, 0xFF);
    r[5] = sim::sensirion_crc8(r + 3, 2, 0xFF);
    ASSERT_FALSE(sht31_decode(r, &t, &h));
  }
  // границите на обхвата се приемат
  for (double tc : {-40.0, 125.0, 0.0}) {
    uint16_t rt = (uint16_t)std::lround((tc + 45.0) / 175.0 * 65535.0);
    uint8_t r[6] = {(uint8_t)(rt >> 8), (uint8_t)(rt & 0xff), 0, 0x00, 0x00, 0};
    r[2] = sim::sensirion_crc8(r, 2, 0xFF);
    r[5] = sim::sensirion_crc8(r + 3, 2, 0xFF);
    ASSERT_TRUE(sht31_decode(r, &t, &h));
    ASSERT_NEAR(t, tc, 0.01);
    ASSERT_NEAR(h, 0.0, 0.001);
  }
}

// ============================================================================ радио и конфигурация
STEST(unconfigured_device_does_not_transmit_but_still_sleeps) {
  Cycle c = runCycle();                                            // NVS е празен: namespace "cfg" не съществува
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_TRUE(sim::log_contains("не е конфигурирано"));
}

STEST(missing_key_or_id_does_not_transmit_garbage) {
  sim::nvs.setStr("id", "S001");
  sim::nvs.setU32("freq", 433000000);                              // няма key
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  sim::reboot();
  sim::nvs.clear();
  sim::nvs.setBlob("key", TEST_KEY, 16);                           // няма id
  c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 0);
}

STEST(defaults_are_used_for_missing_or_invalid_radio_fields) {
  sim::nvs.setStr("id", "S001");
  sim::nvs.setBlob("key", TEST_KEY, 16);
  sim::nvs.setU8("sf", 20);                                        // невалиден
  sim::nvs.setU32("bw", 12345);                                    // невалиден
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 1);
  ASSERT_LE(std::labs(sim::airLog[0].freqHz - 433000000L), 61L);   // default 433 MHz
  ASSERT_EQ(sim::airLog[0].sf, 7);
  ASSERT_EQ((int)sim::airLog[0].bwHz, 125000);
}

STEST(dead_radio_is_not_retried_forever_on_battery) {
  configured();
  sim::radio.present = false;
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);                                            // стига до дълбок сън, не виси в цикъл
  ASSERT_LE((long)c.awakeUs, 3000000L);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_TRUE(sim::log_contains("не стартира"));
}

STEST(jammed_channel_is_sensed_for_the_longest_packet_time_then_the_frame_goes_out_anyway) {
  configured();
  sim::radio.busyUntil = 1000000ULL + 100000000ULL;                // запушен през целия цикъл
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 1);                           // предава въпреки това (измерването е по-ценно)
  ASSERT_GE(sim::radio.cadCount, 3);                               // изчаква паузата (PERSIST), не 2 CAD-а (FORCE)
  ASSERT_TRUE(decode(sim::airLog[0].data).ok);
  ASSERT_GE((long)(sim::airLog[0].tEnd - sim::airLog[0].tStart), 1L);
  ASSERT_LE((long)c.awakeUs, 2500000L);                            // jitter + срок <= най-дългия пакет + airtime
}

STEST(a_short_foreign_transmission_is_waited_out_and_never_overlapped) {
  configured();
  uint64_t t0 = sim::now_us();
  sim::radio.busyUntil = t0 + 150000ULL;                           // чужд предавател до 150 ms (< срока при SF9 ~390 ms)
  sim::nvs.setU8("sf", 9);
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 1);
  ASSERT_TRUE(sim::airLog[0].tStart >= t0 + 150000ULL);            // предава СЛЕД края на чуждото предаване, не върху него
}

STEST(free_channel_means_exactly_one_cad) {
  configured();
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ(sim::radio.cadCount, 1);
}

// ============================================================================ nonce и дълбок сън
STEST(counter_continues_across_deep_sleep_cycles_without_writing_flash_each_time) {
  configured();
  std::vector<std::vector<uint8_t>> frames;
  for (int i = 0; i < 5; i++) {
    Cycle c = runCycle();
    ASSERT_TRUE(c.slept);
    ASSERT_EQ((int)sim::airLog.size(), i + 1);
    sim::reboot();                                                 // събуждане от дълбок сън (RTC паметта е жива)
  }
  // nonce-ите са различни: counter (4 B преди tag-а) е 0,1,2,3,4
  for (int i = 0; i < 5; i++) {
    const std::vector<uint8_t> &f = sim::airLog[i].data;
    uint32_t ctr = ((uint32_t)f[6 + 8] << 24) | ((uint32_t)f[6 + 9] << 16) | ((uint32_t)f[6 + 10] << 8) | f[6 + 11];
    ASSERT_EQ(ctr, (uint32_t)i);
    ASSERT_TRUE(decode(f).ok);
  }
  ASSERT_EQ(sim::nvs.writes("ceil"), 1);                           // една запис за 5 цикъла (watermark 100)
  // всички шифротексти са различни дори при еднакви стойности на сензорите
  for (int i = 0; i < 5; i++)
    for (int j = i + 1; j < 5; j++) ASSERT_MEM_NE(sim::airLog[i].data.data() + 6, sim::airLog[j].data.data() + 6, 8);
}

STEST(power_on_reset_never_reuses_a_counter_value_from_before) {
  configured();
  for (int i = 0; i < 3; i++) { runCycle(); sim::reboot(); }      // брояч 0,1,2
  sim::set_reset_reason(ESP_RST_POWERON);                          // батерията е сменена: RTC е изгубена, NVS - не
  sim::clear_restart_flags();
  runCycle();
  const std::vector<uint8_t> &f = sim::airLog.back().data;
  uint32_t ctr = ((uint32_t)f[14] << 24) | ((uint32_t)f[15] << 16) | ((uint32_t)f[16] << 8) | f[17];
  ASSERT_GE(ctr, 3u);                                              // над всичко използвано
  ASSERT_LE(ctr, 3u + CEILING_COMMIT_INTERVAL);
}

STEST(nvs_write_failure_means_no_transmission_instead_of_a_repeated_nonce) {
  configured();
  sim::nvs.failWrites = true;
  Cycle c = runCycle();
  ASSERT_TRUE(c.slept);
  ASSERT_EQ((int)sim::airLog.size(), 0);
  ASSERT_TRUE(sim::log_contains("ceiling"));
}

STEST(chaos_interleavings_do_not_change_the_result) {
  for (uint32_t seed = 1; seed <= 20; seed++) {
    sim::reset();
    configured();
    sim::chaos(seed, 300);
    Cycle c = runCycle();
    ASSERT_TRUE(c.slept);
    ASSERT_EQ((int)sim::airLog.size(), 1);
    ASSERT_TRUE(decode(sim::airLog[0].data).ok);
  }
}

STEST(tx_power_comes_from_nvs_with_a_default_of_17_dbm_and_is_limited_to_the_module_range) {
  struct { bool set; int dbm; int expectDbm; } cases[] = {
      {false, 0, 17}, {true, 10, 10}, {true, -9, -9}, {true, 22, 22}, {true, 30, 22}, {true, -20, -9}, {true, 0, 0}, {true, 14, 14}};
  for (auto &c : cases) {
    int want = refPowerSetting(c.expectDbm);                         // нулира симулатора
    configured();
    if (c.set) nvsTxPower(c.dbm);
    Cycle cy = runCycle();
    ASSERT_TRUE(cy.slept);
    ASSERT_EQ((int)sim::airLog.size(), 1);
    if (!sim::airLog.empty()) ASSERT_EQ(sim::airLog[0].txPowerDbm, want);
  }
  int wantMin = refPowerSetting(-9);
  configured();
  sim::nvs.setU8("txp", 200);                                        // 200 като u8 = -56 със знак -> под границата -> -9
  runCycle();
  ASSERT_EQ((int)sim::airLog.size(), 1);
  if (!sim::airLog.empty()) ASSERT_EQ(sim::airLog[0].txPowerDbm, wantMin);
}

int main() { return runAllTests(); }
