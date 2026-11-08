#ifndef DEDUP_H
#define DEDUP_H

#include <Arduino.h>

// Пази само суровите байтове на получената структура (без rssi/ts/маркер) - ring buffer,
// без timestamp: най-старият запис просто се презаписва при нов (имплицитно изтичане).
#define DEDUP_BUFFER_SIZE  6
#define DEDUP_MAX_LEN      24   // най-големият пакет през repeater-а: сензорен с батерия от ESP32 PCB (24 B); стар сензорен 22 B; heartbeat 14/16 B. По-дълъг се изхвърля.

bool dedupSeen(const uint8_t *data, uint8_t len);
void dedupAdd(const uint8_t *data, uint8_t len);

#endif
