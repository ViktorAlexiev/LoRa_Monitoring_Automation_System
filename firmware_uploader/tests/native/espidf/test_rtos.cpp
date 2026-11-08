// Тест на самия симулатор (FreeRTOS във виртуално време): ако това греши, останалите тестове не значат нищо.
#include "test_support.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

static std::vector<std::string> g_trace;
static void trace(const std::string &s) { g_trace.push_back(s); }

STEST(delay_advances_virtual_time_instantly) {
  static uint64_t t0, t1;
  sim::boot([] {
    t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(900000));      // 15 минути
    t1 = esp_timer_get_time();
  });
  sim::run_for_ms(1000000);
  ASSERT_EQ((long)(t1 - t0), 900000L * 1000L);
}

STEST(higher_priority_runs_first_and_preempts) {
  g_trace.clear();
  sim::boot([] {
    xTaskCreatePinnedToCore([](void *) { trace("low-start"); vTaskDelay(10); trace("low-end"); }, "low", 2048, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore([](void *) { trace("high-start"); vTaskDelay(5); trace("high-mid"); vTaskDelay(10); trace("high-end"); }, "high", 2048, nullptr, 10, nullptr, 1);
    trace("main-after-create");
  });
  sim::run_for_ms(100);
  // всяка нова задача с по-висок приоритет от създаващата изпреварва веднага; main (най-нисък) продължава последен
  ASSERT_STREQ(g_trace[0], "low-start");
  ASSERT_STREQ(g_trace[1], "high-start");
  ASSERT_STREQ(g_trace[2], "main-after-create");
  ASSERT_STREQ(g_trace[3], "high-mid");
  ASSERT_STREQ(g_trace[4], "low-end");
  ASSERT_STREQ(g_trace[5], "high-end");
}

STEST(queue_blocks_and_wakes_receiver_with_timeout) {
  static QueueHandle_t q;
  static int got = -1, timeouts = 0;
  static uint64_t tGot = 0;
  sim::boot([] {
    q = xQueueCreate(2, sizeof(int));
    xTaskCreatePinnedToCore([](void *) {
      int v;
      if (xQueueReceive(q, &v, pdMS_TO_TICKS(50)) == pdFALSE) timeouts++;   // нищо до 50 ms
      if (xQueueReceive(q, &v, portMAX_DELAY) == pdTRUE) { got = v; tGot = esp_timer_get_time(); }
    }, "rx", 2048, nullptr, 5, nullptr, 1);
    vTaskDelay(200);
    int x = 42;
    xQueueSend(q, &x, 0);
  });
  sim::run_for_ms(1000);
  ASSERT_EQ(timeouts, 1);
  ASSERT_EQ(got, 42);
  ASSERT_EQ((long)((tGot - 1000000) / 1000), 200L);
}

STEST(queue_full_returns_fail_without_waiting_and_blocks_with_timeout) {
  static QueueHandle_t q;
  static int r0 = -1, r1 = -1;
  static uint64_t waited = 0;
  sim::boot([] {
    q = xQueueCreate(1, sizeof(int));
    int x = 1;
    xQueueSend(q, &x, 0);
    r0 = xQueueSend(q, &x, 0);
    uint64_t t = esp_timer_get_time();
    r1 = xQueueSend(q, &x, pdMS_TO_TICKS(30));
    waited = esp_timer_get_time() - t;
  });
  sim::run_for_ms(500);
  ASSERT_EQ(r0, pdFALSE);
  ASSERT_EQ(r1, pdFALSE);
  ASSERT_EQ((long)waited, 30000L);
}

STEST(notify_take_counts_and_times_out) {
  static TaskHandle_t h;
  static uint32_t first = 99, second = 99, third = 99;
  sim::boot([] {
    xTaskCreatePinnedToCore([](void *) {
      vTaskDelay(100);                                   // дава време на 3 notify-я да се натрупат
      first = ulTaskNotifyTake(pdTRUE, 0);               // clearOnExit: връща броя и нулира
      second = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));   // таймаут -> 0
      third = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }, "n", 2048, nullptr, 5, &h, 1);
    xTaskNotifyGive(h); xTaskNotifyGive(h); xTaskNotifyGive(h);
    vTaskDelay(150);
    xTaskNotifyGive(h);
  });
  sim::run_for_ms(1000);
  ASSERT_EQ(first, 3u);
  ASSERT_EQ(second, 0u);
  ASSERT_EQ(third, 1u);
}

STEST(mutex_serialises_and_wakes_waiter) {
  static SemaphoreHandle_t m;
  g_trace.clear();
  sim::boot([] {
    m = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore([](void *) {
      xSemaphoreTake(m, portMAX_DELAY); trace("A-take"); vTaskDelay(50); trace("A-give"); xSemaphoreGive(m);
    }, "A", 2048, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore([](void *) {
      vTaskDelay(10);
      xSemaphoreTake(m, portMAX_DELAY); trace("B-take"); xSemaphoreGive(m);
    }, "B", 2048, nullptr, 5, nullptr, 1);
  });
  sim::run_for_ms(500);
  ASSERT_EQ((int)g_trace.size(), 3);
  ASSERT_STREQ(g_trace[0], "A-take");
  ASSERT_STREQ(g_trace[1], "A-give");
  ASSERT_STREQ(g_trace[2], "B-take");
}

static int g_tickCount = 0;
STEST(esp_timer_periodic_and_one_shot) {
  g_tickCount = 0;
  static int once = 0;
  sim::boot([] {
    esp_timer_handle_t p, o;
    esp_timer_create_args_t a = {};
    a.callback = [](void *) { g_tickCount++; };
    a.name = "per";
    esp_timer_create(&a, &p);
    esp_timer_start_periodic(p, 1000000);
    esp_timer_create_args_t b = {};
    b.callback = [](void *) { once++; };
    b.name = "once";
    esp_timer_create(&b, &o);
    esp_timer_start_once(o, 2500000);
    vTaskDelay(portMAX_DELAY);
  });
  sim::run_for_ms(10500);
  ASSERT_EQ(g_tickCount, 10);
  ASSERT_EQ(once, 1);
}

STEST(idle_time_is_accounted_for_pm) {
  sim::boot([] { vTaskDelay(portMAX_DELAY); });
  sim::run_for_ms(1000);
  sim::PmStats s = sim::pm_stats();
  ASSERT_GE((long)s.idle_us, 990000L);
  ASSERT_EQ((int)s.sleeps, 0);     // PM не е конфигуриран -> няма сън
}

STEST(external_event_wakes_blocked_task_from_isr_context) {
  static TaskHandle_t h;
  static uint32_t woke = 0;
  sim::boot([] {
    h = xTaskGetCurrentTaskHandle();
    woke = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  });
  sim::at_after_ms(300, [] { BaseType_t w = 0; vTaskNotifyGiveFromISR(h, &w); }, true);
  sim::run_for_ms(1000);
  ASSERT_EQ(woke, 1u);
}

#include "esp_system.h"
static void restartTask() { vTaskDelay(50); esp_restart(); }
STEST(restart_from_task) {
  sim::boot(restartTask);
  sim::run_for_ms(1000);
  ASSERT_TRUE(sim::restarted());
}

static QueueHandle_t gq;
STEST(chaos_mode_is_deterministic_for_a_seed) {
  static std::string a, b;
  for (int run = 0; run < 2; run++) {
    sim::reset();
    g_trace.clear();
    sim::chaos(7, 500);
    sim::boot([] {
      gq = xQueueCreate(8, sizeof(int));
      xTaskCreatePinnedToCore([](void *) { for (int i = 0; i < 20; i++) { xQueueSend(gq, &i, portMAX_DELAY); } }, "p", 2048, nullptr, 5, nullptr, 0);
      xTaskCreatePinnedToCore([](void *) { int v; for (int i = 0; i < 20; i++) { xQueueReceive(gq, &v, portMAX_DELAY); trace(std::to_string(v)); } }, "c", 2048, nullptr, 5, nullptr, 1);
    });
    sim::run_for_ms(1000);
    std::string s;
    for (auto &x : g_trace) s += x + ",";
    (run == 0 ? a : b) = s;
  }
  ASSERT_STREQ(a, b);
  ASSERT_TRUE(a.size() > 40);
}

// ============================================================================ Task Watchdog (esp_task_wdt)
#include "esp_task_wdt.h"

STEST(task_wdt_defaults_match_esp_idf_and_do_not_restart_by_default) {
  sim::TwdtInfo w = sim::twdt_info();
  ASSERT_EQ((int)w.timeoutMs, 5000);                    // CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5
  ASSERT_FALSE(w.panic);                                // CONFIG_ESP_TASK_WDT_PANIC е изключен: само предупреждение!
  sim::boot([] {
    esp_task_wdt_add(nullptr);
    vTaskDelay(portMAX_DELAY);                          // задачата не се обажда
  });
  sim::run_for_ms(31000);
  ASSERT_FALSE(sim::restarted());                       // без panic: никакъв рестарт
  w = sim::twdt_info();
  ASSERT_GE(w.expiries, 5);                             // но срокът изтича отново и отново (предупреждение на всеки 5 s)
  ASSERT_TRUE(sim::log_contains("Task watchdog got triggered"));
  ASSERT_TRUE(sim::log_contains("main"));
}

STEST(task_wdt_with_panic_restarts_after_exactly_the_timeout_of_silence) {
  static uint64_t addedAt;
  sim::boot([] {
    esp_task_wdt_config_t c = {};
    c.timeout_ms = 10000;
    c.idle_core_mask = 3;
    c.trigger_panic = true;
    ASSERT_EQ(esp_task_wdt_reconfigure(&c), ESP_OK);
    esp_task_wdt_add(nullptr);
    addedAt = esp_timer_get_time();
    vTaskDelay(portMAX_DELAY);
  });
  sim::run_for_ms(9990);
  ASSERT_FALSE(sim::restarted());
  sim::run_for_ms(20);
  ASSERT_TRUE(sim::restarted());                       // между 9.99 и 10.01 s след абонирането
  ASSERT_GE((long)(sim::now_us() - addedAt), 9990L * 1000L);
  sim::reboot(false);
  ASSERT_EQ((int)esp_reset_reason(), (int)ESP_RST_TASK_WDT);
}

STEST(task_wdt_feeding_task_never_trips_and_the_silent_one_is_named) {
  sim::boot([] {
    esp_task_wdt_config_t c = {};
    c.timeout_ms = 5000;
    c.trigger_panic = true;
    esp_task_wdt_reconfigure(&c);
    xTaskCreatePinnedToCore([](void *) { esp_task_wdt_add(nullptr); for (;;) { vTaskDelay(pdMS_TO_TICKS(4000)); esp_task_wdt_reset(); } }, "good", 2048, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore([](void *) { esp_task_wdt_add(nullptr); vTaskDelay(pdMS_TO_TICKS(20000)); esp_task_wdt_reset(); vTaskDelay(portMAX_DELAY); }, "bad", 2048, nullptr, 3, nullptr, 1);
    vTaskDelay(portMAX_DELAY);
  });
  sim::run_for_ms(4900);
  ASSERT_FALSE(sim::restarted());
  sim::run_for_ms(1000);                               // "bad" не се е обадила 5 s
  ASSERT_TRUE(sim::restarted());
  ASSERT_STREQ(sim::twdt_info().lastExpired, "bad");
}

STEST(task_wdt_api_rules_match_esp_idf) {
  static esp_err_t addNull, addTwice, resetUnsub, delOk, delAgain, initTwice, reconfBad;
  sim::boot([] {
    resetUnsub = esp_task_wdt_reset();                 // не е абонирана
    addNull = esp_task_wdt_add(nullptr);
    addTwice = esp_task_wdt_add(nullptr);
    delOk = esp_task_wdt_delete(nullptr);
    delAgain = esp_task_wdt_delete(nullptr);
    esp_task_wdt_config_t c = {5000, 3, false};
    initTwice = esp_task_wdt_init(&c);                 // вече е стартиран от системата
    esp_task_wdt_config_t z = {0, 3, false};
    reconfBad = esp_task_wdt_reconfigure(&z);
  });
  sim::run_for_ms(100);
  ASSERT_EQ(resetUnsub, ESP_ERR_NOT_FOUND);
  ASSERT_EQ(addNull, ESP_OK);
  ASSERT_EQ(addTwice, ESP_ERR_INVALID_ARG);
  ASSERT_EQ(delOk, ESP_OK);
  ASSERT_EQ(delAgain, ESP_ERR_NOT_FOUND);
  ASSERT_EQ(initTwice, ESP_ERR_INVALID_STATE);
  ASSERT_EQ(reconfBad, ESP_ERR_INVALID_ARG);
}

STEST(task_wdt_a_finished_task_does_not_keep_the_system_under_watch) {
  sim::boot([] {
    esp_task_wdt_config_t c = {};
    c.timeout_ms = 5000;
    c.trigger_panic = true;
    esp_task_wdt_reconfigure(&c);
    esp_task_wdt_add(nullptr);
    vTaskDelay(pdMS_TO_TICKS(1000));
  });                                                    // задачата свършва, без да се отпише
  sim::run_for_ms(30000);
  ASSERT_FALSE(sim::restarted());
}

int main() { return runAllTests(); }
