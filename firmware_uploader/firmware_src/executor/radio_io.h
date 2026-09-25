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

#define CMD_ON  0xA1
#define CMD_OFF 0xB2
#define STATUS_ACK  0
#define STATUS_NACK 2

#define JITTER_MAX_MS  150   // случайно закъснение преди heartbeat - разминава колизии с други устройства
#define LORA_BEGIN_RETRY_MSG_MS  10000UL   // интервал между диагностични съобщения при неуспешен LoRa.begin()

void radio_setup();          // LoRa.setPins/begin/params/enableCrc/receive

void radioReceivePoll();     // приема downlink команди, state request и state-resp ACK (всички криптирани)
void processRx();            // изпълнява команди от rxBuffer, слага ACK/NACK в опашка
void ackManager();           // взема узрелия ACK/NACK и го подава на TX слота (неблокиращ достъп до канала)
void radioTxTick();          // вика се на всяка обиколка: върти достъпа до канала и изпраща

// HB разписание, вътрешно управлявано (wdt_ticks + non-blocking jitter) - loop() само
// вика heartbeat_tick() всяка обиколка и проверява heartbeat_pending() за sleep gate-а.
void heartbeat_tick();
bool heartbeat_pending();

// Специален state response, изпратен автоматично веднага след boot (RESTART вариант) -
// маркира се с отделен crypto type-id, Gateway го препраща в MQTT като нормално съобщение,
// но трябва да го потвърди с ACK. Executor чака до 3 опита (ACK timeout се изчислява от SF/BW);
// ако не получи ACK нито веднъж, се отказва мълчаливо и продължава нормална работа.
void sendRestartStateResponse();   // вика се веднъж от setup(), след radio_setup()
void restartRespManager();         // вика се на всяка обиколка на loop()
bool restart_response_pending();   // gate за дълбок сън, докато чака ACK/retry

bool radio_tx_busy();              // gate за дълбок сън: има пакет в процес на достъп до канала/предаване

#endif
