#ifndef BOARD_PINS_H
#define BOARD_PINS_H


#include <stdint.h>
/*
  board_pins.h - ПИНОВЕ НА ESP32-S3 ПЛАТКАТА (един файл за всички приложения - в ESP-IDF
  компонентите се споделят, няма нужда от копия във всяка папка).

  ВАЖНО: стойностите по-долу са ВРЕМЕННИ (типични свободни GPIO на ESP32-S3-DevKitC). Когато платката е
  готова, попълни реалните пинове тук. Всички приложения ги ползват оттук.

  Радио: SX1262 по SPI (библиотеката RadioLib, виж agro_radio).
*/
#pragma message("ESP32_PCB (ESP-IDF): board_pins.h - пиновете са временни, провери ги за твоята платка")

#include "driver/spi_master.h"

// ---------- LoRa радио SX1262 (SPI) ----------
// Модулът още не е избран: пиновете и настройките на самия модул (TCXO, RF превключвател) са ВРЕМЕННИ - попълни ги за
// избрания модул (E22-400M22S, Ra-01SH, Heltec, Waveshare Core1262 ...). Всичко за радиото е на това място.
#define LORA_SPI_HOST   SPI2_HOST
#define LORA_SPI_HZ     8000000            // SX1262 издържа до 16 MHz; 8 MHz е запас за по-дълги проводници
#define LORA_SCK        12
#define LORA_MISO       13
#define LORA_MOSI       11
#define LORA_NSS        10                 // chip select (RadioLib го управлява като GPIO)
#define LORA_RST        9
#define LORA_BUSY       14                 // BUSY: SX126x е заето, докато е високо
#define LORA_DIO1       8                  // IRQ (RxDone...): ниво HIGH; събужда от лек сън (executor/repeater)

// RF превключвател на модула: или DIO2 на чипа го управлява (по подразбиране на RadioLib), или MCU пинове RXEN/TXEN
// (-1 = няма такъв пин). E22 модулите обикновено изискват RXEN/TXEN.
#define LORA_DIO2_AS_RF_SWITCH  1
#define LORA_RXEN       (-1)
#define LORA_TXEN       (-1)

// Захранване на осцилатора: напрежение на TCXO през DIO3 (1.6, 1.7, 1.8, 2.2, 2.4, 2.7, 3.0 или 3.3 V) или 0 за модул с
// кварц (XTAL). Грешна стойност: RadioLib връща -706/-707 (модул с кварц и зададен TCXO сам се поправя при begin).
#define LORA_TCXO_VOLTAGE       1.6f
#define LORA_REGULATOR_LDO_ONLY 0          // 1 = само LDO (без DC-DC); по подразбиране DC-DC (по-малък ток)

// Изходна мощност: SX1262 поддържа -9..+22 dBm. Горната граница е на избрания МОДУЛ (напр. 22 за E22-400M22S, 20 за
// модули с по-слаб PA) - никоя настройка не може да я надвиши. Работната мощност е настройка "txp" в конфигурацията.
#define LORA_TX_POWER_MIN_DBM   (-9)
#define LORA_TX_POWER_MAX_DBM   22
// Ограничение по тока (OCP). Библиотеката го оставя на 60 mA, което е малко за мощност над около +14 dBm на SX1262
// (PA тегли ~120 mA при +22 dBm); Semtech по подразбиране задава 140 mA за SX1262.
#define LORA_CURRENT_LIMIT_MA   140.0f

// ---------- I2C сензори (само sensor): два SHT31, всеки на СОБСТВЕНА шина ----------
// ESP32-S3 има два I2C контролера (порт 0 и 1), затова и двата сензора могат да са на един и същ адрес (0x44).
// Мултиплексор (напр. TCA9548A) - по-късно: тогава двата канала са на една шина и таблицата в sensors_io.cpp се променя.
#define I2C_SOIL_PORT   0      // почвен сензор
#define I2C_SOIL_SDA    4
#define I2C_SOIL_SCL    5
#define SHT31_SOIL_ADDR 0x44
#define I2C_AIR_PORT    1      // въздушен сензор
#define I2C_AIR_SDA     6
#define I2C_AIR_SCL     7
#define SHT31_AIR_ADDR  0x44
#define I2C_HZ          100000

// ---------- Батерия и ключове за захранване (ВРЕМЕННИ пинове - попълни реалните) ----------
// Литиева батерия (макс. 4.2 V) през делител 1:1 към вход на АЦП. Делителят и интерфейсите (сензори, консуматори) се
// захранват през load switch (HIGH = включен) - виж board_power.h за политиката по устройство.
#define BATT_ADC_GPIO          1      // вход на АЦП: ADC1 (GPIO1..10 на ESP32-S3; ADC2 пречи на Wi-Fi). Канал се извежда от пина
#define BATT_DIV_EN_GPIO       2      // load switch на ДЕЛИТЕЛЯ за батерията (включен само докато трае измерването)
#define IFACE_PWR_EN_GPIO      15     // load switch на ИНТЕРФЕЙСИТЕ (сензори, консуматори)
#define BATT_DIV_RATIO_NUM     2      // делител 1:1: Vбат = 2 x Vвход (при други резистори: (R1+R2)/R2 като дроб)
#define BATT_DIV_RATIO_DEN     1
#define BATT_SETTLE_MS         20     // колко се изчаква след включване на делителя (RC на делителя + кондензатора на входа)

// Кои GPIO на ESP32-S3 може да ползва потребителят за консуматори (валидира се в config стъпката):
//   0..21 и 38..48 съществуват; 22..25 ги няма; 26..32 са SPI flash/PSRAM; 33..37 са octal PSRAM
//   (заети на модулите с R8); 19/20 са USB D-/D+; 43/44 са UART0 (конзола).
// Ако платката ти е различна, промени условието.
static inline bool board_gpio_usable(int32_t pin) {
  if (pin < 0 || pin > 48) return false;
  if (pin >= 22 && pin <= 37) return false;
  if (pin == 19 || pin == 20) return false;
  if (pin == 43 || pin == 44) return false;
  return true;
}

#endif
