// Симулатор на FreeRTOS + esp_timer + захранване във виртуално време (виж sim.h).
//
// Принцип: всяка задача е std::thread, но "baton" (g_current) позволява на най-много една да работи.
// Главната нишка на теста е планировчикът: когато няма готова задача, тя премества виртуалното време
// до най-близкото събитие (таймаут на задача, esp_timer, външно събитие) и продължава.
#include "sim.h"

#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

void sim_periph_reset(bool keepPersistent);   // sim_periph.cpp
void sim_net_reset(bool world);               // sim_net.cpp

namespace {

typedef std::unique_lock<std::mutex> Lock;
const uint64_t INF = UINT64_MAX;

std::mutex g_m;
std::condition_variable g_cv;

struct Task;
struct Queue;
struct Mtx;

Task *g_current = nullptr;       // нишката с baton; nullptr = планировчикът (главната нишка / "прекъсване")
bool g_shutdown = false;
bool g_inIsr = false;
uint64_t g_now = 1000000ULL;
uint64_t g_seq = 0;
uint64_t g_startNow = 1000000ULL;

enum TState { T_READY, T_RUNNING, T_BLOCKED, T_DEAD };
enum WaitKind { W_NONE, W_DELAY, W_QRECV, W_QSEND, W_MUTEX, W_NOTIFY };

struct Task {
  std::string name;
  TaskFunction_t fn = nullptr;
  void *arg = nullptr;
  int prio = 1;
  int core = 0;
  uint32_t stack = 0;
  TState st = T_READY;
  uint64_t wakeAt = INF;
  bool timedOut = false;
  uint32_t notify = 0;
  WaitKind wait = W_NONE;
  void *waitObj = nullptr;
  uint64_t seq = 0;
  std::thread th;
};

struct Queue {
  size_t itemSize = 0, cap = 0;
  std::deque<std::vector<uint8_t>> items;
  std::vector<Task *> recvWait, sendWait;
};

struct Mtx {
  Task *owner = nullptr;
  std::vector<Task *> waiters;
};

std::vector<std::unique_ptr<Task>> g_tasks;
std::vector<std::unique_ptr<Queue>> g_queues;
std::vector<std::unique_ptr<Mtx>> g_mutexes;

struct Ev { std::function<void()> fn; bool gpioWake; };
std::multimap<uint64_t, Ev> g_events;

// ---- флагове на системата ----
bool g_restarted = false, g_deepSlept = false;
uint64_t g_deepSleepUs = 0;
uint64_t g_timerWakeUs = 0;
int g_resetReason = ESP_RST_POWERON;
bool g_twdtPanicReset = false;       // рестартът е от Task Watchdog (ESP_RST_TASK_WDT)

// ---- chaos ----
std::mt19937 g_chaosRng(1);
int g_chaosPm = 0;

std::mt19937 g_rng(12345);

// ---- енергия ----
sim::PmStats g_pm;
bool g_wasIdle = false;

std::vector<std::function<void()>> g_resetHooks;
void runResetHooks() { for (auto &h : g_resetHooks) h(); }

// ---- логове ----
std::vector<std::string> g_log;

void log_push(const std::string &s) {
  if (g_log.size() > 50000) g_log.erase(g_log.begin(), g_log.begin() + 10000);
  g_log.push_back(s);
}

// ======================================================================= baton
void handoff(Task *self) {   // връща управлението на планировчика и чака да бъде избран отново
  Lock lk(g_m);
  g_current = nullptr;
  g_cv.notify_all();
  g_cv.wait(lk, [&] { return g_current == self || g_shutdown; });
  if (g_current != self) throw sim::Killed();
}

void switchTo(Task *t) {     // от планировчика към задача
  Lock lk(g_m);
  t->st = T_RUNNING;
  g_current = t;
  g_cv.notify_all();
  g_cv.wait(lk, [] { return g_current == nullptr; });
}

void threadMain(Task *t) {
  {
    Lock lk(g_m);
    g_cv.wait(lk, [&] { return g_current == t || g_shutdown; });
    if (g_current != t) return;
  }
  try {
    t->fn(t->arg);
  } catch (sim::Killed &) {
  } catch (sim::DeepSleep &d) {
    g_deepSlept = true;
    g_deepSleepUs = d.us;
  } catch (sim::Restart &) {
    g_restarted = true;
  }
  t->st = T_DEAD;
  Lock lk(g_m);
  g_current = nullptr;
  g_cv.notify_all();
}

// ======================================================================= състояния
void removeFromWaits(Task *t) {
  auto rm = [&](std::vector<Task *> &v) { v.erase(std::remove(v.begin(), v.end(), t), v.end()); };
  if (t->waitObj) {
    if (t->wait == W_QRECV) rm(((Queue *)t->waitObj)->recvWait);
    else if (t->wait == W_QSEND) rm(((Queue *)t->waitObj)->sendWait);
    else if (t->wait == W_MUTEX) rm(((Mtx *)t->waitObj)->waiters);
  }
}

void wake(Task *t, bool timedOut) {
  if (t->st != T_BLOCKED) return;
  removeFromWaits(t);
  t->wait = W_NONE;
  t->waitObj = nullptr;
  t->wakeAt = INF;
  t->timedOut = timedOut;
  t->st = T_READY;
  t->seq = ++g_seq;
}

Task *bestReady(bool random) {
  std::vector<Task *> r;
  for (auto &t : g_tasks) if (t->st == T_READY) r.push_back(t.get());
  if (r.empty()) return nullptr;
  if (random) return r[g_chaosRng() % r.size()];
  Task *best = r[0];
  for (Task *t : r) {
    if (t->prio > best->prio || (t->prio == best->prio && t->seq < best->seq)) best = t;
  }
  return best;
}

bool higherReadyThan(Task *self) {
  for (auto &t : g_tasks) if (t->st == T_READY && t->prio > self->prio) return true;
  return false;
}

// превключване при нужда (след събитие, което е направило готова задача с по-висок приоритет)
void preemptIfNeeded() {
  Task *self = g_current;
  if (self == nullptr || g_inIsr) return;
  if (higherReadyThan(self)) {
    self->st = T_READY;           // запазва мястото си (seq) - при равен приоритет не се изпреварва
    handoff(self);
  }
}

void block(Task *self, WaitKind kind, void *obj, uint64_t deadline) {
  self->wait = kind;
  self->waitObj = obj;
  self->wakeAt = deadline;
  self->timedOut = false;
  self->st = T_BLOCKED;
  if (kind == W_QRECV) ((Queue *)obj)->recvWait.push_back(self);
  else if (kind == W_QSEND) ((Queue *)obj)->sendWait.push_back(self);
  else if (kind == W_MUTEX) ((Mtx *)obj)->waiters.push_back(self);
  handoff(self);
}

Task *needTask(const char *what) {
  if (g_current == nullptr) {
    fprintf(stderr, "SIM: %s извикан извън задача (контекст на прекъсване/тест)\n", what);
    abort();
  }
  return g_current;
}

uint64_t deadlineFor(TickType_t ticks) {
  return ticks == portMAX_DELAY ? INF : g_now + (uint64_t)ticks * 1000ULL;
}

// ======================================================================= esp_timer
struct EspTimer {
  esp_timer_cb_t cb;
  void *arg;
  std::string name;
  bool periodic = false;
  uint64_t period = 0;
  uint64_t due = INF;
  bool pending = false;
  bool alive = true;
};
std::vector<std::unique_ptr<EspTimer>> g_timers;
Task *g_timerTask = nullptr;

void timerTaskMain(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (size_t i = 0; i < g_timers.size(); i++) {
      EspTimer *t = g_timers[i].get();
      if (t->alive && t->pending) {
        t->pending = false;
        t->cb(t->arg);
      }
    }
  }
}

void ensureTimerTask() {
  if (g_timerTask) return;
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(timerTaskMain, "esp_timer", 4096, nullptr, 22, &h, 0);
  g_timerTask = (Task *)h;
}

void giveNotify(Task *t) {
  t->notify++;
  if (t->st == T_BLOCKED && t->wait == W_NOTIFY) wake(t, false);
}

// ======================================================================= планировчик
void processDue() {
  // таймаути на задачи
  for (auto &t : g_tasks) {
    if (t->st == T_BLOCKED && t->wakeAt <= g_now) wake(t.get(), true);
  }
  // esp_timer
  bool any = false;
  for (auto &t : g_timers) {
    if (t->alive && !t->pending && t->due <= g_now) {
      t->pending = true;
      any = true;
      if (t->periodic) {
        t->due += t->period;
        if (t->due <= g_now) t->due = g_now + t->period;   // пропуснатите периоди не се наваксват
      } else {
        t->due = INF;
      }
    }
  }
  if (any && g_timerTask) giveNotify(g_timerTask);
  // външни събития ("прекъсвания")
  while (!g_events.empty() && g_events.begin()->first <= g_now) {
    Ev ev = g_events.begin()->second;
    g_events.erase(g_events.begin());
    g_inIsr = true;
    ev.fn();
    g_inIsr = false;
  }
}

uint64_t nextEventTime(bool *gpioWake) {
  uint64_t n = INF;
  *gpioWake = false;
  for (auto &t : g_tasks) if (t->st == T_BLOCKED && t->wakeAt < n) n = t->wakeAt;
  for (auto &t : g_timers) if (t->alive && !t->pending && t->due < n) n = t->due;
  if (!g_events.empty() && g_events.begin()->first <= n) {
    n = g_events.begin()->first;
    *gpioWake = g_events.begin()->second.gpioWake;
  }
  return n;
}

void accountIdle(uint64_t dt, bool gpioWake) {
  g_pm.total_us += dt;
  if (dt == 0) return;
  g_wasIdle = true;
  g_pm.idle_us += dt;
  g_pm.idle_periods++;
  if (g_pm.configured && dt >= 3000ULL) {   // tickless idle: FREERTOS_IDLE_TIME_BEFORE_SLEEP = 3 тика
    g_pm.sleeps++;
    if (gpioWake) g_pm.gpio_wakes++; else g_pm.timer_wakes++;
  }
}

void runUntil(uint64_t target) {
  uint64_t lastNow = g_now;
  long spins = 0;
  for (;;) {
    if (g_restarted || g_deepSlept) return;
    processDue();
    Task *t = bestReady(g_chaosPm > 0 && (int)(g_chaosRng() % 1000) < 400);
    if (t) {
      if (g_now == lastNow) {
        if (++spins > 200000) {
          fprintf(stderr, "SIM: livelock - задача '%s' върти без напредване на времето\n", t->name.c_str());
          abort();
        }
      } else { spins = 0; lastNow = g_now; }
      g_pm.busy_wakeups++;
      g_pm.task_runs[t->name]++;
      if (g_wasIdle) { g_pm.idle_exits++; g_wasIdle = false; }
      switchTo(t);
      continue;
    }
    bool gw = false;
    uint64_t nxt = nextEventTime(&gw);
    if (nxt == INF || nxt > target) {
      accountIdle(target > g_now ? target - g_now : 0, false);
      if (target > g_now) g_now = target;
      return;
    }
    if (nxt > g_now) { accountIdle(nxt - g_now, gw); g_now = nxt; }
  }
}

void killAll() {
  {
    Lock lk(g_m);
    g_shutdown = true;
    g_cv.notify_all();
  }
  for (auto &t : g_tasks) if (t->th.joinable()) t->th.join();
  g_tasks.clear();
  g_queues.clear();
  g_mutexes.clear();
  g_timers.clear();
  g_timerTask = nullptr;
  g_events.clear();
  {
    Lock lk(g_m);
    g_shutdown = false;
    g_current = nullptr;
  }
}

}  // namespace

// ======================================================================= preemption точки за периферията
void sim_preempt_point(void) {
  if (g_chaosPm <= 0 || g_current == nullptr || g_inIsr) return;
  if ((int)(g_chaosRng() % 1000) >= g_chaosPm) return;
  Task *self = g_current;
  bool other = false;
  for (auto &t : g_tasks) if (t->st == T_READY) { other = true; break; }
  if (!other) return;
  self->st = T_READY;
  self->seq = ++g_seq;
  handoff(self);
}

bool sim_in_task(void) { return g_current != nullptr; }
bool sim_in_isr(void) { return g_inIsr; }
// събитие в "контекст на прекъсване" (за периферията, ако е извикана от задача)
void sim_run_in_isr(const std::function<void()> &fn) {
  bool prev = g_inIsr;
  g_inIsr = true;
  fn();
  g_inIsr = prev;
}
const char *sim_current_task_name(void) { return g_current ? g_current->name.c_str() : "-"; }
void sim_notify_from_isr(void *task) { giveNotify((Task *)task); }

// ======================================================================= FreeRTOS API
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core) {
  auto t = std::make_unique<Task>();
  t->name = name;
  t->fn = fn;
  t->arg = arg;
  t->prio = (int)prio;
  t->core = (int)core;
  t->stack = stack;
  t->st = T_READY;
  t->seq = ++g_seq;
  Task *raw = t.get();
  raw->th = std::thread(threadMain, raw);
  g_tasks.push_back(std::move(t));
  if (handle) *handle = raw;
  if (g_current && !g_inIsr) preemptIfNeeded();
  return pdPASS;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h) {
  return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, h, tskNO_AFFINITY);
}

void vTaskDelete(TaskHandle_t h) {
  Task *t = h ? (Task *)h : g_current;
  if (t == nullptr) return;
  removeFromWaits(t);
  t->st = T_DEAD;
  t->wait = W_NONE;
  if (t == g_current) throw sim::Killed();
}

void vTaskDelay(TickType_t ticks) {
  Task *self = needTask("vTaskDelay");
  sim_preempt_point();
  if (ticks == 0) { taskYIELD(); return; }
  block(self, W_DELAY, nullptr, g_now + (uint64_t)ticks * 1000ULL);
}

TickType_t xTaskGetTickCount(void) { return (TickType_t)(g_now / 1000ULL); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return g_current; }
const char *pcTaskGetName(TaskHandle_t t) { return t ? ((Task *)t)->name.c_str() : (g_current ? g_current->name.c_str() : "-"); }

void taskYIELD(void) {
  Task *self = needTask("taskYIELD");
  g_now += 5;                            // отстъпването струва време (като реален активен цикъл): цикли "чакай BUSY" напредват във виртуалното време
  self->st = T_READY;
  self->seq = ++g_seq;
  handoff(self);
}

uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticks) {
  Task *self = needTask("ulTaskNotifyTake");
  sim_preempt_point();
  if (self->notify == 0 && ticks != 0) block(self, W_NOTIFY, nullptr, deadlineFor(ticks));
  uint32_t v = self->notify;
  if (clearOnExit) self->notify = 0; else if (v) self->notify--;
  return v;
}

BaseType_t xTaskNotifyGive(TaskHandle_t t) {
  sim_preempt_point();
  giveNotify((Task *)t);
  preemptIfNeeded();
  return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken) {
  Task *task = (Task *)t;
  bool waiting = task->st == T_BLOCKED && task->wait == W_NOTIFY;
  giveNotify(task);
  if (woken) *woken = (waiting && g_current && task->prio > g_current->prio) ? pdTRUE : pdFALSE;
}

// ---- опашки ----
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
  auto q = std::make_unique<Queue>();
  q->cap = length;
  q->itemSize = itemSize;
  Queue *raw = q.get();
  g_queues.push_back(std::move(q));
  return raw;
}
void vQueueDelete(QueueHandle_t) {}

static BaseType_t queueSend(Queue *q, const void *item, TickType_t ticks, bool fromIsr) {
  if (!fromIsr) sim_preempt_point();
  uint64_t deadline = deadlineFor(ticks);
  for (;;) {
    if (q->items.size() < q->cap) {
      q->items.emplace_back((const uint8_t *)item, (const uint8_t *)item + q->itemSize);
      if (!q->recvWait.empty()) wake(q->recvWait.front(), false);
      if (!fromIsr) preemptIfNeeded();
      return pdTRUE;
    }
    if (fromIsr || ticks == 0 || g_now >= deadline) return pdFALSE;
    block(needTask("xQueueSend"), W_QSEND, q, deadline);
  }
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { return queueSend((Queue *)q, item, t, g_inIsr); }
BaseType_t xQueueSendToBack(QueueHandle_t q, const void *item, TickType_t t) { return queueSend((Queue *)q, item, t, g_inIsr); }
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *woken) {
  if (woken) *woken = pdFALSE;
  return queueSend((Queue *)q, item, 0, true);
}

BaseType_t xQueueReceive(QueueHandle_t h, void *buf, TickType_t ticks) {
  Queue *q = (Queue *)h;
  Task *self = needTask("xQueueReceive");
  sim_preempt_point();
  uint64_t deadline = deadlineFor(ticks);
  for (;;) {
    if (!q->items.empty()) {
      memcpy(buf, q->items.front().data(), q->itemSize);
      q->items.pop_front();
      if (!q->sendWait.empty()) wake(q->sendWait.front(), false);
      preemptIfNeeded();
      return pdTRUE;
    }
    if (ticks == 0 || g_now >= deadline) return pdFALSE;
    block(self, W_QRECV, q, deadline);
  }
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return (UBaseType_t)((Queue *)q)->items.size(); }
BaseType_t xQueueReset(QueueHandle_t q) { ((Queue *)q)->items.clear(); return pdPASS; }

// ---- mutex ----
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  auto m = std::make_unique<Mtx>();
  Mtx *raw = m.get();
  g_mutexes.push_back(std::move(m));
  return raw;
}
void vSemaphoreDelete(SemaphoreHandle_t) {}

BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t ticks) {
  Mtx *m = (Mtx *)h;
  Task *self = needTask("xSemaphoreTake");
  sim_preempt_point();
  uint64_t deadline = deadlineFor(ticks);
  for (;;) {
    if (m->owner == nullptr) { m->owner = self; return pdTRUE; }
    if (m->owner == self) { fprintf(stderr, "SIM: рекурсивно вземане на mutex от '%s'\n", self->name.c_str()); abort(); }
    if (ticks == 0 || g_now >= deadline) return pdFALSE;
    block(self, W_MUTEX, m, deadline);
  }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
  Mtx *m = (Mtx *)h;
  Task *self = needTask("xSemaphoreGive");
  if (m->owner != self) { fprintf(stderr, "SIM: mutex се освобождава от '%s', който не го държи\n", self->name.c_str()); abort(); }
  m->owner = nullptr;
  if (!m->waiters.empty()) wake(m->waiters.front(), false);
  preemptIfNeeded();
  return pdTRUE;
}

// ======================================================================= esp_timer / система
int64_t esp_timer_get_time(void) { return (int64_t)g_now; }

esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out) {
  ensureTimerTask();
  auto t = std::make_unique<EspTimer>();
  t->cb = a->callback;
  t->arg = a->arg;
  t->name = a->name ? a->name : "";
  *out = t.get();
  g_timers.push_back(std::move(t));
  return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us) {
  EspTimer *t = (EspTimer *)h;
  if (t->due != INF) return ESP_ERR_INVALID_STATE;     // като в IDF: вече е стартиран
  t->periodic = false;
  t->due = g_now + us;
  return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us) {
  EspTimer *t = (EspTimer *)h;
  if (t->due != INF) return ESP_ERR_INVALID_STATE;
  t->periodic = true;
  t->period = us;
  t->due = g_now + us;
  return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h) {
  EspTimer *t = (EspTimer *)h;
  if (t->due == INF && !t->pending) return ESP_ERR_INVALID_STATE;
  t->due = INF;
  t->pending = false;
  return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t h) { ((EspTimer *)h)->alive = false; return ESP_OK; }

void esp_rom_delay_us(uint32_t us) { g_now += us; }   // активно изчакване: процесорът е зает, времето тече

uint32_t esp_random(void) { return g_rng(); }

void esp_restart(void) {
  g_restarted = true;
  throw sim::Restart();
}
esp_reset_reason_t esp_reset_reason(void) { return (esp_reset_reason_t)g_resetReason; }

// ======================================================================= Task Watchdog (esp_task_wdt)
// Следи само абонираните задачи (idle задачите не се симулират). Проверката е насрочено събитие на най-близкия срок;
// здрава система има едно събитие на всеки timeout, затова не променя картината на съня/събужданията.
namespace {
struct Twdt {
  bool inited = true;                       // системата го стартира преди app_main (CONFIG_ESP_TASK_WDT_INIT=y)
  uint32_t timeoutMs = 5000;                // CONFIG_ESP_TASK_WDT_TIMEOUT_S по подразбиране
  bool panic = false;                       // CONFIG_ESP_TASK_WDT_PANIC по подразбиране е ИЗКЛЮЧЕНО (само лог!)
  std::map<Task *, uint64_t> deadline;      // абонирана задача -> срок (виртуално us)
  std::set<uint64_t> scheduled;             // моменти с вече насрочена проверка
  int expiries = 0;
  int resets = 0;
  std::string lastExpired;
};
Twdt g_twdt;

void twdtSchedule() {
  uint64_t m = INF;
  for (auto &kv : g_twdt.deadline) if (kv.second < m) m = kv.second;
  if (m == INF || g_twdt.scheduled.count(m)) return;
  g_twdt.scheduled.insert(m);
  g_events.insert({m, Ev{[m] {
    g_twdt.scheduled.erase(m);
    std::vector<Task *> expired;
    for (auto &kv : g_twdt.deadline) {
      if (kv.first->st == T_DEAD) continue;
      if (kv.second <= g_now) expired.push_back(kv.first);
    }
    for (auto it = g_twdt.deadline.begin(); it != g_twdt.deadline.end();) {
      if (it->first->st == T_DEAD) it = g_twdt.deadline.erase(it); else ++it;
    }
    if (!expired.empty()) {
      g_twdt.expiries++;
      char line[300];
      std::string names;
      for (Task *t : expired) names += " - " + t->name + " (CPU " + std::to_string(t->core) + ")";
      g_twdt.lastExpired = expired[0]->name;
      snprintf(line, sizeof(line), "E (%llu) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:%s",
               (unsigned long long)(g_now / 1000ULL), names.c_str());
      log_push(line);
      if (g_twdt.panic) {
        g_twdtPanicReset = true;
        g_restarted = true;                 // паник -> рестарт; планировчикът спира, reboot() връща причина TASK_WDT
        return;
      }
      for (Task *t : expired) g_twdt.deadline[t] = g_now + (uint64_t)g_twdt.timeoutMs * 1000ULL;   // само предупреждение: срокът тръгва пак
    }
    twdtSchedule();
  }, false}});
}
}  // namespace

void twdtReset() { g_twdt = Twdt(); }

esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t *c) {
  if (c == nullptr || c->timeout_ms == 0) return ESP_ERR_INVALID_ARG;
  if (g_twdt.inited) return ESP_ERR_INVALID_STATE;
  g_twdt.inited = true;
  g_twdt.timeoutMs = c->timeout_ms;
  g_twdt.panic = c->trigger_panic;
  return ESP_OK;
}
esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t *c) {
  if (c == nullptr || c->timeout_ms == 0) return ESP_ERR_INVALID_ARG;
  if (!g_twdt.inited) return ESP_ERR_INVALID_STATE;
  g_twdt.timeoutMs = c->timeout_ms;
  g_twdt.panic = c->trigger_panic;
  for (auto &kv : g_twdt.deadline) kv.second = g_now + (uint64_t)g_twdt.timeoutMs * 1000ULL;   // срокът тръгва наново
  twdtSchedule();
  return ESP_OK;
}
esp_err_t esp_task_wdt_add(TaskHandle_t h) {
  Task *t = h ? (Task *)h : g_current;
  if (t == nullptr) return ESP_ERR_INVALID_ARG;
  if (!g_twdt.inited) return ESP_ERR_INVALID_STATE;
  if (g_twdt.deadline.count(t)) return ESP_ERR_INVALID_ARG;          // вече е абонирана
  g_twdt.deadline[t] = g_now + (uint64_t)g_twdt.timeoutMs * 1000ULL;
  twdtSchedule();
  return ESP_OK;
}
esp_err_t esp_task_wdt_delete(TaskHandle_t h) {
  Task *t = h ? (Task *)h : g_current;
  if (g_twdt.deadline.erase(t) == 0) return ESP_ERR_NOT_FOUND;
  return ESP_OK;
}
esp_err_t esp_task_wdt_reset(void) {
  auto it = g_twdt.deadline.find(g_current);
  if (it == g_twdt.deadline.end()) return ESP_ERR_NOT_FOUND;
  it->second = g_now + (uint64_t)g_twdt.timeoutMs * 1000ULL;
  g_twdt.resets++;
  return ESP_OK;
}

esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { g_timerWakeUs = us; return ESP_OK; }
esp_err_t esp_sleep_enable_gpio_wakeup(void) { g_pm.gpio_wake_enabled = true; return ESP_OK; }
void esp_deep_sleep_start(void) { throw sim::DeepSleep{g_timerWakeUs}; }
esp_err_t esp_light_sleep_start(void) { g_pm.manual_light_sleep_calls++; return ESP_OK; }
esp_err_t esp_pm_configure(const void *config) {
  const esp_pm_config_t *c = (const esp_pm_config_t *)config;
  g_pm.configured = c->light_sleep_enable;
  return ESP_OK;
}

const char *esp_err_to_name(esp_err_t e) {
  switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    default: return "ESP_ERR_?";
  }
}

void sim_log(char level, const char *tag, const char *fmt, ...) {
  char buf[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  char line[720];
  snprintf(line, sizeof(line), "%c (%llu) %s: %s", level, (unsigned long long)(g_now / 1000ULL), tag, buf);
  log_push(line);
  static int toStdout = -1;
  if (toStdout < 0) { const char *e = getenv("SIM_LOG_STDOUT"); toStdout = (e && *e == '1') ? 1 : 0; }
  if (toStdout) { puts(line); fflush(stdout); }
}

// ======================================================================= sim:: API
namespace sim {

void reset() {
  killAll();
  g_now = g_startNow = 1000000ULL;
  g_seq = 0;
  g_restarted = g_deepSlept = false;
  g_twdtPanicReset = false;
  twdtReset();
  g_deepSleepUs = g_timerWakeUs = 0;
  g_resetReason = ESP_RST_POWERON;
  g_chaosPm = 0;
  g_pm = PmStats();
  g_log.clear();
  g_rng.seed(12345);
  sim_periph_reset(false);
  sim_net_reset(true);
  runResetHooks();
}

void reboot(bool advanceByDeepSleep) {
  uint64_t slept = (g_deepSlept && advanceByDeepSleep) ? g_deepSleepUs : 0;
  int reason = g_deepSlept ? ESP_RST_DEEPSLEEP : (g_twdtPanicReset ? ESP_RST_TASK_WDT : (g_restarted ? ESP_RST_SW : ESP_RST_POWERON));
  killAll();
  g_now += slept;
  g_restarted = g_deepSlept = false;
  g_twdtPanicReset = false;
  twdtReset();
  g_deepSleepUs = 0;
  g_resetReason = reason;
  g_pm.configured = false;
  g_pm.gpio_wake_enabled = false;
  sim_periph_reset(true);
  sim_net_reset(false);
  runResetHooks();
}

TwdtInfo twdt_info() {
  TwdtInfo i;
  i.timeoutMs = g_twdt.timeoutMs;
  i.panic = g_twdt.panic;
  i.subscribed = (int)g_twdt.deadline.size();
  i.expiries = g_twdt.expiries;
  i.resets = g_twdt.resets;
  i.lastExpired = g_twdt.lastExpired;
  return i;
}

void set_reset_reason(int r) { g_resetReason = r; }
void add_reset_hook(std::function<void()> fn) { g_resetHooks.push_back(fn); }

void boot(void (*appMain)(void), const char *name, int prio) {
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore((TaskFunction_t)appMain, name, 3584, nullptr, (UBaseType_t)prio, &h, 0);
}

uint64_t now_us() { return g_now; }
uint64_t next_event_us() {
  for (auto &t : g_tasks) if (t->st == T_READY) return g_now;
  bool gw = false;
  return nextEventTime(&gw);
}
void set_now_us(uint64_t us) { g_now = us; }
void run_for_us(uint64_t us) { runUntil(g_now + us); }
void run_for_ms(uint32_t ms) { runUntil(g_now + (uint64_t)ms * 1000ULL); }
void at_us(uint64_t when, std::function<void()> fn, bool gpioWake) { g_events.insert({when, Ev{fn, gpioWake}}); }
void at_after_ms(uint32_t ms, std::function<void()> fn, bool gpioWake) { at_us(g_now + (uint64_t)ms * 1000ULL, fn, gpioWake); }

bool restarted() { return g_restarted; }
bool deep_slept() { return g_deepSlept; }
uint64_t deep_sleep_us() { return g_deepSleepUs; }
void clear_restart_flags() { g_restarted = g_deepSlept = false; }

void chaos(uint32_t seed, int perMille) {
  g_chaosRng.seed(seed);
  g_chaosPm = perMille;
}
void seed_random(uint32_t seed) { g_rng.seed(seed); }

std::vector<TaskInfo> tasks() {
  std::vector<TaskInfo> v;
  for (auto &t : g_tasks) {
    TaskInfo i;
    i.name = t->name;
    i.prio = t->prio;
    i.core = t->core;
    i.stack = t->stack;
    i.alive = t->st != T_DEAD;
    i.blocked = t->st == T_BLOCKED;
    v.push_back(i);
  }
  return v;
}

const TaskInfo *find_task(const std::vector<TaskInfo> &v, const char *name) {
  for (auto &t : v) if (t.name == name) return &t;
  return nullptr;
}

PmStats pm_stats() { return g_pm; }
void pm_reset_counters() {
  bool c = g_pm.configured, g = g_pm.gpio_wake_enabled;
  g_pm = PmStats();
  g_pm.configured = c;
  g_pm.gpio_wake_enabled = g;
}

const std::vector<std::string> &log_lines() { return g_log; }
bool log_contains(const std::string &n) { return log_count(n) > 0; }
int log_count(const std::string &n) {
  int c = 0;
  for (auto &l : g_log) if (l.find(n) != std::string::npos) c++;
  return c;
}
void log_clear() { g_log.clear(); }

}  // namespace sim
