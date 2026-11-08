// Драйвер за тестовете на LoRa executor-а: пуска РЕАЛНИЯ executor.ino (setup/loop), radio_io.cpp,
// queues.cpp, consumers.cpp, power_mgmt.cpp и config_storage.cpp с фалшиви LoRa/NVS/сън/GPIO.
// Python тестът (test_executor_lora.py) играе ролята на Gateway: подава команди, чете изпратените пакети.
//
//   executor_rx <M_ID> <KEY_HEX32> <FREQ_HZ> <SF> <BW_HZ> <V01:25,V02:26,...>
//   MKCMD <target> <C_ID> <comHex> <ctr>       команда от Gateway (криптирана с реалния crypto)
//   MKSTATEREQ <target> <ctr>                  заявка за състояния
//   MKRESPACK <target> <ctr>                   потвърждение на restart state response
//   SETKEY <hex32>                             с кой ключ се строят следващите входящи пакети
//   TAMPER <pos>                               обръща бит в последния построен входящ пакет
//   RAW <hex>                                  суров входящ пакет
//   LOOP <n>                                   n x loop() (будно: +1 ms на обиколка; сънят напредва времето)
//   STEP <ms>                                  изминава време будно, после loop()
//   WAKE <us>                                  следващият лек сън се прекъсва от DIO0 след толкова µs
//   BUSY <0|1>                                 зает/свободен канал (CAD)
//   PINS / STATS                               състояние на пиновете / статистика на съня
// Изход: >> <команда>, после:
//   TX @<ms> <hex>  и  TXDEC <ACK|HB|STATE_RESP|STATE_RESP_RESTART> sender=<id> pt=<hex>
//   PINS 25=1 26=0 ...   /   STATS sleeps=<n> gpiowakes=<n> ticks=<n> t_ms=<n>
#include "esp32_test_helpers.h"
#include "power_mgmt.h"
#include <iostream>
#include <sstream>

#include "executor/executor.ino"

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
static void pad(uint8_t *dst, const std::string &s, size_t n) {
  memset(dst, 0, n);
  memcpy(dst, s.c_str(), s.size() < n ? s.size() : n);
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

static uint8_t buildKey[CRYPTO_KEY_LEN];
static size_t txPrinted = 0;
static std::vector<int> pinList;

static void flushOutputs() {
  for (; txPrinted < g_lora.tx.size(); txPrinted++) {
    std::vector<uint8_t> &f = g_lora.tx[txPrinted];
    printf("TX @%lu %s\n", g_fakeMillis, hex(f.data(), f.size()).c_str());
    struct { const char *name; uint8_t type; } types[4] = {
      {"ACK", CRYPTO_TYPE_EXEC_ACK}, {"HB", CRYPTO_TYPE_EXEC_HB},
      {"STATE_RESP", CRYPTO_TYPE_STATE_RESP}, {"STATE_RESP_RESTART", CRYPTO_TYPE_STATE_RESP_RESTART}};
    for (auto &t : types) {
      uint8_t sender[CRYPTO_ID_LEN], pt[CRYPTO_MAX_PT], ptLen = 0;
      if (cryptoParseWirePacket(f.data(), (uint8_t)f.size(), NETWORK_KEY, t.type, sender, pt, &ptLen)) {
        printf("TXDEC %s sender=%s pt=%s\n", t.name, idStr(sender, CRYPTO_ID_LEN).c_str(), hex(pt, ptLen).c_str());
        break;
      }
    }
  }
  fflush(stdout);
}

static void buildDownlink(const std::string &target, uint8_t type, const std::string &sender, unsigned ctr,
                          const uint8_t *pt, uint8_t ptLen) {
  uint8_t frame[6 + CRYPTO_OVERHEAD + 8];
  pad(frame, target, 6);
  uint8_t s6[6];
  pad(s6, sender, 6);
  uint8_t n = cryptoBuildWirePacket(frame + 6, buildKey, type, s6, ctr, pt, ptLen);
  g_lora.rx.push_back(std::vector<uint8_t>(frame, frame + 6 + n));
}

int main(int argc, char **argv) {
  if (argc < 7) { fprintf(stderr, "употреба: executor_rx M_ID KEY FREQ SF BW V01:25,...\n"); return 2; }
  resetFakes();
  std::vector<uint8_t> key = unhex(argv[2]);
  nvsSetStr("id", argv[1]);
  nvsSetBytes("key", key.data(), key.size());
  nvsSetU32("freq", (uint32_t)strtoul(argv[3], nullptr, 10));
  nvsSetU8("sf", (uint8_t)atoi(argv[4]));
  nvsSetU32("bw", (uint32_t)strtoul(argv[5], nullptr, 10));
  {
    std::string list = argv[6];
    int i = 0;
    std::istringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
      size_t c = item.find(':');
      if (c == std::string::npos) continue;
      char kI[8], kP[8];
      snprintf(kI, sizeof(kI), "c%di", i);
      snprintf(kP, sizeof(kP), "c%dp", i);
      nvsSetStr(kI, item.substr(0, c));
      nvsSetU8(kP, (uint8_t)atoi(item.substr(c + 1).c_str()));
      pinList.push_back(atoi(item.substr(c + 1).c_str()));
      i++;
    }
    nvsSetU8("nc", (uint8_t)i);
  }
  memcpy(buildKey, key.data(), CRYPTO_KEY_LEN);
  g_fakeMillis = 5000;
  g_fakeTimeUs = 1000000ULL + 5000ULL * 1000ULL;
  setup();   // РЕАЛНИЯТ setup(): пинове LOW, радио, restart state response в опашката
  printf("READY\n");
  flushOutputs();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    printf(">> %s\n", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "MKCMD") {
      std::string target, c, com; unsigned ctr;
      in >> target >> c >> com >> ctr;
      uint8_t pt[6];
      pad(pt, c, 4);
      pt[4] = (uint8_t)strtol(com.c_str(), nullptr, 16);
      pt[5] = 0xFF;
      buildDownlink(target, CRYPTO_TYPE_GW_CMD, "G001", ctr, pt, 6);
    } else if (cmd == "MKSTATEREQ") {
      std::string target; unsigned ctr;
      in >> target >> ctr;
      buildDownlink(target, CRYPTO_TYPE_STATE_REQ, "G001", ctr, NULL, 0);
    } else if (cmd == "MKRESPACK") {
      std::string target; unsigned ctr;
      in >> target >> ctr;
      buildDownlink(target, CRYPTO_TYPE_STATE_RESP_ACK, "G001", ctr, NULL, 0);
    } else if (cmd == "SETKEY") {
      std::string h; in >> h;
      std::vector<uint8_t> k = unhex(h);
      memcpy(buildKey, k.data(), CRYPTO_KEY_LEN);
    } else if (cmd == "TAMPER") {
      size_t pos; in >> pos;
      if (!g_lora.rx.empty() && pos < g_lora.rx.back().size()) g_lora.rx.back()[pos] ^= 0x01;
    } else if (cmd == "RAW") {
      std::string h; in >> h;
      g_lora.rx.push_back(unhex(h));
    } else if (cmd == "LOOP") {
      int n; in >> n;
      for (int i = 0; i < n; i++) { advance(1); loop(); flushOutputs(); }
    } else if (cmd == "STEP") {
      unsigned long ms; in >> ms;
      advance(ms);
      loop();
    } else if (cmd == "WAKE") {
      unsigned long long us; in >> us;
      g_gpioWakeAfterUs = us;
    } else if (cmd == "BUSY") {
      int b; in >> b;
      g_lora.channelBusy = b != 0;
    } else if (cmd == "PINS") {
      printf("PINS");
      for (int p : pinList) printf(" %d=%d", p, (int)g_pinState[p]);
      printf("\n");
    } else if (cmd == "STATS") {
      printf("STATS sleeps=%d gpiowakes=%d ticks=%d t_ms=%lu\n", g_lightSleepCount, g_lightSleepGpioWakes,
             (int)wdt_ticks, g_fakeMillis);
    }
    flushOutputs();
  }
  return 0;
}
