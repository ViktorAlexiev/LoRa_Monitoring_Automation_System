#include "config_storage.h"
#include <Preferences.h>

char MY_M_ID[MODULE_ID_LEN + 1];
char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
uint8_t consumerPins[MAX_CONSUMERS];
uint8_t NUM_CONSUMERS = 0;
uint32_t LORA_FREQ_HZ = LORA_FREQ_DEFAULT_HZ;
uint8_t  LORA_SF = RADIO_SF_DEFAULT;
uint32_t LORA_BW_HZ = RADIO_BW_DEFAULT_HZ;
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];
CeilingCounter txCounter;

uint8_t pinFromString(const char *s) {
  return (uint8_t)atoi(s);      // на ESP32 пиновете са просто GPIO номера
}

void loadConfigFromNvs() {
  Preferences prefs;
  prefs.begin("cfg", true);

  String id = prefs.getString("id", "");
  id.toCharArray(MY_M_ID, sizeof(MY_M_ID));

  NUM_CONSUMERS = prefs.getUChar("nc", 0);
  if (NUM_CONSUMERS > MAX_CONSUMERS) NUM_CONSUMERS = 0;

  for (uint8_t i = 0; i < NUM_CONSUMERS; i++) {
    char kId[8], kPin[8];
    snprintf(kId, sizeof(kId), "c%ui", (unsigned)i);
    snprintf(kPin, sizeof(kPin), "c%up", (unsigned)i);
    String cid = prefs.getString(kId, "");
    cid.toCharArray(consumerList[i], sizeof(consumerList[i]));
    consumerPins[i] = prefs.getUChar(kPin, 0);
  }

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
