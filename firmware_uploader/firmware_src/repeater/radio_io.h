#ifndef RADIO_IO_H
#define RADIO_IO_H

#include <Arduino.h>

#define LORA_NSS    10
#define LORA_DIO0    2
#define LORA_RST     9

// SF и BW НЕ са константи - идват от EEPROM (LORA_SF/LORA_BW_HZ в config_storage.h)
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   17
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD       0x12

// Малък случаен backoff преди първия опит за препращане - разминава кога двама repeater-и
// на едно ниво биха проверили канала (самият достъп до канала е в channel_access.h).
#define FORWARD_BACKOFF_MAX_MS  50
// HB jitter - разминава heartbeat-и от различни repeater-и.
#define HB_JITTER_MAX_MS        150

// Repeater е САМО uplink (RX честота = лентата на подателите, TX честота = лентата по-близо до
// Gateway). Няма маркер "вече препратен": веригата от repeater-и е разделена по честота (всеки
// хоп е на друга лента), така че loop е физически невъзможен, а препратеният пакет остава
// байт по байт идентичен с оригинала (dedup на следващото ниво го хваща точно).

#define LORA_BEGIN_RETRY_MSG_MS  10000UL   // интервал между диагностични съобщения при неуспешен LoRa.begin()

void radio_setup();       // LoRa.setPins/begin/params, влиза в RX режим
void enter_rx_mode();

void receive_and_queue(int len);       // приема, dedup проверка, слага в forward опашката
void process_pending_forwards();       // неблокиращ достъп до канала + изпращане на узрелите
bool any_forward_pending();

// HB разписание, вътрешно управлявано (wdt_ticks + non-blocking jitter + достъп до канала).
void heartbeat_tick();
bool heartbeat_pending();

#endif
