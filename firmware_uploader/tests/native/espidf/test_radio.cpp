// Тестове на радио слоя (agro_radio + РЕАЛНИЯТ RadioLib с неговия EspHal) срещу модела на SX1262 върху SPI командите.
// Проверява се цялата верига: прошивката -> agro_radio -> RadioLib -> EspHal -> SPI/GPIO заместители -> модел на чипа.
#include "test_support.h"

#include <algorithm>
#include <random>

#include "agro_hal.h"
#include "driver/gpio.h"
#include "agro_radio.h"
#include "board_pins.h"
#include "protocol.h"
#include "radio_timing.h"

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);   // RAM на RadioLib се нулира при рестарт

static SX1262 &rl = radio;     // обектът на RadioLib (глобалният ::radio; sim::radio е моделът на чипа)

// ---------------------------------------------------------------- помощници
static RadioParams params(long freq, uint8_t sf, uint32_t bw, int8_t dbm) {
  RadioParams p;
  p.freqHz = freq; p.sf = sf; p.bwHz = bw; p.txPowerDbm = dbm;
  return p;
}

static bool g_ok;
static bool doSetup(long freq = 433000000, uint8_t sf = 7, uint32_t bw = 125000, int8_t dbm = 17, int tries = 3, uint32_t ms = 30000) {
  g_ok = false;
  runAsTask([=] { RadioParams p = params(freq, sf, bw, dbm); g_ok = radio_setup(&p, tries); }, ms);
  return g_ok;
}

// Нито едно нарушение на правилата на чипа/шината за целия сценарий
static void expectClean() {
  ASSERT_EQ(sim::radio.protocolErrors, 0);
  ASSERT_EQ(sim::radio.modeViolations, 0);
  ASSERT_EQ(sim::radio.csViolations, 0);
  ASSERT_EQ(sim::radio.busyViolations, 0);
  ASSERT_EQ(sim::radio.sleepViolations, 0);
}

static std::vector<uint8_t> frame(size_t n, uint8_t seed = 1) {
  std::vector<uint8_t> f(n);
  for (size_t i = 0; i < n; i++) f[i] = (uint8_t)(seed * 37 + i * 11 + (i >> 3));
  return f;
}

// ============================================================================ константите на RadioLib = datasheet
STEST(radiolib_opcodes_irq_bits_and_registers_match_the_datasheet) {
  struct { const char *name; unsigned datasheet; unsigned radiolib; } t[] = {
      {"NOP", 0x00, RADIOLIB_SX126X_CMD_NOP}, {"SetSleep", 0x84, RADIOLIB_SX126X_CMD_SET_SLEEP},
      {"SetStandby", 0x80, RADIOLIB_SX126X_CMD_SET_STANDBY}, {"SetFs", 0xC1, RADIOLIB_SX126X_CMD_SET_FS},
      {"SetTx", 0x83, RADIOLIB_SX126X_CMD_SET_TX}, {"SetRx", 0x82, RADIOLIB_SX126X_CMD_SET_RX},
      {"SetCad", 0xC5, RADIOLIB_SX126X_CMD_SET_CAD}, {"SetRegulatorMode", 0x96, RADIOLIB_SX126X_CMD_SET_REGULATOR_MODE},
      {"Calibrate", 0x89, RADIOLIB_SX126X_CMD_CALIBRATE}, {"CalibrateImage", 0x98, RADIOLIB_SX126X_CMD_CALIBRATE_IMAGE},
      {"SetPaConfig", 0x95, RADIOLIB_SX126X_CMD_SET_PA_CONFIG}, {"SetRxTxFallbackMode", 0x93, RADIOLIB_SX126X_CMD_SET_RX_TX_FALLBACK_MODE},
      {"WriteRegister", 0x0D, RADIOLIB_SX126X_CMD_WRITE_REGISTER}, {"ReadRegister", 0x1D, RADIOLIB_SX126X_CMD_READ_REGISTER},
      {"WriteBuffer", 0x0E, RADIOLIB_SX126X_CMD_WRITE_BUFFER}, {"ReadBuffer", 0x1E, RADIOLIB_SX126X_CMD_READ_BUFFER},
      {"SetDioIrqParams", 0x08, RADIOLIB_SX126X_CMD_SET_DIO_IRQ_PARAMS}, {"GetIrqStatus", 0x12, RADIOLIB_SX126X_CMD_GET_IRQ_STATUS},
      {"ClearIrqStatus", 0x02, RADIOLIB_SX126X_CMD_CLEAR_IRQ_STATUS}, {"SetDio2AsRfSwitchCtrl", 0x9D, RADIOLIB_SX126X_CMD_SET_DIO2_AS_RF_SWITCH_CTRL},
      {"SetDio3AsTcxoCtrl", 0x97, RADIOLIB_SX126X_CMD_SET_DIO3_AS_TCXO_CTRL}, {"SetRfFrequency", 0x86, RADIOLIB_SX126X_CMD_SET_RF_FREQUENCY},
      {"SetPacketType", 0x8A, RADIOLIB_SX126X_CMD_SET_PACKET_TYPE}, {"GetPacketType", 0x11, RADIOLIB_SX126X_CMD_GET_PACKET_TYPE},
      {"SetTxParams", 0x8E, RADIOLIB_SX126X_CMD_SET_TX_PARAMS}, {"SetModulationParams", 0x8B, RADIOLIB_SX126X_CMD_SET_MODULATION_PARAMS},
      {"SetPacketParams", 0x8C, RADIOLIB_SX126X_CMD_SET_PACKET_PARAMS}, {"SetCadParams", 0x88, RADIOLIB_SX126X_CMD_SET_CAD_PARAMS},
      {"SetBufferBaseAddress", 0x8F, RADIOLIB_SX126X_CMD_SET_BUFFER_BASE_ADDRESS}, {"GetStatus", 0xC0, RADIOLIB_SX126X_CMD_GET_STATUS},
      {"GetRssiInst", 0x15, RADIOLIB_SX126X_CMD_GET_RSSI_INST}, {"GetRxBufferStatus", 0x13, RADIOLIB_SX126X_CMD_GET_RX_BUFFER_STATUS},
      {"GetPacketStatus", 0x14, RADIOLIB_SX126X_CMD_GET_PACKET_STATUS}, {"GetDeviceErrors", 0x17, RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS},
      {"ClearDeviceErrors", 0x07, RADIOLIB_SX126X_CMD_CLEAR_DEVICE_ERRORS},
      {"IRQ TxDone", 0x0001, RADIOLIB_SX126X_IRQ_TX_DONE}, {"IRQ RxDone", 0x0002, RADIOLIB_SX126X_IRQ_RX_DONE},
      {"IRQ PreambleDetected", 0x0004, RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED}, {"IRQ SyncWordValid", 0x0008, RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID},
      {"IRQ HeaderValid", 0x0010, RADIOLIB_SX126X_IRQ_HEADER_VALID}, {"IRQ HeaderErr", 0x0020, RADIOLIB_SX126X_IRQ_HEADER_ERR},
      {"IRQ CrcErr", 0x0040, RADIOLIB_SX126X_IRQ_CRC_ERR}, {"IRQ CadDone", 0x0080, RADIOLIB_SX126X_IRQ_CAD_DONE},
      {"IRQ CadDetected", 0x0100, RADIOLIB_SX126X_IRQ_CAD_DETECTED}, {"IRQ Timeout", 0x0200, RADIOLIB_SX126X_IRQ_TIMEOUT},
      {"REG version string", 0x0320, RADIOLIB_SX126X_REG_VERSION_STRING}, {"REG LoRa sync word MSB", 0x0740, RADIOLIB_SX126X_REG_LORA_SYNC_WORD_MSB},
      {"REG OCP", 0x08E7, RADIOLIB_SX126X_REG_OCP_CONFIGURATION}, {"REG IQ config", 0x0736, RADIOLIB_SX126X_REG_IQ_CONFIG},
      {"CAD 1 symbol", 0x00, RADIOLIB_SX126X_CAD_ON_1_SYMB}, {"CAD 2 symbols", 0x01, RADIOLIB_SX126X_CAD_ON_2_SYMB},
      {"CAD 4 symbols", 0x02, RADIOLIB_SX126X_CAD_ON_4_SYMB}, {"CAD 8 symbols", 0x03, RADIOLIB_SX126X_CAD_ON_8_SYMB},
      {"CAD 16 symbols", 0x04, RADIOLIB_SX126X_CAD_ON_16_SYMB},
  };
  for (auto &c : t) {
    if (c.datasheet != c.radiolib) printf("    %s: datasheet 0x%X, RadioLib 0x%X\n", c.name, c.datasheet, c.radiolib);
    ASSERT_EQ(c.radiolib, c.datasheet);
  }
}

// ============================================================================ старт
STEST(setup_configures_the_chip_for_the_network_and_leaves_it_in_standby) {
  ASSERT_TRUE(doSetup());
  const sim::Sx126xModel &m = sim::radio;
  ASSERT_EQ(m.packetType(), 1);                                    // LoRa
  ASSERT_EQ(m.mode(), (int)sim::Sx126xModel::STBY_RC);             // радиото чака команда (слушане/предаване)
  ASSERT_EQ(m.sf(), 7);
  ASSERT_EQ((int)m.bwHz(), 125000);
  ASSERT_EQ(m.crDenom(), (int)LORA_CR_DENOM);                      // 4/5
  ASSERT_EQ(m.preamble(), (int)LORA_PREAMBLE_LEN);
  ASSERT_TRUE(m.crcOn());
  ASSERT_TRUE(m.explicitHeader());
  ASSERT_EQ((int)m.syncWord(), 0x1424);                            // sync word 0x12 (частна мрежа) както при SX127x/LoRa.h
  ASSERT_EQ((int)(m.freqHz() - 433000000L), 0);
  ASSERT_EQ(m.ocpMilliamps(), 140);                                // RadioLib оставя 60 mA - малко за +17..+22 dBm
  ASSERT_TRUE(m.dio2RfSwitch());
  ASSERT_EQ(m.regulatorMode(), 1);                                 // DC-DC
  ASSERT_TRUE(m.tcxoEnabled());                                    // модулът по подразбиране е с TCXO (board_pins.h)
  ASSERT_EQ(m.tcxoVoltageCode(), 0);                               // 1.6 V
  ASSERT_EQ(m.resets, 1);
  expectClean();
}

STEST(every_sf_bw_is_configured_and_ldro_airtime_and_timing_agree_with_radiolib) {
  const uint32_t bws[3] = {62500, 125000, 250000};
  for (int sf = 7; sf <= 12; sf++) {
    for (uint32_t bw : bws) {
      sim::reset();
      ASSERT_TRUE(doSetup(433000000, (uint8_t)sf, bw));
      const sim::Sx126xModel &m = sim::radio;
      ASSERT_EQ(m.sf(), sf);
      ASSERT_EQ((int)m.bwHz(), (int)bw);
      ASSERT_EQ(m.ldro(), radioLdroNeeded());                      // LDRO на RadioLib = нашето правило (Ts > 16 ms)
      static double toaUs[8];
      static const int lens[8] = {14, 20, 22, 26, 30, 64, 128, 255};
      runAsTask([] { for (int i = 0; i < 8; i++) toaUs[i] = (double)rl.getTimeOnAir((size_t)lens[i]); }, 200);   // чете състоянието на чипа по SPI
      for (int li = 0; li < 8; li++) {
        int len = lens[li];
        double rlUs = toaUs[li];
        ASSERT_NEAR(rlUs, m.airtimeUs(len), 2.0);                  // RadioLib = независимата формула на модела (Semtech AN1200.13)
        if (len <= 64) {
          uint32_t ours = radioToaMs((uint8_t)len);                // нашата формула (времената на протокола)
          ASSERT_GE((long)ours, (long)std::ceil(rlUs / 1000.0) - 1);
          ASSERT_LE((long)ours, (long)std::ceil(rlUs / 1000.0) + 1);
        }
      }
      expectClean();
    }
  }
}

STEST(frequency_is_set_within_a_few_tens_of_hz_across_the_433_band) {
  long worst = 0;
  for (long f : {433050000L, 433175000L, 433500000L, 433920000L, 434000000L, 434790000L, 410000000L, 470000000L}) {
    sim::reset();
    ASSERT_TRUE(doSetup(f));
    long err = std::labs(sim::radio.freqHz() - f);
    worst = std::max(worst, err);
    ASSERT_LE(err, 100L);
  }
  printf("    най-голямо отклонение на честотата: %ld Hz\n", worst);
}

STEST(tx_power_is_clamped_to_the_module_maximum_and_every_valid_value_is_accepted) {
  ASSERT_EQ((int)radio_clamp_tx_power(100), (int)LORA_TX_POWER_MAX_DBM);
  ASSERT_EQ((int)radio_clamp_tx_power(23), (int)LORA_TX_POWER_MAX_DBM);
  ASSERT_EQ((int)radio_clamp_tx_power(-100), (int)LORA_TX_POWER_MIN_DBM);
  for (int d = -128; d <= 127; d++) {
    int c = radio_clamp_tx_power(d);
    ASSERT_TRUE(c >= LORA_TX_POWER_MIN_DBM && c <= LORA_TX_POWER_MAX_DBM);
    if (d >= LORA_TX_POWER_MIN_DBM && d <= LORA_TX_POWER_MAX_DBM) ASSERT_EQ(c, d);   // в границите не се пипа
  }
  // чипът получава ИСТИНСКИ настройка за всяка стойност в границите; извън тях - същата като на границата
  int distinct = 0, prev = 999;
  int atMax = 0, atMin = 0;
  for (int d = LORA_TX_POWER_MIN_DBM; d <= LORA_TX_POWER_MAX_DBM; d++) {
    sim::reset();
    ASSERT_TRUE(doSetup(433000000, 7, 125000, (int8_t)d));
    int pw = sim::radio.txPowerDbm();
    if (pw != prev) distinct++;
    prev = pw;
    if (d == LORA_TX_POWER_MAX_DBM) atMax = pw;
    if (d == LORA_TX_POWER_MIN_DBM) atMin = pw;
    ASSERT_TRUE(pw >= -9 && pw <= 22);
    expectClean();
  }
  ASSERT_TRUE(distinct > 10);
  sim::reset();
  ASSERT_TRUE(doSetup(433000000, 7, 125000, 100));
  ASSERT_EQ(sim::radio.txPowerDbm(), atMax);                       // 100 dBm -> както максимума
  ASSERT_TRUE(sim::log_contains("извън границите на модула"));
  sim::reset();
  ASSERT_TRUE(doSetup(433000000, 7, 125000, -100));
  ASSERT_EQ(sim::radio.txPowerDbm(), atMin);
}

STEST(a_module_with_a_crystal_instead_of_a_tcxo_recovers_by_itself) {
  sim::radio.xtalModule = true;                                    // board_pins.h казва TCXO 1.6 V, а модулът е с кварц
  ASSERT_TRUE(doSetup());                                          // RadioLib хваща грешката на осцилатора и преминава на XTAL
  ASSERT_FALSE(sim::radio.tcxoEnabled());
  ASSERT_EQ(sim::radio.packetType(), 1);
  ASSERT_EQ(sim::radio.sf(), 7);
  // и работи: предаване и приемане
  ASSERT_TRUE(sim::radio.listening() == false);
  static bool txOk;
  runAsTask([] { txOk = radio_transmit((const uint8_t *)"0123456789", 10); }, 2000);
  ASSERT_TRUE(txOk);
  ASSERT_EQ((int)sim::radio.tx.size(), 1);
}

// ============================================================================ мъртъв / закъснял / заключен чип
static long g_setupMs;
static bool timedSetup(int tries) {
  runAsTask([=] {
    uint64_t t0 = sim::now_us();
    RadioParams p = params(433000000, 7, 125000, 17);
    g_ok = radio_setup(&p, tries);
    g_setupMs = (long)((sim::now_us() - t0) / 1000);
  }, 60000);
  return g_ok;
}

STEST(dead_chip_is_reported_quickly_and_a_limited_number_of_times) {
  sim::radio.present = false;
  ASSERT_FALSE(timedSetup(5));
  long ms = g_setupMs;
  printf("    5 опита към мъртъв чип: %ld ms\n", ms);
  ASSERT_LE(ms, 3000L);                                            // батерия: не се чака 10 x 1 s на опит
  ASSERT_TRUE(sim::log_contains("радиото не стартира след 5 опита"));
}

STEST(a_chip_that_appears_late_is_picked_up_when_retrying_forever) {
  sim::radio.present = false;
  static bool ok;
  ok = false;
  sim::boot([] { RadioParams p = params(433000000, 7, 125000, 17); ok = radio_setup(&p, 0); });
  sim::run_for_ms(4000);
  ASSERT_FALSE(ok);
  sim::radio.present = true;
  sim::run_for_ms(3000);
  ASSERT_TRUE(ok);
  ASSERT_EQ(sim::radio.packetType(), 1);
  ASSERT_EQ(sim::log_count("радиото не стартира (RadioLib код"), 1);   // диагностиката не залива лога (раз в 10 s)
  expectClean();
}

STEST(commands_failing_during_setup_are_retried_and_the_chip_ends_up_configured) {
  sim::radio.failCommands = 12;                                    // първите команди се провалят (статус "failure to execute")
  ASSERT_TRUE(doSetup(433000000, 9, 250000, 17, 10));
  ASSERT_EQ(sim::radio.sf(), 9);
  ASSERT_EQ((int)sim::radio.bwHz(), 250000);
}

STEST(a_chip_that_holds_busy_high_forever_never_hangs_setup) {
  sim::radio.busyStuck = true;
  ASSERT_FALSE(timedSetup(2));
  long ms = g_setupMs;
  printf("    2 опита при заключен BUSY: %ld ms\n", ms);
  ASSERT_LE(ms, 6000L);                                            // таймаут на BUSY 100 ms, не 1 s
}

// ============================================================================ приемане
static std::vector<uint8_t> g_rxBuf;
static int g_rxN;
static float g_rxRssi, g_rxSnr;
static void readOne(size_t cap = 64) {
  std::vector<uint8_t> b(cap, 0xEE);
  g_rxN = radio_read_packet(b.data(), cap);
  g_rxRssi = rl.getRSSI();
  g_rxSnr = rl.getSNR();
  g_rxBuf.assign(b.begin(), b.end());
}

STEST(a_received_packet_is_read_exactly_and_the_chip_keeps_listening_without_restarting_rx) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  ASSERT_TRUE(sim::radio.listening());
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
  std::vector<uint8_t> f = frame(22);
  size_t rxCmds = std::count(sim::radio.ops.begin(), sim::radio.ops.end(), (uint8_t)0x82);
  ASSERT_TRUE(sim::radio.deliver(f, 433000000, -70, 6.25f));
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 1);                         // RxDone -> DIO1 (ниво)
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, 22);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(g_rxBuf.begin(), g_rxBuf.begin() + 22)), toHex(f));
  ASSERT_NEAR(g_rxRssi, -70.0, 0.001);
  ASSERT_NEAR(g_rxSnr, 6.25, 0.001);
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);                         // изчистено -> DIO1 пада
  ASSERT_EQ(sim::radio.irqStatus() & 0x0002, 0);
  ASSERT_TRUE(sim::radio.listening());                             // продължава да слуша
  ASSERT_EQ((long)std::count(sim::radio.ops.begin(), sim::radio.ops.end(), (uint8_t)0x82), (long)rxCmds);   // без нов SetRx
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, RADIO_RX_NONE);                                 // няма втори пакет
  expectClean();
}

STEST(rssi_and_snr_cover_the_realistic_range_with_the_chips_resolution) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  for (int rssi : {-20, -60, -101, -120, -127}) {            // най-слабият, който чипът може да докладва, е -127.5 dBm
    for (float snr : {-20.0f, -7.5f, 0.0f, 6.25f, 12.0f}) {
      ASSERT_TRUE(sim::radio.deliver(frame(14), 433000000, rssi, snr));
      runAsTask([] { readOne(); }, 100);
      ASSERT_EQ(g_rxN, 14);
      ASSERT_NEAR(g_rxRssi, rssi, 0.51);
      ASSERT_NEAR(g_rxSnr, snr, 0.26);
    }
  }
  expectClean();
}

STEST(a_packet_with_a_bad_crc_is_dropped_and_reception_continues) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  ASSERT_TRUE(sim::radio.deliver(frame(22, 5), 433000000, -70, 6.0f, false));      // CRC грешка
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 1);
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, RADIO_RX_CRC_ERROR);
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);                         // и този път прекъсването е изчистено
  ASSERT_TRUE(sim::radio.listening());
  ASSERT_TRUE(sim::radio.deliver(frame(22, 6), 433000000, -70, 6.0f));
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, 22);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(g_rxBuf.begin(), g_rxBuf.begin() + 22)), toHex(frame(22, 6)));
  expectClean();
}

STEST(oversized_empty_and_maximum_packets_are_handled_without_leaving_the_interrupt_stuck) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  // по-дълъг от буфера: връща истинската дължина, чете първите cap байта
  std::vector<uint8_t> big = frame(100, 3);
  ASSERT_TRUE(sim::radio.deliver(big, 433000000));
  runAsTask([] { readOne(32); }, 100);
  ASSERT_EQ(g_rxN, 100);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(g_rxBuf.begin(), g_rxBuf.begin() + 32)), toHex(std::vector<uint8_t>(big.begin(), big.begin() + 32)));
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
  // най-дългият възможен пакет
  std::vector<uint8_t> mx = frame(255, 9);
  ASSERT_TRUE(sim::radio.deliver(mx, 433000000));
  runAsTask([] { readOne(255); }, 100);
  ASSERT_EQ(g_rxN, 255);
  ASSERT_STREQ(toHex(g_rxBuf), toHex(mx));
  // празен пакет: няма какво да се чете, но прекъсването трябва да падне
  ASSERT_TRUE(sim::radio.deliver(std::vector<uint8_t>(), 433000000));
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 1);
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, RADIO_RX_NONE);
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
  ASSERT_TRUE(sim::radio.listening());
  expectClean();
}

STEST(flags_without_a_received_packet_do_not_raise_the_line_or_confuse_the_next_read) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  sim::radio.injectIrq(0x0020);                                    // HeaderErr (без RxDone) - не е в маската на DIO1
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, RADIO_RX_NONE);
  ASSERT_TRUE(sim::radio.deliver(frame(20, 2), 433000000, -80, 4.0f));          // добър пакет след шума
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, 20);                                            // не се отчита като CRC грешка
  expectClean();
}

STEST(packets_on_another_frequency_are_not_heard_and_a_second_unread_packet_overruns_like_the_chip) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  ASSERT_FALSE(sim::radio.deliver(frame(20), 434000000));          // друга честота
  ASSERT_EQ(sim::radio.missedRx, 1);
  ASSERT_TRUE(sim::radio.deliver(frame(20, 1), 433000000));
  ASSERT_TRUE(sim::radio.deliver(frame(20, 2), 433000000));        // първият още не е прочетен
  ASSERT_EQ(sim::radio.rxOverrun, 1);                              // записва се върху него (поведение на чипа)
  runAsTask([] { readOne(); }, 100);
  ASSERT_EQ(g_rxN, 20);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(g_rxBuf.begin(), g_rxBuf.begin() + 20)), toHex(frame(20, 2)));
}

// ============================================================================ прекъсване на DIO1 (ниво) и задача
static std::vector<std::vector<uint8_t>> g_frames;
static int g_crc;
static void rxTask() {
  radio_listen();
  hal_irq_attach(LORA_DIO1, xTaskGetCurrentTaskHandle());
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t b[64];
    for (int k = 0; k < 16; k++) {
      int n = radio_read_packet(b, sizeof(b));
      if (n == RADIO_RX_NONE) break;
      if (n == RADIO_RX_CRC_ERROR) { g_crc++; continue; }
      g_frames.push_back(std::vector<uint8_t>(b, b + n));
    }
    hal_irq_rearm(LORA_DIO1);
  }
}

STEST(interrupt_driven_reception_loses_nothing_duplicates_nothing_and_never_storms) {
  ASSERT_TRUE(doSetup());
  g_frames.clear();
  g_crc = 0;
  sim::boot(rxTask);
  sim::run_for_ms(200);
  std::mt19937 rng(7);
  uint32_t t = 100;
  const int N = 300;
  int sentGood = 0, sentBad = 0;
  for (int i = 0; i < N; i++) {
    t += 25 + rng() % 400;
    bool bad = (i % 9) == 4;
    std::vector<uint8_t> f = frame(14 + (i % 3) * 8, (uint8_t)(i + 1));
    f[0] = (uint8_t)(i & 0xff); f[1] = (uint8_t)(i >> 8);
    (bad ? sentBad : sentGood)++;
    sim::at_after_ms(t, [f, bad] { sim::radio.deliver(f, 433000000, -75, 5.0f, !bad); }, true);
  }
  sim::run_for_ms(t + 1000);
  ASSERT_EQ((int)g_frames.size(), sentGood);
  ASSERT_EQ(g_crc, sentBad);
  ASSERT_EQ(sim::radio.rxOverrun, 0);
  // всеки добър пакет е точно веднъж и по ред
  int expect = 0;
  for (auto &fr : g_frames) {
    while ((expect % 9) == 4) expect++;
    int idx = fr[0] | (fr[1] << 8);
    if (idx != expect) { ASSERT_EQ(idx, expect); break; }
    expect++;
  }
  ASSERT_LE(sim::gpio.pins[LORA_DIO1].isrCalls, N + 5);              // по едно прекъсване на пакет, без буря
  ASSERT_TRUE(sim::radio.listening());
  expectClean();
}

// ============================================================================ предаване
static bool g_txOk;
static uint64_t g_txT0, g_txT1;

STEST(transmit_sends_the_exact_bytes_with_the_configured_modem_and_returns_to_standby) {
  ASSERT_TRUE(doSetup(433500000, 9, 250000, 14));
  std::vector<uint8_t> f = frame(26, 4);
  runAsTask([f] { g_txT0 = sim::now_us(); g_txOk = radio_transmit(f.data(), f.size()); g_txT1 = sim::now_us(); }, 5000);
  ASSERT_TRUE(g_txOk);
  ASSERT_EQ((int)sim::radio.tx.size(), 1);
  const sim::RadioFrame &r = sim::radio.tx[0];
  ASSERT_STREQ(toHex(r.data), toHex(f));
  ASSERT_LE(std::labs(r.freqHz - 433500000L), 100L);
  ASSERT_EQ(r.sf, 9);
  ASSERT_EQ((int)r.bwHz, 250000);
  ASSERT_EQ(r.crDenom, 5);
  ASSERT_TRUE(r.crc);
  ASSERT_EQ(r.preamble, 8);
  ASSERT_EQ((int)r.syncWord, 0x1424);
  long ms = (long)((g_txT1 - g_txT0) / 1000);
  long toa = (long)std::ceil(sim::radio.airtimeUs(26) / 1000.0);
  ASSERT_GE(ms, toa);                                              // не се връща преди края на предаването
  ASSERT_LE(ms, toa + 12);                                         // и не чака излишно
  ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::STBY_RC);
  ASSERT_EQ((int)sim::radio.irqStatus(), 0);                       // флаговете са изчистени
  ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
  expectClean();
}

STEST(transmit_handles_the_length_limits_and_every_length_up_to_the_maximum) {
  ASSERT_TRUE(doSetup());
  size_t spi0 = (size_t)sim::radio.spiTransfers;
  runAsTask([] { g_txOk = radio_transmit(nullptr, 5); }, 100);
  ASSERT_FALSE(g_txOk);
  runAsTask([] { uint8_t b[1] = {0}; g_txOk = radio_transmit(b, 0); }, 100);
  ASSERT_FALSE(g_txOk);
  runAsTask([] { uint8_t b[300] = {0}; g_txOk = radio_transmit(b, 256); }, 100);
  ASSERT_FALSE(g_txOk);
  ASSERT_EQ((long)sim::radio.spiTransfers, (long)spi0);            // отказът е без обмен с чипа
  int sent = 0;
  for (size_t n : {1u, 2u, 14u, 22u, 26u, 64u, 127u, 128u, 200u, 255u}) {
    std::vector<uint8_t> f = frame(n, (uint8_t)n);
    runAsTask([f] { g_txOk = radio_transmit(f.data(), f.size()); }, 8000);
    ASSERT_TRUE(g_txOk);
    sent++;
    ASSERT_EQ((int)sim::radio.tx.size(), sent);
    ASSERT_STREQ(toHex(sim::radio.tx.back().data), toHex(f));
  }
  expectClean();
}

STEST(transmit_is_bounded_when_the_chip_never_finishes_and_the_radio_recovers) {
  ASSERT_TRUE(doSetup());
  sim::radio.stuckTx = true;
  std::vector<uint8_t> f = frame(30);
  runAsTask([f] { g_txT0 = sim::now_us(); g_txOk = radio_transmit(f.data(), f.size()); g_txT1 = sim::now_us(); }, 10000);
  ASSERT_FALSE(g_txOk);
  long ms = (long)((g_txT1 - g_txT0) / 1000);
  long toa = (long)std::ceil(sim::radio.airtimeUs(30) / 1000.0);
  ASSERT_LE(ms, toa + toa / 4 + 150);                              // срокът е от времето във въздуха, не безкраен
  ASSERT_EQ(sim::radio.abortedTx, 1);                              // зациклилото предаване е прекъснато
  ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::STBY_RC);
  ASSERT_TRUE(sim::log_contains("предаването не приключи"));
  sim::radio.stuckTx = false;
  runAsTask([f] { g_txOk = radio_transmit(f.data(), f.size()); }, 5000);
  ASSERT_TRUE(g_txOk);                                             // и следващото предаване работи
}

STEST(transmit_leaves_the_dio1_interrupt_off_until_listen_and_listen_turns_it_back_on) {
  ASSERT_TRUE(doSetup());
  static bool onBefore, onAfterTx, onAfterListen, onAfterCad, onAfterListen2, txResult;
  static int isrBefore, isrAfterTx, isrAfterCad;
  sim::boot([] {
    radio_listen();
    hal_irq_attach(LORA_DIO1, xTaskGetCurrentTaskHandle());
    onBefore = sim::gpio.pins[LORA_DIO1].intrEnabled;
    isrBefore = sim::gpio.pins[LORA_DIO1].isrCalls;
    uint8_t pkt[26] = {1, 2, 3};
    txResult = radio_transmit(pkt, sizeof pkt);
    vTaskDelay(pdMS_TO_TICKS(50));                                 // TX_DONE е минало; прекъсване не трябва да е дошло
    onAfterTx = sim::gpio.pins[LORA_DIO1].intrEnabled;
    isrAfterTx = sim::gpio.pins[LORA_DIO1].isrCalls;
    radio_listen();
    onAfterListen = sim::gpio.pins[LORA_DIO1].intrEnabled;
    channelActive();                                               // CAD вдига DIO1 (CadDone): и той не бива да буди rx задачата
    vTaskDelay(pdMS_TO_TICKS(50));
    onAfterCad = sim::gpio.pins[LORA_DIO1].intrEnabled;
    isrAfterCad = sim::gpio.pins[LORA_DIO1].isrCalls;
    radio_listen();
    onAfterListen2 = sim::gpio.pins[LORA_DIO1].intrEnabled;
    vTaskDelay(portMAX_DELAY);
  });
  sim::run_for_ms(2500);
  ASSERT_TRUE(txResult);
  ASSERT_TRUE(onBefore);
  ASSERT_FALSE(onAfterTx);                                         // TX_DONE не буди rx задачата и не се губи процесорно време
  ASSERT_EQ(isrAfterTx, isrBefore);
  ASSERT_TRUE(onAfterListen);                                      // radio_listen() връща прекъсването
  ASSERT_FALSE(onAfterCad);                                        // CAD също го държи изключено
  ASSERT_EQ(isrAfterCad, isrBefore);
  ASSERT_TRUE(onAfterListen2);
  ASSERT_TRUE(sim::radio.listening());
  expectClean();
}

// ============================================================================ CAD
static bool g_cad;

STEST(cad_reports_a_busy_and_a_free_channel_leaves_standby_and_stays_inside_its_time_bound) {
  for (int sf : {7, 9, 12}) {
    sim::reset();
    ASSERT_TRUE(doSetup(433000000, (uint8_t)sf, 125000));
    runAsTask([] { g_txT0 = sim::now_us(); g_cad = channelActive(); g_txT1 = sim::now_us(); }, 2000);
    ASSERT_FALSE(g_cad);                                           // свободен
    long freeMs = (long)((g_txT1 - g_txT0) / 1000);
    ASSERT_LE(freeMs, (long)radioCadTimeoutMs());
    ASSERT_GE(freeMs, (long)(sim::radio.symbolUs() * RADIO_CAD_SYMBOLS / 1000.0));   // не по-бързо от самото сканиране
    ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::STBY_RC);  // CAD оставя радиото в standby
    ASSERT_EQ((int)sim::radio.irqStatus(), 0);
    ASSERT_EQ(sim::radio.cadCount, 1);
    sim::radio.busyUntil = sim::now_us() + 1000000ULL;             // чужд предавател
    runAsTask([] { g_cad = channelActive(); }, 2000);
    ASSERT_TRUE(g_cad);                                            // зает
    ASSERT_EQ(sim::radio.cadCount, 2);
    ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::STBY_RC);
    expectClean();
  }
}

STEST(cad_uses_the_configured_symbol_count_and_default_detection_thresholds) {
  ASSERT_TRUE(doSetup(433000000, 9, 125000));
  runAsTask([] { g_cad = channelActive(); }, 2000);
  ASSERT_EQ(sim::radio.cadSymbols(), (int)RADIOLIB_SX126X_CAD_ON_2_SYMB);   // RADIO_CAD_SYMBOLS = 2
  ASSERT_EQ((int)RADIO_CAD_SYMBOLS, 2);
}

STEST(cad_that_never_finishes_is_aborted_and_does_not_block_the_radio) {
  ASSERT_TRUE(doSetup(433000000, 7, 125000));
  sim::radio.stuckCad = true;
  runAsTask([] { g_txT0 = sim::now_us(); g_cad = channelActive(); g_txT1 = sim::now_us(); }, 3000);
  ASSERT_FALSE(g_cad);                                             // не се твърди "зает" без доказателство
  long ms = (long)((g_txT1 - g_txT0) / 1000);
  ASSERT_LE(ms, (long)radioCadTimeoutMs() + 15);
  ASSERT_EQ(sim::radio.cadAborted, 1);
  ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::STBY_RC);
  ASSERT_TRUE(sim::log_contains("CAD не приключи"));
  sim::radio.stuckCad = false;
  runAsTask([] { g_cad = channelActive(); }, 2000);
  ASSERT_FALSE(g_cad);                                             // следващият работи
}

STEST(a_failing_cad_command_is_treated_as_a_free_channel_not_a_hang) {
  ASSERT_TRUE(doSetup());
  sim::radio.failCommands = 30;
  runAsTask([] { g_cad = channelActive(); }, 3000);
  ASSERT_FALSE(g_cad);
  ASSERT_EQ(sim::radio.mode() == (int)sim::Sx126xModel::STBY_RC || sim::radio.mode() == (int)sim::Sx126xModel::SLEEP, true);
  sim::radio.failCommands = 0;
  runAsTask([] { g_cad = channelActive(); }, 2000);
  ASSERT_FALSE(g_cad);
}

// ============================================================================ смяна на честота (repeater), сън (sensor)
STEST(frequency_switching_between_rx_and_tx_bands_works_repeatedly_without_recalibrating_every_time) {
  ASSERT_TRUE(doSetup(434000000));
  size_t calib0 = std::count(sim::radio.ops.begin(), sim::radio.ops.end(), (uint8_t)0x98);
  for (int i = 0; i < 100; i++) {
    long f = (i & 1) ? 434000000 : 433000000;
    static long target;
    target = f;
    runAsTask([] { g_ok = radio_set_frequency(target); radio_listen(); }, 200);
    ASSERT_TRUE(g_ok);
    ASSERT_LE(std::labs(sim::radio.freqHz() - f), 100L);
    ASSERT_TRUE(sim::radio.listening());
  }
  size_t calib1 = std::count(sim::radio.ops.begin(), sim::radio.ops.end(), (uint8_t)0x98);
  ASSERT_EQ((long)calib1, (long)calib0);                           // 1 MHz разлика: без калибриране на изображението всеки път
  expectClean();
}

STEST(sleep_wake_cycle_like_the_sensor_works_cold_and_warm_and_the_chip_is_asleep_in_between) {
  for (int cycle = 0; cycle < 3; cycle++) {
    ASSERT_TRUE(doSetup(433000000, 7, 125000, 17));
    static std::vector<uint8_t> f;
    f = frame(22, (uint8_t)cycle);
    runAsTask([] { g_txOk = radio_transmit(f.data(), f.size()); rl.sleep(false); }, 3000);
    ASSERT_TRUE(g_txOk);
    ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::SLEEP);    // заспал (най-малък ток)
    ASSERT_EQ(sim::radio.busyLevel(), 1);                          // BUSY е висок в сън
    ASSERT_EQ(sim::gpio.read(LORA_DIO1), 0);
    ASSERT_EQ((int)sim::airLog.size(), cycle + 1);
    sim::reboot();                                                 // следващото събуждане: RAM на MCU се нулира (setup рестартира чипа през RST и го вади от съня)
  }
  ASSERT_EQ((int)sim::airLog.size(), 3);
  ASSERT_EQ(sim::radio.sleepViolations, 0);
  expectClean();
}

STEST(warm_sleep_keeps_the_configuration_and_wakes_with_standby) {
  ASSERT_TRUE(doSetup(433000000, 9, 125000));
  runAsTask([] { rl.sleep(true); }, 100);
  ASSERT_EQ(sim::radio.mode(), (int)sim::Sx126xModel::SLEEP);
  runAsTask([] { g_ok = rl.standby() == RADIOLIB_ERR_NONE; }, 100);
  ASSERT_TRUE(g_ok);
  ASSERT_EQ(sim::radio.sf(), 9);                                   // конфигурацията е запазена
  ASSERT_EQ(sim::radio.packetType(), 1);
  expectClean();
}

// ============================================================================ HAL (EspHal) и шина
STEST(spi_and_gpio_are_set_up_through_the_real_esphal_as_the_board_pins_say) {
  ASSERT_TRUE(doSetup());
  runAsTask([] { radio_listen(); }, 100);
  ASSERT_EQ(sim::spi.busInits, 1);
  ASSERT_EQ(sim::spi.host, (int)LORA_SPI_HOST);
  ASSERT_EQ(sim::spi.sclk, LORA_SCK);
  ASSERT_EQ(sim::spi.miso, LORA_MISO);
  ASSERT_EQ(sim::spi.mosi, LORA_MOSI);
  ASSERT_EQ(sim::spi.clockHz, (int)LORA_SPI_HZ);
  ASSERT_EQ(sim::spi.mode, 0);
  ASSERT_EQ(sim::spi.cs, -1);                                      // CS го управлява RadioLib като GPIO
  ASSERT_EQ(sim::spi.devices, 1);
  ASSERT_EQ(sim::spi.acquires, sim::spi.releases);                 // шината се връща след всяка транзакция
  ASSERT_GE(sim::spi.transfers, 20);
  ASSERT_EQ(sim::spi.unknownDevice, 0);
  ASSERT_EQ(sim::gpio.pins[LORA_NSS].level, 1);                    // CS е неактивен в покой
  ASSERT_EQ(sim::gpio.pins[LORA_NSS].mode, (int)GPIO_MODE_OUTPUT);
  ASSERT_EQ(sim::gpio.pins[LORA_BUSY].mode, (int)GPIO_MODE_INPUT);
  ASSERT_EQ(sim::gpio.pins[LORA_DIO1].mode, (int)GPIO_MODE_INPUT);
  ASSERT_EQ(sim::gpio.pins[LORA_RST].level, 1);                    // чипът не е държан в reset
  ASSERT_GE((int)sim::gpio.pins[LORA_RST].history.size(), 2);
  ASSERT_EQ(sim::gpio.pins[LORA_RST].history.front(), 0);          // RST импулс: ниско, после високо
  ASSERT_EQ(sim::gpio.pins[LORA_RST].history.back(), 1);
  expectClean();
}

STEST(radiolib_state_is_rebuilt_after_a_reboot_so_the_second_boot_initializes_everything_again) {
  ASSERT_TRUE(doSetup(433000000, 8, 125000));
  ASSERT_EQ(sim::spi.busInits, 1);
  sim::reboot();
  ASSERT_TRUE(doSetup(433000000, 10, 250000));
  ASSERT_EQ(sim::spi.busInits, 1);                                 // нов процес: шината (в симулатора - нулирана при рестарт) е вдигната наново
  ASSERT_EQ(sim::radio.sf(), 10);
  ASSERT_EQ((int)sim::radio.bwHz(), 250000);
  expectClean();
}

STEST(a_complete_working_session_never_violates_a_chip_or_bus_rule) {
  // setup, слушане, приемане, CAD, предаване, смяна на честота, още приемане, сън
  ASSERT_TRUE(doSetup(434000000, 9, 125000, 20));
  static int rounds;
  rounds = 0;
  sim::boot([] {
    radio_listen();
    hal_irq_attach(LORA_DIO1, xTaskGetCurrentTaskHandle());
    for (;;) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
      uint8_t b[64];
      for (int k = 0; k < 8; k++) { int n = radio_read_packet(b, sizeof b); if (n == RADIO_RX_NONE) break; }
      if (!radio_set_frequency(433000000)) break;
      bool busy = channelActive();
      if (!busy) { uint8_t pkt[26]; memset(pkt, rounds, sizeof pkt); radio_transmit(pkt, sizeof pkt); }
      radio_set_frequency(434000000);
      radio_listen();
      rounds++;
    }
  });
  for (int i = 0; i < 40; i++) {
    sim::at_after_ms(100 + 90 * i, [i] { sim::radio.deliver(frame(22, (uint8_t)i), 434000000, -70, 5.0f, (i % 5) != 0); }, true);
    if (i == 20) sim::radio.busyUntil = sim::now_us() + 3500000ULL;
  }
  sim::run_for_ms(6000);
  ASSERT_GE(rounds, 5);
  ASSERT_GE((int)sim::radio.tx.size(), 3);
  ASSERT_GE(sim::radio.cadCount, 5);
  expectClean();
}

int main() { return runAllTests(); }
