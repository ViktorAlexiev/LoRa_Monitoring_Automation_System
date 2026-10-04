#include "config_storage.h"
#include <Preferences.h>

char GATEWAY_ID[MODULE_ID_LEN + 1];
char WIFI_SSID_BUF[WIFI_SSID_LEN + 1];
char WIFI_PASSWORD_BUF[WIFI_PASSWORD_LEN + 1];
char MQTT_IP_BUF[MQTT_IP_LEN + 1];
uint16_t MQTT_PORT_VAL = 1883;
char MQTT_USER_BUF[MQTT_USER_LEN + 1];
char MQTT_PASSWORD_BUF[MQTT_PASSWORD_LEN + 1];
uint8_t  LORA_SF = RADIO_SF_DEFAULT;
uint32_t LORA_BW_HZ = RADIO_BW_DEFAULT_HZ;
uint32_t LORA_FREQ_HZ = 433000000UL;   // default, презаписва се от NVS в loadConfigFromNvs()
uint8_t NETWORK_KEY[CRYPTO_KEY_LEN];

#define CMD_CEILING_COMMIT_INTERVAL 100UL
static uint32_t cmdCeilingCurrent = 0;
static uint32_t cmdCeilingCommitted = 0;

static void cmdCeilingBegin() {
  Preferences p;
  p.begin("cfg", true);
  uint32_t stored = p.getULong("cmdceil", 0);
  p.end();
  cmdCeilingCurrent = stored;
  cmdCeilingCommitted = stored;
}

uint32_t cmdCeilingNext() {
  if (cmdCeilingCurrent >= cmdCeilingCommitted) {
    cmdCeilingCommitted = cmdCeilingCurrent + CMD_CEILING_COMMIT_INTERVAL;
    Preferences p;
    p.begin("cfg", false);
    p.putULong("cmdceil", cmdCeilingCommitted);
    p.end();
  }
  uint32_t val = cmdCeilingCurrent;
  cmdCeilingCurrent++;
  return val;
}

void loadConfigFromNvs() {
  Preferences prefs;
  prefs.begin("cfg", true);   // true = read-only

  String id = prefs.getString("id", "");
  id.toCharArray(GATEWAY_ID, sizeof(GATEWAY_ID));

  String ssid = prefs.getString("wssid", "");
  ssid.toCharArray(WIFI_SSID_BUF, sizeof(WIFI_SSID_BUF));

  String wpass = prefs.getString("wpass", "");
  wpass.toCharArray(WIFI_PASSWORD_BUF, sizeof(WIFI_PASSWORD_BUF));

  String mqip = prefs.getString("mqip", "");
  mqip.toCharArray(MQTT_IP_BUF, sizeof(MQTT_IP_BUF));

  String mqport = prefs.getString("mqport", "1883");
  MQTT_PORT_VAL = (uint16_t)mqport.toInt();
  if (MQTT_PORT_VAL == 0) MQTT_PORT_VAL = 1883;

  String mquser = prefs.getString("mquser", "");
  mquser.toCharArray(MQTT_USER_BUF, sizeof(MQTT_USER_BUF));

  String mqpass = prefs.getString("mqpass", "");
  mqpass.toCharArray(MQTT_PASSWORD_BUF, sizeof(MQTT_PASSWORD_BUF));

  uint32_t storedFreq = prefs.getULong("freq", 0);
  if (storedFreq != 0) LORA_FREQ_HZ = storedFreq;

  uint8_t sf = prefs.getUChar("sf", RADIO_SF_DEFAULT);
  LORA_SF = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  uint32_t bw = prefs.getULong("bw", RADIO_BW_DEFAULT_HZ);
  LORA_BW_HZ = (radioBwToIndex(bw) != 0xFF) ? bw : RADIO_BW_DEFAULT_HZ;

  size_t keyLen = prefs.getBytes("key", NETWORK_KEY, CRYPTO_KEY_LEN);
  if (keyLen != CRYPTO_KEY_LEN) {
    Serial.println("[WARN] мрежов ключ не е зареден от NVS - крипто операции ще се провалят");
  }

  prefs.end();

  cmdCeilingBegin();

  // DEBUG: покажи какво реално е заредено от NVS
  Serial.println("=== NVS CONFIG LOADED ===");
  Serial.print("id=");        Serial.println(GATEWAY_ID);
  Serial.print("wifi_ssid="); Serial.print("[");Serial.print(WIFI_SSID_BUF);Serial.println("]");
  Serial.print("wifi_pass_len="); Serial.println(strlen(WIFI_PASSWORD_BUF));
  Serial.print("mqtt_ip=");   Serial.print("[");Serial.print(MQTT_IP_BUF);Serial.println("]");
  Serial.print("mqtt_port="); Serial.println(MQTT_PORT_VAL);
  Serial.print("freq="); Serial.print(LORA_FREQ_HZ);
  Serial.print(" sf="); Serial.print(LORA_SF);
  Serial.print(" bw="); Serial.println(LORA_BW_HZ);
  Serial.println("=========================");
}
