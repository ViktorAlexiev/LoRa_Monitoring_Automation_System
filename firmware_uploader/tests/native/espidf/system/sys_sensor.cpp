// Sensor: след дълбокия сън чипът се буди сам (по виртуалния час) и app_main стартира наново
#include "sys_common.h"

static uint64_t s_wakeAt = 0;
static bool s_sleeping = false;

int main(int argc, char **argv) {
  SysNode n;
  n.advance = [](uint64_t us) {
    uint64_t target = sim::now_us() + us;
    while (sim::now_us() < target) {
      if (sim::deep_slept()) {
        if (!s_sleeping) { s_sleeping = true; s_wakeAt = sim::now_us() + sim::deep_sleep_us(); }
        if (s_wakeAt <= target) {
          sim::set_now_us(s_wakeAt);
          sim::reboot(false);                       // времето е вече преместено; RTC паметта е жива (reset reason = DEEPSLEEP)
          s_sleeping = false;
          sim::boot(app_main);
        } else {
          sim::set_now_us(target);
        }
      } else {
        sim::run_for_us(target - sim::now_us());
        if (sim::deep_slept() && !s_sleeping) { s_sleeping = true; s_wakeAt = sim::now_us() + sim::deep_sleep_us(); }
      }
    }
  };
  n.nextEvent = [] { return s_sleeping ? s_wakeAt : sim::next_event_us(); };
  n.set = [](const std::string &k, const std::string &v) {
    double x = atof(v.c_str());
    if (k == "soil_t") sim::i2c.soil().temp = x;
    else if (k == "soil_h") sim::i2c.soil().hum = x;
    else if (k == "air_t") sim::i2c.air().temp = x;
    else if (k == "air_h") sim::i2c.air().hum = x;
    else if (k == "soil_present") sim::i2c.soil().present = x != 0;
    else if (k == "air_present") sim::i2c.air().present = x != 0;
  };
  return sysMain(argc, argv, n);
}
