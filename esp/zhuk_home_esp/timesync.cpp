// Час: синхронізація з NTP і передача його Nanit.
//
// Nanit не має годинника реального часу — після кожного перезапуску він знає лише, скільки
// мілісекунд минуло від старту. Тому час у будинок приносить ESP: сам бере його з інтернету
// й надсилає командою TIME, а Nanit ставить мітки на подіях і показує годинник на екрані.
//
// Зона зберігається правилом POSIX («EET-2EEST,M3.5.0/3,M10.5.0/4»), а не готовим зсувом:
// у правилі записані й самі дати переходу на літній час, тож бібліотека рахує зсув сама, і
// в жовтні годинник на екрані Nanit переводиться без нас. Nanit отримує вже готові хвилини —
// зайвого коду в ньому немає. Правило міняється з веба, база зон на 500 КБ тут не потрібна.
#include <coredecls.h>  // settimeofday_cb
#include <time.h>

#include "zhe.h"

static bool synced;
static uint32_t sentAt;
static uint32_t askedAt;
static int16_t offMin;  // поточний зсув від UTC, хвилини

// Зсув беремо не з тексту правила, а з самої бібліотеки: лише вона знає, чи діє зараз
// літній час. Поля tm_gmtoff у цьому ядрі немає, тож порівнюємо місцевий час із UTC.
static int16_t tzOffset(time_t t) {
  struct tm lt, gt;
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);
  int d = (lt.tm_hour - gt.tm_hour) * 60 + (lt.tm_min - gt.tm_min);
  int dd = lt.tm_yday - gt.tm_yday;
  if (dd == 1 || dd < -1) d += 1440;       // місцева доба вже наступна
  else if (dd == -1 || dd > 1) d -= 1440;  // ще попередня
  return (int16_t)d;
}

int16_t timeOffset() { return offMin; }

bool timeValid() { return synced; }

// Нове правило зони з веба: застосовуємо одразу й надсилаємо Nanit, не чекаючи
// десяти хвилин, — інакше людина змінила зону, а годинник на екрані той самий.
void timeSetZone(const char *rule) {
  strlcpy(cfg.tz, rule, sizeof(cfg.tz));
  setTZ(cfg.tz);
  offMin = synced ? tzOffset(time(nullptr)) : 0;
  sentAt = 0;
  logf("time: зона %s, зсув %+d хв", cfg.tz, offMin);
}

// Nanit тримає час у пам'яті й губить його при перезапуску, а на екрані показує «--:--».
// Тому після кожного його старту (кадр HI) надсилаємо час негайно, не чекаючи десяти
// хвилин до наступної передачі.
void timeResend() { sentAt = 0; }

uint32_t timeNow() {
  return synced ? (uint32_t)time(nullptr) : 0;
}

void timeBegin() {
  // Час тримаємо в UTC, а зсув передаємо окремо: так Nanit сам вирішує, що показати
  // на екрані, і нам не треба домовлятися про формат місцевого часу.
  configTime(cfg.tz, cfg.ntp[0] ? cfg.ntp : "pool.ntp.org", "time.google.com");
  settimeofday_cb([]() {
    if (synced) return;
    synced = true;
    sentAt = 0;  // надіслати Nanit негайно, не чекаючи десяти хвилин
    time_t t = time(nullptr);
    offMin = tzOffset(t);
    struct tm tm;
    localtime_r(&t, &tm);
    logf("time: %04d-%02d-%02d %02d:%02d, зона %s (%+d хв)", tm.tm_year + 1900, tm.tm_mon + 1,
         tm.tm_mday, tm.tm_hour, tm.tm_min, cfg.tz, offMin);
  });
  logf("time: беру з %s", cfg.ntp[0] ? cfg.ntp : "pool.ntp.org");
}

void timeLoop(uint32_t now) {
  if (net.mode != NM_STA) return;  // у власній точці доступу інтернету немає
  if (!synced) {
    // SDK повторює запити сам; нам лишається не спамити журнал і чекати відповіді
    if (now - askedAt >= 60000) {
      askedAt = now;
      logPrint("time: чекаю відповіді NTP");
    }
    return;
  }
  // У ніч переходу на зимовий час зсув міняється сам, і Nanit має дізнатися про це
  // не через десять хвилин, а одразу. Перевіряємо раз на хвилину: localtime_r щоразу
  // звіряється з правилом зони, і в головному циклі це була б робота на порожньому місці.
  static uint32_t seenAt;
  if (!seenAt || now - seenAt >= 60000) {
    seenAt = now ? now : 1;
    int16_t off = tzOffset(time(nullptr));
    if (off != offMin) {
      logf("time: зсув змінився з %+d на %+d хв", offMin, off);
      offMin = off;
      sentAt = 0;  // надіслати новий зсув негайно
    }
  }
  // Раз на 10 хв, як вимагає протокол: інакше Nanit лічить час власним таймером і
  // потроху відстає, а після його перезапуску годинник узагалі починається з нуля.
  if (!nanit.online || (sentAt && now - sentAt < 600000)) return;
  char args[32];
  snprintf(args, sizeof(args), "%lu,%d", (unsigned long)time(nullptr), offMin);
  if (linkSend("TIME", args, 0, 0)) sentAt = now ? now : 1;
}
