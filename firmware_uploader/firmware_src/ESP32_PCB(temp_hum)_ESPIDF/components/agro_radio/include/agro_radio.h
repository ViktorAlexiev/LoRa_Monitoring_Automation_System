/*
  agro_radio.h - радио слоят на всички LoRa устройства: SX1262 през RadioLib.

  RadioLib (проверената библиотека за SX126x) се ползва ДИРЕКТНО - приложенията викат методите ѝ по RadioLib имената
  (radio.startReceive, radio.readData, radio.getRSSI, radio.sleep ...). Собственият HAL е готовият EspHal на самата
  библиотека (spi_master + GPIO на ESP-IDF). Тук има само последователностите, които библиотеката няма:

    radio_setup       begin + модем (честота, SF, BW, CR, sync word, преамбюл, мощност) + OCP + RF превключвател
    radio_listen      непрекъснат прием и повторно включване на прекъсването на DIO1
    radio_read_packet прочита получен пакет (или отхвърля повреден), изчиства прекъсването
    radio_transmit    излъчване със защитен таймаут (RadioLib няма таймаут на чакането)
    channelActive     CAD (listen-before-talk)

  Прекъсването на DIO1 е собствено (hal_irq_attach: ниво HIGH, ISR само праща notify на задачата), а не колбекът на
  RadioLib: нивото е нужно и за събуждане от автоматичния лек сън (виж agro_hal.h/power.h).

  Радиото има ЕДИН собственик (задача) или външен mutex (repeater): RadioLib не е thread-safe.
*/
#ifndef AGRO_RADIO_H
#define AGRO_RADIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "RadioLib.h"

extern SX1262 radio;      // единственият радио модул на платката

struct RadioParams {
  uint32_t freqHz;
  uint8_t sf;
  uint32_t bwHz;
  int8_t txPowerDbm;      // изходна мощност; ограничава се до LORA_TX_POWER_MIN_DBM..LORA_TX_POWER_MAX_DBM
};

// radioTimingInit + radio.begin + OCP + RF превключвател. Радиото остава в standby - извикващият вика radio_listen()
// или radio_transmit().
// maxTries == 0: опитва докато успее (за постоянно захранени устройства; диагностично съобщение на всеки
//   RADIO_BEGIN_RETRY_MSG_MS, между опитите задачата отстъпва процесора);
// maxTries  > 0: най-много толкова опита (sensor на батерия: не бива да изтощава батерията в цикъл).
#define RADIO_SPI_BUSY_TIMEOUT_MS  UINT32_C(100)      // колко чакаме BUSY да падне (RadioLib по подразбиране: 1000 ms)
#define RADIO_BEGIN_RETRY_MSG_MS   UINT32_C(10000)
#define RADIO_BEGIN_RETRY_PAUSE_MS UINT32_C(200)
bool radio_setup(const RadioParams *p, int32_t maxTries);

// Мощността се ограничава до максимума на модула (board_pins.h) - никоя настройка не може да го надвиши.
int8_t radio_clamp_tx_power(int32_t dbm);

// Сменя честотата (в standby) - repeater-ът има отделни честоти за приемане и предаване. false при грешка на чипа.
bool radio_set_frequency(uint32_t hz);

// Непрекъснат прием (RX continuous, DIO1 = RxDone) и връщане на прекъсването в готовност. Вика се след предаване,
// след CAD и при старт. false при грешка на чипа.
bool radio_listen(void);

// Прочита ЕДИН получен пакет в buf (до cap байта):
//   > 0                истинската дължина на пакета; в buf са прочетени първите min(дължина, cap) байта (по-дълъг от cap
//                      пакет се отрязва - извикващият го приема по дължината или го изхвърля); RSSI/SNR на последния
//                      пакет: radio.getRSSI() / radio.getSNR()
//   RADIO_RX_NONE      няма (повече) пакети
//   RADIO_RX_CRC_ERROR получен е пакет с лош CRC - изхвърлен е (радио-ниво CRC), извикващият продължава
// Изчиства прекъсването на чипа (DIO1 пада). Извикващият вика hal_irq_rearm(LORA_DIO1) СЛЕД като е свършил.
#define RADIO_RX_NONE       0
#define RADIO_RX_CRC_ERROR  (-1)
int32_t radio_read_packet(uint8_t *buf, size_t cap);

// true = в чипа чака непрочетен пакет (RxDone). Не започвай CAD/предаване и не сменяй честотата, докато не е прочетен -
// стартът на CAD и чистенето на флаговете го унищожават. Каналът в такъв момент е очевидно зает.
bool radio_rx_pending(void);

// Излъчва пакет и чака края (с таймаут, изведен от времето във въздуха). false при провал/таймаут. Оставя радиото в
// standby и прекъсването на DIO1 ИЗКЛЮЧЕНО - извикващият вика radio_listen(), когато иска да слуша пак.
bool radio_transmit(const uint8_t *buf, size_t len);

// CAD: true = каналът е зает точно сега (не предавай). Оставя радиото в standby и прекъсването на DIO1 изключено -
// извикващият вика radio_listen(), когато иска да слуша пак.
bool channelActive(void);

#ifdef AGRO_HOST_TEST
// Само за хост тестовете: RAM се нулира при рестарт (глобалните обекти на RadioLib се създават наново).
void radio_ram_reset(void);
#endif

#endif
