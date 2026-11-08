#ifndef FAKE_ESP_WIFI_H
#define FAKE_ESP_WIFI_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

typedef struct { int dummy; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { 0 }

typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1 } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0 } wifi_interface_t;
typedef enum { WIFI_PS_NONE = 0, WIFI_PS_MIN_MODEM = 1 } wifi_ps_type_t;

typedef struct {
  struct {
    uint8_t ssid[32];
    uint8_t password[64];
  } sta;
} wifi_config_t;

typedef enum { WIFI_EVENT_STA_START = 2, WIFI_EVENT_STA_DISCONNECTED = 5 } wifi_event_t;
typedef enum { IP_EVENT_STA_GOT_IP = 0 } ip_event_t;

esp_err_t esp_wifi_init(const wifi_init_config_t *cfg);
esp_err_t esp_wifi_set_mode(wifi_mode_t m);
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t t);

#endif
