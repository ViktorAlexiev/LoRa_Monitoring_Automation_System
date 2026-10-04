#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/*
  board_pins.h - ПИНОВЕ НА ESP32 ПЛАТКАТА (ESP32_PCB)
  ----------------------------------------------------
  ВАЖНО: стойностите по-долу са ВРЕМЕННИ - взети са от TTGO LoRa32 V1. Когато платката е
  готова, ПОПЪЛНИ ги с реалните пинове (същият файл седи във всяка папка - sensor/, executor/,
  repeater/, gateway/, executor_wifi/ - arduino-cli компилира всяка като отделен скеч и не
  може да включва файлове извън нея; смени ги във всички копия).

  Радио: SX127x по SPI (библиотеката LoRa.h, непроменена спрямо ATmega версията).
*/
#pragma message("ESP32_PCB: board_pins.h - пиновете са временни (TTGO LoRa32 V1), провери ги за твоята платка")

// ---------- LoRa радио (SPI) ----------
#define LORA_SCK    5
#define LORA_MISO   19
#define LORA_MOSI   27
#define LORA_NSS    18     // chip select
#define LORA_CS     LORA_NSS
#define LORA_RST    14
#define LORA_DIO0   26     // RxDone/TxDone - събужда от лек сън (executor/repeater)

// ---------- I2C сензори (само sensor/) ----------
#define I2C_SDA     21
#define I2C_SCL     22

#endif
