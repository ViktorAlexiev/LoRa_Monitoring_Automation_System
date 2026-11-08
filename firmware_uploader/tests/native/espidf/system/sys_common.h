/*
  Системна симулация: всяко устройство е ОТДЕЛЕН процес с РЕАЛНОТО приложение (app_main) върху симулатора.
  Python координаторът (test_system.py) държи общия виртуален час и общия радио ефир и свързва процесите.

  Протокол (stdin -> stdout, по един ред):
    STEP <us>            напредва точно толкова виртуално време; отговор:
                           S <tStart_us> <tEnd_us> <freqHz> <hex>   предаване (веднъж, още при началото му)
                           P <topic> <qos> <retain> <payload>       публикувано в MQTT (gateway, Wi-Fi executor)
                           N <следващо_събитие_абс_us>   T <now_us>   END
    RX <freqHz> <crcok> <hex>   пакет от ефира, доставен СЕГА            -> R <0|1>  END
    MQ <topic> <payload>        съобщение от брокера към устройството     -> R <0|1>  END
    ENV <freqHz> <t0> <t1>      в ефира има предаване (за CAD)
    SET <ключ> <стойност>       среда (сензори); REBOOT = студен рестарт; STAT = статистика; LOG <текст> = брой логове
*/
#ifndef SYS_COMMON_H
#define SYS_COMMON_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim.h"
#include "sim_net.h"
#include "sim_periph.h"
#include "agro_radio.h"

extern "C" void app_main(void);

// Какво "чува" устройството в ефира (координаторът го праща като ENV): CAD открива активност само ако на
// честотата, на която радиото е в момента, в момента на проверката има предаване от чуваем източник.
struct EnvEntry { long freq; uint64_t t0, t1; };
static std::vector<EnvEntry> g_env;
static bool envBusy(uint64_t now) {
  long f = sim::radio.freqHz();
  for (auto &e : g_env) if (labs(e.freq - f) < 15000 && e.t0 <= now && now < e.t1) return true;
  return false;
}
static int s_sysHook = (sim::add_reset_hook([] { radio_ram_reset(); sim::radio.busy = envBusy; }), 0);

static inline std::string hexs(const std::vector<uint8_t> &v) {
  static const char *d = "0123456789abcdef";
  std::string s;
  for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
  return s;
}
static inline std::vector<uint8_t> unhex(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

struct Args {
  std::map<std::string, std::string> kv;
  Args(int argc, char **argv) { for (int i = 1; i + 1 < argc; i += 2) kv[argv[i] + 2] = argv[i + 1]; }   // --ключ стойност
  std::string get(const char *k, const char *def = "") const { auto it = kv.find(k); return it == kv.end() ? def : it->second; }
  long num(const char *k, long def = 0) const { auto it = kv.find(k); return it == kv.end() ? def : strtol(it->second.c_str(), nullptr, 10); }
  bool has(const char *k) const { return kv.count(k) > 0; }
};

// NVS както би го записала config стъпката
static inline void sysNvs(const Args &a) {
  sim::nvs.setStr("id", a.get("id"));
  std::vector<uint8_t> key = unhex(a.get("key", "00112233445566778899aabbccddeeff"));
  sim::nvs.setBlob("key", key.data(), key.size());
  sim::nvs.setU32("freq", (uint32_t)a.num("freq", 433000000));
  if (a.has("freqtx")) sim::nvs.setU32("freqtx", (uint32_t)a.num("freqtx"));
  sim::nvs.setU8("sf", (uint8_t)a.num("sf", 7));
  sim::nvs.setU32("bw", (uint32_t)a.num("bw", 125000));
  if (a.has("consumers")) {
    std::istringstream ss(a.get("consumers"));
    std::string item;
    int i = 0;
    while (std::getline(ss, item, ',')) {
      size_t c = item.find(':');
      char ki[8], kp[8];
      snprintf(ki, sizeof(ki), "c%di", i);
      snprintf(kp, sizeof(kp), "c%dp", i);
      sim::nvs.setStr(ki, item.substr(0, c));
      sim::nvs.setU8(kp, (uint8_t)atoi(item.substr(c + 1).c_str()));
      i++;
    }
    sim::nvs.setU8("nc", (uint8_t)i);
  }
  if (a.has("net")) {
    sim::nvs.setStr("wssid", "FarmNet");
    sim::nvs.setStr("wpass", "pw");
    sim::nvs.setStr("mqip", "192.168.1.10");
    sim::nvs.setStr("mqport", "1883");
  }
}

struct SysNode {
  std::function<void(uint64_t)> advance = [](uint64_t us) { sim::run_for_us(us); };
  std::function<uint64_t()> nextEvent = [] { return sim::next_event_us(); };
  std::function<void(const std::string &, const std::string &)> set;
  std::function<void()> reboot = [] { sim::reboot(false); sim::boot(app_main); };
  std::function<void(const std::vector<int> &)> printPins;     // за executor
  std::vector<int> pins;
};

static inline int sysMain(int argc, char **argv, SysNode &node) {
  setvbuf(stdout, NULL, _IONBF, 0);
  Args a(argc, argv);
  sim::reset();
  if (a.has("t0")) sim::set_now_us((uint64_t)strtoull(a.get("t0").c_str(), nullptr, 10));
  sim::seed_random((uint32_t)a.num("seed", 1));
  sim::radio.busy = envBusy;
  sysNvs(a);
  long rxFreqDefault = a.num("freq", 433000000);
  sim::boot(app_main);

  std::set<uint64_t> reported;
  size_t airDone = 0, pubDone = 0;
  auto report = [&]() {
    // предавания: най-напред завършилите, после текущото (още в ход)
    for (; airDone < sim::airLog.size(); airDone++) {
      const sim::RadioFrame &f = sim::airLog[airDone];
      if (reported.insert(f.tStart).second)
        printf("S %llu %llu %ld %s\n", (unsigned long long)f.tStart, (unsigned long long)f.tEnd, f.freqHz, hexs(f.data).c_str());
    }
    if (sim::radio.txActive && reported.insert(sim::radio.txStart).second)
      printf("S %llu %llu %ld %s\n", (unsigned long long)sim::radio.txStart, (unsigned long long)sim::radio.txEnd, sim::radio.txFreq,
             hexs(sim::radio.txData).c_str());
    for (; pubDone < sim::broker.published.size(); pubDone++) {
      const sim::MqttPub &p = sim::broker.published[pubDone];
      printf("P %s %d %d %s\n", p.topic.c_str(), p.qos, p.retain ? 1 : 0, p.payload.c_str());
    }
    uint64_t n = node.nextEvent();
    printf("N %llu\nT %llu\nEND\n", (unsigned long long)n, (unsigned long long)sim::now_us());
  };

  report();
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "STEP") {
      unsigned long long us;
      in >> us;
      node.advance(us);
      report();
    } else if (cmd == "RX") {
      long freq; int crc; std::string h;
      in >> freq >> crc >> h;
      bool ok = sim::radio.deliver(unhex(h), freq ? freq : rxFreqDefault, -60, 8.0f, crc != 0);
      node.advance(0);                              // задачите, събудени от прекъсването, работят веднага
      printf("R %d\n", ok ? 1 : 0);
      report();
    } else if (cmd == "MQ") {
      std::string topic, payload;
      in >> topic;
      std::getline(in, payload);
      if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
      bool ok = sim::broker.deliver(topic, payload);
      node.advance(0);
      printf("R %d\n", ok ? 1 : 0);
      report();
    } else if (cmd == "ENV") {
      long f; unsigned long long t0, t1;
      in >> f >> t0 >> t1;
      g_env.push_back({f, t0, t1});
      if (g_env.size() > 64) g_env.erase(g_env.begin(), g_env.begin() + 32);
      printf("END\n");
    } else if (cmd == "SET") {
      std::string k, v;
      in >> k >> v;
      if (node.set) node.set(k, v);
      printf("END\n");
    } else if (cmd == "REBOOT") {
      node.reboot();
      report();
    } else if (cmd == "PINS") {
      printf("PINS");
      for (int p : node.pins) printf(" %d=%d", p, sim::gpio.read(p));
      printf("\nEND\n");
    } else if (cmd == "STAT") {
      sim::PmStats pm = sim::pm_stats();
      printf("STAT idle_us=%llu total_us=%llu sleeps=%u gpio_wakes=%u timer_wakes=%u restarted=%d tx=%d rxdel=%d missed=%d overrun=%d\nEND\n",
             (unsigned long long)pm.idle_us, (unsigned long long)pm.total_us, pm.sleeps, pm.gpio_wakes, pm.timer_wakes,
             sim::restarted() ? 1 : 0, (int)sim::airLog.size(), sim::radio.rxDelivered, sim::radio.missedRx, sim::radio.rxOverrun);
    } else if (cmd == "LOG") {
      std::string needle;
      std::getline(in, needle);
      if (!needle.empty() && needle[0] == ' ') needle.erase(0, 1);
      printf("LOGCOUNT %d\nEND\n", sim::log_count(needle));
    } else if (cmd == "QUIT") {
      break;
    }
  }
  _Exit(0);   // нишките на симулатора не се чистят тук (процесът е на края си)
  return 0;
}

#endif
