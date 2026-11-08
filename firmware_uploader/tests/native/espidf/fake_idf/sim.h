/*
  sim.h - API на симулатора за тестове (хост). Не се включва от прошивката.

  Симулаторът изпълнява РЕАЛНИЯ код на прошивката (components/ и apps/<приложение>/main) срещу заместители на
  ESP-IDF:
    * FreeRTOS: задачи (всяка е нишка, но работи само една наведнъж - "baton"), приоритети, опашки,
      mutex-и, notify, закъснения - във ВИРТУАЛНО време. Когато всички задачи са блокирани, времето
      скача до най-близкото събитие (затова 15 минути сън се симулират мигновено). Задачите са
      детерминирани; "chaos" режимът добавя случайни превключвания между всеки два извиквания на
      FreeRTOS/периферни функции (имитира двете ядра, които реално работят паралелно).
    * esp_timer, автоматичен лек сън (брои се кога системата е "заспала" и защо се е събудила),
      дълбок сън и рестарт (хвърлят изключение, което убива задачата).
    * периферия: GPIO с прекъсвания, SPI + модел на SX127x (регистри, FIFO, TX/RX/CAD във виртуално
      време), I2C + SHT31/SHT21, NVS, Wi-Fi + event loop, MQTT брокер, mbedtls (AES/CMAC).
*/
#ifndef SIM_H
#define SIM_H

#include <stdint.h>
#include <functional>
#include <string>
#include <vector>
#include <deque>
#include <map>

namespace sim {

struct Killed {};                          // изключение за разплитане на нишка при reset
struct DeepSleep { uint64_t us; };         // хвърля се от esp_deep_sleep_start()
struct Restart {};                         // хвърля се от esp_restart()

// ---- жизнен цикъл ----
void reset();                              // всичко: задачи, периферия, NVS, RTC, време, логове
void reboot(bool advanceByDeepSleep = true);  // като рестарт/събуждане: убива задачите, нулира GPIO/радио/мрежа,
                                           // ЗАПАЗВА NVS и времето (виж reset_reason). advanceByDeepSleep=false: времето се
                                           // управлява отвън (системната симулация)
void set_reset_reason(int esp_reset_reason);
// RAM се нулира при рестарт: хуковете връщат глобалните обекти (напр. драйвера на радиото) в начално
// състояние при reset()/reboot(). RTC паметта (RTC_DATA_ATTR) умишлено НЕ се пипа.
void add_reset_hook(std::function<void()> fn);
void boot(void (*appMain)(void), const char *name = "main", int prio = 1);   // app_main като задача на ядро 0

// ---- време ----
uint64_t now_us();
uint64_t next_event_us();                  // най-близкото бъдещо събитие (таймаут/таймер/външно) или UINT64_MAX; now ако има готова задача
void set_now_us(uint64_t us);
void run_for_us(uint64_t us);
void run_for_ms(uint32_t ms);
void at_us(uint64_t when, std::function<void()> fn, bool gpio_wake = false);   // външно събитие (контекст на прекъсване)
void at_after_ms(uint32_t ms, std::function<void()> fn, bool gpio_wake = false);

// ---- състояние ----
bool restarted();
bool deep_slept();
uint64_t deep_sleep_us();
void clear_restart_flags();

// ---- детерминизъм / паралелизъм ----
void chaos(uint32_t seed, int per_mille);  // вероятност (0..1000) за превключване на всяко извикване
void seed_random(uint32_t seed);

// ---- задачи ----
struct TaskInfo {
  std::string name;
  int prio;
  int core;
  uint32_t stack;
  bool alive;
  bool blocked;
};
std::vector<TaskInfo> tasks();
const TaskInfo *find_task(const std::vector<TaskInfo> &v, const char *name);

// ---- енергия ----
struct PmStats {
  bool configured = false;       // esp_pm_configure(light_sleep_enable = true)
  bool gpio_wake_enabled = false;
  int manual_light_sleep_calls = 0;   // извиквания на esp_light_sleep_start() (прошивките не бива да го викат)
  uint64_t total_us = 0;
  uint64_t idle_us = 0;          // всички задачи блокирани (= време за автоматичен лек сън при PM)
  uint32_t idle_periods = 0;
  uint32_t sleeps = 0;           // idle периоди, в които чипът реално заспива (PM включен)
  uint32_t gpio_wakes = 0;
  uint32_t timer_wakes = 0;
  uint32_t busy_wakeups = 0;     // колко пъти планировчикът е превключвал към задача
  uint32_t idle_exits = 0;       // колко пъти системата е излизала от бездействие (= реални събуждания)
  std::map<std::string, uint32_t> task_runs;   // превключвания по задача (диагностика: кой буди чипа)
};
PmStats pm_stats();
void pm_reset_counters();

// ---- Task Watchdog (esp_task_wdt) ----
struct TwdtInfo {
  uint32_t timeoutMs = 0;
  bool panic = false;
  int subscribed = 0;             // абонирани задачи
  int expiries = 0;               // колко пъти е изтичал срок
  int resets = 0;                 // колко esp_task_wdt_reset() са успели
  std::string lastExpired;        // име на последната закъсняла задача
};
TwdtInfo twdt_info();

// ---- логове ----
const std::vector<std::string> &log_lines();
bool log_contains(const std::string &needle);
int log_count(const std::string &needle);
void log_clear();

}  // namespace sim

#endif
