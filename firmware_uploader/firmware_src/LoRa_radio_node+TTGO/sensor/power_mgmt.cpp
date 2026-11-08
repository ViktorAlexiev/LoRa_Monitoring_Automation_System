#include "power_mgmt.h"
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include "debug_log.h"

#if AGRO_HW_WDT
// След рестарт от watchdog той остава включен с най-късия период; тук (още преди main) се изключва и флагът се чисти.
void wdt_early_init(void) __attribute__((naked, used, section(".init3")));
void wdt_early_init(void) { MCUSR = 0; wdt_disable(); }
#endif

static volatile uint8_t wdt_count = 0;
// Само брои. WDIE се вдига отново от главната програма (wdt_feed), не оттук - виж power_mgmt.h.
ISR(WDT_vect) { wdt_count++; }

void wdt_feed() {
#if AGRO_HW_WDT
  // Четене-промяна-запис по ред: WDIF се чисти със запис на 1, затова се маскира (иначе чакащо прекъсване се губи).
  uint8_t v = WDTCSR;
  WDTCSR = (uint8_t)((v & ~(1 << WDIF)) | (1 << WDIE));
#endif
}

void wdt_start() {
  MCUSR &= ~(1 << WDRF);
  wdt_reset();
#if AGRO_HW_WDT
  wdt_enable(WDTO_8S);       // WDE=1, период 8 s (защитената последователност е в макроса)
  wdt_feed();                // + WDIE: прекъсване при първия таймаут, рестарт при втория без feed
#else
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0);
#endif
}

static void power_down_once() {
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  cli();
  sleep_enable();
#if defined(BODS) && defined(BODSE)
  sleep_bod_disable();       // brown-out детекторът добавя десетки uA в сън (само ATmega328P/picoPower); трябва веднага преди sleep_cpu()
#endif
  sei();                     // инструкцията след sei() се изпълнява преди прекъсване: няма празнина до sleep_cpu()
  sleep_cpu();
  sleep_disable();
}

void deep_sleep(uint8_t cycles) {
  DBG_FLUSH();               // изпразва серийния буфер: след power_usart0_disable() недопратените байтове се режат
  wdt_count = 0;
  wdt_reset();               // нова фаза: първият тик идва след пълни ~8 s
  wdt_feed();
  // Всичко, което не трябва в сън, се изключва (PRR): всеки включен модул тегли ток. SPI/TWI се включват наново от
  // LoRa.begin()/Wire.begin() на следващия цикъл; Timer0 остава (millis() след събуждане).
  power_adc_disable();
  power_usart0_disable();
  power_spi_disable();
  power_twi_disable();
  power_timer1_disable();
  power_timer2_disable();
  while (wdt_count < cycles) { power_down_once(); wdt_feed(); }   // всяко събуждане от watchdog = "жив съм"
  power_timer2_enable();
  power_timer1_enable();
  power_twi_enable();
  power_spi_enable();
  power_usart0_enable();
  power_adc_enable();
}
