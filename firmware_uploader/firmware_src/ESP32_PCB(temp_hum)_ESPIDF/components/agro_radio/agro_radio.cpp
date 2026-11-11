#include "agro_radio.h"
#include <inttypes.h>

#include <new>
#include "board_pins.h"
#include "esp_log.h"
#include "agro_hal.h"
#include "protocol.h"
#include "radio_timing.h"

static const char *TAG = "radio";

// ---------------- Обектите на RadioLib ----------------
// EspHal е готовият HAL на библиотеката: SPI през spi_master (софтуерен CS от RadioLib), GPIO, закъснения през FreeRTOS
// (изисква CONFIG_FREERTOS_HZ=1000 - вижте sdkconfig.defaults).
static EspHal s_hal(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SPI_HOST, LORA_SPI_HZ);
static Module s_module(&s_hal, LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
SX1262 radio(&s_module);

#ifdef AGRO_HOST_TEST
void radio_ram_reset(void) {
  s_hal.~EspHal();
  new (&s_hal) EspHal(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SPI_HOST, LORA_SPI_HZ);
  s_module.~Module();
  new (&s_module) Module(&s_hal, LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);
  radio.~SX1262();
  new (&radio) SX1262(&s_module);
}
#endif

int8_t radio_clamp_tx_power(int32_t dbm) {
  if (dbm > LORA_TX_POWER_LIMIT_DBM) return (int8_t)LORA_TX_POWER_LIMIT_DBM;   // модул И регулатор
  if (dbm < LORA_TX_POWER_MIN_DBM) return (int8_t)LORA_TX_POWER_MIN_DBM;
  return (int8_t)dbm;
}

bool radio_setup(const RadioParams *p, int32_t maxTries) {
  // времената (CAD, ACK timeout-и) се извеждат от SF/BW, прочетени от NVS
  radioTimingInit(p->sf, p->bwHz, LORA_CR_DENOM, LORA_PREAMBLE_LEN, true);

  int8_t power = radio_clamp_tx_power(p->txPowerDbm);
  if (power != p->txPowerDbm) ESP_LOGW(TAG, "мощност %" PRId32 " dBm е извън границите (модул/регулатор) - ограничена до %" PRId32 " dBm", (int32_t)p->txPowerDbm, (int32_t)power);

  ConfigLoRa_t cfg;
  cfg.frequency = (float)p->freqHz / 1.0e6f;
  cfg.bandwidth = (float)radioBw() / 1000.0f;
  cfg.spreadingFactor = radioSf();
  cfg.codingRate = radioCrDenom();             // знаменателят 5..8 (4/5..4/8), както иска RadioLib
  cfg.syncWord = LORA_SYNC_WORD;
  cfg.power = power;
  cfg.preambleLength = radioPreambleLen();

  radio.tcxoVoltage = LORA_TCXO_VOLTAGE;
  radio.useRegulatorLDO = LORA_REGULATOR_LDO_ONLY != 0;
  if (LORA_RXEN >= 0 && LORA_TXEN >= 0) s_module.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

  // Мъртъв или неправилно свързан чип: стандартният begin() на RadioLib чака до 10 x 1 s (reset с проверка + таймаут на
  // BUSY) - непоносимо за sensor на батерия. Затова рестартът е наш (импулс на RST; BUSY се чака в SPI слоя), а
  // таймаутът на BUSY е кратък (нормално BUSY е високо най-много няколко ms, дори при калибриране).
  s_module.spiConfig.timeout = RADIO_SPI_BUSY_TIMEOUT_MS;
  radio.resetOnStartup = false;

  uint32_t lastMsg = 0;
  bool first = true;
  for (int32_t attempt = 1;; attempt++) {
    radio.reset(false);                        // RST импулс; без чакане за standby
    int16_t st = radio.begin(cfg);
    if (st == RADIOLIB_ERR_NONE) break;
    if (maxTries > 0 && attempt >= maxTries) {
      ESP_LOGE(TAG, "радиото не стартира след %" PRId32 " опита (RadioLib код %" PRId32 ")", attempt, (int32_t)st);
      return false;
    }
    if (first || hal_millis() - lastMsg >= RADIO_BEGIN_RETRY_MSG_MS) {
      ESP_LOGE(TAG, "радиото не стартира (RadioLib код %" PRId32 "), продължавам да опитвам...", (int32_t)st);
      lastMsg = hal_millis();
      first = false;
    }
    hal_delay_ms(RADIO_BEGIN_RETRY_PAUSE_MS);
  }

  // RadioLib оставя OCP на 60 mA - твърде малко за PA на SX1262 над ~+14 dBm; Semtech по подразбиране задава 140 mA
  if (radio.setCurrentLimit(LORA_CURRENT_LIMIT_MA) != RADIOLIB_ERR_NONE) ESP_LOGW(TAG, "ограничението по ток не се зададе");
  if (!LORA_DIO2_AS_RF_SWITCH) radio.setDio2AsRfSwitch(false);       // begin() го включва; някои модули го нямат свързан към ключа
  ESP_LOGI(TAG, "SX1262: %" PRIu32 " Hz SF%" PRIu32 " BW%" PRIu32 " Hz CR4/%" PRIu32 " sync 0x%02" PRIX32 " %" PRId32 " dBm", p->freqHz, (uint32_t)cfg.spreadingFactor,
           (uint32_t)radioBw(), (uint32_t)cfg.codingRate, (uint32_t)cfg.syncWord, (int32_t)power);   // без %f: nano printf не печата float
  return true;
}

bool radio_set_frequency(uint32_t hz) {
  int16_t st = radio.standby();                    // честотата се сменя в standby
  if (st == RADIOLIB_ERR_NONE) st = radio.setFrequency((float)hz / 1.0e6f);
  if (st != RADIOLIB_ERR_NONE) ESP_LOGE(TAG, "смяна на честотата на %" PRIu32 " Hz се провали (RadioLib код %" PRId32 ")", hz, (int32_t)st);
  return st == RADIOLIB_ERR_NONE;
}

bool radio_listen(void) {
  int16_t st = radio.startReceive();               // RX continuous (таймаут 0xFFFFFF): чипът остава в RX и след пакет
  hal_irq_rearm(LORA_DIO1);                    // startReceive изчиства флаговете -> DIO1 е ниско; прекъсването може да се върне
  if (st != RADIOLIB_ERR_NONE) ESP_LOGE(TAG, "startReceive се провали (RadioLib код %" PRId32 ")", (int32_t)st);
  return st == RADIOLIB_ERR_NONE;
}

int32_t radio_read_packet(uint8_t *buf, size_t cap) {
  uint32_t irq = radio.getIrqFlags();
  if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) return RADIO_RX_NONE;

  size_t len = radio.getPacketLength();
  if (len == 0) {
    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);          // DIO1 трябва да падне, иначе прекъсването се повтаря
    return RADIO_RX_NONE;
  }
  int16_t st = radio.readData(buf, len < cap ? len : cap);     // чете буфера (до cap байта) и изчиства флаговете
  if (st == RADIOLIB_ERR_CRC_MISMATCH) return RADIO_RX_CRC_ERROR;
  if (st != RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "readData се провали (RadioLib код %" PRId32 "), пакетът е изхвърлен", (int32_t)st);
    radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    return RADIO_RX_NONE;
  }
  return (int32_t)len;
}

bool radio_rx_pending(void) {
  return (radio.getIrqFlags() & RADIOLIB_SX126X_IRQ_RX_DONE) != 0;
}

bool radio_transmit(const uint8_t *buf, size_t len) {
  if (buf == nullptr || len == 0 || len > 255) return false;
  hal_irq_disable(LORA_DIO1);                  // TX_DONE вдига DIO1; края на предаването го следим сами
  // Параметрите на пакета се пишат само в standby (чипът може да е в непрекъснат прием); startTransmit() на RadioLib
  // не го прави сам (за разлика от startReceive и transmit)
  int16_t st = radio.standby();
  if (st == RADIOLIB_ERR_NONE) st = radio.startTransmit(buf, len);
  bool ok = false;
  if (st == RADIOLIB_ERR_NONE) {
    // RadioLib няма таймаут на чакането (SetTx е без таймаут): сроковете са от времето във въздуха
    uint32_t toaMs = (uint32_t)((radio.getTimeOnAir(len) + 999) / 1000);
    uint32_t start = hal_millis();
    uint32_t deadline = toaMs + toaMs / 4 + 100;
    if (toaMs > 2) hal_delay_ms(toaMs - 1);    // чипът излъчва сам; не заемаме шината и процесора с въпроси
    for (;;) {
      uint32_t irq = radio.getIrqFlags();
      if (irq & RADIOLIB_SX126X_IRQ_TX_DONE) { ok = true; break; }
      if (irq & RADIOLIB_SX126X_IRQ_TIMEOUT) break;
      if (hal_millis() - start > deadline) break;
      hal_delay_ms(1);
    }
    if (!ok) ESP_LOGE(TAG, "предаването не приключи за %" PRIu32 " ms (време във въздуха %" PRIu32 " ms)", (uint32_t)(hal_millis() - start), (uint32_t)toaMs);
  } else {
    ESP_LOGE(TAG, "startTransmit се провали (RadioLib код %" PRId32 ")", (int32_t)st);
  }
  int16_t fin = radio.finishTransmit();            // изчиства флаговете и връща в standby (и прекъсва зациклило предаване)
  if (fin != RADIOLIB_ERR_NONE) ESP_LOGW(TAG, "finishTransmit код %" PRId32, (int32_t)fin);
  return ok;
}

// RADIO_CAD_SYMBOLS (radio_timing.h) -> константата на RadioLib
static uint8_t cadSymbolConst() {
  switch (RADIO_CAD_SYMBOLS) {
    case 1: return RADIOLIB_SX126X_CAD_ON_1_SYMB;
    case 2: return RADIOLIB_SX126X_CAD_ON_2_SYMB;
    case 4: return RADIOLIB_SX126X_CAD_ON_4_SYMB;
    case 8: return RADIOLIB_SX126X_CAD_ON_8_SYMB;
    default: return RADIOLIB_SX126X_CAD_ON_16_SYMB;
  }
}

bool channelActive(void) {
  hal_irq_disable(LORA_DIO1);                  // CAD вдига DIO1 (CadDone) - не искаме да буди задачата
  // standby + DIO1 = CadDone|CadDetected + SetCad. Прагът на откриване (detPeak, detMin) и режимът на излизане
  // (обратно в standby) са по подразбиране на RadioLib - препоръките на Semtech за SF.
  ChannelScanConfig_t scan = {};
  scan.cad.symNum = cadSymbolConst();
  scan.cad.detPeak = RADIOLIB_SX126X_CAD_PARAM_DEFAULT;
  scan.cad.detMin = RADIOLIB_SX126X_CAD_PARAM_DEFAULT;
  scan.cad.exitMode = RADIOLIB_SX126X_CAD_PARAM_DEFAULT;
  scan.cad.timeout = 0;
  scan.cad.irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS;
  scan.cad.irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK;
  int16_t st = radio.startChannelScan(scan);
  if (st != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "startChannelScan се провали (RadioLib код %" PRId32 ") - считам канала за свободен", (int32_t)st);
    radio.standby();
    return false;
  }
  uint32_t start = hal_millis();
  uint32_t timeoutMs = radioCadTimeoutMs();
  bool done = false;
  while (!(done = hal_gpio_read(LORA_DIO1))) {
    if (hal_millis() - start > timeoutMs) break;
    hal_delay_ms(1);                           // CAD е от няколко ms до стотици ms (според SF/BW): процесорът спи между проверките
  }
  int16_t r = radio.getChannelScanResult();        // LORA_DETECTED / CHANNEL_FREE (чете флаговете на чипа)
  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  if (!done) {
    ESP_LOGW(TAG, "CAD не приключи за %" PRIu32 " ms - прекъсвам", (uint32_t)timeoutMs);
    radio.standby();
  }
  return r == RADIOLIB_LORA_DETECTED;
}
