// Ранній старт: запам'ятати причину перезапуску й одразу вимкнути сторожовий
// таймер, щоб після його спрацювання плата не перезапускалася по колу.
#include <avr/wdt.h>
#include "zh.h"

uint8_t bootResetFlags __attribute__((section(".noinit")));

void bootEarly() __attribute__((naked, used, section(".init3")));
void bootEarly() {
  bootResetFlags = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

uint8_t bootResetCause() {
  if (bootResetFlags & _BV(WDRF)) return RC_WDT;
  if (bootResetFlags & _BV(BORF)) return RC_BOR;
  if (bootResetFlags & _BV(EXTRF)) return RC_EXT;
  return RC_POR;
}
