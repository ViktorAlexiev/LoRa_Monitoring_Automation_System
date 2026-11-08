// Драйвер: РЕАЛНИЯТ ESP-IDF sensor. Протокол като на стария sensor_capture:
//   compat_sensor <ID> <KEY_HEX32> <FREQ> <SF> <BW>
//   CYCLE <airT> <airH> <air_ok:0|1> <busy:0|1>      -> FRAME <hex> BYTES=<n> SLEEP=<s> TXCOUNT=<n>
#include "compat_common.h"

static int s_hook = (sim::add_reset_hook([] { radio_ram_reset(); }), 0);

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 6) { fprintf(stderr, "употреба: compat_sensor ID KEY FREQ SF BW\n"); return 2; }
  sim::reset();
  nvsCommon(argv[1], argv[2], strtoul(argv[3], 0, 10), atoi(argv[4]), strtoul(argv[5], 0, 10));
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd != "CYCLE") continue;
    double at, ah; int ok, busy;
    in >> at >> ah >> ok >> busy;
    sim::i2c.soil().temp = 25.0; sim::i2c.soil().hum = 50.0;
    sim::i2c.air().present = ok != 0; sim::i2c.air().temp = at; sim::i2c.air().hum = ah;
    size_t before = sim::airLog.size();
    sim::boot(app_main);
    sim::radio.busyUntil = busy ? sim::now_us() + 600000000ULL : 0;
    sim::run_for_ms(30000);
    uint64_t sleepUs = sim::deep_sleep_us();
    bool slept = sim::deep_slept();
    if (sim::airLog.size() > before) {
      printf("FRAME %s BYTES=%d SLEEP=%d TXCOUNT=%d\n", hexs(sim::airLog.back().data).c_str(), (int)sim::airLog.back().data.size(),
             slept ? (int)(sleepUs / 1000000ULL) : -1, (int)(sim::airLog.size() - before));
    } else {
      printf("NOFRAME SLEEP=%d\n", slept ? (int)(sleepUs / 1000000ULL) : -1);
    }
    sim::reboot();
  }
  sim::reset();
  return 0;
}
