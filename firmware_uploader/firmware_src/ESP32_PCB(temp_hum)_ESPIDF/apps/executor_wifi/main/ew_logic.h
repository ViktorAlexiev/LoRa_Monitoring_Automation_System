/*
  ew_logic - Wi-Fi executor (ESP-IDF): приема команди директно по MQTT (без LoRa), превключва консуматори (GPIO)
  и отговаря със същите теми/JSON като gateway-я - бекендът не се променя.

  Теми:
    КЪМ модула (единствената нова): wifi_commands  (QoS 1, без retain, clean session)
        команда:             {"M_ID":"E007","C_ID":"V01","com":"A1"}    (A1 = включи, B2 = изключи)
        заявка за състояние: {"M_ID":"E007","com":"C3"}
      Всички Wi-Fi модули са абонирани за нея и филтрират по M_ID (чужда команда се игнорира). Gateway-ят НЕ е абониран.
    ОТ модула: commands_status (QoS 1), module_states_response (QoS 1; след boot с "restart":true), heartbeat (QoS 0),
               executors/<id>/lwt (retained; Last Will "offline" + "online" при свързване)

  Задачи: ядро 0 - Wi-Fi стек + esp-mqtt (системни); ядро 1 - "ew_exec" (приоритет 5): изпълнение и отговори.
  Колбекът на MQTT (ядро 0) само филтрира по M_ID и слага в опашка (8). Командите се изпълняват една по една;
  следващата тръгва от бекенда след отговора на предишната (опашката е в бекенда). Няма failsafe/seq.
*/
#ifndef EW_LOGIC_H
#define EW_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include "ew_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TOPIC_WIFI_COMMANDS   "wifi_commands"
#define TOPIC_COMMANDS_STATUS "commands_status"
#define TOPIC_STATE_RESP      "module_states_response"
#define TOPIC_HEARTBEAT       "heartbeat"

#define EW_HEARTBEAT_INTERVAL_MS (UINT32_C(30) * UINT32_C(1000))
#define EW_MSG_QUEUE_SIZE        8

#define EW_MSG_COMMAND    1
#define EW_MSG_STATE_REQ  2
#define EW_MSG_CONNECTED  3

struct EwMsg {
  uint8_t kind;
  uint8_t com;
  char c_id[CONSUMER_ID_LEN + 1];
};

void ew_reset(const EwConfig *cfg, QueueHandle_t msgQ);

// MQTT колбеци (контекст на esp-mqtt задачата)
void ew_on_message(const char *topic, const char *data, size_t len, void *ctx);
void ew_on_connection(bool connected, void *ctx);

void ew_handle(const EwMsg &m);           // една опашна единица (задачата и тестовете)
void ew_tick(void);                       // heartbeat
uint32_t ew_next_wait_ms(void);
void ew_exec_task(void *arg);

// Last Will тема/съобщение (за конфигурацията на MQTT клиента)
void ew_lwt_topic(const EwConfig *cfg, char *out, size_t cap);
void ew_lwt_message(const EwConfig *cfg, bool online, char *out, size_t cap);

#endif
