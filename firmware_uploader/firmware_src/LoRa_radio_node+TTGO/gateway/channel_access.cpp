#include "channel_access.h"

#define CH_ST_FIRST_CAD  0
#define CH_ST_SENSING    1
#define CH_ST_BACKOFF    2
#define CH_ST_TELE_WAIT  3

#define CH_BACKOFF_SLOTS 4   // случаен backoff 0..3 слота

// signed сравнение - коректно и при пренасяне на 32-битовия millis()
static inline bool reached(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

void channelAccessStart(ChannelAccess *ca, uint8_t policy) {
  ca->policy   = policy;
  ca->state    = CH_ST_FIRST_CAD;
  ca->nextAt   = 0;
  ca->deadline = 0;
}

uint8_t channelAccessPoll(ChannelAccess *ca, uint32_t now, ChannelBusyFn busy, ChannelRandomFn rnd) {
  switch (ca->state) {

    case CH_ST_FIRST_CAD:
      if (!busy()) return CH_CLEAR;
      if (ca->policy == CH_POLICY_COMMAND) {
        ca->deadline = now + radioSensingWindowMs();
        ca->nextAt   = now + radioCadStepMs();
        ca->state    = CH_ST_SENSING;
      } else {
        ca->nextAt = now + rnd(radioTelemetryWaitMs() + 1);
        ca->state  = CH_ST_TELE_WAIT;
      }
      return CH_WAIT;

    case CH_ST_TELE_WAIT:
      if (!reached(now, ca->nextAt)) return CH_WAIT;
      if (!busy()) return CH_CLEAR;
      return ca->policy == CH_POLICY_TELEMETRY_FORCE ? CH_CLEAR : CH_GIVEUP;

    case CH_ST_SENSING:
      if (reached(now, ca->deadline)) return CH_GIVEUP;
      if (!reached(now, ca->nextAt)) return CH_WAIT;
      if (busy()) {
        ca->nextAt = now + radioCadStepMs();
        return CH_WAIT;
      }
      // каналът се освободи след като беше зает -> случаен слот, после последен CAD
      ca->nextAt = now + rnd(CH_BACKOFF_SLOTS) * radioSlotMs();
      ca->state  = CH_ST_BACKOFF;
      return CH_WAIT;

    case CH_ST_BACKOFF:
      if (!reached(now, ca->nextAt)) return CH_WAIT;
      if (!busy()) return CH_CLEAR;
      ca->nextAt = now + radioCadStepMs();   // пак зает -> обратно на сондиране (същият срок)
      ca->state  = CH_ST_SENSING;
      return CH_WAIT;
  }
  return CH_GIVEUP;   // недостижимо
}
