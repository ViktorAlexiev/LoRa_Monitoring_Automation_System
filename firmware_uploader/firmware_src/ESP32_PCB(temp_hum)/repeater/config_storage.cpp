#include "config_storage.h"
#include <Preferences.h>

char REPEATER_ID[MODULE_ID_LEN + 1];
uint32_t LORA_FREQ_RX_HZ = LORA_FREQ_RX_DEFAULT_HZ;
uint32_t LORA_FREQ_TX_HZ = LORA_FREQ_TX_DEFAULT_HZ;
uint8_t  LORA_SF = RADIO_SF_DEFAULT;
uint32_t LORA_BW_HZ = RADIO_BW_DEFAULT_HZ;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter hbCounter;

void loadConfigFromNvs() {
  Preferences prefs;
  prefs.begin("cfg", true);

  String id = prefs.getString("id", "");
  id.toCharArray(REPEATER_ID, sizeof(REPEATER_ID));

  uint32_t rx = prefs.getULong("freq", 0);
  LORA_FREQ_RX_HZ = (rx != 0) ? rx : LORA_FREQ_RX_DEFAULT_HZ;
  uint32_t tx = prefs.getULong("freqtx", 0);
  LORA_FREQ_TX_HZ = (tx != 0) ? tx : LORA_FREQ_TX_DEFAULT_HZ;

  uint8_t sf = prefs.getUChar("sf", RADIO_SF_DEFAULT);
  LORA_SF = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  uint32_t bw = prefs.getULong("bw", RADIO_BW_DEFAULT_HZ);
  LORA_BW_HZ = (radioBwToIndex(bw) != 0xFF) ? bw : RADIO_BW_DEFAULT_HZ;

  size_t keyLen = prefs.getBytes("key", NETWORK_KEY, CRYPTO_KEY_LEN);
  if (keyLen != CRYPTO_KEY_LEN) {
    Serial.println(F("[WARN] мрежов ключ не е зареден от NVS - крипто операции ще се провалят"));
  }
  prefs.end();

  hbCounter.begin("ceil");
}
