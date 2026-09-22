#ifndef RADIO_IO_H
#define RADIO_IO_H

#include <Arduino.h>

#define LORA_NSS    10
#define LORA_DIO0    2
#define LORA_RST     9

#define LORA_BANDWIDTH_HZ   125E3
#define LORA_SF             7
#define LORA_CR_DENOM       5
#define LORA_TX_POWER_DBM   17
#define LORA_PREAMBLE_LEN   8
#define LORA_SYNC_WORD       0x12

// Backoff преди forward - малък прозорец (CAD-ът поема реалната "канала зает ли е" проверка
// точно преди TX; jitter-ът само разминава КОГА двама repeater-и биха тръгнали да проверяват).
#define FORWARD_BACKOFF_MAX_MS  50
// HB jitter - разминава heartbeat-и от различни repeater-и (различна цел от backoff-а горе).
#define HB_JITTER_MAX_MS        150

// Слага се в края на препратен пакет - сигнализира на друг repeater "вече е препратен,
// не го препращай пак". Съществува само тук - Gateway не борави с тази константа изобщо,
// дедупликацията му сравнява само първите SENSOR_WIRE_LEN байта, независимо от маркера.
#define REPEATED_MARKER  0xD1

#define LORA_BEGIN_RETRY_MSG_MS  10000UL   // интервал между диагностични съобщения при неуспешен LoRa.begin()

void radio_setup();       // LoRa.setPins/begin/params, влиза в RX режим
void enter_rx_mode();

void receive_and_queue(int len);       // приема, marker/dedup проверка, слага в forward опашката
void process_pending_forwards();       // изпраща узрелите (backoff + CAD)
bool any_forward_pending();

// HB разписание, вътрешно управлявано (wdt_ticks + non-blocking jitter + CAD retry).
void heartbeat_tick();
bool heartbeat_pending();

#endif
