#include "gw_config.h"
#include <inttypes.h>

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_store.h"
#include "radio_timing.h"

static const char *TAG = "gw_cfg";

void gw_config_load(GwConfig *c) {
  memset(c, 0, sizeof(*c));
  c->mqttPort = 1883;
  c->freqHz = LORA_FREQ_DEFAULT_HZ;
  c->sf = RADIO_SF_DEFAULT;
  c->bwHz = RADIO_BW_DEFAULT_HZ;
  c->txPowerDbm = LORA_TX_POWER_DBM;

  NvsStore s;
  if (!s.open(NVS_NAMESPACE, true)) {
    ESP_LOGW(TAG, "NVS е празен - устройството не е конфигурирано (качи config приложението)");
    return;
  }
  c->hasId = s.getString("id", c->id, sizeof(c->id)) && c->id[0] != 0;
  s.getString("wssid", c->wifiSsid, sizeof(c->wifiSsid));
  s.getString("wpass", c->wifiPass, sizeof(c->wifiPass));
  s.getString("mqip", c->mqttHost, sizeof(c->mqttHost));
  char port[8] = "";
  if (s.getString("mqport", port, sizeof(port))) {
    int32_t p = (int32_t)strtol(port, nullptr, 10);
    if (p > 0 && p <= 65535) c->mqttPort = (uint16_t)p;       // 0/невалиден -> 1883
  }
  s.getString("mquser", c->mqttUser, sizeof(c->mqttUser));
  s.getString("mqpass", c->mqttPass, sizeof(c->mqttPass));

  uint32_t f = s.getU32("freq", 0);
  if (f != 0) c->freqHz = f;
  uint8_t sf = s.getU8("sf", RADIO_SF_DEFAULT);
  c->sf = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  uint32_t bw = s.getU32("bw", RADIO_BW_DEFAULT_HZ);
  c->bwHz = (radioBwToIndex(bw) != 0xFF) ? bw : RADIO_BW_DEFAULT_HZ;
  c->txPowerDbm = (int8_t)s.getU8("txp", (uint8_t)LORA_TX_POWER_DBM);   // със знак (два комплемента)
  c->hasKey = s.getBlob("key", c->key, CRYPTO_KEY_LEN) == CRYPTO_KEY_LEN;
  c->hasNet = c->wifiSsid[0] != 0 && c->mqttHost[0] != 0;

  // Какво е заредено от NVS (без паролите)
  ESP_LOGI(TAG, "=== NVS CONFIG LOADED === id=%s wifi_ssid=[%s] wifi_pass_len=%" PRIu32 " mqtt=%s:%" PRIu32 " freq=%" PRIu32 " sf=%" PRIu32 " bw=%" PRIu32 " key=%s",
           c->id, c->wifiSsid, (uint32_t)strlen(c->wifiPass), c->mqttHost, (uint32_t)c->mqttPort, (uint32_t)c->freqHz,
           (uint32_t)c->sf, (uint32_t)c->bwHz, c->hasKey ? "да" : "НЕ");
}
