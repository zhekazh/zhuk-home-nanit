// Тести допоміжних функцій: CRC-8, списки назв, розбір чисел
#include "zh.h"
#include <stdio.h>
#include <assert.h>
int main() {
  assert(crc8((const uint8_t*)"123456789", 9) == 0xF4);
  const char *f = "SEC,DIS,-,0,-,0,0"; assert(crc8((const uint8_t*)f, strlen(f)) == 0xD0);
  char b[16];
  assert(pgmItem(N_SEC, 0, b, 16) && !strcmp(b, "DIS"));
  assert(pgmItem(N_SEC, 5, b, 16) && !strcmp(b, "ALARM"));
  assert(!pgmItem(N_SEC, 6, b, 16) && b[0] == 0);
  assert(pgmItem(N_COMP, 15, b, 16) && !strcmp(b, "tm1637"));
  assert(pgmFind(N_ONOFF, "on") == 1 && pgmFind(N_ONOFF, "o") == -1 && pgmFind(N_ONOFF, "offf") == -1 && pgmFind(N_ONOFF, "off") == 2);
  assert(pgmFind(N_COMP, "board") == 0 && pgmFind(N_COMP, "tm1637") == 15 && pgmFind(N_COMP, "") == -1);
  assert(pgmFind(N_MODE, "ARM") == 1 && pgmFind(N_MODE, "NIGHT") == 2);
  long v; assert(parseNum("28", 20, 40, &v) && v == 28); assert(!parseNum("41", 20, 40, &v)); assert(!parseNum("2x", 0, 99, &v)); assert(!parseNum("", 0, 9, &v));
  char item[16]; int n = 0; for (int i = 0; pgmItem(N_EV, i, item, 16); i++) n++; assert(n == 24);
  n = 0; for (int i = 0; pgmItem(N_COMP, i, item, 16); i++) n++; assert(n == 16);
  printf("util: all tests passed\n");
}
