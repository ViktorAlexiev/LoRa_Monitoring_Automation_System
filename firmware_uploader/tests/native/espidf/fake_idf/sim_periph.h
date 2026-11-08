// Периферия на симулатора - тестовата страна (виж sim.h).
#ifndef SIM_PERIPH_H
#define SIM_PERIPH_H

#include <stdint.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <deque>

namespace sim {

// ============================================================ NVS
struct NvsFake {
  std::map<std::string, std::pair<int, std::vector<uint8_t>>> data;   // "ns/key" -> (тип, байтове)
  std::vector<std::string> namespaces;
  std::map<std::string, int> writesPerKey;
  int commits = 0;
  int totalWrites = 0;
  bool failWrites = false;                 // всеки запис връща ESP_FAIL
  std::deque<int> initResults;             // резултати на следващите nvs_flash_init() (след това ESP_OK)
  int eraseCalls = 0;

  void clear();
  bool has(const char *key, const char *ns = "cfg") const;
  std::string str(const char *key, const char *ns = "cfg") const;     // "<нищо>" ако липсва
  int64_t num(const char *key, const char *ns = "cfg") const;         // u8/u32; -1 ако липсва
  std::vector<uint8_t> blob(const char *key, const char *ns = "cfg") const;
  void setStr(const char *key, const std::string &v, const char *ns = "cfg");
  void setU8(const char *key, uint8_t v, const char *ns = "cfg");
  void setU32(const char *key, uint32_t v, const char *ns = "cfg");
  void setBlob(const char *key, const void *p, size_t n, const char *ns = "cfg");
  int writes(const char *key, const char *ns = "cfg") const;
};
extern NvsFake nvs;

// ============================================================ GPIO
struct GpioPin {
  int mode = 0;               // gpio_mode_t
  int level = 0;              // изходен latch
  int ext = 0;                // външно зададено ниво (вход)
  int intrType = 0;
  bool intrEnabled = false;
  bool wakeEnabled = false;
  int wakeType = 0;
  void (*isr)(void *) = nullptr;
  void *isrArg = nullptr;
  int isrCalls = 0;
  int outputWrites = 0;
  std::vector<int> history;   // нивата, записани от прошивката (за проверка на последователността)
  std::function<void(int)> onChange;   // слушател (напр. RST и NSS на радиото)
  std::function<int()> dynamicIn;      // входно ниво, което зависи от времето (BUSY на радиото)
};
struct GpioFake {
  GpioPin pins[64];
  bool isrServiceInstalled = false;
  int isrServiceInstalls = 0;
  int read(int pin);                       // нивото, което прошивката вижда
  void drive(int pin, int level);          // външно задаване на вход (с изчисляване на прекъсването)
  void reset();
};
extern GpioFake gpio;

// ============================================================ SX1262
// Модел на SX1262 върху SPI командите на чипа (SX1261/2 datasheet rev 2.1): BUSY пин с време, DIO1 по маска на
// прекъсванията, режими (sleep/standby/RX/TX/CAD) във виртуално време, регистри, буфер от 256 B. Опкодовете са
// описани НЕЗАВИСИМО от RadioLib (виж test_radio.cpp - сверяват се с константите на библиотеката).
struct RadioFrame {
  uint64_t tStart = 0, tEnd = 0;
  long freqHz = 0;
  int sf = 0;
  long bwHz = 0;
  int crDenom = 0;
  bool crc = false;
  bool ldro = false;
  int preamble = 0;
  int txPowerDbm = 0;
  uint16_t syncWord = 0;
  std::vector<uint8_t> data;
};

struct Sx126xModel {
  // ---- управление от теста ----
  bool present = true;                     // чипът отговаря по SPI (false: MISO мълчи)
  bool stuckTx = false;                    // чипът никога не докладва TxDone
  bool stuckCad = false;                   // CAD никога не приключва
  bool busyStuck = false;                  // BUSY остава висок завинаги (заключен чип)
  bool xtalModule = false;                 // модул с кварц (без TCXO): SetDio3AsTcxoCtrl води до грешка при старт на осцилатора
  int failCommands = 0;                    // следващите толкова команди връщат статус "failure to execute"
  uint64_t busyUntil = 0;                  // удобство: каналът е "зает" (CAD го открива) до този момент
  std::function<bool(uint64_t)> busy;      // или функция: true = активност на канала в момента
  std::function<void(uint8_t)> onCommand;  // наблюдение: всяка изпълнена команда (опкод)

  // ---- журнали и броячи ----
  std::vector<RadioFrame> tx;              // завършили предавания
  std::vector<uint8_t> ops;                // опкодовете на изпълнените команди (последователности)
  int abortedTx = 0, cadAborted = 0;
  int cadCount = 0;
  int missedRx = 0, rxDelivered = 0, rxOverrun = 0;
  int modeViolations = 0;                  // команди, недопустими в текущия режим (напр. смяна на параметри по време на RX)
  int protocolErrors = 0;                  // невалидни последователности/неизвестни опкодове/лоши дължини
  int csViolations = 0;                    // SPI обмен при вдигнат NSS
  int sleepViolations = 0;                 // команда към заспал чип (без събуждане с NSS)
  int busyViolations = 0;                  // SPI команда при висок BUSY (драйверът не е изчакал чипа)
  int resets = 0;                          // хардуерни рестарти през RST
  int spiTransfers = 0;
  int wakeups = 0;                         // събуждания от sleep през NSS

  // ---- текущо предаване (за системната симулация) ----
  bool txActive = false;
  uint64_t txStart = 0, txEnd = 0;
  long txFreq = 0;
  std::vector<uint8_t> txData;

  enum Mode { SLEEP = 0, STBY_RC = 1, STBY_XOSC = 2, FS = 3, RX = 5, TX = 6, CAD = 7 };

  Sx126xModel() { hardReset(false); }
  void hardReset(bool count = true);       // RST пинът: конфигурацията се губи, режим STBY_RC, BUSY ~3 ms
  void fullReset();                        // и журналите
  void onNss(int level);                   // NSS пинът: спадане събужда от sleep

  // ---- наблюдение на конфигурацията ----
  int mode() const { return (int)mode_; }
  bool listening() const { return mode_ == RX; }
  uint8_t packetType() const { return packetType_; }          // 0 = GFSK, 1 = LoRa
  long freqHz() const;
  int sf() const { return sf_; }
  long bwHz() const;
  int crDenom() const { return cr_ + 4; }
  bool ldro() const { return ldro_ != 0; }
  bool crcOn() const { return crcType_ != 0; }
  bool explicitHeader() const { return hdrType_ == 0; }
  int preamble() const { return preamble_; }
  int txPowerDbm() const { return (int8_t)power_; }
  uint16_t syncWord() const { return (uint16_t)((reg(0x0740) << 8) | reg(0x0741)); }
  int ocpMilliamps() const { return (int)(reg(0x08E7) * 5 / 2); }
  bool dio2RfSwitch() const { return dio2Rf_; }
  bool tcxoEnabled() const { return tcxoOn_; }                // DIO3 захранва TCXO (SetDio3AsTcxoCtrl)
  int tcxoVoltageCode() const { return tcxoOn_ ? tcxoCode_ : -1; }   // кодът на напрежението (0 = 1.6 V ... 7 = 3.3 V)
  int regulatorMode() const { return regulator_; }            // 0 = само LDO, 1 = DC-DC
  uint16_t irqStatus() const { return irq_; }
  uint16_t dio1Mask() const { return dio1Mask_; }
  int cadSymbols() const { return cadSym_; }
  int dio1Level() const;
  int busyLevel() const;
  uint8_t reg(uint16_t a) const;
  double airtimeUs(int payloadLen) const;                     // независима формула (Semtech AN1200.13) върху регистрите
  double symbolUs() const;

  // за тестове: чипът вдига прекъсване (само ако е разрешено в IrqMask), напр. HeaderErr без RxDone
  void injectIrq(uint16_t bits) { raise(bits); updateDio(); }

  // ---- вход от "ефира" ----
  // Доставя пакет, ако радиото слуша на тази честота (freqHz == 0 -> без проверка на честотата).
  bool deliver(const std::vector<uint8_t> &frame, long freqHz = 0, int rssi = -60, float snr = 8.0f, bool crcOk = true);

  // ---- вътрешни ----
  void xfer(const uint8_t *tx, uint8_t *rx, size_t n);
  void tick();

 private:
  void raise(uint16_t bits);
  void updateDio();
  void command(const uint8_t *tx, uint8_t *rx, size_t n);
  void setMode(Mode m);
  void startTx();
  void startCad();
  void setBusy(uint64_t us);
  uint8_t status() const;
  void loadDefaults();
  bool xoscOk() const;

  Mode mode_ = STBY_RC;
  bool configLost_ = false;
  uint8_t packetType_ = 0;
  uint8_t sf_ = 0, bwRaw_ = 0, cr_ = 0, ldro_ = 0;
  uint16_t preamble_ = 0;
  uint8_t hdrType_ = 0, payloadLen_ = 0, crcType_ = 0, invertIq_ = 0;
  uint32_t frf_ = 0;
  uint8_t power_ = 0, ramp_ = 0;
  uint8_t paDuty_ = 0, paHpMax_ = 0, paDevice_ = 0, paLut_ = 0;
  uint16_t irqMask_ = 0, dio1Mask_ = 0, dio2Mask_ = 0, dio3Mask_ = 0, irq_ = 0;
  uint8_t txBase_ = 0, rxBase_ = 0;
  uint8_t buf_[256];
  uint8_t rxLen_ = 0, rxStart_ = 0;
  uint8_t pktRssi_ = 0, pktSnr_ = 0, pktSigRssi_ = 0;
  uint8_t cadSym_ = 0, cadPeak_ = 0, cadMin_ = 0, cadExit_ = 0;
  uint32_t cadTimeoutRaw_ = 0;
  uint32_t rxTimeoutRaw_ = 0, txTimeoutRaw_ = 0;
  uint8_t fallback_ = 0x20;                // STDBY_RC
  bool dio2Rf_ = false;
  uint8_t regulator_ = 0;
  uint8_t tcxoCode_ = 0;
  bool tcxoOn_ = false;
  uint32_t tcxoDelayRaw_ = 0;
  uint16_t deviceErrors_ = 0;
  uint8_t symbTimeout_ = 0;
  uint8_t sleepCfg_ = 0;
  uint64_t cadEnd_ = 0, rxEnd_ = 0, busyEnd_ = 0, txTimeoutAt_ = 0;
  bool cadActive_ = false;
  bool xoscRunning_ = false;
  uint8_t regs_[0x1000];
  int lastDio1_ = 0;
  bool wokeNow_ = false;
};
extern Sx126xModel radio;
extern std::vector<RadioFrame> airLog;      // ВСИЧКИ предавания (оцелява reboot(); чисти се от reset())

// ============================================================ I2C сензори (НОВ драйвер i2c_master)
struct Sht31Model {
  bool present = true;
  bool failRead = false;
  bool badCrc = false;
  bool useRawOverride = false;             // връща суровите rawTemp/rawHum без преобразуване (стойности извън обхвата)
  uint16_t rawTemp = 0, rawHum = 0;
  double temp = 25.0, hum = 50.0;
  int resets = 0, measurements = 0, reads = 0;
};
struct I2cBusFake {
  Sht31Model sht31;                        // сензорът на тази шина
  uint8_t addr = 0x44;                     // адресът, на който сензорът отговаря
  bool failCreate = false;                 // i2c_new_master_bus връща грешка
  bool failAddDevice = false;              // i2c_master_bus_add_device връща грешка
  bool created = false;                    // шината е вдигната в момента
  bool deviceAdded = false;                // устройство е добавено в момента
  int sda = -1, scl = -1;
  uint32_t hz = 0;
  bool pullup = false;
  int busesCreated = 0, devicesAdded = 0;  // общо за теста
  int nackWrites = 0, nackReads = 0;
  bool measurePending = false;
  uint64_t measureAt = 0;
};
struct I2cFake {
  I2cBusFake bus[2];                       // порт 0 = почва, порт 1 = въздух (виж board_pins.h)
  int usageErrors = 0;                     // грешна употреба на API (двойно създаване, изтриване със заредено устройство ...)
  Sht31Model &soil() { return bus[0].sht31; }
  Sht31Model &air() { return bus[1].sht31; }
  int liveBuses() const { return (bus[0].created ? 1 : 0) + (bus[1].created ? 1 : 0); }
  int liveDevices() const { return (bus[0].deviceAdded ? 1 : 0) + (bus[1].deviceAdded ? 1 : 0); }
};
extern I2cFake i2c;

// ============================================================ SPI
struct SpiFake {
  int busInits = 0;
  int busFrees = 0;
  int devices = 0;
  int acquires = 0, releases = 0;          // spi_device_acquire_bus / release_bus (трябва да са в баланс)
  int host = -1;
  int transfers = 0;
  int clockHz = 0;
  int mosi = -1, miso = -1, sclk = -1, cs = -1;
  int mode = -1;
  int unknownDevice = 0;
};
extern SpiFake spi;

// Изчислява Sensirion CRC-8 (poly 0x31, init 0xFF за SHT31; init 0 за SHT21)
uint8_t sensirion_crc8(const uint8_t *d, size_t n, uint8_t init);

}  // namespace sim

#endif
