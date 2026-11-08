#include "power_mgmt.h"
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <util/atomic.h>
#include "debug_log.h"
#include "radio_io.h"      // LORA_DIO0 - пинът, чието ниво HIGH значи "в чипа чака пакет"

// ---------------- Watchdog: свободно тиктакащ таймер за разписанието И реален watchdog срещу забиване ----------------
// Режим "прекъсване И рестарт" (виж power_mgmt.h): на всеки ~8 s прекъсване (събужда от съня, брои тикове за heartbeat),
// като хардуерът чисти WDIE. Главният цикъл го вдига отново с wdt_feed(); ако цикълът е забит, следващият таймаут
// рестартира чипа. WDIE НЕ се вдига в ISR (препоръка на datasheet).
#if AGRO_HW_WDT
// След рестарт от watchdog той остава включен с най-късия период; тук (още преди main) се изключва и флагът се чисти.
void wdt_early_init(void) __attribute__((naked, used, section(".init3")));
void wdt_early_init(void) { MCUSR = 0; wdt_disable(); }
#endif

volatile uint8_t wdt_ticks = 0;
ISR(WDT_vect) {
  wdt_ticks++;
}

void wdt_feed() {
#if AGRO_HW_WDT
  // WDIF се чисти със запис на 1, затова се маскира: иначе чакащо прекъсване би се загубило.
  uint8_t v = WDTCSR;
  WDTCSR = (uint8_t)((v & ~(1 << WDIF)) | (1 << WDIE));
#endif
}

// Атомарно: ISR увеличава wdt_ticks, а четене-изваждане-запис в основната програма е няколко инструкции -
// без защита един тик би се изгубил (heartbeat със закъснение от ~8 s).
bool wdt_ticks_consume(uint8_t n) {
  bool ok = false;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (wdt_ticks >= n) { wdt_ticks -= n; ok = true; }
  }
  return ok;
}

void wdt_arm_8s_continuous() {
  MCUSR &= ~(1 << WDRF);
  wdt_reset();
#if AGRO_HW_WDT
  wdt_enable(WDTO_8S);       // WDE=1, период 8 s
  wdt_feed();                // + WDIE: първият таймаут е прекъсване, вторият без feed е рестарт
#else
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0);
#endif
}

ISR(PCINT2_vect) {}  // само за събуждане (PCINT18 = D2 = LoRa DIO0)

void enablePinChangeWake() {
  PCICR  |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT18);
}

void deep_sleep_until_event() {
  DBG_FLUSH();                 // изпразва серийния буфер: след power_usart0_disable() недопратените байтове се губят/режат
  power_adc_disable();
  power_usart0_disable();

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  cli();
  // DIO0 е HIGH, докато RxDone не е изчистен. PCINT е по смяна на нивото: ако пакетът е дошъл след последното
  // parsePacket() и преди cli(), прекъсването вече е минало и нова смяна няма - без тази проверка чипът би спал
  // до следващия тик на watchdog-а (до ~8 s) с пакет в радиото.
  if (digitalRead(LORA_DIO0) == HIGH) {
    sei();
    power_usart0_enable();
    power_adc_enable();
    return;
  }
  sleep_enable();
#if defined(BODS) && defined(BODSE)
  sleep_bod_disable();         // brown-out детекторът добавя десетки uA в сън (само ATmega328P/picoPower)
#endif
  sei();                       // инструкцията след sei() се изпълнява преди прекъсване: няма празнина до sleep_cpu()
  sleep_cpu();
  sleep_disable();

  power_adc_enable();
  power_usart0_enable();
}
