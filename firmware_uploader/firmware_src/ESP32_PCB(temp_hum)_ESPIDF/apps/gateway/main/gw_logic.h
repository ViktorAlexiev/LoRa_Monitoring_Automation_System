/*
  gw_logic - логиката на gateway-я (ESP-IDF): задачата "gw_cmd" (ядро 0, до Wi-Fi/MQTT стека).

  Държи състоянието на командите "в полет" (команда, state request), ACK таймаутите/повторенията и публикува в
  MQTT. Получава ВСИЧКО през ЕДНА опашка (gwInQ): валидирани радио събития от gw_radio и MQTT съобщения от
  задачата на esp-mqtt. Така няма общи променливи между ядрата - само опашки.

  Теми (същите като в Arduino версията - бекендът не се променя):
    IN  (подписани, QoS 1): commands, module_states_requests
    OUT: sensors (QoS 0), heartbeat (QoS 0), commands_status (QoS 1), module_states_response (QoS 1)
  Нищо не е retained. Не се публикува без връзка (виж mqtt_link.h) - няма "стари" показания след свързване.
*/
#ifndef GW_LOGIC_H
#define GW_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw_config.h"
#include "gw_types.h"

#define TOPIC_SENSORS         "sensors"
#define TOPIC_HEARTBEAT       "heartbeat"
#define TOPIC_COMMANDS        "commands"
#define TOPIC_COMMANDS_STATUS "commands_status"
#define TOPIC_STATE_REQ       "module_states_requests"
#define TOPIC_STATE_RESP      "module_states_response"

#define MAX_RETRIES              2                      // команда/state request: до 2 повторения
#define GW_HEARTBEAT_INTERVAL_MS (UINT32_C(30) * UINT32_C(1000))
#define GW_LOGIC_MAX_WAIT_MS     UINT32_C(5000)

void gw_logic_reset(const GwConfig *cfg, QueueHandle_t gwInQ, QueueHandle_t radioInQ);
void gw_logic_set_radio_task(TaskHandle_t t);

// Колбек на esp-mqtt (контекст на MQTT задачата): само слага в опашката
void gw_mqtt_on_message(const char *topic, const char *data, size_t len, void *ctx);

void gw_logic_handle(const GwMsg &m);          // една опашна единица
void gw_logic_tick(void);                      // таймаути, повторения, heartbeat
uint32_t gw_logic_next_wait_ms(void);          // до следващия срок

void gw_logic_task(void *arg);

// наблюдение/тестове
bool gw_cmd_pending(void);
bool gw_state_req_pending(void);

#endif
