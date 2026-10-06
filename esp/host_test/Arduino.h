// Мінімальна імітація Arduino для тестів прошивки ESP на комп'ютері.
// Заліза тут немає: порт — два рядки в пам'яті, час — лічильник, який рухає сам тест.
#pragma once
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#define PROGMEM
#define PSTR(s) (s)
typedef const char *PGM_P;
class __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper *>(s))
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define strcmp_P strcmp
#define strcpy_P strcpy
#define memcpy_P memcpy
#define snprintf_P snprintf

// glibc до 2.38 не має strlcpy, а прошивка на неї спирається
inline size_t strlcpy(char *d, const char *s, size_t n) {
  size_t l = strlen(s);
  if (n) {
    size_t c = l < n - 1 ? l : n - 1;
    memcpy(d, s, c);
    d[c] = 0;
  }
  return l;
}

extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }

// Порт до Nanit. `txFree` імітує буфер передачі: прошивка не пише кадр, який не влазить
// цілком, і тест мусить уміти цю ситуацію відтворити.
class HardwareSerial {
 public:
  std::string in, out;
  int txFree = 256;

  void begin(unsigned long) {}
  int available() { return (int)in.size(); }
  int read() {
    if (in.empty()) return -1;
    char c = in[0];
    in.erase(0, 1);
    return (uint8_t)c;
  }
  int availableForWrite() { return txFree; }
  size_t write(uint8_t c) {
    out += (char)c;
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) {
    out.append((const char *)b, n);
    return n;
  }
  size_t print(const char *s) {
    out += s;
    return strlen(s);
  }
  size_t print(const __FlashStringHelper *s) { return print((const char *)s); }
  size_t println(const char *s) {
    out += s;
    out += '\n';
    return strlen(s) + 1;
  }
};
extern HardwareSerial Serial;
