#pragma once
#include <string.h>
#include <stdint.h>
struct FakeEEPROM {
  uint8_t mem[4096];
  FakeEEPROM() { memset(mem, 0xFF, sizeof(mem)); }
  template <typename T> void get(int a, T &v) { memcpy(&v, mem + a, sizeof(T)); }
  template <typename T> void put(int a, const T &v) { memcpy(mem + a, &v, sizeof(T)); }
  uint8_t read(int a) { return mem[a]; }
  void update(int a, uint8_t v) { mem[a] = v; }
};
extern FakeEEPROM EEPROM;
