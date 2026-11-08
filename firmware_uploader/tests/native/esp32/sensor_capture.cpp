// Драйвер за e2e теста: пуска РЕАЛНИЯ sensor.ino (setup/loop) с фалшиви SHT/I2C/LoRa/NVS и печата
// всеки изпратен LoRa пакет (hex). Python тестът (test_e2e.py) подава пакетите на gateway_rx.
//
//   sensor_capture <ID> <KEY_HEX32> <FREQ_HZ> <SF> <BW_HZ>        # чете команди от stdin
//   CYCLE <airT> <airH> <air_ok:0|1> <channel_busy:0|1>           # един loop() на сензора
//   -> FRAME <hex> CAD=<брой CAD> SLEEP=<секунди дълбок сън> BYTES=<дължина>
//
// Почвените стойности идват от fakelibs/Adafruit_SHT31.h (25.0 °C / 50.0 %), въздушните - от I2C
// отговорите на SHT21, които драйверът изчислява обратно от желаните температура/влажност.
#include "esp32_test_helpers.h"
#include "Wire.h"
#include <iostream>
#include <sstream>

#include "sensor/sensor.ino"   // -I <папката с firmware> (виж run_esp32.sh)

static std::string hex(const std::vector<uint8_t> &b) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (uint8_t c : b) { s += d[c >> 4]; s += d[c & 15]; }
  return s;
}
static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "употреба: sensor_capture ID KEY_HEX FREQ SF BW\n"); return 2; }
  resetFakes();
  std::vector<uint8_t> key = unhex(argv[2]);
  nvsSetStr("id", argv[1]);
  nvsSetBytes("key", key.data(), key.size());
  nvsSetU32("freq", (uint32_t)strtoul(argv[3], nullptr, 10));
  nvsSetU8("sf", (uint8_t)atoi(argv[4]));
  nvsSetU32("bw", (uint32_t)strtoul(argv[5], nullptr, 10));
  g_fakeMillis = 5000;
  setup();
  g_resetReason = ESP_RST_POWERON;

  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd != "CYCLE") continue;
    double airT, airH; int ok, busy;
    in >> airT >> airH >> ok >> busy;

    Wire.reset();
    if (ok) {
      // sht21_read: raw -> T = -46.85 + 175.72 * raw/65536 ; RH = -6 + 125 * raw/65536
      uint16_t rawT = (uint16_t)(((airT + 46.85) / 175.72) * 65536.0 + 0.5);
      uint16_t rawH = (uint16_t)(((airH + 6.0) / 125.0) * 65536.0 + 0.5);
      Wire.queueResponse({(uint8_t)(rawT >> 8), (uint8_t)(rawT & 0xFC), 0x00});
      Wire.queueResponse({(uint8_t)(rawH >> 8), (uint8_t)(rawH & 0xFC), 0x00});
    } else {
      Wire.queueEndTransmissionResult(1);   // температура: I2C грешка
      Wire.queueEndTransmissionResult(1);   // влажност: I2C грешка
    }
    g_lora.channelBusy = busy != 0;
    g_lora.cadCount = 0;
    g_lora.tx.clear();
    g_deepSleepRequested = false;

    loop();   // РЕАЛНИЯТ loop(): измерва, праща, заспива

    std::string frames;
    for (auto &f : g_lora.tx) frames += hex(f) + " ";
    printf("FRAME %s CAD=%d SLEEP=%lld BYTES=%d TXCOUNT=%d\n",
           g_lora.tx.empty() ? "-" : hex(g_lora.tx[0]).c_str(), g_lora.cadCount,
           (long long)(g_deepSleepUs / 1000000ULL), g_lora.tx.empty() ? 0 : (int)g_lora.tx[0].size(),
           (int)g_lora.tx.size());
    fflush(stdout);
  }
  return 0;
}
