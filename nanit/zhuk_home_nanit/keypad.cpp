// Мембранна клавіатура 4×4. Власне сканування замість бібліотеки Keypad, щоб
// калібруванням можна було поміняти місцями рядки й стовпці без перепаювання.
#include "zh.h"

static const char KEYS[16] PROGMEM = {'1', '2', '3', 'A', '4', '5', '6', 'B',
                                      '7', '8', '9', 'C', '*', '0', '#', 'D'};
static uint32_t kpAt;
static char kpPrev, kpStable;

void kpBegin() {
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(pins.kpRow[i], INPUT);
    pinMode(pins.kpCol[i], INPUT);
  }
}

// Рядки по черзі тягнемо до LOW, стовпці читаємо з підтяжкою.
// Якщо шлейф увімкнено навпаки, «рядками» стають піни стовпців.
static char kpScan() {
  const uint8_t *rows = cal.kpSwap ? pins.kpCol : pins.kpRow;
  const uint8_t *cols = cal.kpSwap ? pins.kpRow : pins.kpCol;
  for (uint8_t i = 0; i < 4; i++) {
    digitalWrite(rows[i], LOW);
    pinMode(rows[i], INPUT);
    pinMode(cols[i], INPUT_PULLUP);
  }
  char found = 0;
  for (uint8_t r = 0; r < 4 && !found; r++) {
    pinMode(rows[r], OUTPUT);
    delayMicroseconds(5);
    for (uint8_t c = 0; c < 4; c++) {
      if (digitalRead(cols[c]) == LOW) {
        found = pgm_read_byte(&KEYS[r * 4 + c]);
        break;
      }
    }
    pinMode(rows[r], INPUT);
  }
  return found;
}

// Повертає клавішу в момент натискання (після двох однакових сканувань), інакше 0
char kpUpdate(uint32_t now) {
  if (now - kpAt < cal.kpDeb) return 0;
  kpAt = now;
  char k = kpScan();
  char pressed = 0;
  if (k == kpPrev && k != kpStable) {
    kpStable = k;
    pressed = k;
  }
  kpPrev = k;
  return pressed;
}
