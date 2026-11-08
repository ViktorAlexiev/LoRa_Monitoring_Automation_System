#include "ew_config.h"
#include <inttypes.h>

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_store.h"

static const char *TAG = "ew_cfg";

void ew_config_load(EwConfig *c) {
  memset(c, 0, sizeof(*c));
  c->mqttPort = 1883;

  NvsStore s;
  if (!s.open(NVS_NAMESPACE, true)) {
    ESP_LOGW(TAG, "NVS е празен - устройството не е конфигурирано (качи config приложението)");
    return;
  }
  c->hasId = s.getString("id", c->id, sizeof(c->id)) && c->id[0] != 0;
  consumers_load(&c->consumers, &s);
  s.getString("wssid", c->wifiSsid, sizeof(c->wifiSsid));
  s.getString("wpass", c->wifiPass, sizeof(c->wifiPass));
  s.getString("mqip", c->mqttHost, sizeof(c->mqttHost));
  char port[8] = "";
  if (s.getString("mqport", port, sizeof(port))) {
    int32_t p = (int32_t)strtol(port, nullptr, 10);
    if (p > 0 && p <= 65535) c->mqttPort = (uint16_t)p;
  }
  s.getString("mquser", c->mqttUser, sizeof(c->mqttUser));
  s.getString("mqpass", c->mqttPass, sizeof(c->mqttPass));
  c->hasNet = c->wifiSsid[0] != 0 && c->mqttHost[0] != 0;

  ESP_LOGI(TAG, "=== NVS CONFIG LOADED (executor_wifi) === id=%s consumers=%" PRIu32 " wifi_ssid=[%s] mqtt=%s:%" PRIu32, c->id,
           (uint32_t)c->consumers.count, c->wifiSsid, c->mqttHost, (uint32_t)c->mqttPort);
}
