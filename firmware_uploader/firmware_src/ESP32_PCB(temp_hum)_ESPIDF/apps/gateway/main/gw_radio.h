/*
  gw_radio - радио слоят на gateway-я (ESP-IDF). ЕДНА задача ("gw_radio", ядро 1, най-висок приоритет) е
  единственият собственик на SX1262: приема (DIO1), декриптира/валидира/dedup-ва, предава командите.
  Не прави JSON и не говори с MQTT - това е на ядро 0 (gw_cmd): блокиращо Wi-Fi/MQTT свързване или бавна
  публикация не могат да отнемат ефирно време от приемането.

      gw_radio --[GwMsg]--> gw_cmd        (sensor, heartbeat, ACK, state response)
      gw_cmd   --[TxReq]--> gw_radio      (команда, state request, state-resp ACK)

  Предаването е със същата политика като преди (COMMAND): неблокиращ достъп до канала в рамките на задачата,
  без да се прекъсва приемането между проверките.
*/
#ifndef GW_RADIO_H
#define GW_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw_config.h"
#include "gw_types.h"

#define GW_RADIO_MAX_WAIT_MS   UINT32_C(5000)      // най-много толкова блокира задачата (Task Watchdog-ът иска признаци на живот)
#define GW_RX_BUF_MAX          220
#define GW_DEDUP_BUFFER_SIZE   8
#define GW_DEDUP_MAX_LEN       (CRYPTO_OVERHEAD + CRYPTO_MAX_PT)   // 64: най-големият пакет в системата
#define GW_TX_QUEUE_SIZE       4

void gw_radio_reset(const GwConfig *cfg, QueueHandle_t gwInQ, QueueHandle_t radioInQ);

// Една обиколка; връща ms до следващото задължително повикване (EXEC-подобно: 0xFFFFFFFF = само събитие)
uint32_t gw_radio_service(void);

void gw_radio_set_task(TaskHandle_t t);
void gw_radio_task(void *arg);

// наблюдение/тестове
uint32_t gw_radio_dedup_hits(void);
bool gw_radio_tx_active(void);

#endif
