#include "config_storage.h"
#include <Preferences.h>

char MY_M_ID[MODULE_ID_LEN + 1];
char consumerList[MAX_CONSUMERS][CONSUMER_ID_LEN + 1];
uint8_t consumerPins[MAX_CONSUMERS];
uint8_t NUM_CONSUMERS = 0;
char WIFI_SSID_BUF[WIFI_SSID_LEN + 1];
char WIFI_PASSWORD_BUF[WIFI_PASSWORD_LEN + 1];
char MQTT_IP_BUF[MQTT_IP_LEN + 1];
uint16_t MQTT_PORT_VAL = 1883;
char MQTT_USER_BUF[MQTT_USER_LEN + 1];
char MQTT_PASSWORD_BUF[MQTT_PASSWORD_LEN + 1];

int consumerIndex(const char* c_id) {
  for (uint8_t i = 0; i < NUM_CONSUMERS; i++)
    if (strncmp(consumerList[i], c_id, CONSUMER_ID_LEN) == 0) return i;
  return -1;
}

void loadConfigFromNvs() {
  Preferences prefs;
  prefs.begin("cfg", true);   // true = само четене

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

  prefs.end();

  Serial.println(F("=== NVS CONFIG LOADED (executor_wifi) ==="));
  Serial.print(F("id="));        Serial.println(MY_M_ID);
  Serial.print(F("consumers=")); Serial.println(NUM_CONSUMERS);
  Serial.print(F("wifi_ssid=[")); Serial.print(WIFI_SSID_BUF); Serial.println(F("]"));
  Serial.print(F("mqtt_ip=[")); Serial.print(MQTT_IP_BUF); Serial.println(F("]"));
  Serial.print(F("mqtt_port=")); Serial.println(MQTT_PORT_VAL);
  Serial.println(F("========================================="));
}
