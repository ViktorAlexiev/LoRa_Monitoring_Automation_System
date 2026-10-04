/*
  config_esp32.ino  (ESP32_PCB)
  -----------------------------
  Stage-1 firmware за two-stage upload процеса - ЕДИН за всички ESP32 устройства
  (sensor, executor, repeater, gateway, executor_wifi).

  Качва се ПЪРВО (преди реалния firmware). Слуша серийния порт за пакет от Python програмата:

      CFG:{"id":"CS001","freq":"433000000","freq_tx":"433500000","sf":"7","bw":"125000",
           "key":"<32 hex>",
           "consumers":[{"id":"V01","pin":"25"},{"id":"P01","pin":"26"}],
           "net":"1","wifi_ssid":"MyNet","wifi_password":"secret",
           "mqtt_ip":"192.168.4.2","mqtt_port":"1883","mqtt_user":"","mqtt_password":""}

  Само "id" е задължително. Останалите полета са по тип устройство:
    - sensor:        id, freq, sf, bw, key
    - repeater:      id, freq (RX), freq_tx, sf, bw, key
    - executor:      id, freq, sf, bw, key, consumers
    - gateway:       id, freq, sf, bw, key, net=1 + wifi/mqtt
    - executor_wifi: id, consumers, net=1 + wifi/mqtt
  "net":"1" означава, че wifi_ssid и mqtt_ip са задължителни.

  Записва в NVS (Preferences) под namespace "cfg" и отговаря ACK или NACK:<причина> по серийния.
  Реалният firmware само ЧЕТЕ NVS при boot (не съдържа parsing/config логика).

  NVS ключове (споделени с реалните firmware-и):
    id, freq, freqtx, sf, bw, key, nc, c<i>i (ID на консуматор), c<i>p (GPIO пин),
    wssid, wpass, mqip, mqport, mquser, mqpass
  Ключът "ceil" (nonce watermark) НЕ се пипа тук - управлява се само от реалния firmware.
*/

#include <Preferences.h>

#define MODULE_ID_LEN      6
#define CONSUMER_ID_LEN    4
#define MAX_CONSUMERS      10
#define WIFI_SSID_LEN       32
#define WIFI_PASSWORD_LEN   63
#define MQTT_IP_LEN         64
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

// Праща "READY" на всеки 500ms, за да знае Python кога firmware-ът реално слуша сериен порт.
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

void handleConfigLine(const String &line) {
  String json = line.substring(4);   // след "CFG:"

  String moduleId;
  if (!extractStringField(json, "id", moduleId)) { sendNack("Липсва 'id' поле"); return; }
  if (moduleId.length() == 0 || moduleId.length() > MODULE_ID_LEN) { sendNack("Невалидна дължина на module id"); return; }

  String freqStr, freqTxStr, sfStr, bwStr, keyStr, net;
  extractStringField(json, "freq", freqStr);
  extractStringField(json, "freq_tx", freqTxStr);
  extractStringField(json, "sf", sfStr);
  extractStringField(json, "bw", bwStr);
  extractStringField(json, "key", keyStr);
  extractStringField(json, "net", net);

  if (sfStr.length() > 0) {
    long sf = sfStr.toInt();
    if (sf < 7 || sf > 12) { sendNack("невалиден sf (трябва 7..12)"); return; }
  }
  if (bwStr.length() > 0) {
    long bw = bwStr.toInt();
    if (bw != 62500 && bw != 125000 && bw != 250000) { sendNack("невалиден bw (трябва 62500, 125000 или 250000 Hz)"); return; }
  }

  // ---- Консуматори (executor / executor_wifi) ----
  String cIds[MAX_CONSUMERS];
  uint8_t cPins[MAX_CONSUMERS];
  uint8_t numConsumers = 0;
  bool hasConsumers = false;
  int arrKey = json.indexOf("\"consumers\":[");
  if (arrKey != -1) {
    hasConsumers = true;
    int arrStart = arrKey + 13;
    int arrEnd = json.indexOf(']', arrStart);
    if (arrEnd == -1) { sendNack("Невалиден списък консуматори"); return; }
    String arr = json.substring(arrStart, arrEnd);
    int pos = 0;
    while (numConsumers < MAX_CONSUMERS) {
      int os = arr.indexOf('{', pos);
      if (os == -1) break;
      int oe = arr.indexOf('}', os);
      if (oe == -1) break;
      String obj = arr.substring(os, oe + 1);
      String cid, cpin;
      if (!extractStringField(obj, "id", cid) || !extractStringField(obj, "pin", cpin)) {
        sendNack("Невалиден консуматор запис"); return;
      }
      if (cid.length() == 0 || cid.length() > CONSUMER_ID_LEN) { sendNack("Невалидна дължина на consumer id"); return; }
      if (cpin.length() == 0 || cpin.length() > 2) { sendNack("Невалиден pin (GPIO номер)"); return; }
      long pinNum = cpin.toInt();
      if (pinNum < 0 || pinNum > 39) { sendNack("Невалиден GPIO номер (0..39)"); return; }
      cIds[numConsumers] = cid;
      cPins[numConsumers] = (uint8_t)pinNum;
      numConsumers++;
      pos = oe + 1;
    }
  }

  // ---- Wi-Fi / MQTT (gateway, executor_wifi) ----
  String wifiSsid, wifiPassword, mqttIp, mqttPort, mqttUser, mqttPassword;
  extractStringField(json, "wifi_ssid", wifiSsid);
  extractStringField(json, "wifi_password", wifiPassword);
  extractStringField(json, "mqtt_ip", mqttIp);
  extractStringField(json, "mqtt_port", mqttPort);
  extractStringField(json, "mqtt_user", mqttUser);
  extractStringField(json, "mqtt_password", mqttPassword);
  bool needNet = (net == "1");
  if (needNet) {
    if (wifiSsid.length() == 0) { sendNack("Липсва или е празно wifi_ssid"); return; }
    if (mqttIp.length() == 0) { sendNack("Липсва или е празно mqtt_ip"); return; }
  }
  if (wifiSsid.length() > WIFI_SSID_LEN) { sendNack("wifi_ssid твърде дълго"); return; }
  if (wifiPassword.length() > WIFI_PASSWORD_LEN) { sendNack("wifi_password твърде дълго"); return; }
  if (mqttIp.length() > MQTT_IP_LEN) { sendNack("mqtt_ip твърде дълго"); return; }
  if (mqttUser.length() > MQTT_USER_LEN) { sendNack("mqtt_user твърде дълго"); return; }
  if (mqttPassword.length() > MQTT_PASSWORD_LEN) { sendNack("mqtt_password твърде дълго"); return; }
  if (mqttPort.length() == 0) mqttPort = "1883";

  // ---- Запис в NVS ----
  prefs.begin("cfg", false);
  prefs.putString("id", moduleId);
  if (freqStr.length() > 0)   prefs.putULong("freq", (uint32_t)freqStr.toInt());
  if (freqTxStr.length() > 0) prefs.putULong("freqtx", (uint32_t)freqTxStr.toInt());
  if (sfStr.length() > 0)     prefs.putUChar("sf", (uint8_t)sfStr.toInt());
  if (bwStr.length() > 0)     prefs.putULong("bw", (uint32_t)bwStr.toInt());
  if (keyStr.length() == 32) {
    uint8_t keyBytes[16];
    if (hexToBytes(keyStr, keyBytes, 16)) prefs.putBytes("key", keyBytes, 16);
  }
  if (hasConsumers) {
    prefs.putUChar("nc", numConsumers);
    for (uint8_t i = 0; i < numConsumers; i++) {
      char kId[8], kPin[8];
      snprintf(kId, sizeof(kId), "c%ui", (unsigned)i);
      snprintf(kPin, sizeof(kPin), "c%up", (unsigned)i);
      prefs.putString(kId, cIds[i]);
      prefs.putUChar(kPin, cPins[i]);
    }
  }
  if (needNet) {
    prefs.putString("wssid", wifiSsid);
    prefs.putString("wpass", wifiPassword);
    prefs.putString("mqip", mqttIp);
    prefs.putString("mqport", mqttPort);
    prefs.putString("mquser", mqttUser);
    prefs.putString("mqpass", mqttPassword);
  }
  prefs.end();

  sendAck();
}

void sendAck() {
  Serial.println("ACK");
}

void sendNack(const char *reason) {
  Serial.print("NACK:");
  Serial.println(reason);
}
