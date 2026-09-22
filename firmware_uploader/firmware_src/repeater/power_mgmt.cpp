#include "power_mgmt.h"
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/power.h>

// ---------------- Watchdog - свободно тиктакащ таймер за HB разписание ----------------
// Арменпризиран ЕДИН път при boot (interrupt-only режим). AVR quirk: след първото
// прекъсване хардуерът се връща в reset-режим, освен ако ISR-ът не пре-разреши WDIE -
// затова го правим изрично на всяко прекъсване, иначе втория watchdog timeout би
// рестартирал чипа вместо просто да го събуди.
volatile uint8_t wdt_ticks = 0;
ISR(WDT_vect) {
  WDTCSR |= (1 << WDIE);
  wdt_ticks++;
}

void wdt_arm_8s_continuous() {
  MCUSR &= ~(1 << WDRF);
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0);
}

ISR(PCINT2_vect) {}  // само за събуждане (PCINT18 = D2 = LoRa DIO0)

void enablePinChangeWake() {
  PCICR  |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT18);
}

void deep_sleep_until_event() {
  power_adc_disable();
  power_usart0_disable();

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  cli();
  sleep_enable();
  sei();
  sleep_cpu();
  sleep_disable();

  power_adc_enable();
  power_usart0_enable();
}
