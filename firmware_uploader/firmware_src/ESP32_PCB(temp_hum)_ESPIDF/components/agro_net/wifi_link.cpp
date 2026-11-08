#include "wifi_link.h"
#include <stdint.h>

#include <atomic>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "agro_hal.h"

static const char *TAG = "wifi";

static std::atomic<bool> s_connected(false);
static std::atomic<uint32_t> s_downSinceMs(0);
static std::atomic<bool> s_isDown(true);
static WifiGotIpFn s_onGotIp = nullptr;
static void *s_ctx = nullptr;
static esp_timer_handle_t s_retryTimer = nullptr;
static esp_timer_handle_t s_superTimer = nullptr;

static void retryCb(void *) {
  esp_wifi_connect();
}

static void superCb(void *) {
  if (s_connected.load()) return;
  if ((uint32_t)(hal_millis() - s_downSinceMs.load()) >= WIFI_RESTART_MS) {
    ESP_LOGE(TAG, "няма Wi-Fi 2 мин - РЕСТАРТ");
    hal_restart();
  }
}

static void eventHandler(void *, esp_event_base_t base, int32_t id, void *) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_START) {
      esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
      s_connected.store(false);
      if (!s_isDown.exchange(true)) {
        s_downSinceMs.store(hal_millis());
        ESP_LOGW(TAG, "изгубена връзка");
      }
      esp_timer_stop(s_retryTimer);                       // игнорира се грешката (не е стартиран)
      esp_timer_start_once(s_retryTimer, (uint64_t)WIFI_RECONNECT_MS * UINT64_C(1000));
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_connected.store(true);
    s_isDown.store(false);
    ESP_LOGI(TAG, "свързан, има IP");
    if (s_onGotIp) s_onGotIp(s_ctx);
  }
}

bool wifi_link_start(const char *ssid, const char *password, WifiGotIpFn onGotIp, void *ctx) {
  s_connected.store(false);
  s_retryTimer = nullptr;
  s_superTimer = nullptr;
  s_onGotIp = onGotIp;
  s_ctx = ctx;
  s_downSinceMs.store(hal_millis());
  s_isDown.store(true);

  esp_timer_create_args_t ra = {};
  ra.callback = retryCb;
  ra.name = "wifi_retry";
  esp_timer_create_args_t sa = {};
  sa.callback = superCb;
  sa.name = "wifi_super";
  if (esp_timer_create(&ra, &s_retryTimer) != ESP_OK) return false;
  if (esp_timer_create(&sa, &s_superTimer) != ESP_OK) return false;

  if (esp_netif_init() != ESP_OK) return false;
  esp_err_t e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;   // INVALID_STATE = цикълът вече съществува
  if (esp_netif_create_default_wifi_sta() == nullptr) return false;

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) return false;
  if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, NULL) != ESP_OK) return false;
  if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, NULL) != ESP_OK) return false;

  wifi_config_t wc;
  memset(&wc, 0, sizeof(wc));
  strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
  if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
  if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
  if (esp_wifi_start() != ESP_OK) return false;
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) ESP_LOGW(TAG, "енергоспестяването на Wi-Fi не се изключи");

  // без този таймер няма 2-минутен рестарт при липса на мрежа - затова неуспехът е грешка на старта
  return esp_timer_start_periodic(s_superTimer, UINT64_C(1000000)) == ESP_OK;
}

bool wifi_link_connected(void) { return s_connected.load(); }

uint32_t wifi_link_down_ms(void) {
  if (s_connected.load()) return 0;
  return (uint32_t)(hal_millis() - s_downSinceMs.load());
}
