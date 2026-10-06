// Налаштування, калібрування й код охорони в EEPROM.
// Діапазони й типові значення — docs/protocol.md, розділи 5.2 і 5.3.
// NanitLib зберігає серійний номер у кінці EEPROM, тому ми пишемо з початку.
#include <EEPROM.h>
#include <stddef.h>
#include "zh.h"

Cfg cfg;
Cal cal;
char secPin[7];

// humOn = 85, а не 66: у житловій кімнаті вологість 70 % — звичайна річ, і з порогом 66
// вентиляція вмикалася щоразу після старту й не вимикалася, поки вологість не впаде до 61.
// А поки двигун крутиться, він глушить канал від ESP, тож команду «вимкнути» не надіслати —
// вихід був лише один: вимкнути живлення будинку. Провітрювання за вологістю доречне від 85 %.
static const Cfg CFG_DEFAULT = {28, 26, 85, 40, 70, 8, 10, 3, 30, 15, 60, 1, 7, 6};
static const Cal CAL_DEFAULT = {
    0, 90, 90, 0,       // servo: closed, open, speed, rev
    100, 0,             // fan: speed, rev
    512, 3, 0,          // gate: steps, delay, rev
    0, 0,               // rgb: order, inv
    0,                  // tlight: rgb (0 — три світлодіоди)
    60, 200,            // pir: warm, pulse
    3, 2000, 1, 0,      // sound: peaks, win, siren, inv
    2000, 1,            // buzzer: tone, keys
    100, 0, 1, 1000,    // wsens: deb, inv, gest, hold
    0, 0,               // dht: tOff, hOff
    180, 190,           // mq7: warm, base
    400, 40,            // ldr: th, hyst
    0, 10,              // keypad: swap, deb
    0, 5, 200,          // sonar: off, n, max
    7                   // tm1637: br
};

// ---------- таблиця полів ----------
enum : uint8_t { FT_U8, FT_U16, FT_I8, FT_ORDER };
static const uint8_t CFG_SCOPE = 0xFF;
struct Field {
  uint8_t comp;  // CFG_SCOPE — налаштування, інакше номер модуля (Comp)
  char name[11];
  uint8_t off;
  uint8_t type;
  int16_t lo, hi;
};

#define CF(n, m, t, lo, hi) {CFG_SCOPE, n, offsetof(Cfg, m), t, lo, hi}
#define CL(c, n, m, t, lo, hi) {c, n, offsetof(Cal, m), t, lo, hi}
static const Field FIELDS[] PROGMEM = {
    CF("fanOn", fanOn, FT_U8, 20, 40),
    CF("fanOff", fanOff, FT_U8, 15, 39),
    CF("humOn", humOn, FT_U8, 40, 95),
    CF("coWarn", coWarn, FT_U8, 10, 90),
    CF("coDanger", coDanger, FT_U8, 20, 99),
    CF("openDist", openDist, FT_U8, 3, 30),
    CF("closeDelay", closeDelay, FT_U8, 3, 120),
    CF("stopDist", stopDist, FT_U8, 2, 10),
    CF("exitDelay", exitDelay, FT_U8, 0, 120),
    CF("entryDelay", entryDelay, FT_U8, 0, 60),
    CF("siren", siren, FT_U16, 10, 300),
    CF("quickArm", quickArm, FT_U8, 0, 1),
    CF("armMask", armMask, FT_U8, 1, 7),
    CF("nightMask", nightMask, FT_U8, 0, 7),
    CL(C_SERVO, "closed", servoClosed, FT_U8, 0, 180),
    CL(C_SERVO, "open", servoOpen, FT_U8, 0, 180),
    CL(C_SERVO, "speed", servoSpeed, FT_U8, 10, 180),
    CL(C_SERVO, "rev", servoRev, FT_U8, 0, 1),
    CL(C_FAN, "speed", fanSpeed, FT_U8, 30, 100),
    CL(C_FAN, "rev", fanRev, FT_U8, 0, 1),
    CL(C_TLIGHT, "rgb", tlRgb, FT_U8, 0, 16),
    CL(C_GATE, "steps", gateSteps, FT_U16, 100, 2048),
    CL(C_GATE, "delay", gateDelay, FT_U8, 2, 10),
    CL(C_GATE, "rev", gateRev, FT_U8, 0, 1),
    CL(C_RGB, "order", rgbOrder, FT_ORDER, 0, 5),
    CL(C_RGB, "inv", rgbInv, FT_U8, 0, 1),
    CL(C_PIR, "warm", pirWarm, FT_U8, 0, 120),
    CL(C_PIR, "pulse", pirPulse, FT_U16, 0, 2000),
    CL(C_SOUND, "peaks", soundPeaks, FT_U8, 1, 10),
    CL(C_SOUND, "win", soundWin, FT_U16, 500, 5000),
    CL(C_SOUND, "siren", soundSiren, FT_U8, 0, 1),
    CL(C_SOUND, "inv", soundInv, FT_U8, 0, 1),
    CL(C_BUZZER, "tone", buzTone, FT_U16, 500, 4000),
    CL(C_BUZZER, "keys", buzKeys, FT_U8, 0, 1),
    CL(C_WSENS, "deb", wsDeb, FT_U16, 0, 1000),
    CL(C_WSENS, "inv", wsInv, FT_U8, 0, 1),
    CL(C_WSENS, "gest", wsGest, FT_U8, 0, 1),
    CL(C_WSENS, "hold", wsHold, FT_U16, 200, 3000),
    CL(C_DHT, "tOff", dhtT, FT_I8, -5, 5),
    CL(C_DHT, "hOff", dhtH, FT_I8, -10, 10),
    CL(C_MQ7, "warm", mqWarm, FT_U16, 0, 600),
    CL(C_MQ7, "base", mqBase, FT_U16, 0, 1023),
    CL(C_LDR, "th", ldrTh, FT_U16, 0, 1023),
    CL(C_LDR, "hyst", ldrHyst, FT_U8, 0, 200),
    CL(C_KEYPAD, "swap", kpSwap, FT_U8, 0, 1),
    CL(C_KEYPAD, "deb", kpDeb, FT_U8, 5, 100),
    CL(C_SONAR, "off", sonarOff, FT_I8, -10, 10),
    CL(C_SONAR, "n", sonarN, FT_U8, 1, 9),
    CL(C_SONAR, "max", sonarMax, FT_U16, 50, 400),
    CL(C_TM1637, "br", tmBr, FT_U8, 0, 7),
};
#undef CF
#undef CL
static const uint8_t FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

static void fieldLoad(uint8_t i, Field *f) { memcpy_P(f, &FIELDS[i], sizeof(Field)); }

static long fieldRead(const Field &f, const uint8_t *base) {
  const uint8_t *p = base + f.off;
  switch (f.type) {
    case FT_U16: {
      uint16_t v;
      memcpy(&v, p, 2);
      return v;
    }
    case FT_I8: return (int8_t)*p;
    default: return *p;
  }
}

static void fieldWrite(const Field &f, uint8_t *base, long v) {
  uint8_t *p = base + f.off;
  if (f.type == FT_U16) {
    uint16_t w = v;
    memcpy(p, &w, 2);
  } else {
    *p = (uint8_t)v;
  }
}

// Знайти поле; повертає номер або -1
static int8_t fieldFind(uint8_t comp, const char *key, Field *f) {
  for (uint8_t i = 0; i < FIELD_COUNT; i++) {
    fieldLoad(i, f);
    if (f->comp == comp && strcmp(f->name, key) == 0) return i;
  }
  return -1;
}

static bool parseValue(const Field &f, const char *val, long *out) {
  if (f.type == FT_ORDER) {
    int8_t idx = pgmFind(N_ORDER, val);
    if (idx < 0) return false;
    *out = idx;
    return true;
  }
  return parseNum(val, f.lo, f.hi, out);
}

static bool cfgValid(const Cfg &c) {
  return c.fanOff < c.fanOn && c.coWarn < c.coDanger && c.stopDist < c.openDist;
}

static bool calValid(const Cal &c) { return c.servoClosed != c.servoOpen; }

// ---------- EEPROM ----------
static const uint8_t STORE_MAGIC = 'Z';
// Версія 5 відрізняється від 4 лише піднятим порогом humOn: структура та сама, тож запис
// версії 4 читається як є, міграція застосовується один раз (див. settingsBegin)
static const uint8_t STORE_VERSION = 5;
static const int STORE_ADDR = 0;
static const int SEC_STATE_ADDR = 256;
static const int SEC_STATE_LEN = 3;     // магія, стан, CRC
// Кільце слотів для положення стріли: 8 × 4 байти (кроки, номер, CRC)
static const int GATE_POS_ADDR = SEC_STATE_ADDR + SEC_STATE_LEN;  // 259..290
struct Store {
  uint8_t magic, version;
  Cfg cfg;
  Cal cal;
  char pin[7];
  uint8_t crc;
};

static bool dirty;
static uint32_t dirtyAt;

static void storeSave() {
  Store s;
  s.magic = STORE_MAGIC;
  s.version = STORE_VERSION;
  s.cfg = cfg;
  s.cal = cal;
  memcpy(s.pin, secPin, sizeof(s.pin));
  s.crc = crc8((const uint8_t *)&s, offsetof(Store, crc));
  EEPROM.put(STORE_ADDR, s);  // put записує лише змінені байти
}

void settingsBegin() {
  Store s;
  EEPROM.get(STORE_ADDR, s);
  bool old4 = s.version == 4;  // структура не змінювалась, тож поля запису версії 4 беремо як є
  bool ok = s.magic == STORE_MAGIC && (s.version == STORE_VERSION || old4) &&
            s.crc == crc8((const uint8_t *)&s, offsetof(Store, crc)) && s.pin[6] == 0 &&
            cfgValid(s.cfg) && calValid(s.cal);
  if (ok) {
    cfg = s.cfg;
    cal = s.cal;
    memcpy(secPin, s.pin, sizeof(secPin));
    if (old4) {
      // Підняття порога вологості для плат, які вже мають у EEPROM старі 66 %: інакше зміну
      // типового значення побачили б лише ті, хто скидав налаштування. Міграція прив'язана
      // до версії запису, а не до самого значення: перевірка «humOn < 80» не мала за що
      // зачепитися й спрацьовувала при кожному старті, тож задекларована половина
      // діапазону (40–79) була недосяжна — налаштування поверталось до 85 після перезапуску.
      cfg.humOn = CFG_DEFAULT.humOn;
      storeSave();  // уже з версією 5, тож наступного старту міграції не буде
    }
    return;
  }
  cfg = CFG_DEFAULT;
  cal = CAL_DEFAULT;
  strcpy_P(secPin, PSTR("1234"));
  storeSave();
  secStateSave(SEC_DIS);  // разом із заводським кодом не лишаємо будинок під охороною
  evPush(EV_CFG, -1);     // -1 — «reset»
}

void settingsTouch() {
  dirty = true;
  dirtyAt = millis();
}

// Запис через 1,5 с після останньої зміни, щоб повзунок не зношував EEPROM
void settingsUpdate(uint32_t now) {
  if (dirty && now - dirtyAt >= 1500) {
    dirty = false;
    storeSave();
  }
}

uint8_t cfgSet(const char *key, const char *val) {
  Field f;
  int8_t idx = fieldFind(CFG_SCOPE, key, &f);
  long v;
  if (idx < 0 || !parseValue(f, val, &v)) return NAK_ARG;
  Cfg next = cfg;
  fieldWrite(f, (uint8_t *)&next, v);
  if (!cfgValid(next)) return NAK_ARG;
  cfg = next;
  settingsTouch();
  evPush(EV_CFG, idx);
  return NAK_NONE;
}

uint8_t calSet(uint8_t comp, const char *key, const char *val) {
  Field f;
  int8_t idx = fieldFind(comp, key, &f);
  long v;
  if (idx < 0 || !parseValue(f, val, &v)) return NAK_ARG;
  Cal next = cal;
  fieldWrite(f, (uint8_t *)&next, v);
  if (!calValid(next)) return NAK_ARG;
  cal = next;
  settingsTouch();
  evPush(EV_CAL, comp);
  return NAK_NONE;
}

uint8_t fieldCount() { return FIELD_COUNT; }

bool fieldInfo(uint8_t i, uint8_t *comp, char *name, uint8_t nameSize, char *value, uint8_t valueSize) {
  if (i >= FIELD_COUNT) return false;
  Field f;
  fieldLoad(i, &f);
  *comp = f.comp;
  strlcpy(name, f.name, nameSize);
  long v = fieldRead(f, f.comp == CFG_SCOPE ? (const uint8_t *)&cfg : (const uint8_t *)&cal);
  if (f.type == FT_ORDER) pgmItem(N_ORDER, (uint8_t)v, value, valueSize);
  else ltoa(v, value, 10);
  return true;
}

// Стан охорони переживає вимкнення живлення: знятий, під охороною чи нічний.
// Магічне число плюс CRC, а не просто контрольний байт: з одним XOR випадковий вміст
// комірок мав шанс скластися в дійсний запис, і будинок піднімався б «під охороною» сам,
// а після скидання налаштувань — ще й із заводським кодом. Тепер на чистій EEPROM
// (0xFF) і на будь-якому смітті виходить «знято», бо магія не збігається.
static const uint8_t SEC_MAGIC = 0xA7;

void secStateSave(uint8_t state) {
  uint8_t b[2] = {SEC_MAGIC, state};
  EEPROM.update(SEC_STATE_ADDR, b[0]);
  EEPROM.update(SEC_STATE_ADDR + 1, b[1]);
  EEPROM.update(SEC_STATE_ADDR + 2, crc8(b, sizeof(b)));
}

uint8_t secStateLoad() {
  uint8_t b[2] = {EEPROM.read(SEC_STATE_ADDR), EEPROM.read(SEC_STATE_ADDR + 1)};
  if (b[0] != SEC_MAGIC || EEPROM.read(SEC_STATE_ADDR + 2) != crc8(b, sizeof(b))) return SEC_DIS;
  return (b[1] == SEC_ARM || b[1] == SEC_NIGHT) ? b[1] : (uint8_t)SEC_DIS;
}

// Положення стріли теж переживає вимкнення живлення. Датчика положення в шлагбаума
// немає, тож поки позиція жила лише в ОЗП, після старту прошивка вважала підняту
// стрілу опущеною — і перша ж команда «відкрити» довертала її ще на 90° у той самий бік.
// Пишемо лише в кінці руху, а не на кожен крок: 512 кроків на хід стерли б комірку.
// Перед рухом натомість пишемо ЦІЛЬ — саме це й дає гарантію «після втрати живлення на ходу
// вважаємо, що стріла доїхала»: помилка буде не більша за один хід, і лише тут.
// Пишемо не в одне місце, а по колу у вісім слотів: комірка EEPROM витримує близько
// 100 000 стирань, а на кожен рух стріли йде два записи (ціль перед рухом і позиція в
// кінці). В одному місці це дало б близько 25 000 циклів «відкрив-закрив», по колу —
// уже двісті тисяч. Найновіший слот шукаємо за номером: він зростає по колу, тож
// порівнюємо різницю як знакову, інакше після 255 «найновішим» став би найстаріший.
static const uint8_t GATE_SLOTS = 8, GATE_SLOT_LEN = 4;
static uint8_t gateSlot = 0xFF, gateSeq;

static bool gateSlotRead(uint8_t i, int16_t *pos, uint8_t *seq) {
  int addr = GATE_POS_ADDR + i * GATE_SLOT_LEN;
  uint8_t b[3];
  for (uint8_t k = 0; k < 3; k++) b[k] = EEPROM.read(addr + k);
  if (EEPROM.read(addr + 3) != crc8(b, sizeof(b))) return false;
  *pos = (int16_t)((uint16_t)b[1] << 8 | b[0]);
  *seq = b[2];
  return true;
}

void gatePosSave(int16_t pos) {
  if (gateSlot == 0xFF) gatePosLoad();  // ще не знаємо, у який слот писати
  gateSlot = (gateSlot + 1) % GATE_SLOTS;
  gateSeq++;
  uint8_t b[3] = {(uint8_t)pos, (uint8_t)(pos >> 8), gateSeq};
  int addr = GATE_POS_ADDR + gateSlot * GATE_SLOT_LEN;
  for (uint8_t i = 0; i < 3; i++) EEPROM.update(addr + i, b[i]);
  EEPROM.update(addr + 3, crc8(b, sizeof(b)));
}

int16_t gatePosLoad() {
  int16_t best = 0;
  bool found = false;
  uint8_t bestSeq = 0, bestSlot = GATE_SLOTS - 1;
  for (uint8_t i = 0; i < GATE_SLOTS; i++) {
    int16_t p;
    uint8_t s;
    if (!gateSlotRead(i, &p, &s)) continue;
    if (found && (int8_t)(s - bestSeq) <= 0) continue;
    best = p;
    bestSeq = s;
    bestSlot = i;
    found = true;
  }
  gateSlot = bestSlot;
  gateSeq = bestSeq;
  if (!found) return 0;                                    // чиста плата
  if (best < 0 || best > (int16_t)cal.gateSteps) return 0;  // калібрування змінили — нуль надійніший
  return best;
}
