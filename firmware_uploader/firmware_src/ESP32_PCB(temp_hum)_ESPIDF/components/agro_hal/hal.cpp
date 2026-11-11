#include "agro_hal.h"
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"

static const char *TAG = "hal";

uint64_t hal_micros(void) { return (uint64_t)esp_timer_get_time(); }
uint32_t hal_millis(void) { return (uint32_t)(esp_timer_get_time() / INT64_C(1000)); }

void hal_delay_ms(uint32_t ms) {
  if (ms == 0) { taskYIELD(); return; }
  vTaskDelay(pdMS_TO_TICKS(ms));
}

void hal_delay_us(uint32_t us) {
  if (us >= 1000) { hal_delay_ms(us / 1000); us %= 1000; }
  if (us > 0) esp_rom_delay_us(us);
}

uint32_t hal_random(uint32_t max_excl) {
  return max_excl <= 1 ? 0 : esp_random() % max_excl;
}

uint32_t hal_random_range(uint32_t lo, uint32_t hi_excl) {
  return hi_excl <= lo ? lo : lo + hal_random(hi_excl - lo);
}

// ---------------- GPIO ----------------
void hal_gpio_output(int32_t pin, bool level) {
  gpio_config_t io = {};
  io.pin_bit_mask = UINT64_C(1) << pin;
  io.mode = GPIO_MODE_INPUT_OUTPUT;           // INPUT_OUTPUT: gpio_get_level() връща реалното ниво на изхода
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_set_level((gpio_num_t)pin, level ? 1 : 0);   // нивото се задава ПРЕДИ режимът да стане изход (без къс импулс)
  gpio_config(&io);
  gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

void hal_gpio_write(int32_t pin, bool level) { gpio_set_level((gpio_num_t)pin, level ? 1 : 0); }
bool hal_gpio_read(int32_t pin) { return gpio_get_level((gpio_num_t)pin) != 0; }

// ---------------- Прекъсване на пин ----------------
struct IrqCtx { uint8_t pin; TaskHandle_t task; };
static IrqCtx s_irq[4];
static uint8_t s_irqCount = 0;

static void IRAM_ATTR irqIsr(void *arg) {
  IrqCtx *c = (IrqCtx *)arg;
  gpio_intr_disable((gpio_num_t)c->pin);     // нивото ще е високо до прочитане на радиото
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(c->task, &woken);
  portYIELD_FROM_ISR(woken);                 // стандартният вид: превключва само ако woken == pdTRUE
}

bool hal_irq_attach(int32_t pin, TaskHandle_t task) {
  gpio_config_t io = {};
  io.pin_bit_mask = UINT64_C(1) << pin;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_HIGH_LEVEL;
  if (gpio_config(&io) != ESP_OK) return false;

  // службата за GPIO прекъсвания е една на чип; повторно инсталиране връща INVALID_STATE - не е грешка
  esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;

  IrqCtx *c = nullptr;
  for (uint8_t i = 0; i < s_irqCount; i++) if (s_irq[i].pin == pin) c = &s_irq[i];   // повторно закачане на същия пин
  if (c == nullptr) {
    if (s_irqCount >= (uint8_t)(sizeof(s_irq) / sizeof(s_irq[0]))) return false;
    c = &s_irq[s_irqCount++];
  }
  c->pin = (uint8_t)pin;
  c->task = task;
  if (gpio_isr_handler_add((gpio_num_t)pin, irqIsr, c) != ESP_OK) return false;
  gpio_intr_enable((gpio_num_t)pin);
  return true;
}

void hal_irq_rearm(int32_t pin) { gpio_intr_enable((gpio_num_t)pin); }
void hal_irq_disable(int32_t pin) { gpio_intr_disable((gpio_num_t)pin); }

// ---------------- Система ----------------
bool hal_nvs_init(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // разделът е пълен или е от друга версия: изтриването губи конфигурацията -> трябва нов config stage
    ESP_LOGW(TAG, "NVS: изтривам и инициализирам наново (%s)", esp_err_to_name(e));
    if (nvs_flash_erase() != ESP_OK) return false;
    e = nvs_flash_init();
  }
  if (e != ESP_OK) { ESP_LOGE(TAG, "NVS init провал: %s", esp_err_to_name(e)); return false; }
  return true;
}

void hal_restart(void) {
  esp_restart();
  for (;;) {}   // esp_restart() не връща
}

#if AGRO_STACK_PROBE
void hal_stack_probe(void) {
  static struct { TaskHandle_t task; uint32_t minFree; } tab[8];
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  uint32_t freeB = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  bool report = false;
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0; i < 8; i++) {
    if (tab[i].task == me || tab[i].task == nullptr) {
      if (tab[i].task == nullptr) { tab[i].task = me; tab[i].minFree = UINT32_MAX; }
      if (freeB < tab[i].minFree) { tab[i].minFree = freeB; report = true; }
      break;
    }
  }
  portEXIT_CRITICAL(&mux);
  if (report) ESP_LOGI(TAG, "стек %s: най-малко свободно %" PRIu32 " B", pcTaskGetName(nullptr), freeB);
}
#endif

void hal_fatal_restart(const char *what) {
  ESP_LOGE(TAG, "КРИТИЧНА ГРЕШКА при старт: %s - рестарт след 2 s", what);
  hal_delay_ms(2000);
  hal_restart();
}

bool hal_task_wdt_start(uint32_t timeout_ms) {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = timeout_ms;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_err_t e = esp_task_wdt_reconfigure(&cfg);
  if (e == ESP_ERR_INVALID_STATE) e = esp_task_wdt_init(&cfg);      // още не е инициализиран (CONFIG_ESP_TASK_WDT_INIT=n)
  if (e != ESP_OK) { ESP_LOGE(TAG, "Task watchdog не се настрои (%s) - няма защита от зависване", esp_err_to_name(e)); return false; }
  return true;
}

bool hal_woke_from_deep_sleep(void) {
  return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}
