#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include <Arduino.h>
#include <avr/wdt.h>

// РЕАЛЕН watchdog срещу забиване (AGRO_HW_WDT=1, по подразбиране). Режим "прекъсване И рестарт" (WDE=1, WDIE=1), 8 s:
//   - на всеки ~8 s таймаутът вдига прекъсване (будилникът за съня/разписанието), а хардуерът САМ чисти WDIE;
//   - главната програма трябва да вдигне WDIE отново (wdt_feed()) преди СЛЕДВАЩИЯ таймаут - това е доказателството, че
//     не е забита. Ако не го направи, следващият таймаут (8 s по-късно) РЕСТАРТИРА чипа.
// По datasheet WDIE не се вдига отново в самата ISR (би изпразнило смисъла на защитата) - затова е в главния цикъл.
// ВАЖНО: изисква bootloader, който след рестарт от watchdog изключва watchdog-а (Optiboot го прави). Със стар
// ATmegaBOOT чипът би влязъл в безкраен цикъл на рестарти (16 ms) и се възстановява само с ISP програматор.
// -DAGRO_HW_WDT=0 връща старото поведение (само прекъсване, без рестарт).
#ifndef AGRO_HW_WDT
#define AGRO_HW_WDT 1
#endif

// HB на всеки HB_INTERVAL_CYCLES watchdog цикъла (~8s всеки) - 38 × 8s ≈ 304s ≈ 5 мин
// (вместо точно 5 мин - не е критично, heartbeat е "жив съм" сигнал).
#define HB_INTERVAL_CYCLES  38

extern volatile uint8_t wdt_ticks;
// Атомарно изважда n тика, ако има поне толкова (ISR на watchdog-а ги увеличава). false = още няма n тика.
bool wdt_ticks_consume(uint8_t n);

void wdt_arm_8s_continuous();   // стартира watchdog-а (8 s); вика се първо в setup()
// "Жив съм": вдига WDIE отново. Вика се от главния цикъл и от всяко дълго изчакване (но най-много ~8 s между две
// извиквания, иначе следва рестарт). Без ефект при AGRO_HW_WDT=0.
void wdt_feed();
void enablePinChangeWake();

// Дълбок сън - буди се от watchdog (~8s, HB разписание) ИЛИ мигновено от PCINT (DIO0,
// входяща команда/state request). Извиква се само когато няма чакащи ACK/RX/HB.
void deep_sleep_until_event();

#endif
