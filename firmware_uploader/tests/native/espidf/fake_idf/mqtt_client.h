// Заместител на esp-mqtt (ESP-IDF 5.x API; ползват се само полетата, които прошивките реално ползват)
#ifndef FAKE_MQTT_CLIENT_H
#define FAKE_MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

typedef enum {
  MQTT_EVENT_ANY = -1, MQTT_EVENT_ERROR = 0, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
  MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_UNSUBSCRIBED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_DATA
} esp_mqtt_event_id_t;

typedef enum { MQTT_TRANSPORT_UNKNOWN = 0, MQTT_TRANSPORT_OVER_TCP } esp_mqtt_transport_t;

typedef struct {
  struct {
    struct {
      const char *hostname;
      uint32_t port;
      esp_mqtt_transport_t transport;
    } address;
  } broker;
  struct {
    const char *username;
    const char *client_id;
    struct { const char *password; } authentication;
  } credentials;
  struct {
    struct { const char *topic; const char *msg; int msg_len; int qos; int retain; } last_will;
    bool disable_clean_session;
    int keepalive;
  } session;
  struct {
    int reconnect_timeout_ms;
    int timeout_ms;
    bool disable_auto_reconnect;
  } network;
  struct { int size; int out_size; } buffer;
} esp_mqtt_client_config_t;

typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;

typedef struct {
  esp_mqtt_event_id_t event_id;
  esp_mqtt_client_handle_t client;
  char *data;
  int data_len;
  char *topic;
  int topic_len;
  int total_data_len;
  int current_data_offset;
  int msg_id;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, esp_mqtt_event_id_t id,
                                         esp_event_handler_t h, void *arg);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos, int retain);

#endif
