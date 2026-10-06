// Журнал налагодження: кільцевий буфер у RAM і дзеркало в UART0.
#include "zhe.h"

static char lines[LOG_LINES][LOG_LEN];
static uint8_t head, count;
static uint32_t seq;  // скільки рядків написано від старту: за ним ведеться віддача історії
static bool toUart = true;  // до підняття мережі веб-консолі ще немає

void logBegin() {
  head = count = 0;
  seq = 0;
  toUart = true;
}

void logToUart(bool on) { toUart = on; }

uint8_t logCount() { return count; }
uint32_t logSeq() { return seq; }

const char *logAt(uint8_t i) {
  if (i >= count) return "";
  uint8_t start = count == LOG_LINES ? head : 0;
  return lines[(start + i) % LOG_LINES];
}

// Віддача історії новій сторінці йде за НОМЕРОМ рядка, а не за індексом у кільці. Поки
// сторінка дістає історію порціями (див. webLoop), у кільце можуть додаватися рядки; коли
// воно прокрутиться, усі індекси зсунуться — клієнт дістав би не той рядок або пропустив
// його. Номер від зсуву не залежить. `at` — номер, який хочемо: якщо його вже витіснили,
// підтягуємо до найстарішого наявного. nullptr — курсор дійшов до кінця журналу, а отже й
// рядки, що з'явилися за час віддачі, вже надіслані.
const char *logSince(uint32_t *at) {
  uint32_t first = seq - count;  // номер найстарішого рядка, який ще лежить у кільці
  if ((int32_t)(*at - first) < 0) *at = first;
  if ((int32_t)(*at - seq) >= 0) return nullptr;
  const char *s = logAt((uint8_t)(*at - first));
  (*at)++;
  return s;
}

void logPrint(const char *s) {
  char *dst = lines[head];
  uint8_t n = 0;
  uint32_t ms = millis();
  // мітка часу від старту: за нею видно, що сталося до підняття мережі
  n += snprintf(dst, LOG_LEN, "%lu.%03lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  while (*s && n < LOG_LEN - 1) {
    dst[n++] = (*s == '\n' || *s == '\r') ? ' ' : *s;
    s++;
  }
  dst[n] = 0;
  head = (head + 1) % LOG_LINES;
  if (count < LOG_LINES) count++;
  seq++;

  // «#» на початку — щоб рядок не сплутали з кадром: Nanit починає розбір із «$».
  // Пишемо, поки ніхто не дивиться веб-консоль: інакше журнал зникав би саме тоді,
  // коли він потрібен — після підняття мережі й до відкриття сторінки.
  if (toUart || !wsHasClients()) {
    Serial.print(F("# "));
    Serial.println(dst);
  }
  if (wsHasClients()) {  // веб-консоль отримує рядок одразу
    char frame[LOG_LEN + 8];
    snprintf(frame, sizeof(frame), "LOG,%s", dst);
    // Не wsSend: сторінка, яка саме дістає історію журналу, цей рядок отримає своєю чергою
    // за номером. Інакше він прийшов би до неї двічі й поперед старіших рядків, а сторінка
    // рахує стрибок мітки часу назад за перезапуск модуля.
    wsSendLog(frame);
  }
}

void logf(const char *fmt, ...) {
  char buf[LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  logPrint(buf);
}
