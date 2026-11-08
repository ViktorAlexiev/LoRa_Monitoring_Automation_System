// Драйвер за тестовете на repeater-а: пуска РЕАЛНИЯ repeater.ino (setup/loop), radio_io.cpp, dedup.cpp,
// power_mgmt.cpp и config_storage.cpp с фалшиви LoRa/NVS/сън. Python тестът (test_repeater.py) подава
// пакети и чете какво и на коя честота е препратено.
//
//   repeater_rx <M_ID> <KEY_HEX32> <FREQ_RX_HZ> <FREQ_TX_HZ> <SF> <BW_HZ>
//   RX <hex>        входящ пакет (на RX честотата)
//   LOOP <n>        n x loop() (будно: +1 ms на обиколка; сънят напредва времето)
//   STEP <ms>       изминава време будно, после loop()
//   BUSY <0|1>      зает/свободен канал (CAD)
//   STATS           статистика на съня и честотата, на която е радиото в момента
// Изход: >> <команда>, после:
//   TX @<ms> f=<Hz> <hex>    и (ако е heartbeat) TXDEC HB sender=<id>
//   STATS sleeps=<n> ticks=<n> t_ms=<n> radio_hz=<Hz> cad=<n>
#include "esp32_test_helpers.h"
#include "power_mgmt.h"
#include <iostream>
#include <sstream>

#include "repeater/repeater.ino"

static std::string hex(const uint8_t *b, size_t n) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
  return s;
}
static std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}
static std::string idStr(const uint8_t *b, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && b[i]; i++) s += (char)b[i];
  return s;
}
static void advance(unsigned long ms) {
  g_fakeMillis += ms;
  g_fakeTimeUs += (uint64_t)ms * 1000ULL;
}

static size_t txPrinted = 0;

static void flushOutputs() {
  for (; txPrinted < g_lora.tx.size(); txPrinted++) {
    std::vector<uint8_t> &f = g_lora.tx[txPrinted];
    printf("TX @%lu f=%ld %s\n", g_fakeMillis, g_lora.txFreq[txPrinted], hex(f.data(), f.size()).c_str());
    uint8_t sender[CRYPTO_ID_LEN], pt[CRYPTO_MAX_PT], ptLen = 0;
    if (f.size() == CRYPTO_OVERHEAD &&
        cryptoParseWirePacket(f.data(), (uint8_t)f.size(), NETWORK_KEY, CRYPTO_TYPE_REPEATER_HB, sender, pt, &ptLen)) {
      printf("TXDEC HB sender=%s\n", idStr(sender, CRYPTO_ID_LEN).c_str());
    }
  }
  fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc < 7) { fprintf(stderr, "употреба: repeater_rx M_ID KEY RX TX SF BW\n"); return 2; }
  resetFakes();
  std::vector<uint8_t> key = unhex(argv[2]);
  nvsSetStr("id", argv[1]);
  nvsSetBytes("key", key.data(), key.size());
  nvsSetU32("freq", (uint32_t)strtoul(argv[3], nullptr, 10));
  nvsSetU32("freqtx", (uint32_t)strtoul(argv[4], nullptr, 10));
  nvsSetU8("sf", (uint8_t)atoi(argv[5]));
  nvsSetU32("bw", (uint32_t)strtoul(argv[6], nullptr, 10));
  g_fakeMillis = 5000;
  g_fakeTimeUs = 1000000ULL + 5000ULL * 1000ULL;
  setup();
  printf("READY radio_hz=%ld\n", g_lora.freq);
  flushOutputs();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    printf(">> %s\n", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "RX") {
      std::string h; in >> h;
      g_lora.rx.push_back(unhex(h));
    } else if (cmd == "LOOP") {
      int n; in >> n;
      for (int i = 0; i < n; i++) { advance(1); loop(); flushOutputs(); }
    } else if (cmd == "STEP") {
      unsigned long ms; in >> ms;
      advance(ms);
      loop();
    } else if (cmd == "BUSY") {
      int b; in >> b;
      g_lora.channelBusy = b != 0;
    } else if (cmd == "STATS") {
      printf("STATS sleeps=%d ticks=%d t_ms=%lu radio_hz=%ld cad=%d\n", g_lightSleepCount, (int)wdt_ticks,
             g_fakeMillis, g_lora.freq, g_lora.cadCount);
    }
    flushOutputs();
  }
  return 0;
}
