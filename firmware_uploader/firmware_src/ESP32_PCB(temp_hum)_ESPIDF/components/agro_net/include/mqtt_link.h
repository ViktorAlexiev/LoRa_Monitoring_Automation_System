/*
  mqtt_link.h - MQTT клиент върху ESP-IDF esp-mqtt (вътрешна задача, автоматичен reconnect, събития).

  Заменя PubSubClient. Какво се променя спрямо Arduino версията:
    * абонирането е със QoS 1 и се повтаря при ВСЯКО свързване (clean session = брокерът не помни
      абонаментите между връзките; esp-mqtt не ги възстановява сам);
    * публикуването може да е QoS 1 (PubSubClient можеше само QoS 0);
    * НЕ се публикува, докато няма връзка (mqtt_link_publish връща false): иначе esp-mqtt би натрупал
      QoS 1 съобщения в outbox и би ги доставил късно, като "стари" показания след връзка;
    * съобщенията НИКОГА не са retained (освен изричен Last Will/онлайн статус на executor_wifi);
    * clean session = true: брокерът не доставя команди, пропуснати докато модулът е бил офлайн.

  Колбеците (съобщение, промяна на връзката) се изпълняват в задачата на esp-mqtt (ядро 0) - трябва да са
  кратки: само слагат в опашка.
*/
#ifndef MQTT_LINK_H
#define MQTT_LINK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MQTT_RECONNECT_MS   UINT32_C(5000)
#define MQTT_RESTART_MS     (UINT32_C(2) * UINT32_C(60) * UINT32_C(1000))
#define MQTT_KEEPALIVE_S    30
#define MQTT_MAX_TOPIC_LEN  96

typedef void (*MqttMsgFn)(const char *topic, const char *data, size_t dataLen, void *ctx);
typedef void (*MqttConnFn)(bool connected, void *ctx);

struct MqttLinkConfig {
  const char *host;
  uint16_t port;
  const char *user;        // "" = без автентикация
  const char *password;
  const char *clientId;
  // Last Will (retained, QoS 1); lwtTopic = NULL = без LWT
  const char *lwtTopic;
  const char *lwtMessage;
  // абонаменти (QoS subQos), повтарят се при всяко свързване
  const char *const *subTopics;
  uint8_t subCount;
  uint8_t subQos;
  MqttMsgFn onMessage;
  MqttConnFn onConnection;
  void *ctx;
};

// Подготвя клиента (копира низовете). Свързването почва при mqtt_link_network_up().
bool mqtt_link_init(const MqttLinkConfig *cfg);
// Вика се, когато мрежата е горе (wifi got-IP); стартира esp-mqtt еднократно.
void mqtt_link_network_up(void);

bool mqtt_link_connected(void);
// НЕ блокира: съобщението се записва в outbox на esp-mqtt и се изпраща от неговата задача (не от викащата).
// false (без записване), ако няма връзка или outbox-ът е пълен/клиентът отказва. true значи "прието за изпращане",
// не "доставено" - при внезапно падане на връзката съобщението може да не стигне до брокера.
bool mqtt_link_publish(const char *topic, const char *payload, size_t len, int32_t qos, bool retain);
// Колко ms Wi-Fi е свързан, а MQTT - не (0 = всичко е наред). След MQTT_RESTART_MS прошивката се рестартира.
uint32_t mqtt_link_down_ms(void);

#endif
