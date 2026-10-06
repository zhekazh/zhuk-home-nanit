// Тести кільцевого журналу налагодження: він же веб-консоль. Тут перевіряється те,
// чого не видно на макеті, — що буфер не росте, рядок не вилазить за межі слота,
// а перенесення рядка не розриває кадр у порту.
#include <string>
#include <vector>

#include "check.h"
#include "zhe.h"

int checks = 0, fails = 0;

// ---------- заглушки веба ----------
static bool clients;
static std::vector<std::string> sent;
bool wsHasClients() { return clients; }
void wsSend(const char *line) { sent.push_back(line); }
void wsSendTo(uint8_t, const char *) {}
// Жива розсилка рядка журналу оминає сторінки, які саме дістають історію; кому саме вона
// дійшла, вирішує web.cpp, тут важливо лише те, що рядок пішов
void wsSendLog(const char *line) { sent.push_back(line); }

static std::string at(uint8_t i) { return logAt(i); }
// Мітка часу попереду («12.345 ») тесту нецікава — важливий сам текст
static std::string text(uint8_t i) {
  std::string s = at(i);
  size_t sp = s.find(' ');
  return sp == std::string::npos ? s : s.substr(sp + 1);
}

int main() {
  logBegin();
  clients = false;
  CHECK(logCount() == 0);
  CHECK(logSeq() == 0);
  CHECK_EQ(at(0), "");  // порожній буфер не віддає сміття

  // ---------- мітка часу ----------
  fakeMillis = 12345;
  logPrint("старт");
  CHECK(logCount() == 1);
  CHECK(logSeq() == 1);
  CHECK_EQ(at(0), "12.345 старт");  // секунди від увімкнення модуля

  // ---------- дзеркало в порт ----------
  // Поки сторінка не відкрита, журнал іде в UART0 з «#»: Nanit такі рядки відкидає,
  // бо кадр починається з «$», а з $DBG,n,2 ретранслює їх на USB
  CHECK_EQ(Serial.out, "# 12.345 старт\n");
  Serial.out.clear();

  // ---------- перенесення рядка ----------
  // Якби «\n» лишився всередині, рядок у порту розпався б на два, і другий шматок
  // виглядав би як окреме повідомлення
  logPrint("два\nрядки\rу одному");
  CHECK_EQ(text(1), "два рядки у одному");
  Serial.out.clear();

  // ---------- задовгий рядок ----------
  {
    std::string big(LOG_LEN * 2, 'Q');
    logPrint(big.c_str());
    CHECK(at(2).size() == LOG_LEN - 1);  // рівно під розмір слота, без виходу за межі
  }
  Serial.out.clear();

  // ---------- logf ----------
  logf("nanit: fw %s, плата %s, proto %u", "0.1.0", "3.1", 1u);
  CHECK_EQ(text(3), "nanit: fw 0.1.0, плата 3.1, proto 1");
  Serial.out.clear();

  // ---------- кільце ----------
  logBegin();
  for (int i = 1; i <= LOG_LINES + 5; i++) {
    char b[32];
    snprintf(b, sizeof b, "рядок %d", i);
    logPrint(b);
  }
  Serial.out.clear();
  CHECK(logCount() == LOG_LINES);      // буфер не росте
  CHECK(logSeq() == LOG_LINES + 5);    // а лічильник рахує все
  CHECK_EQ(text(0), "рядок 6");        // найстаріші п'ять витіснені
  CHECK_EQ(text(LOG_LINES - 1), "рядок 45");
  CHECK_EQ(at(LOG_LINES), "");         // за межами буфера — порожньо

  // ---------- коли сторінка відкрита ----------
  sent.clear();
  clients = true;
  logToUart(false);
  logPrint("для веба");
  CHECK(sent.size() == 1);
  if (sent.size() == 1) CHECK(sent[0].compare(0, 4, "LOG,") == 0);
  CHECK(Serial.out.empty());  // порт до Nanit не засмічуємо

  // Поки веб-консолі немає, рядки однаково йдуть у порт: інакше журнал зникав би
  // саме тоді, коли він потрібен — після підняття мережі й до відкриття сторінки
  sent.clear();
  clients = false;
  logPrint("нікому");
  CHECK(sent.empty());
  CHECK(!Serial.out.empty());
  Serial.out.clear();

  // Примусове дзеркало в порт працює і при відкритій сторінці
  clients = true;
  logToUart(true);
  sent.clear();
  logPrint("обом");
  CHECK(sent.size() == 1);
  CHECK(!Serial.out.empty());

  // ---------- віддача історії новому клієнтові ----------
  // web.cpp віддає її порціями по кілька рядків за прохід циклу, тож курсор іде за НОМЕРОМ
  // рядка. Поки він іде, у кільце можуть додаватися рядки — вони теж мусять дійти, і саме
  // по одному разу: сторінка рахує стрибок мітки часу назад за перезапуск модуля.
  {
    logBegin();
    clients = false;
    for (int i = 1; i <= 5; i++) {
      char b[32];
      snprintf(b, sizeof b, "рядок %d", i);
      logPrint(b);
    }
    Serial.out.clear();

    uint32_t at = logSeq() - logCount();  // з цього номера починає новий клієнт
    std::vector<std::string> got;
    for (int guard = 0; guard < 100; guard++) {
      const char *l = logSince(&at);
      if (!l) break;
      got.push_back(l);
      if (got.size() == 2) {  // рядок з'явився посеред віддачі
        logPrint("рядок 6");
        Serial.out.clear();
      }
    }
    CHECK(got.size() == 6);
    if (got.size() == 6) {
      CHECK(got[0].find("рядок 1") != std::string::npos);
      CHECK(got[4].find("рядок 5") != std::string::npos);
      CHECK(got[5].find("рядок 6") != std::string::npos);  // і той, що з'явився під час віддачі
    }
    CHECK(logSince(&at) == nullptr);  // курсор дійшов до кінця журналу — фаза скінчилась
  }

  // Те саме, але кільце під час віддачі прокрутилось: індекси зсунулись, номери — ні
  {
    logBegin();
    clients = false;
    for (int i = 1; i <= LOG_LINES; i++) {
      char b[32];
      snprintf(b, sizeof b, "рядок %d", i);
      logPrint(b);
    }
    Serial.out.clear();

    uint32_t at = logSeq() - logCount();
    std::vector<std::string> got;
    for (int guard = 0; guard < 200; guard++) {
      const char *l = logSince(&at);
      if (!l) break;
      got.push_back(l);
      if (got.size() == 2) {  // три нових рядки витісняють три найстаріших
        for (int i = LOG_LINES + 1; i <= LOG_LINES + 3; i++) {
          char b[32];
          snprintf(b, sizeof b, "рядок %d", i);
          logPrint(b);
        }
        Serial.out.clear();
      }
    }
    // Віддані: рядки 1 і 2, далі з найстарішого наявного (4) по найновіший (43). Рядок 3
    // витіснений, поки курсор до нього не дійшов, — його вже ніде взяти.
    CHECK(got.size() == 2 + LOG_LINES);
    if (got.size() == 2 + LOG_LINES) {
      CHECK(got[1].find("рядок 2") != std::string::npos);
      CHECK(got[2].find("рядок 4") != std::string::npos);  // а не «рядок 6», як при індексі
      char last[32];
      snprintf(last, sizeof last, "рядок %d", LOG_LINES + 3);
      CHECK(got.back().find(last) != std::string::npos);
    }
  }

  return report("logbuf");
}
