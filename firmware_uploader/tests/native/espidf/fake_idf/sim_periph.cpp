// Периферия на симулатора: NVS, GPIO, SPI + модел на SX1262, I2C + SHT31/SHT21, mbedtls (виж sim_periph.h).
#include "sim_periph.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aes128.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sim.h"

enum { NT_U8 = 1, NT_U32 = 2, NT_STR = 3, NT_BLOB = 4 };

void sim_preempt_point(void);
void sim_run_in_isr(const std::function<void()> &fn);

namespace sim {

NvsFake nvs;
GpioFake gpio;
Sx126xModel radio;
std::vector<RadioFrame> airLog;
I2cFake i2c;
SpiFake spi;

// ======================================================================= NVS

void NvsFake::clear() {
  data.clear();
  namespaces.clear();
  writesPerKey.clear();
  commits = totalWrites = eraseCalls = 0;
  failWrites = false;
  initResults.clear();
}
bool NvsFake::has(const char *key, const char *ns) const { return data.count(std::string(ns) + "/" + key) > 0; }
std::string NvsFake::str(const char *key, const char *ns) const {
  auto it = data.find(std::string(ns) + "/" + key);
  if (it == data.end() || it->second.first != NT_STR) return "<нищо>";
  return std::string(it->second.second.begin(), it->second.second.end());
}
int64_t NvsFake::num(const char *key, const char *ns) const {
  auto it = data.find(std::string(ns) + "/" + key);
  if (it == data.end()) return -1;
  uint32_t v = 0;
  memcpy(&v, it->second.second.data(), it->second.second.size() < 4 ? it->second.second.size() : 4);
  return v;
}
std::vector<uint8_t> NvsFake::blob(const char *key, const char *ns) const {
  auto it = data.find(std::string(ns) + "/" + key);
  return it == data.end() ? std::vector<uint8_t>() : it->second.second;
}
static void nvsPut(NvsFake &n, const std::string &ns, const char *key, int type, const void *p, size_t len) {
  std::string k = ns + "/" + key;
  n.data[k] = {type, std::vector<uint8_t>((const uint8_t *)p, (const uint8_t *)p + len)};
  n.writesPerKey[k]++;
  n.totalWrites++;
  bool known = false;
  for (auto &s : n.namespaces) if (s == ns) known = true;
  if (!known) n.namespaces.push_back(ns);
}
void NvsFake::setStr(const char *key, const std::string &v, const char *ns) { nvsPut(*this, ns, key, NT_STR, v.c_str(), v.size()); }
void NvsFake::setU8(const char *key, uint8_t v, const char *ns) { nvsPut(*this, ns, key, NT_U8, &v, 1); }
void NvsFake::setU32(const char *key, uint32_t v, const char *ns) { nvsPut(*this, ns, key, NT_U32, &v, 4); }
void NvsFake::setBlob(const char *key, const void *p, size_t n, const char *ns) { nvsPut(*this, ns, key, NT_BLOB, p, n); }
int NvsFake::writes(const char *key, const char *ns) const {
  auto it = writesPerKey.find(std::string(ns) + "/" + key);
  return it == writesPerKey.end() ? 0 : it->second;
}

}  // namespace sim

namespace {
struct NvsHandle { std::string ns; bool readOnly; bool open; };
std::vector<NvsHandle> g_nvsHandles(1);   // индексът 0 не се ползва (handle 0 = невалиден)

NvsHandle *nvsH(nvs_handle_t h) {
  return (h > 0 && h < g_nvsHandles.size() && g_nvsHandles[h].open) ? &g_nvsHandles[h] : nullptr;
}
}  // namespace

esp_err_t nvs_flash_init(void) {
  if (!sim::nvs.initResults.empty()) {
    int r = sim::nvs.initResults.front();
    sim::nvs.initResults.pop_front();
    return r;
  }
  return ESP_OK;
}
esp_err_t nvs_flash_erase(void) {
  sim::nvs.data.clear();
  sim::nvs.namespaces.clear();
  sim::nvs.eraseCalls++;
  return ESP_OK;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
  sim_preempt_point();
  bool exists = false;
  for (auto &s : sim::nvs.namespaces) if (s == ns) exists = true;
  if (!exists) {
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;   // като в IDF: няма такъв namespace
    sim::nvs.namespaces.push_back(ns);
  }
  g_nvsHandles.push_back({ns, mode == NVS_READONLY, true});
  *out = (nvs_handle_t)(g_nvsHandles.size() - 1);
  return ESP_OK;
}
void nvs_close(nvs_handle_t h) {
  NvsHandle *x = nvsH(h);
  if (x) x->open = false;
}

static esp_err_t getRaw(nvs_handle_t h, const char *key, int type, std::vector<uint8_t> **out) {
  NvsHandle *x = nvsH(h);
  if (!x) return ESP_ERR_INVALID_ARG;
  auto it = sim::nvs.data.find(x->ns + "/" + key);
  if (it == sim::nvs.data.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (it->second.first != type) return ESP_ERR_NVS_TYPE_MISMATCH;
  *out = &it->second.second;
  return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
  std::vector<uint8_t> *v;
  esp_err_t e = getRaw(h, key, NT_U8, &v);
  if (e == ESP_OK) *out = (*v)[0];
  return e;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
  std::vector<uint8_t> *v;
  esp_err_t e = getRaw(h, key, NT_U32, &v);
  if (e == ESP_OK) memcpy(out, v->data(), 4);
  return e;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {
  std::vector<uint8_t> *v;
  esp_err_t e = getRaw(h, key, NT_STR, &v);
  if (e != ESP_OK) return e;
  size_t need = v->size() + 1;
  if (out == nullptr) { *len = need; return ESP_OK; }
  if (*len < need) { *len = need; return ESP_ERR_NVS_INVALID_LENGTH; }
  memcpy(out, v->data(), v->size());
  out[v->size()] = 0;
  *len = need;
  return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {
  std::vector<uint8_t> *v;
  esp_err_t e = getRaw(h, key, NT_BLOB, &v);
  if (e != ESP_OK) return e;
  if (out == nullptr) { *len = v->size(); return ESP_OK; }
  if (*len < v->size()) { *len = v->size(); return ESP_ERR_NVS_INVALID_LENGTH; }
  memcpy(out, v->data(), v->size());
  *len = v->size();
  return ESP_OK;
}

static esp_err_t setRaw(nvs_handle_t h, const char *key, int type, const void *p, size_t n) {
  sim_preempt_point();
  NvsHandle *x = nvsH(h);
  if (!x) return ESP_ERR_INVALID_ARG;
  if (x->readOnly) return ESP_ERR_NVS_READ_ONLY;
  if (sim::nvs.failWrites) return ESP_FAIL;
  nvsPut(sim::nvs, x->ns, key, type, p, n);
  return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) { return setRaw(h, key, NT_U8, &v, 1); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) { return setRaw(h, key, NT_U32, &v, 4); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { return setRaw(h, key, NT_STR, v, strlen(v)); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n) { return setRaw(h, key, NT_BLOB, v, n); }
esp_err_t nvs_commit(nvs_handle_t h) {
  NvsHandle *x = nvsH(h);
  if (!x) return ESP_ERR_INVALID_ARG;
  if (sim::nvs.failWrites) return ESP_FAIL;
  sim::nvs.commits++;
  return ESP_OK;
}

// ======================================================================= GPIO
namespace sim {

int GpioFake::read(int pin) {
  GpioPin &p = pins[pin];
  if (p.dynamicIn && (p.mode == GPIO_MODE_INPUT || p.mode == GPIO_MODE_DISABLE)) return p.dynamicIn();
  return (p.mode == GPIO_MODE_INPUT || p.mode == GPIO_MODE_DISABLE) ? p.ext : p.level;
}

static void evalIntr(GpioPin &p, int oldLevel, int newLevel) {
  if (!p.intrEnabled || p.isr == nullptr) return;
  bool fire = false;
  switch (p.intrType) {
    case GPIO_INTR_HIGH_LEVEL: fire = newLevel == 1; break;
    case GPIO_INTR_LOW_LEVEL: fire = newLevel == 0; break;
    case GPIO_INTR_POSEDGE: fire = oldLevel == 0 && newLevel == 1; break;
    case GPIO_INTR_NEGEDGE: fire = oldLevel == 1 && newLevel == 0; break;
    case GPIO_INTR_ANYEDGE: fire = oldLevel != newLevel; break;
    default: break;
  }
  if (!fire) return;
  p.isrCalls++;
  void (*fn)(void *) = p.isr;
  void *arg = p.isrArg;
  sim_run_in_isr([&] { fn(arg); });
}

void GpioFake::drive(int pin, int level) {
  GpioPin &p = pins[pin];
  int old = p.ext;
  p.ext = level;
  evalIntr(p, old, level);
}

void GpioFake::reset() {
  for (auto &p : pins) p = GpioPin();
  isrServiceInstalled = false;
  isrServiceInstalls = 0;
}

}  // namespace sim

esp_err_t gpio_config(const gpio_config_t *cfg) {
  sim_preempt_point();
  for (int pin = 0; pin < 64; pin++) {
    if (cfg->pin_bit_mask & (1ULL << pin)) {
      sim::GpioPin &p = sim::gpio.pins[pin];
      p.mode = cfg->mode;
      p.intrType = cfg->intr_type;
    }
  }
  return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
  sim_preempt_point();
  sim::GpioPin &p = sim::gpio.pins[pin];
  int lv = level ? 1 : 0;
  int old = p.level;
  p.level = lv;
  p.outputWrites++;
  p.history.push_back(lv);
  if (old != lv && p.onChange) p.onChange(lv);
  return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) { return sim::gpio.read(pin); }

esp_err_t gpio_install_isr_service(int) {
  sim::gpio.isrServiceInstalls++;
  if (sim::gpio.isrServiceInstalled) return ESP_ERR_INVALID_STATE;
  sim::gpio.isrServiceInstalled = true;
  return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg) {
  if (!sim::gpio.isrServiceInstalled) return ESP_ERR_INVALID_STATE;
  sim::gpio.pins[pin].isr = fn;
  sim::gpio.pins[pin].isrArg = arg;
  return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t pin) {
  sim::GpioPin &p = sim::gpio.pins[pin];
  p.intrEnabled = true;
  // ниво-чувствително прекъсване: ако условието вече е вярно, прекъсването идва веднага
  if (p.intrType == GPIO_INTR_HIGH_LEVEL || p.intrType == GPIO_INTR_LOW_LEVEL) {
    int lv = sim::gpio.read(pin);
    sim::evalIntr(p, lv, lv);
  }
  return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t pin) {
  sim::gpio.pins[pin].intrEnabled = false;
  return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) {
  sim::gpio.pins[pin].intrType = type;
  return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
  sim::gpio.pins[pin].isr = nullptr;
  sim::gpio.pins[pin].isrArg = nullptr;
  return ESP_OK;
}

esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t mode) {
  (void)pin; (void)mode;
  return ESP_OK;
}

esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type) {
  sim::GpioPin &p = sim::gpio.pins[pin];
  p.wakeEnabled = true;
  p.wakeType = type;
  p.intrType = type;    // в IDF прекъсването и събуждането делят един и същ тип (int_type)
  return ESP_OK;
}

// ======================================================================= LEDC (само за RadioLib tone(); прошивките не го викат)
esp_err_t ledc_timer_config(const ledc_timer_config_t *) { return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *) { return ESP_OK; }
esp_err_t ledc_set_freq(ledc_mode_t, ledc_timer_t, uint32_t) { return ESP_OK; }
esp_err_t ledc_set_duty(ledc_mode_t, ledc_channel_t, uint32_t) { return ESP_OK; }
esp_err_t ledc_update_duty(ledc_mode_t, ledc_channel_t) { return ESP_OK; }
esp_err_t ledc_stop(ledc_mode_t, ledc_channel_t, uint32_t) { return ESP_OK; }

// ======================================================================= SPI
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg, int) {
  if (sim::spi.busInits > sim::spi.busFrees) return ESP_ERR_INVALID_STATE;       // шината е инициализирана вече
  sim::spi.busInits++;
  sim::spi.host = host;
  sim::spi.mosi = cfg->mosi_io_num;
  sim::spi.miso = cfg->miso_io_num;
  sim::spi.sclk = cfg->sclk_io_num;
  return ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t) {
  if (sim::spi.busInits <= sim::spi.busFrees) return ESP_ERR_INVALID_STATE;
  sim::spi.busFrees++;
  return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *h) {
  if (sim::spi.busInits <= sim::spi.busFrees) return ESP_ERR_INVALID_STATE;
  sim::spi.devices++;
  sim::spi.cs = cfg->spics_io_num;
  sim::spi.clockHz = cfg->clock_speed_hz;
  sim::spi.mode = cfg->mode;
  *h = (spi_device_handle_t)(intptr_t)(host + 1);               // handle = SPI хостът
  return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t h) {
  if (h == nullptr || sim::spi.devices <= 0) return ESP_ERR_INVALID_STATE;
  sim::spi.devices--;
  return ESP_OK;
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t h, uint32_t) {
  if (h == nullptr) return ESP_ERR_INVALID_ARG;
  sim::spi.acquires++;
  return ESP_OK;
}

void spi_device_release_bus(spi_device_handle_t h) {
  if (h != nullptr) sim::spi.releases++;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t) {
  sim_preempt_point();
  int host = (int)(intptr_t)h - 1;
  size_t n = (t->length + 7) / 8;
  uint8_t txTmp[300];
  uint8_t rxTmp[300];
  if (n > sizeof(txTmp)) return ESP_ERR_INVALID_ARG;
  const uint8_t *tx = (t->flags & SPI_TRANS_USE_TXDATA) ? t->tx_data : (const uint8_t *)t->tx_buffer;
  if (tx) memcpy(txTmp, tx, n); else memset(txTmp, 0, n);
  memset(rxTmp, 0, sizeof(rxTmp));
  sim::spi.transfers++;
  if (host == LORA_SPI_HOST) {
    sim::radio.xfer(txTmp, rxTmp, n);
  } else {
    sim::spi.unknownDevice++;
  }
  if (t->flags & SPI_TRANS_USE_RXDATA) memcpy(t->rx_data, rxTmp, n < 4 ? n : 4);
  else if (t->rx_buffer) memcpy(t->rx_buffer, rxTmp, n);
  return ESP_OK;
}

// ======================================================================= SX1262 модел
namespace sim {

// Опкоди на SX126x (datasheet 13.x) - описани тук независимо от RadioLib
enum : uint8_t {
  OP_NOP = 0x00, OP_CLR_IRQ = 0x02, OP_SET_DIO_IRQ = 0x08, OP_CLR_DEV_ERR = 0x07, OP_WRITE_REG = 0x0D, OP_WRITE_BUF = 0x0E,
  OP_GET_PKT_TYPE = 0x11, OP_GET_IRQ = 0x12, OP_GET_RX_BUF = 0x13, OP_GET_PKT_STATUS = 0x14, OP_GET_RSSI = 0x15,
  OP_GET_STATS = 0x10, OP_GET_DEV_ERR = 0x17, OP_READ_REG = 0x1D, OP_READ_BUF = 0x1E,
  OP_SET_STANDBY = 0x80, OP_SET_RX = 0x82, OP_SET_TX = 0x83, OP_SET_SLEEP = 0x84, OP_SET_RF_FREQ = 0x86, OP_CALIB_IMG = 0x98,
  OP_SET_CAD_PARAMS = 0x88, OP_CALIBRATE = 0x89, OP_SET_PKT_TYPE = 0x8A, OP_SET_MOD_PARAMS = 0x8B, OP_SET_PKT_PARAMS = 0x8C,
  OP_SET_TX_PARAMS = 0x8E, OP_SET_BUF_BASE = 0x8F, OP_SET_FALLBACK = 0x93, OP_SET_PA_CFG = 0x95, OP_SET_REG_MODE = 0x96,
  OP_SET_DIO3_TCXO = 0x97, OP_SET_DIO2_RF = 0x9D, OP_SET_CAD = 0xC5, OP_SET_FS = 0xC1, OP_GET_STATUS = 0xC0,
  OP_SET_LORA_SYMB_TIMEOUT = 0xA0, OP_STOP_TIMER_ON_PREAMBLE = 0x9F, OP_SET_RX_DUTY = 0x94,
};
enum : uint16_t {
  IRQ_TX_DONE = 0x0001, IRQ_RX_DONE = 0x0002, IRQ_PREAMBLE = 0x0004, IRQ_SYNC = 0x0008, IRQ_HDR_VALID = 0x0010,
  IRQ_HDR_ERR = 0x0020, IRQ_CRC_ERR = 0x0040, IRQ_CAD_DONE = 0x0080, IRQ_CAD_DET = 0x0100, IRQ_TIMEOUT = 0x0200,
};
static const uint16_t DEV_ERR_XOSC_START = 0x0020;

void Sx126xModel::loadDefaults() {
  memset(regs_, 0, sizeof(regs_));
  memset(buf_, 0, sizeof(buf_));
  const char *ver = "SX1261";
  for (int i = 0; ver[i]; i++) regs_[0x0320 + i] = (uint8_t)ver[i];
  regs_[0x08E7] = 0x38;                      // OCP: SX1262 по подразбиране 140 mA (0x38 * 2.5)
  regs_[0x08D8] = 0xC8;                      // TX clamp config (стойност след reset)
  regs_[0x0889] = 0xD8;                      // sensitivity config
  regs_[0x0736] = 0x0D;                      // IQ config
  regs_[0x0740] = 0x14; regs_[0x0741] = 0x24;   // LoRa sync word: частна мрежа 0x1424
  regs_[0x08AC] = 0x94;                      // RX gain (нормален)
  packetType_ = 0;                           // GFSK след reset
  sf_ = bwRaw_ = cr_ = ldro_ = 0;
  preamble_ = 0; hdrType_ = 0; payloadLen_ = 0; crcType_ = 0; invertIq_ = 0;
  frf_ = 0xE4C000;                           // 915 MHz по подразбиране (стойност след reset)
  power_ = 0; ramp_ = 0;
  paDuty_ = paHpMax_ = paDevice_ = paLut_ = 0;
  irqMask_ = dio1Mask_ = dio2Mask_ = dio3Mask_ = irq_ = 0;
  txBase_ = 0x80; rxBase_ = 0x80;            // ВАЖНО: след reset базовите адреси са 0x80 - драйверът трябва да ги нулира
  rxLen_ = rxStart_ = 0;
  cadSym_ = cadPeak_ = cadMin_ = cadExit_ = 0; cadTimeoutRaw_ = 0;
  rxTimeoutRaw_ = txTimeoutRaw_ = 0;
  fallback_ = 0x20;
  dio2Rf_ = false;
  regulator_ = 0;                            // LDO след reset
  tcxoCode_ = 0; tcxoOn_ = false; tcxoDelayRaw_ = 0;
  deviceErrors_ = 0;
  symbTimeout_ = 0;
  cadActive_ = false;
  xoscRunning_ = false;
  txActive = false;
}

void Sx126xModel::hardReset(bool count) {
  loadDefaults();
  mode_ = STBY_RC;
  if (count) resets++;
  busyEnd_ = sim::now_us() + 3000;           // след reset BUSY е висок ~3 ms
  lastDio1_ = 0;
}

void Sx126xModel::fullReset() { *this = Sx126xModel(); }

void Sx126xModel::onNss(int level) {
  if (!present || level != 0 || mode_ != SLEEP) return;
  mode_ = STBY_RC;
  wakeups++;
  if (sleepCfg_ & 0x04) {
    // warm start: конфигурацията е запазена
  } else {
    uint8_t keepIrq = 0; (void)keepIrq;
    loadDefaults();
  }
  setBusy(150 + (tcxoDelayRaw_ ? (uint64_t)(tcxoDelayRaw_ * 15.625) : 0));
  wokeNow_ = true;
}

uint8_t Sx126xModel::reg(uint16_t a) const { return a < sizeof(regs_) ? regs_[a] : 0; }

long Sx126xModel::freqHz() const { return (long)(((uint64_t)frf_ * 32000000ULL) >> 25); }

long Sx126xModel::bwHz() const {
  switch (bwRaw_) {
    case 0x00: return 7810;   case 0x08: return 10420;  case 0x01: return 15630;
    case 0x09: return 20830;  case 0x02: return 31250;  case 0x0A: return 41670;
    case 0x03: return 62500;  case 0x04: return 125000; case 0x05: return 250000;
    case 0x06: return 500000;
  }
  return 0;
}

double Sx126xModel::symbolUs() const { return std::pow(2.0, sf_) / (double)bwHz() * 1e6; }

double Sx126xModel::airtimeUs(int payloadLen) const {
  double ts = std::pow(2.0, sf_) / (double)bwHz();
  int de = ldro_ ? 1 : 0;
  int ih = hdrType_ ? 1 : 0;                 // 0 = explicit
  double num = 8.0 * payloadLen - 4.0 * sf_ + 28 + 16.0 * (crcOn() ? 1 : 0) - 20.0 * ih;
  double den = 4.0 * (sf_ - 2 * de);
  double blocks = std::ceil(num / den);
  if (blocks < 0) blocks = 0;
  double npay = 8 + blocks * crDenom();
  return ((preamble_ + 4.25) * ts + npay * ts) * 1e6;
}

int Sx126xModel::busyLevel() const {
  if (!present) return 0;
  if (busyStuck) return 1;
  if (mode_ == SLEEP) return 1;
  return sim::now_us() < busyEnd_ ? 1 : 0;
}

int Sx126xModel::dio1Level() const {
  if (!present || mode_ == SLEEP) return 0;
  return (irq_ & dio1Mask_) ? 1 : 0;
}

void Sx126xModel::updateDio() {
  int lv = dio1Level();
  if (lv != lastDio1_) {
    lastDio1_ = lv;
    gpio.drive(LORA_DIO1, lv);
  }
}

void Sx126xModel::raise(uint16_t bits) {
  irq_ |= (uint16_t)(bits & irqMask_);       // чипът вдига само разрешените в IrqMask прекъсвания
}

void Sx126xModel::setBusy(uint64_t us) {
  uint64_t e = sim::now_us() + us;
  if (e > busyEnd_) busyEnd_ = e;
}

uint8_t Sx126xModel::status() const {
  int m = mode_ == SLEEP ? 0 : (mode_ == STBY_RC ? 2 : mode_ == STBY_XOSC ? 3 : mode_ == FS ? 4 : mode_ == RX ? 5 : mode_ == TX ? 6 : 2);
  return (uint8_t)((m << 4) | (1 << 1));
}

void Sx126xModel::setMode(Mode m) {
  mode_ = m;
  if (m == STBY_RC) xoscRunning_ = false;
}

void Sx126xModel::startTx() {
  int len = payloadLen_;
  if (len == 0) protocolErrors++;
  txActive = true;
  txStart = sim::now_us();
  txEnd = txStart + (uint64_t)airtimeUs(len);
  txFreq = freqHz();
  txData.clear();
  for (int i = 0; i < len; i++) txData.push_back(buf_[(uint8_t)(txBase_ + i)]);
  txTimeoutAt_ = txTimeoutRaw_ ? txStart + (uint64_t)(txTimeoutRaw_ * 15.625) : 0;
  setMode(TX);
  setBusy(100 + (uint64_t)ramp_ * 20);       // BUSY е висок, докато PA рампата свърши
  sim::at_us(txEnd + 1, [this] { tick(); }, false);          // DIO1 се вдига точно в края на предаването (и без SPI обмен)
}

void Sx126xModel::startCad() {
  cadActive_ = true;
  cadCount++;
  double ts = symbolUs();
  cadEnd_ = sim::now_us() + (uint64_t)((cadSym_ == 0x00 ? 1 : cadSym_ == 0x01 ? 2 : cadSym_ == 0x02 ? 4 : cadSym_ == 0x03 ? 8 : 16) * ts + 1.5 * ts + 500.0);
  setMode(CAD);
  sim::at_us(cadEnd_ + 1, [this] { tick(); }, false);         // DIO1 се вдига точно в края на CAD (прошивката го следи по пина)
}

void Sx126xModel::tick() {
  uint64_t now = sim::now_us();
  if (mode_ == TX && txActive) {
    if (!stuckTx && now >= txEnd) {
      RadioFrame f;
      f.tStart = txStart; f.tEnd = txEnd; f.freqHz = txFreq; f.sf = sf_; f.bwHz = bwHz(); f.crDenom = crDenom();
      f.crc = crcOn(); f.ldro = ldro(); f.preamble = preamble_; f.txPowerDbm = (int8_t)power_; f.syncWord = syncWord();
      f.data = txData;
      tx.push_back(f);
      airLog.push_back(f);
      raise(IRQ_TX_DONE);
      txActive = false;
      setMode(fallback_ == 0x30 ? STBY_XOSC : STBY_RC);
    } else if (txTimeoutAt_ != 0 && now >= txTimeoutAt_) {
      raise(IRQ_TIMEOUT);
      abortedTx++;
      txActive = false;
      setMode(STBY_RC);
    }
  }
  if (cadActive_ && !stuckCad && now >= cadEnd_) {
    bool det = busy ? busy(now) : (now < busyUntil);
    raise(IRQ_CAD_DONE | (det ? IRQ_CAD_DET : 0));
    cadActive_ = false;
    setMode(STBY_RC);                        // CadOnly: връща се в стендбай
  }
  if (mode_ == RX && rxTimeoutRaw_ != 0 && rxTimeoutRaw_ != 0xFFFFFF && rxEnd_ != 0 && now >= rxEnd_) {
    raise(IRQ_TIMEOUT);
    setMode(STBY_RC);
    rxEnd_ = 0;
  }
  updateDio();
}

// ---------------------------------------------------------------- SPI команди
void Sx126xModel::xfer(const uint8_t *txb, uint8_t *rx, size_t n) {
  spiTransfers++;
  memset(rx, 0, n);
  if (!present || n == 0) return;
  tick();
  if (sim::gpio.pins[LORA_NSS].level != 0) csViolations++;
  if (mode_ == SLEEP) { sleepViolations++; return; }
  if (busyLevel() && !wokeNow_) busyViolations++;     // команда при висок BUSY (освен будещия NOP)
  wokeNow_ = false;
  command(txb, rx, n);
}

void Sx126xModel::command(const uint8_t *t, uint8_t *rx, size_t n) {
  const uint8_t op = t[0];
  ops.push_back(op);
  if (onCommand) onCommand(op);
  const bool activeRxTx = (mode_ == RX || mode_ == TX || mode_ == CAD);
  bool failed = false;
  if (failCommands > 0 && op != OP_NOP && op != OP_GET_STATUS) { failCommands--; failed = true; }

  // конфигурационни команди не са допустими, докато чипът приема/предава/сканира
  auto needIdle = [&]() { if (activeRxTx) modeViolations++; };

  auto st = [&]() { return (uint8_t)(status() | (failed ? (5 << 1) : 0)); };
  // по подразбиране всеки байт след опкода връща статуса
  for (size_t i = 1; i < n; i++) rx[i] = st();
  auto need = [&](size_t bytes) { if (n < 1 + bytes) { protocolErrors++; return false; } return true; };
  uint64_t busyUs = 40;

  switch (op) {
    case OP_NOP: break;
    case OP_GET_STATUS:
      if (n >= 2) rx[1] = st();
      break;
    case OP_SET_STANDBY: {
      if (!need(1)) break;
      if (mode_ == TX && txActive) { abortedTx++; txActive = false; }
      if (cadActive_) { cadAborted++; cadActive_ = false; }
      bool xosc = t[1] == 1;
      if (xosc && !xoscOk()) { deviceErrors_ |= DEV_ERR_XOSC_START; failed = true; setMode(STBY_RC); break; }
      setMode(xosc ? STBY_XOSC : STBY_RC);
      busyUs = xosc ? 300 : 60;
    } break;
    case OP_SET_SLEEP:
      if (!need(1)) break;
      if (mode_ == TX && txActive) { abortedTx++; txActive = false; }
      if (cadActive_) { cadAborted++; cadActive_ = false; }
      sleepCfg_ = t[1];
      setMode(SLEEP);
      irq_ = 0;                              // IRQ регистърът не оцелява в sleep
      break;
    case OP_SET_FS:
      needIdle();
      if (!xoscOk()) { deviceErrors_ |= DEV_ERR_XOSC_START; failed = true; break; }
      setMode(FS);
      break;
    case OP_SET_RX: {
      if (!need(3)) break;
      if (!xoscOk()) { deviceErrors_ |= DEV_ERR_XOSC_START; failed = true; break; }
      if (mode_ == TX && txActive) { abortedTx++; txActive = false; }
      if (cadActive_) { cadAborted++; cadActive_ = false; }
      rxTimeoutRaw_ = ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
      if (packetType_ != 1) protocolErrors++;                 // приемане без LoRa режим
      rxEnd_ = (rxTimeoutRaw_ != 0 && rxTimeoutRaw_ != 0xFFFFFF) ? sim::now_us() + (uint64_t)(rxTimeoutRaw_ * 15.625) : 0;
      if (rxEnd_ != 0) sim::at_us(rxEnd_ + 1, [this] { tick(); }, false);
      setMode(RX);
      busyUs = 100;
    } break;
    case OP_SET_TX: {
      if (!need(3)) break;
      if (!xoscOk()) { deviceErrors_ |= DEV_ERR_XOSC_START; failed = true; break; }
      if (activeRxTx) modeViolations++;
      txTimeoutRaw_ = ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
      if (packetType_ != 1) protocolErrors++;
      startTx();
    } break;
    case OP_SET_CAD:
      needIdle();
      if (!xoscOk()) { deviceErrors_ |= DEV_ERR_XOSC_START; failed = true; break; }
      if (packetType_ != 1) protocolErrors++;
      startCad();
      break;
    case OP_SET_CAD_PARAMS:
      if (!need(7)) break;
      needIdle();
      cadSym_ = t[1]; cadPeak_ = t[2]; cadMin_ = t[3]; cadExit_ = t[4];
      cadTimeoutRaw_ = ((uint32_t)t[5] << 16) | ((uint32_t)t[6] << 8) | t[7];
      break;
    case OP_SET_PKT_TYPE:
      if (!need(1)) break;
      needIdle();
      packetType_ = t[1];
      break;
    case OP_GET_PKT_TYPE:
      if (n >= 3) rx[2] = packetType_;
      break;
    case OP_SET_RF_FREQ:
      if (!need(4)) break;
      needIdle();
      frf_ = ((uint32_t)t[1] << 24) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 8) | t[4];
      break;
    case OP_CALIB_IMG:
      if (!need(2)) break;
      needIdle();
      busyUs = 2000;
      break;
    case OP_CALIBRATE:
      if (!need(1)) break;
      needIdle();
      if (!xoscOk()) { deviceErrors_ |= DEV_ERR_XOSC_START; failed = true; }
      busyUs = 3500;
      break;
    case OP_SET_PA_CFG:
      if (!need(4)) break;
      needIdle();
      paDuty_ = t[1]; paHpMax_ = t[2]; paDevice_ = t[3]; paLut_ = t[4];
      if (paDevice_ != 0x00) protocolErrors++;                // SX1262: deviceSel = 0 (0x01 е SX1261)
      regs_[0x08E7] = 0x38;                                   // SetPaConfig настройва и OCP: SX1262 -> 140 mA
      break;
    case OP_SET_TX_PARAMS:
      if (!need(2)) break;
      needIdle();
      power_ = t[1]; ramp_ = t[2];
      break;
    case OP_SET_MOD_PARAMS:
      if (!need(4)) break;
      needIdle();
      if (packetType_ != 1) { protocolErrors++; break; }
      sf_ = t[1]; bwRaw_ = t[2]; cr_ = t[3]; ldro_ = t[4];
      if (bwHz() == 0 || sf_ < 5 || sf_ > 12 || cr_ < 1 || cr_ > 4) protocolErrors++;
      break;
    case OP_SET_PKT_PARAMS:
      if (!need(6)) break;
      needIdle();
      if (packetType_ != 1) { protocolErrors++; break; }
      preamble_ = (uint16_t)((t[1] << 8) | t[2]); hdrType_ = t[3]; payloadLen_ = t[4]; crcType_ = t[5]; invertIq_ = t[6];
      break;
    case OP_SET_BUF_BASE:
      if (!need(2)) break;
      needIdle();
      txBase_ = t[1]; rxBase_ = t[2];
      break;
    case OP_SET_DIO_IRQ:
      if (!need(8)) break;
      irqMask_ = (uint16_t)((t[1] << 8) | t[2]); dio1Mask_ = (uint16_t)((t[3] << 8) | t[4]);
      dio2Mask_ = (uint16_t)((t[5] << 8) | t[6]); dio3Mask_ = (uint16_t)((t[7] << 8) | t[8]);
      irq_ &= irqMask_;
      break;
    case OP_GET_IRQ:
      if (n >= 4) { rx[2] = (uint8_t)(irq_ >> 8); rx[3] = (uint8_t)(irq_ & 0xff); }
      break;
    case OP_CLR_IRQ:
      if (!need(2)) break;
      irq_ &= (uint16_t)~(uint16_t)((t[1] << 8) | t[2]);
      break;
    case OP_SET_DIO2_RF:
      if (!need(1)) break;
      needIdle();
      dio2Rf_ = t[1] != 0;
      break;
    case OP_SET_DIO3_TCXO:
      if (!need(4)) break;
      needIdle();
      tcxoCode_ = t[1];
      tcxoOn_ = true;
      tcxoDelayRaw_ = ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 8) | t[4];
      if (tcxoCode_ > 7) protocolErrors++;
      break;
    case OP_SET_REG_MODE:
      if (!need(1)) break;
      needIdle();
      regulator_ = t[1];
      break;
    case OP_SET_FALLBACK:
      if (!need(1)) break;
      fallback_ = t[1];
      break;
    case OP_SET_LORA_SYMB_TIMEOUT:
      if (!need(1)) break;
      symbTimeout_ = t[1];
      break;
    case OP_GET_RX_BUF:
      if (n >= 4) { rx[2] = rxLen_; rx[3] = rxStart_; }
      break;
    case OP_GET_PKT_STATUS:
      if (n >= 5) { rx[2] = pktRssi_; rx[3] = pktSnr_; rx[4] = pktSigRssi_; }
      break;
    case OP_GET_RSSI:
      if (n >= 3) rx[2] = (mode_ == RX) ? 2 * 100 / 1 : 0;   // (мигновен RSSI: -100 dBm)
      break;
    case OP_GET_DEV_ERR:
      if (n >= 4) { rx[2] = (uint8_t)(deviceErrors_ >> 8); rx[3] = (uint8_t)(deviceErrors_ & 0xff); }
      break;
    case OP_CLR_DEV_ERR:
      deviceErrors_ = 0;
      break;
    case OP_WRITE_REG: {
      if (n < 3) { protocolErrors++; break; }
      uint16_t a = (uint16_t)((t[1] << 8) | t[2]);
      for (size_t i = 3; i < n; i++) { if (a + (i - 3) < sizeof(regs_)) regs_[a + (i - 3)] = t[i]; }
    } break;
    case OP_READ_REG: {
      // MOSI: 0x1D, адрес(2), NOP, данни...; MISO: -, статус, статус, статус, данни...
      if (n < 4) { protocolErrors++; break; }
      uint16_t a = (uint16_t)((t[1] << 8) | t[2]);
      for (size_t i = 4; i < n; i++) rx[i] = a + (i - 4) < sizeof(regs_) ? regs_[a + (i - 4)] : 0;
    } break;
    case OP_WRITE_BUF: {
      if (n < 2) { protocolErrors++; break; }
      uint8_t off = t[1];
      for (size_t i = 2; i < n; i++) buf_[(uint8_t)(off + (i - 2))] = t[i];
    } break;
    case OP_READ_BUF: {
      // MOSI: 0x1E, отместване, NOP, данни...; MISO: -, статус, статус, данни...
      if (n < 3) { protocolErrors++; break; }
      uint8_t off = t[1];
      for (size_t i = 3; i < n; i++) rx[i] = buf_[(uint8_t)(off + (i - 3))];
    } break;
    case OP_GET_STATS: case OP_STOP_TIMER_ON_PREAMBLE: case OP_SET_RX_DUTY:
      break;
    default:
      protocolErrors++;                      // неизвестен опкод
      break;
  }
  if (failed) for (size_t i = 1; i < n; i++) if (op != OP_READ_REG && op != OP_READ_BUF) rx[i] = (uint8_t)((rx[i] & 0xF1) | (5 << 1));
  setBusy(busyUs);
  updateDio();
}

bool Sx126xModel::xoscOk() const { return xtalModule ? !tcxoOn_ : tcxoOn_; }   // TCXO модул иска DIO3 да го захранва; модул с кварц - не

bool Sx126xModel::deliver(const std::vector<uint8_t> &frame, long fHz, int rssi, float snr, bool crcOk) {
  tick();
  if (!present || !listening()) { missedRx++; return false; }
  if (fHz != 0 && std::labs(fHz - freqHz()) > 15000) { missedRx++; return false; }
  size_t n = frame.size() > 255 ? 255 : frame.size();
  uint8_t base = rxBase_;
  for (size_t i = 0; i < n; i++) buf_[(uint8_t)(base + i)] = frame[i];
  rxStart_ = base;
  rxLen_ = (uint8_t)n;
  pktRssi_ = (uint8_t)std::lround(-2.0 * rssi);
  pktSigRssi_ = pktRssi_;
  pktSnr_ = (uint8_t)(int8_t)std::lround(snr * 4.0);
  if (irq_ & IRQ_RX_DONE) rxOverrun++;
  raise(IRQ_HDR_VALID);
  raise(IRQ_RX_DONE | (crcOk ? 0 : IRQ_CRC_ERR));
  rxDelivered++;
  updateDio();
  return true;
}

}  // namespace sim

// ======================================================================= I2C сензори
namespace sim {

uint8_t sensirion_crc8(const uint8_t *d, size_t n, uint8_t init) {
  uint8_t crc = init;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

}  // namespace sim

// ======================================================================= I2C (нов драйвер)
struct i2c_master_bus_t { int port; };
struct i2c_master_dev_t { int port; uint8_t addr; bool alive; };
namespace {
i2c_master_bus_t g_i2cBus[2] = {{0}, {1}};
i2c_master_dev_t g_i2cDev[2] = {{0, 0, false}, {1, 0, false}};
sim::I2cBusFake *busOf(int port) { return (port >= 0 && port < 2) ? &sim::i2c.bus[port] : nullptr; }
}  // namespace

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *ret) {
  sim_preempt_point();
  if (cfg == nullptr || ret == nullptr) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  sim::I2cBusFake *b = busOf(cfg->i2c_port);
  if (b == nullptr) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  if (cfg->sda_io_num < 0 || cfg->scl_io_num < 0 || cfg->sda_io_num == cfg->scl_io_num) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  if (b->created) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }          // портът вече е зает
  if (b->failCreate) return ESP_FAIL;
  b->created = true;
  b->sda = cfg->sda_io_num;
  b->scl = cfg->scl_io_num;
  b->pullup = cfg->flags.enable_internal_pullup;
  b->busesCreated++;
  *ret = &g_i2cBus[cfg->i2c_port];
  return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus) {
  sim_preempt_point();
  if (bus == nullptr) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  sim::I2cBusFake *b = busOf(bus->port);
  if (b == nullptr || !b->created) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }
  if (b->deviceAdded) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }       // първо се махат устройствата
  b->created = false;
  return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *ret) {
  sim_preempt_point();
  if (bus == nullptr || cfg == nullptr || ret == nullptr) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  sim::I2cBusFake *b = busOf(bus->port);
  if (b == nullptr || !b->created) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }
  if (cfg->dev_addr_length != I2C_ADDR_BIT_LEN_7 || cfg->device_address > 0x7F || cfg->scl_speed_hz == 0) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  if (b->deviceAdded) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }       // (в този модел - по едно устройство на шина)
  if (b->failAddDevice) return ESP_FAIL;
  b->deviceAdded = true;
  b->hz = cfg->scl_speed_hz;
  b->devicesAdded++;
  g_i2cDev[bus->port].addr = (uint8_t)cfg->device_address;
  g_i2cDev[bus->port].alive = true;
  *ret = &g_i2cDev[bus->port];
  return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) {
  sim_preempt_point();
  if (dev == nullptr) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  sim::I2cBusFake *b = busOf(dev->port);
  if (b == nullptr || !dev->alive) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }
  dev->alive = false;
  b->deviceAdded = false;
  return ESP_OK;
}

static esp_err_t i2cCheck(i2c_master_dev_handle_t dev, const void *data, size_t len, int timeoutMs, sim::I2cBusFake **bus) {
  if (dev == nullptr || data == nullptr || len == 0 || timeoutMs < -1) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_ARG; }
  *bus = busOf(dev->port);
  if (*bus == nullptr || !dev->alive || !(*bus)->created || !(*bus)->deviceAdded) { sim::i2c.usageErrors++; return ESP_ERR_INVALID_STATE; }
  return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeoutMs) {
  sim_preempt_point();
  sim::I2cBusFake *b = nullptr;
  esp_err_t e = i2cCheck(dev, data, len, timeoutMs, &b);
  if (e != ESP_OK) return e;
  uint64_t now = sim::now_us();
  sim::Sht31Model &m = b->sht31;
  if (dev->addr != b->addr || !m.present || len != 2) { b->nackWrites++; return ESP_FAIL; }   // нито един ACK
  uint16_t cmd = (uint16_t)((data[0] << 8) | data[1]);
  if (cmd == 0x2400) { b->measureAt = now + 15000; b->measurePending = true; m.measurements++; return ESP_OK; }
  if (cmd == 0x30A2) { m.resets++; b->measurePending = false; return ESP_OK; }
  b->nackWrites++;
  return ESP_FAIL;
}

esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev, uint8_t *data, size_t len, int timeoutMs) {
  sim_preempt_point();
  sim::I2cBusFake *b = nullptr;
  esp_err_t e = i2cCheck(dev, data, len, timeoutMs, &b);
  if (e != ESP_OK) return e;
  uint64_t now = sim::now_us();
  sim::Sht31Model &m = b->sht31;
  m.reads++;
  if (dev->addr != b->addr || !m.present || m.failRead || len != 6 || !b->measurePending || now < b->measureAt) { b->nackReads++; return ESP_FAIL; }
  b->measurePending = false;
  uint16_t rt, rh;
  if (m.useRawOverride) {
    rt = m.rawTemp;
    rh = m.rawHum;
  } else {
    double t = m.temp < -45 ? -45 : (m.temp > 130 ? 130 : m.temp);
    double h = m.hum < 0 ? 0 : (m.hum > 100 ? 100 : m.hum);
    rt = (uint16_t)std::lround((t + 45.0) / 175.0 * 65535.0);
    rh = (uint16_t)std::lround(h / 100.0 * 65535.0);
  }
  data[0] = rt >> 8; data[1] = rt & 0xff; data[2] = sim::sensirion_crc8(data, 2, 0xFF);
  data[3] = rh >> 8; data[4] = rh & 0xff; data[5] = sim::sensirion_crc8(data + 3, 2, 0xFF);
  if (m.badCrc) { data[2] ^= 0x5a; }
  return ESP_OK;
}

// ======================================================================= mbedtls
void mbedtls_aes_init(mbedtls_aes_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
void mbedtls_aes_free(mbedtls_aes_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
int mbedtls_aes_setkey_enc(mbedtls_aes_context *ctx, const unsigned char *key, unsigned int bits) {
  if (bits != 128) return -0x0020;       // MBEDTLS_ERR_AES_INVALID_KEY_LENGTH
  memcpy(ctx->key, key, 16);
  ctx->keyed = 1;
  return 0;
}
int mbedtls_aes_crypt_ecb_enc(mbedtls_aes_context *ctx, const unsigned char in[16], unsigned char out[16]) {
  if (!ctx->keyed) return -1;
  aes128::AES128 a;
  a.setKey(ctx->key);
  a.encryptBlock(out, in);
  return 0;
}
int mbedtls_aes_crypt_ctr(mbedtls_aes_context *ctx, size_t length, size_t *nc_off, unsigned char nonce_counter[16],
                          unsigned char stream_block[16], const unsigned char *input, unsigned char *output) {
  if (!ctx->keyed) return -1;
  aes128::AES128 a;
  a.setKey(ctx->key);
  size_t n = *nc_off;
  while (length--) {
    if (n == 0) {
      a.encryptBlock(stream_block, nonce_counter);
      for (int i = 16; i > 0; i--) if (++nonce_counter[i - 1] != 0) break;
    }
    *output++ = (unsigned char)(*input++ ^ stream_block[n]);
    n = (n + 1) & 0x0F;
  }
  *nc_off = n;
  return 0;
}

const mbedtls_cipher_info_t *mbedtls_cipher_info_from_type(mbedtls_cipher_type_t type) {
  static const mbedtls_cipher_info_t aes128ecb = {MBEDTLS_CIPHER_AES_128_ECB};
  return type == MBEDTLS_CIPHER_AES_128_ECB ? &aes128ecb : nullptr;
}

static void leftShiftXorRb(uint8_t *out, const uint8_t *in) {
  uint8_t msb = in[0] & 0x80, carry = 0;
  for (int i = 15; i >= 0; i--) {
    uint8_t nc = (in[i] & 0x80) ? 1 : 0;
    out[i] = (uint8_t)((in[i] << 1) | carry);
    carry = nc;
  }
  if (msb) out[15] ^= 0x87;
}

int mbedtls_cipher_cmac(const mbedtls_cipher_info_t *info, const unsigned char *key, size_t keylen,
                        const unsigned char *input, size_t ilen, unsigned char *output) {
  if (info == nullptr || keylen != 128) return -1;
  aes128::AES128 a;
  a.setKey(key);
  uint8_t L[16], zero[16] = {0}, K1[16], K2[16];
  a.encryptBlock(L, zero);
  leftShiftXorRb(K1, L);
  leftShiftXorRb(K2, K1);
  size_t n = ilen == 0 ? 1 : (ilen + 15) / 16;
  bool complete = ilen != 0 && ilen % 16 == 0;
  uint8_t last[16];
  if (complete) {
    memcpy(last, input + (n - 1) * 16, 16);
    for (int i = 0; i < 16; i++) last[i] ^= K1[i];
  } else {
    size_t rem = ilen - (n - 1) * 16;
    memset(last, 0, 16);
    if (rem) memcpy(last, input + (n - 1) * 16, rem);
    last[rem] = 0x80;
    for (int i = 0; i < 16; i++) last[i] ^= K2[i];
  }
  uint8_t X[16] = {0}, Y[16];
  for (size_t i = 0; i + 1 < n; i++) {
    for (int j = 0; j < 16; j++) Y[j] = X[j] ^ input[i * 16 + j];
    a.encryptBlock(X, Y);
  }
  for (int j = 0; j < 16; j++) Y[j] = X[j] ^ last[j];
  a.encryptBlock(output, Y);
  return 0;
}

// ======================================================================= нулиране
void sim_periph_reset(bool keepPersistent) {
  if (!keepPersistent) { sim::nvs.clear(); sim::airLog.clear(); }
  sim::gpio.reset();
  sim::radio.fullReset();
  sim::spi = sim::SpiFake();
  if (!keepPersistent) sim::i2c = sim::I2cFake();          // сензорите са в "света": оцеляват при рестарт, не и при reset()
  for (int i = 0; i < 2; i++) {                            // контролерите се нулират при рестарт (шини и устройства изчезват)
    sim::I2cBusFake &b = sim::i2c.bus[i];
    b.created = b.deviceAdded = false;
    b.nackReads = b.nackWrites = 0;
    b.measurePending = false;
    g_i2cDev[i].alive = false;
  }
  // радио: RST пинът рестартира чипа (ниско -> високо), NSS спадането го събужда от sleep, BUSY зависи от времето
  sim::gpio.pins[LORA_RST].onChange = [](int lv) {
    if (lv == 1) sim::radio.hardReset();
  };
  sim::gpio.pins[LORA_NSS].onChange = [](int lv) { sim::radio.onNss(lv); };
  sim::gpio.pins[LORA_BUSY].dynamicIn = [] { return sim::radio.busyLevel(); };
}
