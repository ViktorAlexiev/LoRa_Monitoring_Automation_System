// Драйвер: РЕАЛНИЯТ ESP-IDF LoRa executor (протокол като на стария executor_rx):
//   compat_executor <M_ID> <KEY_HEX32> <FREQ> <SF> <BW> <V01:4,P01:5,...>
//   RAW <hex>        входящ пакет            LOOP <n>   напредва n ms            PINS   състояние на пиновете
// Изход:  TX @<ms> <hex>   и   PINS 4=1 5=0
#include "compat_common.h"

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);
static size_t s_txDone = 0;

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 7) { fprintf(stderr, "употреба: compat_executor M_ID KEY FREQ SF BW V01:4,...\n"); return 2; }
  sim::reset();
  nvsCommon(argv[1], argv[2], strtoul(argv[3], 0, 10), atoi(argv[4]), strtoul(argv[5], 0, 10));
  std::vector<int> pins;
  {
    std::istringstream ss(argv[6]);
    std::string item;
    int i = 0;
    while (std::getline(ss, item, ',')) {
      size_t c = item.find(':');
      if (c == std::string::npos) continue;
      char ki[8], kp[8];
      snprintf(ki, sizeof(ki), "c%di", i);
      snprintf(kp, sizeof(kp), "c%dp", i);
      sim::nvs.setStr(ki, item.substr(0, c));
      sim::nvs.setU8(kp, (uint8_t)atoi(item.substr(c + 1).c_str()));
      pins.push_back(atoi(item.substr(c + 1).c_str()));
      i++;
    }
    sim::nvs.setU8("nc", (uint8_t)i);
  }
  long freq = (long)strtoul(argv[3], 0, 10);
  sim::boot(app_main);
  sim::run_for_ms(200);
  printf("READY\n");
  for (; s_txDone < sim::airLog.size(); s_txDone++) printf("TX @%lu %s\n", (unsigned long)(sim::airLog[s_txDone].tStart / 1000), hexs(sim::airLog[s_txDone].data).c_str());

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    printf(">> %s\n", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "RAW") {
      std::string h; in >> h;
      sim::radio.deliver(unhex(h), freq, -50, 7.5f);
      sim::run_for_ms(20);
    } else if (cmd == "LOOP") {
      int n; in >> n;
      sim::run_for_ms((uint32_t)n);
    } else if (cmd == "PINS") {
      printf("PINS");
      for (int p : pins) printf(" %d=%d", p, sim::gpio.read(p));
      printf("\n");
    }
    for (; s_txDone < sim::airLog.size(); s_txDone++) printf("TX @%lu %s\n", (unsigned long)(sim::airLog[s_txDone].tStart / 1000), hexs(sim::airLog[s_txDone].data).c_str());
  }
  sim::reset();
  return 0;
}
