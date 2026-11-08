// Драйвер: РЕАЛНИЯТ ESP-IDF gateway. Протокол като на стария gateway_rx (само каквото е нужно за съвместимост):
//   compat_gateway <GW_ID> <KEY_HEX32> <FREQ> <SF> <BW>
//   RX <hex>                  входящ LoRa пакет          MQTT <topic> <json>   съобщение от брокера
//   TICK <ms>                 напредва времето           BUSY <0|1>            зает/свободен канал
// Изход след всяка команда:  PUB @<ms> <topic> <retained> <payload>   и   TX @<ms> <hex>
#include "compat_common.h"

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);
static size_t s_pubDone = 0, s_txDone = 0;

static void flush() {
  for (; s_pubDone < sim::broker.published.size(); s_pubDone++) {
    const sim::MqttPub &p = sim::broker.published[s_pubDone];
    printf("PUB @%lu %s %d %s\n", (unsigned long)(sim::now_us() / 1000), p.topic.c_str(), p.retain ? 1 : 0, p.payload.c_str());
  }
  for (; s_txDone < sim::airLog.size(); s_txDone++) {
    printf("TX @%lu %s\n", (unsigned long)(sim::airLog[s_txDone].tStart / 1000), hexs(sim::airLog[s_txDone].data).c_str());
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 6) { fprintf(stderr, "употреба: compat_gateway GW_ID KEY FREQ SF BW\n"); return 2; }
  sim::reset();
  nvsCommon(argv[1], argv[2], strtoul(argv[3], 0, 10), atoi(argv[4]), strtoul(argv[5], 0, 10));
  sim::nvs.setStr("wssid", "FarmNet");
  sim::nvs.setStr("wpass", "pw");
  sim::nvs.setStr("mqip", "192.168.1.10");
  sim::nvs.setStr("mqport", "1883");
  long freq = (long)strtoul(argv[3], 0, 10);
  sim::boot(app_main);
  sim::run_for_ms(3000);
  printf("READY subs=%d\n", (int)sim::broker.subs.size());
  for (auto &s : sim::broker.subs) printf("SUB %s %d\n", s.topic.c_str(), s.qos);
  flush();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    printf(">> %s\n", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "RX") {
      std::string h; in >> h;
      sim::radio.deliver(unhex(h), freq, -50, 7.5f);
      sim::run_for_ms(100);
    } else if (cmd == "MQTT") {
      std::string topic; in >> topic;
      std::string payload; std::getline(in, payload);
      if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
      sim::broker.deliver(topic, payload);
      sim::run_for_ms(100);
    } else if (cmd == "TICK") {
      unsigned long ms; in >> ms;
      sim::run_for_ms((uint32_t)ms);
    } else if (cmd == "BUSY") {
      int b; in >> b;
      sim::radio.busyUntil = b ? sim::now_us() + 600000000ULL : 0;
    }
    flush();
  }
  sim::reset();
  return 0;
}
