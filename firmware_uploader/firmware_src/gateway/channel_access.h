/*
  channel_access.h / channel_access.cpp
  -------------------------------------
  Неблокираща state machine за достъп до канала (listen-before-talk) с CAD. Не знае нищо
  за радиото - получава функциите за CAD и за случайно число отвън (за да се тества
  нативно). Всяко извикване на channelAccessPoll() прави НАЙ-МНОГО един CAD (няколко ms) и
  връща веднага - не прекъсва останалата част от кода.

  Две политики:

  CH_POLICY_COMMAND (команда, ACK, restart response, state request/response):
    1. Първи CAD веднага. Свободно -> CH_CLEAR (без забавяне: никой не чака).
    2. Зает -> сондиране: по един CAD на всеки radioCadStepMs() до срок radioSensingWindowMs()
       (= airtime на най-дългия пакет). Броят проверки е ~15-24 на всеки SF.
    3. Щом каналът е свободен (след като е бил зает): случаен backoff 0..3 слота (слот =
       стъпката), после един последен CAD. Свободно -> CH_CLEAR; зает -> обратно на сондиране
       (срокът не се нулира).
    4. Срокът изтече -> CH_GIVEUP (по-горният слой решава: retry / отказ).

  CH_POLICY_TELEMETRY_FORCE / _SKIP (Sensor, heartbeat-и, препращане от Repeater):
    1. Първи CAD. Свободно -> CH_CLEAR.
    2. Зает -> ЕДНО случайно изчакване до radioTelemetryWaitMs(), после втори CAD.
    3. Свободно -> CH_CLEAR. Пак зает: FORCE -> CH_CLEAR (предай въпреки това - измерването е
       по-ценно от чистия ефир), SKIP -> CH_GIVEUP (heartbeat-ът се пропуска до следващия).
    Общо най-много 2 CAD-а.
*/
#ifndef CHANNEL_ACCESS_H
#define CHANNEL_ACCESS_H

#include <Arduino.h>
#include "radio_timing.h"

typedef bool     (*ChannelBusyFn)();                     // true = каналът е зает (CAD)
typedef uint32_t (*ChannelRandomFn)(uint32_t maxExcl);   // равномерно в [0, maxExcl)

#define CH_POLICY_COMMAND          0
#define CH_POLICY_TELEMETRY_FORCE  1
#define CH_POLICY_TELEMETRY_SKIP   2

#define CH_WAIT     0   // още не е решено - извикай пак по-късно (неблокиращо)
#define CH_CLEAR    1   // предавай сега
#define CH_GIVEUP   2   // откажи се от този опит

struct ChannelAccess {
  uint8_t  policy;
  uint8_t  state;
  uint32_t nextAt;     // millis() на следващата проверка
  uint32_t deadline;   // millis() на края на сондирането (само COMMAND)
};

void    channelAccessStart(ChannelAccess *ca, uint8_t policy);
uint8_t channelAccessPoll(ChannelAccess *ca, uint32_t now, ChannelBusyFn busy, ChannelRandomFn rnd);

// Стандартна реализация на случайното число (Arduino random)
inline uint32_t channelRandom(uint32_t maxExcl) {
  return maxExcl <= 1 ? 0 : (uint32_t)random((long)maxExcl);
}

#endif
