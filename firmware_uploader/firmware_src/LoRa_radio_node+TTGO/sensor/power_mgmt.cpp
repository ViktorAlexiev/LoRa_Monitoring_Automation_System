#include "power_mgmt.h"
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/power.h>

static volatile uint8_t wdt_count = 0;
ISR(WDT_vect) { wdt_count++; }

static void wdt_arm_8s() {
  MCUSR &= ~(1 << WDRF);
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0);
}

static void power_down_once() {
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  cli();
  sleep_enable();
  sei();
  sleep_cpu();
  sleep_disable();
}

void deep_sleep(uint8_t cycles) {
  wdt_count = 0;
  wdt_arm_8s();
  power_adc_disable();
  power_usart0_disable();
  while (wdt_count < cycles) power_down_once();
  wdt_disable();
  power_adc_enable();
  power_usart0_enable();
}
