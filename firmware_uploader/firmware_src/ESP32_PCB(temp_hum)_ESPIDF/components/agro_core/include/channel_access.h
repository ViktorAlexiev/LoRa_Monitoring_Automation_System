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

  CH_POLICY_TELEMETRY_PERSIST (ESP-IDF: sensor и препращане от Repeater; НЯМА в Arduino версията):
    Като COMMAND (сондиране до свободен канал, случаен слот, последен CAD) - НО при изтичане на срока
    (= чуждото предаване е по-дълго от най-дългия възможен пакет, т.е. каналът е "запушен", не зает за кратко)
    се ПРЕДАВА въпреки това (CH_CLEAR, не CH_GIVEUP): измерването е по-ценно от чистия ефир.
    Защо: при FORCE (по-долу) вторият, който чуе зает канал, изчаква само случайно време (<= airtime на 30 B) и
    предава ВЪРХУ още траещото предаване - два repeater-а, препращащи един и същ кадър, го разваляха в 2 от 3
    цикъла (виж системната симулация). Тук вторият изчаква края на първия (дубликатът се отсява от dedup).

  CH_POLICY_TELEMETRY_FORCE / _SKIP (heartbeat-и; Arduino: и Sensor/Repeater):
    1. Първи CAD. Свободно -> CH_CLEAR.
    2. Зает -> ЕДНО случайно изчакване до radioTelemetryWaitMs(), после втори CAD.
    3. Свободно -> CH_CLEAR. Пак зает: FORCE -> CH_CLEAR (предай въпреки това - измерването е
       по-ценно от чистия ефир), SKIP -> CH_GIVEUP (heartbeat-ът се пропуска до следващия).
    Общо най-много 2 CAD-а.
*/
#ifndef CHANNEL_ACCESS_H
#define CHANNEL_ACCESS_H

#include <stdint.h>
#include <stdbool.h>
#include "radio_timing.h"
#include "agro_hal.h"

typedef bool     (*ChannelBusyFn)();                     // true = каналът е зает (CAD)
typedef uint32_t (*ChannelRandomFn)(uint32_t maxExcl);   // равномерно в [0, maxExcl)

#define CH_POLICY_COMMAND          0
#define CH_POLICY_TELEMETRY_FORCE  1
#define CH_POLICY_TELEMETRY_SKIP   2
#define CH_POLICY_TELEMETRY_PERSIST 3

#define CH_WAIT     0   // още не е решено - извикай пак по-късно (неблокиращо)
#define CH_CLEAR    1   // предавай сега
#define CH_GIVEUP   2   // откажи се от този опит

struct ChannelAccess {
  uint8_t  policy;
  uint8_t  state;
  uint32_t nextAt;     // hal_millis() на следващата проверка
  uint32_t deadline;   // hal_millis() на края на сондирането (само COMMAND)
};

void    channelAccessStart(ChannelAccess *ca, uint8_t policy);
uint8_t channelAccessPoll(ChannelAccess *ca, uint32_t now, ChannelBusyFn busy, ChannelRandomFn rnd);

// След колко ms има смисъл да се извика channelAccessPoll() пак (0 = веднага). Позволява на задача да
// блокира до следващото реално събитие вместо да върти в празно: между проверките процесорът (и чипът,
// при автоматичния лек сън) може да спи.
uint32_t channelAccessNextMs(const ChannelAccess *ca, uint32_t now);

// Стандартна реализация на случайното число (хардуерен RNG)
inline uint32_t channelRandom(uint32_t maxExcl) {
  return hal_random(maxExcl);
}

#endif
