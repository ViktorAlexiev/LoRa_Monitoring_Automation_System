/*
  rep_radio - радио слоят на repeater-а (ESP-IDF). Repeater е САМО uplink: слуша на RX честотата (лентата на
  подателите), препраща БАЙТ ПО БАЙТ непроменени пакети на TX честотата (лентата по-близо до Gateway) и праща
  свой heartbeat. Няма маркер "вече препратен": веригата е разделена по честота (всеки хоп е на друга лента),
  така че цикъл е физически невъзможен, а препратеният пакет остава идентичен с оригинала (dedup на
  следващото ниво го хваща точно).

  Две задачи делят ЕДНО радио през mutex:
    rep_rx  (приоритет 6) - събужда се от DIO1, чете пакета, dedup, слага го в опашката за препращане
    rep_fwd (приоритет 5) - изчаква backoff, върти достъпа до канала на TX честотата, предава
  Докато rep_fwd чака канала (между проверките), mutex-ът е свободен и rep_rx продължава да приема.
*/
#ifndef REP_RADIO_H
#define REP_RADIO_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rep_config.h"

// Малък случаен backoff преди първия опит за препращане - разминава кога двама repeater-и на едно ниво
// биха проверили канала
#define FORWARD_BACKOFF_MAX_MS  50
#define HB_JITTER_MAX_MS        1000
#define DEDUP_BUFFER_SIZE       6
#define DEDUP_MAX_LEN           32     // покрива всички пакети, минаващи през repeater-а
#define FWD_QUEUE_SIZE          4

#define FWD_KIND_FRAME  1
#define FWD_KIND_HB     2

struct FwdItem {
  uint8_t kind;
  uint8_t len;
  uint8_t buf[DEDUP_MAX_LEN];
  uint32_t sendAt;         // hal_millis(): не по-рано
};

void rep_radio_reset(const RepConfig *cfg, QueueHandle_t fwdQ, SemaphoreHandle_t radioMutex);

// Стъпки (викат се с взет mutex на радиото; задачите ги обвиват)
bool rep_radio_begin(void);                 // radio_setup (докато успее) + режим на приемане на RX честотата
void rep_rx_service(void);                  // чете всички чакащи пакети -> dedup -> опашка; връща прекъсването
void rep_enter_rx_mode(void);

// Обработва един елемент от опашката (блокира в рамките на задачата fwd до предаването/отказа)
void rep_fwd_process(const FwdItem &it);

// От таймера на HB
void rep_hb_due(void);

void rep_rx_task(void *arg);
void rep_fwd_task(void *arg);

// за тестове/наблюдение
uint32_t rep_dedup_hits(void);

#endif
