// Назви для протоколу, CRC-8 і дрібні допоміжні функції.
#include "zh.h"

const char N_SEC[] PROGMEM = "DIS|EXIT|ARM|NIGHT|ENTRY|ALARM";
const char N_MODE[] PROGMEM = "-|ARM|NIGHT";
const char N_SENSOR[] PROGMEM = "pir|sound|wsens";
const char N_SRC[] PROGMEM = "kp|web|scene|gesture|timeout";
const char N_ONOFF[] PROGMEM = "auto|on|off";
const char N_WINM[] PROGMEM = "auto|man";
const char N_GATEM[] PROGMEM = "auto|open|close";
const char N_GATES[] PROGMEM = "closed|opening|open|closing";
const char N_DEV[] PROGMEM = "fan|win|gate|light";
const char N_SCENE[] PROGMEM = "away|home|night";
const char N_COMP[] PROGMEM = "board|servo|fan|gate|tlight|rgb|pir|sound|buzzer|wsens|dht|mq7|ldr|keypad|sonar|tm1637";
const char N_EV[] PROGMEM =
    "BOOT|ARMING|ARMED|DISARM|CANCEL|ENTRY|ALARM|SIREN_END|PIN_BAD|PIN_LOCK|PIN_SET|CO_HIGH|CO_OK|"
    "CO_MUTE|FAN|GATE|WIN_BLOCK|MODE|SVC|CFG|CAL|HW_ERR|HW_OK|EV_LOST";
const char N_NAK[] PROGMEM = "-|type|arg|state|pin|lock|busy";
const char N_HWST[] PROGMEM = "none|ok|warn|err";
const char N_HWERR[] PROGMEM = "timeout|crc|noecho|nack";
const char N_FANR[] PROGMEM = "temp|hum|co|manual|svc";
const char N_GATER[] PROGMEM = "car|left|manual|armed|test";
const char N_RESET[] PROGMEM = "POR|EXT|WDT|BOR";
const char N_NET[] PROGMEM = "off|sta|ap";
const char N_ORDER[] PROGMEM = "RGB|RBG|GRB|GBR|BRG|BGR";

// CRC-8/SMBUS: поліном 0x07, початкове значення 0x00. crc8("123456789") = 0xF4
uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t c = 0;
  while (len--) {
    c ^= *data++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

// Елемент списку «a|b|c» за номером
bool pgmItem(const char *list, uint8_t idx, char *out, uint8_t size) {
  uint8_t cur = 0, n = 0;
  for (;; list++) {
    char c = pgm_read_byte(list);
    if (c == '|' || c == 0) {
      if (cur == idx) {
        out[n] = 0;
        return true;
      }
      if (c == 0) break;
      cur++;
      n = 0;
      continue;
    }
    if (cur == idx && n + 1 < size) out[n++] = c;
  }
  out[0] = 0;
  return false;
}

// Номер елемента списку «a|b|c» або -1
int8_t pgmFind(const char *list, const char *s) {
  int8_t idx = 0;
  const char *p = s;
  bool match = true;
  for (;; list++) {
    char c = pgm_read_byte(list);
    if (c == '|' || c == 0) {
      if (match && *p == 0) return idx;
      if (c == 0) return -1;
      idx++;
      p = s;
      match = true;
      continue;
    }
    if (match) {
      if (*p == c) p++;
      else match = false;
    }
  }
}

// Список назв режимів пристрою. Один на всіх, хто його потребує (кадр EV і autoMode):
// інакше додавання пристрою вимагало б однакової правки у двох файлах
const char *devModeList(uint8_t devId) {
  switch (devId) {
    case DEV_WIN: return N_WINM;
    case DEV_GATE: return N_GATEM;
    default: return N_ONOFF;  // вентилятор і світло
  }
}

bool parseNum(const char *s, long lo, long hi, long *out) {
  if (!s || !*s) return false;
  if (!isdigit((unsigned char)s[0]) && s[0] != '-') return false;  // без пробілів і «+»
  char *end;
  long v = strtol(s, &end, 10);
  if (*end || v < lo || v > hi) return false;
  *out = v;
  return true;
}

extern char *__brkval;
extern char __heap_start;
int freeRam() {
  char top;
  return &top - (__brkval ? __brkval : &__heap_start);
}
