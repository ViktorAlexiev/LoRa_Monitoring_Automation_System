/*
  exec_radio - радио слоят на LoRa изпълнителя (ESP-IDF).

  ЕДНА задача ("exec_radio") е единственият собственик на SX1262: тя приема (събуждане от DIO1), строи и
  изпраща всичко (ACK/NACK, state response, restart response, heartbeat) и върти неблокиращия достъп до канала.
  Останалите задачи говорят с нея само през опашки:
      exec_radio --[CmdMsg]--> exec_cmd      (валидирана команда за изпълнение)
      exec_cmd   --[AckReq]--> exec_radio    (какво да се потвърди и кога)
  Така няма гонка за SPI шината и няма нужда от mutex.

  Тази задача НЕ върти в празно: exec_radio_service() връща след колко ms има следващо задължително събитие
  (край на пауза за ACK, стъпка на достъпа до канала, таймаут на retry, jitter на HB) и задачата чака
  точно толкова в ulTaskNotifyTake() - събужда я още DIO1, нова заявка от опашката или таймерът на HB.
  Когато няма какво да се чака, задачата блокира безкрайно и чипът заспива (автоматичен лек сън).
*/
#ifndef EXEC_RADIO_H
#define EXEC_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "exec_config.h"
#include "exec_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define EXEC_WAIT_FOREVER  UINT32_C(0xFFFFFFFF)

// Подготвя състоянието (чисто начално при всяко стартиране) и опашките.
void exec_radio_reset(const ExecConfig *cfg, QueueHandle_t cmdQ, QueueHandle_t ackInQ);

// Една обиколка: приема, обработва, строи и изпраща. Връща ms до следващото задължително повикване
// (EXEC_WAIT_FOREVER = само при събитие: DIO1, заявка от опашката, таймер на HB).
uint32_t exec_radio_service(void);

// Заявка за автоматичен state response след boot (RESTART вариант, чака ACK от gateway-я до 3 опита)
void exec_radio_request_restart_response(void);

// От таймера на HB (контекст на esp_timer задачата): вдига флаг и будi радио задачата
void exec_radio_hb_due(void);

void exec_radio_set_task(TaskHandle_t t);

// Само за наблюдение/тестове
bool exec_radio_tx_busy(void);
bool exec_radio_restart_pending(void);
int32_t exec_radio_ack_pending(void);

// Тялото на задачата "exec_radio" (arg = NULL)
void exec_radio_task(void *arg);

#endif
