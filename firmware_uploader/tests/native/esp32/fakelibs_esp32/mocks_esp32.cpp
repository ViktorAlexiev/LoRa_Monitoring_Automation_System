// Дефиниции на глобалните състояния на ESP32 заместителите (виж съответните .h файлове).
// Включва се във всеки ESP32 тест. Функциите за мрежата (WiFi/PubSubClient/LoRa) са header-only,
// затова тук са само глобалните променливи и малкото функции с логика (сън, часовник).
#include "Arduino.h"
#include "Preferences.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "sys/time.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include "LoRa.h"

// ---- Arduino ----
std::string g_serialOut;
std::string g_serialIn;
bool g_serialEcho = false;
void serialEmit(const char *s) {
  g_serialOut += s;
  if (g_serialEcho) fputs(s, stdout);
}
FakeSerial Serial;
unsigned long g_fakeMillis = 0;
uint8_t g_pinState[64] = {0};
int g_pinMode[64] = {0};
EspClass ESP;
bool g_espRestarted = false;
void EspClass::restart() { g_espRestarted = true; }

// ---- NVS ----
std::map<std::string, std::vector<uint8_t>> g_nvs;
int g_nvsWrites = 0;
int g_nvsReadOnlyWriteAttempts = 0;

// ---- ESP-IDF ----
esp_reset_reason_t g_resetReason = ESP_RST_POWERON;
uint64_t g_fakeTimeUs = 1000000ULL;
uint64_t g_timerWakeUs = 0;
uint64_t g_gpioWakeAfterUs = 0;
bool g_gpioWakeEnabled = false;
int g_lightSleepCount = 0;
int g_lightSleepGpioWakes = 0;
bool g_deepSleepRequested = false;
uint64_t g_deepSleepUs = 0;
int g_wakeGpio = -1;
int g_wakeGpioLevel = 0;

int gettimeofday(struct timeval *tv, void *) {
  tv->tv_sec = (long long)(g_fakeTimeUs / 1000000ULL);
  tv->tv_usec = (long long)(g_fakeTimeUs % 1000000ULL);
  return 0;
}

void esp_light_sleep_start() {
  g_lightSleepCount++;
  // събитие на DIO0 (входящ пакет) прекъсва съня по-рано, ако е разрешено и е преди таймера
  if (g_gpioWakeEnabled && g_gpioWakeAfterUs > 0 && g_gpioWakeAfterUs < g_timerWakeUs) {
    g_fakeTimeUs += g_gpioWakeAfterUs;
    g_fakeMillis += (unsigned long)(g_gpioWakeAfterUs / 1000ULL);
    g_gpioWakeAfterUs = 0;
    g_lightSleepGpioWakes++;
  } else {
    g_fakeTimeUs += g_timerWakeUs;
    g_fakeMillis += (unsigned long)(g_timerWakeUs / 1000ULL);
  }
}

// ---- WiFi / MQTT / LoRa ----
int g_wifiStatus = WL_CONNECTED;
int g_wifiBeginCalls = 0;
std::string g_wifiSsid;
std::string g_wifiPass;
WiFiClass WiFi;
MqttFake g_mqtt;
LoRaMock g_lora;
LoRaClass LoRa;
