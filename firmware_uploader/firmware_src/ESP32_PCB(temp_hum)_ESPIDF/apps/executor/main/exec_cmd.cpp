#include "exec_cmd.h"
#include <inttypes.h>

#include <string.h>
#include "esp_log.h"
#include "consumers.h"
#include "agro_hal.h"
#include "protocol.h"
#include "radio_timing.h"

static const char *TAG = "exec_cmd";

static const ExecConfig *s_cfg = nullptr;
static QueueHandle_t s_cmdQ = nullptr, s_ackInQ = nullptr;
static TaskHandle_t s_radioTask = nullptr;

void exec_cmd_init(const ExecConfig *cfg, QueueHandle_t cmdQ, QueueHandle_t ackInQ, TaskHandle_t radioTask) {
  s_cfg = cfg;
  s_cmdQ = cmdQ;
  s_ackInQ = ackInQ;
  s_radioTask = radioTask;
}

void exec_cmd_process(const CommandPacket &in) {
  CommandPacket p = in;
  char c_id[5] = {0};
  memcpy(c_id, p.C_ID, 4);

  int32_t idx = consumers_index(&s_cfg->consumers, p.C_ID);
  bool cmdOk = (p.com == CMD_ON || p.com == CMD_OFF);

  if (idx >= 0 && consumers_usable(&s_cfg->consumers, idx) && cmdOk) {
    consumers_set(&s_cfg->consumers, idx, p.com == CMD_ON);
    ESP_LOGI(TAG, "[EXEC] %s (pin %" PRIu32 ") -> %s", c_id, (uint32_t)s_cfg->consumers.pin[idx], p.com == CMD_ON ? "ON" : "OFF");
    p.status = STATUS_ACK;
  } else {
    ESP_LOGW(TAG, "[REJECT] %s%s%s", c_id, idx < 0 ? " - консуматор не съществува" : (consumers_usable(&s_cfg->consumers, idx) ? "" : " - пинът не е използваем"),
             cmdOk ? "" : " - невалидна команда");
    p.status = STATUS_NACK;
  }

  // Пауза преди ACK: Gateway трябва да излезе от TX и да влезе в RX (half-duplex)
  AckReq r;
  r.pkt = p;
  r.sendAt = hal_millis() + RADIO_EXEC_ACK_DELAY_MS;
  if (xQueueSend(s_ackInQ, &r, 0) != pdTRUE) {
    ESP_LOGE(TAG, "[ERR] ACK опашката е пълна, потвърждението за %s е изгубено", c_id);
    return;
  }
  if (s_radioTask) xTaskNotifyGive(s_radioTask);
}

void exec_cmd_task(void *) {
  CommandPacket p;
  for (;;) {
    HAL_STACK_PROBE();
    if (xQueueReceive(s_cmdQ, &p, portMAX_DELAY) == pdTRUE) exec_cmd_process(p);
  }
}
