/*
  config_esp32.ino
  ----------------
  Stage-1 firmware за two-stage upload процеса (ESP32 / TTGO gateway).

  Качва се ПЪРВО (преди реалния gateway firmware).
  Слуша серийния порт за пакет от Python програмата във формат:

      CFG:{"id":"CS001","wifi_ssid":"MyNet","wifi_password":"secret",
           "mqtt_ip":"192.168.4.2","mqtt_port":"1883","mqtt_user":"","mqtt_password":""}

  Записва данните в NVS (чрез Preferences.h) под namespace "cfg",
  после отговаря ACK или NACK:<причина> по серийния.

  След това Python програмата качва РЕАЛНИЯ gateway firmware, който
  само ЧЕТЕ NVS при boot (не съдържа parsing/config логика изобщо).
*/

#include <Preferences.h>

#define MODULE_ID_LEN      6
#define WIFI_SSID_LEN       32
#define WIFI_PASSWORD_LEN   63
#define MQTT_IP_LEN         64
#define MQTT_PORT_LEN       5
#define MQTT_USER_LEN       32
#define MQTT_PASSWORD_LEN   63
#define CONFIG_TIMEOUT_MS   60000

Preferences prefs;

void setup() {
  Serial.begin(115200);
  waitForConfigPacket();
}

void loop() {
  // config-firmware не прави нищо друго - само чака в setup().
}

// ---------------------------------------------------------------------
// Чака до CONFIG_TIMEOUT_MS, но НЕ разчита на точен timing -
// праща "READY" на всеки 500ms, за да знае Python кога firmware-ът
// реално слуша сериен порт (независимо кога е станал reset-ът).
// ---------------------------------------------------------------------
void waitForConfigPacket() {
  unsigned long startTime = millis();
  unsigned long lastReadyMsg = 0;

  while (millis() - startTime < CONFIG_TIMEOUT_MS) {
    if (millis() - lastReadyMsg >= 500) {
      Serial.println("READY");
      lastReadyMsg = millis();
    }

    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.startsWith("CFG:")) {
        handleConfigLine(line);
        return;
      }
    }
  }
  sendNack("Timeout - няма CFG пакет в рамките на прозореца");
}

void handleConfigLine(const String &line) {
  String json = line.substring(4); // след "CFG:"

  String moduleId;
  if (!extractStringField(json, "id", moduleId)) {
    sendNack("Липсва 'id' поле");
    return;
  }
  if (moduleId.length() == 0 || moduleId.length() > MODULE_ID_LEN) {
    sendNack("Невалидна дължина на module id");
    return;
  }

  // WiFi/MQTT полета - задължителни само wifi_ssid и mqtt_ip; останалите могат да липсват/са празни
  String wifiSsid, wifiPassword, mqttIp, mqttPort, mqttUser, mqttPassword, freqStr, keyStr;
  bool hasSsid = extractStringField(json, "wifi_ssid", wifiSsid);
  bool hasMqttIp = extractStringField(json, "mqtt_ip", mqttIp);
  extractStringField(json, "wifi_password", wifiPassword);   // може да липсва -> празно
  extractStringField(json, "mqtt_port", mqttPort);
  extractStringField(json, "mqtt_user", mqttUser);
  extractStringField(json, "mqtt_password", mqttPassword);
  extractStringField(json, "freq", freqStr);                 // LoRa честота в Hz, опционално
  extractStringField(json, "key", keyStr);                   // AES мрежов ключ, hex, опционално

  if (!hasSsid || wifiSsid.length() == 0) {
    sendNack("Липсва или е празно wifi_ssid");
    return;
  }
  if (!hasMqttIp || mqttIp.length() == 0) {
    sendNack("Липсва или е празно mqtt_ip");
    return;
  }
  if (wifiSsid.length() > WIFI_SSID_LEN) {
    sendNack("wifi_ssid твърде дълго");
    return;
  }
  if (wifiPassword.length() > WIFI_PASSWORD_LEN) {
    sendNack("wifi_password твърде дълго");
    return;
  }
  if (mqttIp.length() > MQTT_IP_LEN) {
    sendNack("mqtt_ip твърде дълго");
    return;
  }
  if (mqttUser.length() > MQTT_USER_LEN) {
    sendNack("mqtt_user твърде дълго");
    return;
  }
  if (mqttPassword.length() > MQTT_PASSWORD_LEN) {
    sendNack("mqtt_password твърде дълго");
    return;
  }
  if (mqttPort.length() == 0) {
    mqttPort = "1883"; // default
  }

  writeConfigToNvs(moduleId, wifiSsid, wifiPassword, mqttIp, mqttPort, mqttUser, mqttPassword,
                    freqStr, keyStr);

  sendAck();
}

// ---------------------------------------------------------------------
// hex низ (2*N символа) -> N сурови байта. Връща false при невалиден hex.
// ---------------------------------------------------------------------
bool hexToBytes(const String &hex, uint8_t *out, uint8_t outLen) {
  if ((int)hex.length() != outLen * 2) return false;
  for (uint8_t i = 0; i < outLen; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    int8_t hiVal = -1, loVal = -1;
    if (hi >= '0' && hi <= '9') hiVal = hi - '0';
    else if (hi >= 'a' && hi <= 'f') hiVal = hi - 'a' + 10;
    else if (hi >= 'A' && hi <= 'F') hiVal = hi - 'A' + 10;
    if (lo >= '0' && lo <= '9') loVal = lo - '0';
    else if (lo >= 'a' && lo <= 'f') loVal = lo - 'a' + 10;
    else if (lo >= 'A' && lo <= 'F') loVal = lo - 'A' + 10;
    if (hiVal < 0 || loVal < 0) return false;
    out[i] = (uint8_t)((hiVal << 4) | loVal);
  }
  return true;
}

// ---------------------------------------------------------------------
// Записва в NVS (namespace "cfg")
// ---------------------------------------------------------------------
void writeConfigToNvs(const String &moduleId, const String &wifiSsid, const String &wifiPassword,
                       const String &mqttIp, const String &mqttPort,
                       const String &mqttUser, const String &mqttPassword,
                       const String &freqStr, const String &keyStr) {
  prefs.begin("cfg", false); // false = read/write mode
  prefs.putString("id", moduleId);
  prefs.putString("wssid", wifiSsid);
  prefs.putString("wpass", wifiPassword);
  prefs.putString("mqip", mqttIp);
  prefs.putString("mqport", mqttPort);
  prefs.putString("mquser", mqttUser);
  prefs.putString("mqpass", mqttPassword);
  // freq е опционално (само ако Python-ът го е пратил) - не презаписвай със 0, ако липсва
  if (freqStr.length() > 0) {
    prefs.putULong("freq", (uint32_t)freqStr.toInt());
  }
  // key - 32 hex символа = 16 bytes, само ако е валиден
  if (keyStr.length() == 32) {
    uint8_t keyBytes[16];
    if (hexToBytes(keyStr, keyBytes, 16)) {
      prefs.putBytes("key", keyBytes, 16);
    }
  }
  prefs.end();
}

// ---------------------------------------------------------------------
// Малка помощна функция за parsing (НЕ е пълен JSON parser -
// разчита на фиксирания формат, който винаги праща Python програмата)
// ---------------------------------------------------------------------
bool extractStringField(const String &src, const char *key, String &out) {
  String pattern = String("\"") + key + "\":\"";
  int start = src.indexOf(pattern);
  if (start == -1) return false;
  start += pattern.length();
  int end = src.indexOf('"', start);
  if (end == -1) return false;
  out = src.substring(start, end);
  return true;
}

void sendAck() {
  Serial.println("ACK");
}

void sendNack(const char *reason) {
  Serial.print("NACK:");
  Serial.println(reason);
}