#include "sensor_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_store.h"
#include "radio_timing.h"

static const char *TAG = "sensor_cfg";

void sensor_config_load(SensorConfig *c) {
  memset(c, 0, sizeof(*c));
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

  uint32_t f = s.getU32("freq", 0);
  if (f != 0) c->freqHz = f;                       // 0/липсва -> честотата "до Gateway" (по-безопасен fallback)
  uint8_t sf = s.getU8("sf", RADIO_SF_DEFAULT);
  c->sf = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  uint32_t bw = s.getU32("bw", RADIO_BW_DEFAULT_HZ);
  c->bwHz = (radioBwToIndex(bw) != 0xFF) ? bw : RADIO_BW_DEFAULT_HZ;
  c->txPowerDbm = (int8_t)s.getU8("txp", (uint8_t)LORA_TX_POWER_DBM);   // със знак (два комплемента)

  c->hasBattery = s.getU8("bat", 0) == 1;
  c->hasKey = s.getBlob("key", c->key, CRYPTO_KEY_LEN) == CRYPTO_KEY_LEN;
  if (!c->hasKey) ESP_LOGW(TAG, "мрежов ключ не е зареден от NVS - няма да предавам");
}
