#include "config_storage.h"
#include <Preferences.h>

char SENSOR_ID[MODULE_ID_LEN + 1];
uint32_t LORA_FREQ_HZ = LORA_FREQ_DEFAULT_HZ;
uint8_t  LORA_SF = RADIO_SF_DEFAULT;
uint32_t LORA_BW_HZ = RADIO_BW_DEFAULT_HZ;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter txCounter;

void loadConfigFromNvs() {
  Preferences prefs;
  prefs.begin("cfg", true);   // true = само четене

  String id = prefs.getString("id", "");
  id.toCharArray(SENSOR_ID, sizeof(SENSOR_ID));

  uint32_t storedFreq = prefs.getULong("freq", 0);
  LORA_FREQ_HZ = (storedFreq != 0) ? storedFreq : LORA_FREQ_DEFAULT_HZ;

  uint8_t sf = prefs.getUChar("sf", RADIO_SF_DEFAULT);
  LORA_SF = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  uint32_t bw = prefs.getULong("bw", RADIO_BW_DEFAULT_HZ);
  LORA_BW_HZ = (radioBwToIndex(bw) != 0xFF) ? bw : RADIO_BW_DEFAULT_HZ;

  size_t keyLen = prefs.getBytes("key", NETWORK_KEY, CRYPTO_KEY_LEN);
  if (keyLen != CRYPTO_KEY_LEN) {
    Serial.println(F("[WARN] мрежов ключ не е зареден от NVS - крипто операции ще се провалят"));
  }
  prefs.end();

  txCounter.begin("ceil");
}
