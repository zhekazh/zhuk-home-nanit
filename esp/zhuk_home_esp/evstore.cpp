// Журнал подій у файлі: переживає перезапуск ESP.
//
// Флеш витримує близько 100 000 стирань на сектор, а LittleFS у цьому ядрі переносить
// дані з блоку після 16 циклів, тобто розмазує зношування по всьому розділу. При записі
// раз на секунду розділу на 1 МБ вистачило б менш ніж на рік, тому тут два запобіжники:
//
//   1. файл має **постійний розмір** і перезаписується на місці — він не росте, і LittleFS
//      не мусить щоразу перебудовувати метадані під нову довжину;
//   2. записи накопичуються в пам'яті й скидаються **порціями** — раз на п'ять хвилин або
//      коли черга майже повна, а також перед перезапуском, оновленням і скиданням.
//
// Заголовка з позицією тут навмисно немає: кожен запис несе свій монотонний номер, тож
// голову кільця знаходимо як запис із найбільшим номером. Інакше заголовок оновлювався б
// при кожному скиданні й зношувався б швидше за самі записи.
//
// Саме номер, а не час: подія до синхронізації з NTP має час 0 (перша така — BOOT, вона
// приходить при кожному старті), і по часу голова стала б на неї, щойно кільце обернеться.
// Тоді перезаписувались би свіжі події, а старші за неї лежали б вічно. Так само зроблено
// на Nanit для кільця положень стріли — gatePosLoad у nanit/zhuk_home_nanit/settings.cpp.
#include <LittleFS.h>

#include "zhe.h"

static const char EV_PATH[] = "/events";
static const uint16_t EV_SLOTS = 120;  // 120 × 80 Б = 9600 Б, менше за три сектори
static const uint8_t EV_SLOT = 80;

struct Pending {
  uint32_t at;
  char raw[EV_LEN];
};

static Pending queue[12];
static uint8_t qCount;
static uint16_t slotNext;    // куди писати наступний запис
static uint32_t seqNext = 1; // яким номером його підписати; 0 не використовуємо
static uint32_t flushAt;
static bool ready;

// Слот у файлі: «<номер запису>\t<unix>\t<кадр EV>», добитий пробілами до постійної
// довжини. Два числа беруть щонайбільше 22 знаки з 79 наявних, тож на кадр лишається 57 —
// кадри EV коротші: найдовший у docs/protocol.md, «EV,65535,MODE,fan,auto,timeout», це
// 30 знаків. Якщо кадр усе ж довший, обрізається саме він, а числа з табами лишаються
// цілими — тобто слот розбирається далі, хай і з обрізаною подією.
static void slotWrite(File &f, uint16_t slot, uint32_t seq, uint32_t at, const char *raw) {
  char buf[EV_SLOT + 1];
  int n = snprintf(buf, sizeof(buf), "%lu\t%lu\t%s", (unsigned long)seq, (unsigned long)at, raw);
  if (n < 0) n = 0;
  if (n > EV_SLOT - 1) n = EV_SLOT - 1;
  memset(buf + n, ' ', EV_SLOT - 1 - n);
  buf[EV_SLOT - 1] = '\n';
  f.seek((uint32_t)slot * EV_SLOT, SeekSet);
  f.write((const uint8_t *)buf, EV_SLOT);
}

// Створює файл повного розміру одразу: далі він лише перезаписується на місці.
static bool fileInit() {
  File f = LittleFS.open(EV_PATH, "w");
  if (!f) return false;
  char blank[EV_SLOT];
  memset(blank, ' ', EV_SLOT - 1);
  blank[EV_SLOT - 1] = '\n';
  for (uint16_t i = 0; i < EV_SLOTS; i++) f.write((const uint8_t *)blank, EV_SLOT);
  f.close();
  logf("журнал: створено %u слотів", EV_SLOTS);
  return true;
}

// Розбирає слот на місці: номер запису, час і кадр без хвостових пробілів. false — слот
// порожній, зіпсований або в старому форматі («<unix>\t<кадр>», без номера): там другого
// таба немає, тож сплутати їх неможливо.
static bool slotParse(char *buf, uint32_t *seq, uint32_t *at, char **raw) {
  if (!isdigit((unsigned char)buf[0])) return false;
  char *t1 = strchr(buf, '\t');
  if (!t1 || !isdigit((unsigned char)t1[1])) return false;
  char *t2 = strchr(t1 + 1, '\t');
  if (!t2 || !t2[1] || t2[1] == ' ' || t2[1] == '\n') return false;
  *t1 = 0;
  *t2 = 0;
  char *end = t2 + 1;
  while (*end && *end != '\n' && *end != ' ') end++;
  *end = 0;
  *seq = strtoul(buf, nullptr, 10);
  *at = strtoul(t1 + 1, nullptr, 10);
  *raw = t2 + 1;
  return true;
}

void evStoreBegin() {
  if (!LittleFS.exists(EV_PATH) || LittleFS.open(EV_PATH, "r").size() != (uint32_t)EV_SLOTS * EV_SLOT) {
    if (!fileInit()) {
      logPrint("журнал: файл не створився");
      return;
    }
  }
  ready = true;
  flushAt = millis();

  // Голова кільця — одразу за записом із найбільшим номером. Різницю номерів порівнюємо
  // як знакову: після переповнення лічильника найновіший запис інакше виглядав би
  // найстарішим. Зіпсований слот просто пропускаємо — він не збиває пошук.
  File f = LittleFS.open(EV_PATH, "r");
  if (!f) return;
  uint32_t best = 0;
  bool found = false, junk = false;
  uint16_t bestSlot = 0;
  char buf[EV_SLOT + 1];
  for (uint16_t i = 0; i < EV_SLOTS; i++) {
    f.seek((uint32_t)i * EV_SLOT, SeekSet);
    int got = f.read((uint8_t *)buf, EV_SLOT);
    if (got != EV_SLOT) break;
    buf[EV_SLOT] = 0;
    uint32_t seq, at;
    char *raw;
    if (!slotParse(buf, &seq, &at, &raw)) {
      if (buf[0] != ' ') junk = true;  // слот не порожній, а прочитати його нічим
      continue;
    }
    if (found && (int32_t)(seq - best) <= 0) continue;
    best = seq;
    bestSlot = i;
    found = true;
  }
  f.close();

  if (found) {
    slotNext = (bestSlot + 1) % EV_SLOTS;
    seqNext = best + 1;
  } else {
    slotNext = 0;
    seqNext = 1;
    // Жоден слот не розбирається, а порожнім файл не є — це файл від старішої прошивки, де
    // слот був «<unix>\t<кадр>», без номера запису. Створюємо заново: у ньому лежать
    // десятки записів налагодження, але втратити їх один раз при оновленні прошивки
    // прийнятно, а мовчки віддавати сторінці кашу — ні.
    if (junk) {
      logPrint("журнал: слоти в старому форматі, створюю файл заново");
      if (!fileInit()) {
        ready = false;
        return;
      }
    }
  }
  logf("журнал: пишу зі слота %u, запис №%lu", slotNext, (unsigned long)seqNext);
}

void evStorePush(uint32_t at, const char *raw) {
  if (!ready) return;
  if (qCount >= sizeof(queue) / sizeof(queue[0])) evStoreFlush();
  if (qCount >= sizeof(queue) / sizeof(queue[0])) return;  // не скинулось — місця немає
  queue[qCount].at = at;
  strlcpy(queue[qCount].raw, raw, EV_LEN);
  qCount++;
}

void evStoreFlush() {
  if (!ready || !qCount) return;
  File f = LittleFS.open(EV_PATH, "r+");
  if (!f) {
    logPrint("журнал: не відкривається на запис");
    return;
  }
  for (uint8_t i = 0; i < qCount; i++) {
    slotWrite(f, slotNext, seqNext++, queue[i].at, queue[i].raw);
    slotNext = (slotNext + 1) % EV_SLOTS;
  }
  f.close();
  logf("журнал: збережено %u подій", qCount);
  qCount = 0;
  flushAt = millis();
}

void evStoreLoop(uint32_t now) {
  if (!ready || !qCount) return;
  if (now - flushAt >= 300000) evStoreFlush();  // раз на 5 хв, якщо є що зберігати
}

// Віддає збережені події від найстарішої до найновішої. Порожні й нерозбірливі слоти
// пропускаються: одна зіпсована комірка флешу не мусить приховувати решту журналу.
void evStoreEach(void (*fn)(uint32_t at, const char *raw)) {
  if (!ready) return;
  File f = LittleFS.open(EV_PATH, "r");
  if (!f) return;
  char buf[EV_SLOT + 1];
  // Читаємо, починаючи з місця запису: там лежить найстаріше, далі по колу — новіші
  for (uint16_t k = 0; k < EV_SLOTS; k++) {
    uint16_t i = (slotNext + k) % EV_SLOTS;
    f.seek((uint32_t)i * EV_SLOT, SeekSet);
    if (f.read((uint8_t *)buf, EV_SLOT) != EV_SLOT) break;
    buf[EV_SLOT] = 0;
    uint32_t seq, at;
    char *raw;
    if (!slotParse(buf, &seq, &at, &raw)) continue;
    fn(at, raw);
  }
  f.close();
}
