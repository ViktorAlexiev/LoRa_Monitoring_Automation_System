// Заместител на WiFi.h - статусът на връзката се контролира от теста.
#ifndef FAKE_WIFI_H
#define FAKE_WIFI_H

#include "Arduino.h"

#define WIFI_STA 1
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6

extern int g_wifiStatus;
extern int g_wifiBeginCalls;
extern std::string g_wifiSsid;
extern std::string g_wifiPass;

class WiFiClass {
public:
  void mode(int) {}
  void begin(const char *ssid, const char *pass) { g_wifiBeginCalls++; g_wifiSsid = ssid ? ssid : ""; g_wifiPass = pass ? pass : ""; }
  int status() { return g_wifiStatus; }
};
extern WiFiClass WiFi;

class WiFiClient {};

#endif
