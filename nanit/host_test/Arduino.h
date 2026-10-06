// Мінімальна імітація Arduino для тестів логіки на комп'ютері
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <algorithm>
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
#define SERIAL_TX_BUFFER_SIZE 64
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
inline size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
inline char *ltoa(long v, char *b, int) { sprintf(b, "%ld", v); return b; }
extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
inline uint32_t micros() { return fakeMillis * 1000; }
inline int digitalRead(uint8_t) { return 1; }
class HardwareSerial {
 public:
  std::string in, out;
  int txFree = 63;  // скільки місця в буфері передачі; тест може забрати його
  int available() { return in.size(); }
  int read() { if (in.empty()) return -1; char c = in[0]; in.erase(0, 1); return (uint8_t)c; }
  int availableForWrite() { return txFree; }
  size_t write(uint8_t c) { out += (char)c; return 1; }
  size_t write(const uint8_t *b, size_t n) { out.append((const char *)b, n); return n; }
};
extern HardwareSerial Serial, Serial3;
