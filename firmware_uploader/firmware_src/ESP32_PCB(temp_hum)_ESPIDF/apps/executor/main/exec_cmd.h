/*
  exec_cmd - задачата "exec_cmd": изпълнява валидираните команди (GPIO) и заявява ACK/NACK към радио задачата.

  Изпълнението е НЕЗАБАВНО; само ACK-ът се бави RADIO_EXEC_ACK_DELAY_MS (gateway-ят трябва да излезе от TX и да
  влезе в RX - half-duplex), и това закъснение се носи от AckReq.sendAt, а не със заспиване в задачата - така
  следващата команда не чака ACK-а на предходната.
*/
#ifndef EXEC_CMD_H
#define EXEC_CMD_H

#include "exec_config.h"
#include "exec_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

void exec_cmd_init(const ExecConfig *cfg, QueueHandle_t cmdQ, QueueHandle_t ackInQ, TaskHandle_t radioTask);

// Изпълнява една команда (за тестове и от задачата)
void exec_cmd_process(const CommandPacket &in);

void exec_cmd_task(void *arg);

#endif
