#include "power_mgmt.h"
#include "board_pins.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <sys/time.h>

// Времето се чете от системния часовник (gettimeofday) - той продължава да върви и през лек сън
// (ESP-IDF го коригира по RTC таймера), за разлика от millis() в някои версии на ядрото.
static const uint64_t TICK_US = 8000000ULL;   // 8 секунди
static uint64_t tickBaseUs = 0;
volatile uint8_t wdt_ticks = 0;

static uint64_t nowUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void wdt_arm_8s_continuous() {
  tickBaseUs = nowUs();
  wdt_ticks = 0;
}

void powerTicksPoll() {
  uint64_t now = nowUs();
  while (now - tickBaseUs >= TICK_US) {
    tickBaseUs += TICK_US;
    wdt_ticks++;
  }
}

void enablePinChangeWake() {
  gpio_wakeup_enable((gpio_num_t)LORA_DIO0, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}

void deep_sleep_until_event() {
  powerTicksPoll();
  uint64_t elapsed = nowUs() - tickBaseUs;
  uint64_t untilTick = (elapsed < TICK_US) ? (TICK_US - elapsed) : 1000ULL;
  esp_sleep_enable_timer_wakeup(untilTick);
  Serial.flush();
  esp_light_sleep_start();
  powerTicksPoll();
}
