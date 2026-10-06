// Тести транспорту ESP ↔ Nanit: CRC, розбір кадрів, черга команд із повторами,
// підтвердження подій. Заліза не треба — порт тут просто два рядки в пам'яті.
#include <string>
#include <vector>

#include "check.h"
#include "zhe.h"

int checks = 0, fails = 0;

// ---------- заглушки сусідніх модулів ----------
// Журнал і вузол підмінені навмисно: інакше рядки журналу пішли б у той самий порт,
// що й кадри, і тест читав би свій власний шум.
void logPrint(const char *) {}
void logf(const char *, ...) {}

static int timeResends;
void timeResend() { timeResends++; }

// Довжина черги команд: у прошивці її ніхто не питає, тож у zhe.h оголошення немає (див.
// комментар у link.cpp). Тут воно потрібне — черга це статик модуля, а половина перевірок
// транспорту саме про те, коли команда з неї йде й коли лишається.
uint8_t linkQueued();

static std::vector<std::string> hubGot;
void hubFrame(char **, uint8_t, const char *raw) { hubGot.push_back(raw); }

struct DoneRec {
  uint16_t webId;
  uint8_t client;
  std::string raw;
};
static std::vector<DoneRec> hubDone;
void hubCmdDone(uint16_t webId, uint8_t client, const char *raw) {
  hubDone.push_back({webId, client, raw});
}
// Індексація порожнього вектора — це падіння замість «FAIL», а тест, який упав,
// не показує решти перевірок, хоч саме вони зазвичай пояснюють причину.
static DoneRec doneAt(size_t i) {
  return i < hubDone.size() ? hubDone[i] : DoneRec{0, 0, "<нічого не завершилось>"};
}

// ---------- помічники ----------
static uint8_t crc8(const char *s, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= (uint8_t)*s++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

static std::string framed(const std::string &body) {
  char t[8];
  snprintf(t, sizeof t, "*%02X\n", crc8(body.data(), body.size()));
  return "$" + body + t;
}

static void fromNanit(const std::string &body) { Serial.in += framed(body); }
static void rawFromNanit(const std::string &s) { Serial.in += s; }

static bool starts(const std::string &s, const std::string &p) {
  return s.compare(0, p.size(), p) == 0;
}

// Розбирає те, що прошивка написала в порт, і заразом звіряє CRC кожного кадру
static std::vector<std::string> outFrames() {
  std::vector<std::string> v;
  std::string &o = Serial.out;
  size_t p = 0;
  while ((p = o.find('$', p)) != std::string::npos) {
    size_t e = o.find('\n', p);
    if (e == std::string::npos) break;
    std::string f = o.substr(p + 1, e - p - 1);
    size_t star = f.rfind('*');
    if (star == std::string::npos || f.size() - star != 3) {
      printf("FAIL: у порт пішов кадр без CRC: %s\n", f.c_str());
      fails++;
      p = e;
      continue;
    }
    std::string body = f.substr(0, star);
    unsigned want = strtoul(f.c_str() + star + 1, nullptr, 16);
    if (want != crc8(body.data(), body.size())) {
      printf("FAIL: битий CRC у виході: %s\n", f.c_str());
      fails++;
    }
    v.push_back(body);
    p = e;
  }
  o.clear();
  return v;
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    fakeMillis += 10;
    linkLoop(fakeMillis);
  }
}

static bool has(const std::vector<std::string> &v, const std::string &s) {
  for (auto &x : v) if (x == s) return true;
  return false;
}
static int countPrefix(const std::vector<std::string> &v, const std::string &p) {
  int n = 0;
  for (auto &x : v) if (starts(x, p)) n++;
  return n;
}
static std::string firstPrefix(const std::vector<std::string> &v, const std::string &p) {
  for (auto &x : v) if (starts(x, p)) return x;
  return "";
}
// Другим полем у кадрі завжди йде номер команди
static std::string seqOf(const std::string &frame) {
  size_t a = frame.find(',');
  if (a == std::string::npos) return "";
  size_t b = frame.find(',', a + 1);
  return frame.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
}

// Імітує Nanit, який відповідає на все, і крутить цикл, доки черга не спорожніє.
// Без цього кожен тест мусив би сам розбиратися зі службовими кадрами: прошивка
// щосекунди шле PING, а щойно Nanit озвався — ще й NET зі станом мережі.
static std::vector<std::string> settle() {
  std::vector<std::string> seen;
  for (int i = 0; i < 30; i++) {
    run(20);
    for (auto &f : outFrames()) {
      seen.push_back(f);
      if (starts(f, "ACK,")) continue;  // це наше підтвердження події, не команда
      std::string s = seqOf(f);
      if (!s.empty()) fromNanit("ACK," + s);
    }
    if (!linkQueued()) break;
  }
  run(20);
  for (auto &f : outFrames()) seen.push_back(f);
  return seen;
}

int main() {
  // ---------- CRC ----------
  // Контрольний вектор CRC-8/SMBUS: якщо він зійшовся, поліном і початкове значення
  // ті самі, що на Nanit, і кадри взагалі мають шанс дійти
  CHECK(crc8("123456789", 9) == 0xF4);

  // ---------- старт ----------
  linkBegin();
  run(20);
  {
    auto f = outFrames();
    // HI ми могли проспати, тому при старті питаємо його самі
    CHECK(has(f, "GET,1,hi"));
    CHECK(countPrefix(f, "PING,") >= 1);
    for (auto &x : f) {
      std::string s = seqOf(x);
      if (!s.empty()) fromNanit("ACK," + s);
    }
  }
  settle();
  CHECK(linkQueued() == 0);

  // ---------- правильний кадр ----------
  {
    uint32_t okWas = nanit.frames;
    size_t hubWas = hubGot.size();
    fromNanit("SEC,DIS,-,0,0");
    run(20);
    CHECK(nanit.frames == okWas + 1);
    CHECK(hubGot.size() == hubWas + 1);
    CHECK(hubGot.back() == "SEC,DIS,-,0,0");
    outFrames();
  }

  // ---------- биті кадри ----------
  // Кожен має лише збільшити лічильник відкинутих: Nanit повторить сам
  {
    uint32_t badWas = nanit.bad, okWas = nanit.frames;
    size_t hubWas = hubGot.size();
    rawFromNanit("$SEC,DIS*00\n");    // не той CRC
    rawFromNanit("$SEC,DIS\n");       // зовсім без CRC
    rawFromNanit("$SEC,DIS*F\n");     // огризок CRC
    rawFromNanit("$SEC,DIS*F4A\n");   // задовгий CRC
    rawFromNanit("$SEC,DIS*GG\n");    // не шістнадцяткове
    run(60);
    CHECK(nanit.bad == badWas + 5);
    CHECK(nanit.frames == okWas);
    CHECK(hubGot.size() == hubWas);  // жоден із них не дійшов до вузла
    outFrames();
  }

  // ---------- задовгий кадр не ламає наступний ----------
  {
    size_t hubWas = hubGot.size();
    rawFromNanit("$" + std::string(FRAME_MAX + 40, 'X') + "\n");
    fromNanit("ENV,240,540,12,0");
    run(80);
    CHECK(hubGot.size() == hubWas + 1);
    CHECK(hubGot.back() == "ENV,240,540,12,0");
    outFrames();
  }

  // ---------- команда й відповідь ----------
  {
    hubDone.clear();
    CHECK(linkQueued() == 0);
    CHECK(linkSend("MODE", "fan,on", 7, 3));
    run(20);
    std::string sent = firstPrefix(outFrames(), "MODE,");
    CHECK(!sent.empty());
    std::string seq = seqOf(sent);
    CHECK_EQ(sent, "MODE," + seq + ",fan,on");
    CHECK(linkQueued() == 1);

    // Чужий номер — це відповідь на щось інше: команда лишається в польоті
    fromNanit("ACK,9999");
    run(20);
    CHECK(linkQueued() == 1);
    CHECK(has(hubGot, "ACK,9999"));
    CHECK(hubDone.empty());

    fromNanit("ACK," + seq);
    run(20);
    CHECK(linkQueued() == 0);
    CHECK(hubDone.size() == 1);
    CHECK(doneAt(0).webId == 7);  // відповідь піде саме тій сторінці, що просила
    CHECK(doneAt(0).client == 3);
    CHECK_EQ(doneAt(0).raw, "ACK," + seq);
    outFrames();
  }

  // ---------- NAK повертається сторінці так само ----------
  {
    hubDone.clear();
    linkSend("PIN", "1111", 9, 1);
    run(20);
    std::string seq = seqOf(firstPrefix(outFrames(), "PIN,"));
    fromNanit("NAK," + seq + ",pin,2");
    run(20);
    CHECK(hubDone.size() == 1);
    CHECK_EQ(doneAt(0).raw, "NAK," + seq + ",pin,2");
    CHECK(linkQueued() == 0);
    outFrames();
  }

  // ---------- повтори й таймаут ----------
  {
    hubDone.clear();
    outFrames();
    linkSend("MODE", "fan,off", 11, 1);
    run(20);
    std::vector<std::string> all = outFrames();
    std::string seq = seqOf(firstPrefix(all, "MODE,"));
    int tries = countPrefix(all, "MODE,");
    bool sameSeq = true;
    for (int i = 0; i < 60 && linkQueued(); i++) {
      run(100);
      for (auto &x : outFrames())
        if (starts(x, "MODE,")) {
          tries++;
          if (seqOf(x) != seq) sameSeq = false;  // повтор іде з тим самим номером
        }
    }
    CHECK(sameSeq);  // інакше Nanit виконав би команду двічі
    CHECK(tries == 12);
    CHECK(linkQueued() == 0);
    CHECK(hubDone.size() == 1);
    CHECK_EQ(doneAt(0).raw, "TIMEOUT");
    CHECK(doneAt(0).webId == 11);
  }

  // ---------- версію Nanit перепитуємо, доки не дізнаємось ----------
  // Єдиний GET,hi при старті пропадає, якщо Nanit цієї миті ще не відповідає: живлення в обох
  // спільне, тож після просадки вони встають разом. Доти nanit.fw лишався «-» до перезапуску
  // ESP — саме так і сталося на макеті, і сторінка показувала порожню версію в кадрі NANIT.
  {
    // Nanit «говорить» кадрами стану, але HI серед них немає — тримаємо його на зв'язку самі
    auto alive = [&](uint32_t ms) {
      for (uint32_t t = 0; t < ms; t += 900) {
        fromNanit("SEC,DIS,-,0,0");
        run(900);
      }
    };
    alive(1000);
    CHECK_EQ(std::string(nanit.fw), "-");  // відповіді на перший GET,hi ми так і не дістали
    settle();
    outFrames();

    // Перший перепит уже з'їв settle() вище, тож чекаємо повний строк у 30 с
    alive(31000);
    auto again = outFrames();
    CHECK(countPrefix(again, "GET,") >= 1);  // перепитує, бо версія досі невідома
    CHECK(firstPrefix(again, "GET,").find(",hi") != std::string::npos);

    fromNanit("HI,1,0.1.0,3.1,POR");  // відповідаємо
    run(20);
    CHECK_EQ(std::string(nanit.fw), "0.1.0");
    settle();
    outFrames();
    alive(35000);  // версія відома — перепити мусять припинитись
    CHECK(countPrefix(outFrames(), "GET,") == 0);
  }

  // ---------- вікно повторів обмежене й у часі ----------
  // Цикл може стояти: сканування мереж блокує ~2 с, повільний WebSocket-клієнт — до 5 с.
  // Тоді спроб ще не дванадцять, але кеш повторів Nanit (4 с) уже забув номер, і команда
  // виконалася б удруге. Після 3,5 с від першої відправки ми мусимо здатися самі.
  {
    hubDone.clear();
    CHECK(linkSend("CALDO", "gate,step,1", 21, 4));
    run(20);
    CHECK(countPrefix(outFrames(), "CALDO,") == 1);  // перша спроба пішла
    fakeMillis += 4000;                              // цикл стояв увесь цей час
    run(20);
    auto after = outFrames();                        // службовий PING тут можливий, команда — ні
    CHECK(countPrefix(after, "CALDO,") == 0);        // після строку команда в лінію не йде
    CHECK(linkQueued() == 0);
    CHECK(hubDone.size() == 1);
    CHECK_EQ(doneAt(0).raw, "TIMEOUT");
    CHECK(doneAt(0).webId == 21);
  }

  fromNanit("SEC,DIS,-,0,0");  // Nanit знову озвався
  settle();

  // ---------- черга не безмежна ----------
  {
    CHECK(linkQueued() == 0);
    int taken = 0;
    while (taken < 10 && linkSend("PING", nullptr, 0, 0)) taken++;
    CHECK(taken == 6);
    CHECK(!strcmp(linkWhy(), "busy"));
    CHECK(linkQueued() == 6);
    settle();
    CHECK(linkQueued() == 0);
  }

  // ---------- команда, яка не вміщається в кадр ----------
  // Обрізок пішов би до Nanit із правильним CRC — сума рахується вже по обрізку, тож
  // виглядав би цілим кадром, а команда була б іншою
  {
    hubDone.clear();
    outFrames();
    CHECK(linkQueued() == 0);
    std::string huge(80, 'x');
    CHECK(!linkSend("MODE", huge.c_str(), 5, 1));
    CHECK(!strcmp(linkWhy(), "toolong"));  // сторінці «зайнято» тут сказало б неправду
    CHECK(linkQueued() == 0);
    run(100);
    CHECK(countPrefix(outFrames(), "MODE,") == 0);  // у порт не пішло нічого

    CHECK(linkSend("MODE", "fan,on", 6, 1));  // черга від відмови не постраждала
    settle();
    CHECK(linkQueued() == 0);
    CHECK(hubDone.size() == 1);
  }

  // ---------- підтвердження подій ----------
  {
    size_t hubWas = hubGot.size();
    fromNanit("EV,55,SEC_ARM,-");
    run(20);
    CHECK(has(outFrames(), "ACK,55"));
    CHECK(hubGot.size() == hubWas + 1);

    // Той самий номер — це повтор Nanit, який не дочекався підтвердження.
    // Підтвердити треба знову, а от у журнал подія вже потрапила.
    fromNanit("EV,55,SEC_ARM,-");
    run(20);
    CHECK(has(outFrames(), "ACK,55"));
    CHECK(hubGot.size() == hubWas + 1);

    fromNanit("EV,56,SEC_DIS,-");
    run(20);
    CHECK(has(outFrames(), "ACK,56"));
    CHECK(hubGot.size() == hubWas + 2);
  }

  // ---------- підтвердження не губиться, коли порт зайнятий ----------
  {
    Serial.txFree = 2;  // у буфер не влазить навіть «ACK,57»
    fromNanit("EV,57,PIR,-");
    run(100);
    CHECK(!has(outFrames(), "ACK,57"));
    Serial.txFree = 256;
    run(20);
    CHECK(has(outFrames(), "ACK,57"));  // щойно звільнилось — підтвердження пішло
  }

  // ---------- кадр не пишеться наполовину ----------
  {
    CHECK(linkQueued() == 0);
    Serial.txFree = 5;
    outFrames();
    linkSend("SVC", "1", 0, 0);
    run(100);
    CHECK(Serial.out.empty());  // нічого, навіть обрізка
    CHECK(linkQueued() == 1);   // команда чекає далі
    Serial.txFree = 256;
    settle();
    CHECK(linkQueued() == 0);
  }

  // ---------- HI: Nanit перезапустився ----------
  {
    timeResends = 0;
    net.mode = NM_STA;
    net.rssi = -61;
    strlcpy(net.ip, "192.168.0.106", sizeof(net.ip));
    net.why[0] = 0;
    fromNanit("HI,1,0.1.0,3.1,POR");
    run(20);
    CHECK(nanit.proto == 1);
    CHECK(!strcmp(nanit.fw, "0.1.0"));
    CHECK(!strcmp(nanit.board, "3.1"));
    CHECK(!strcmp(nanit.reset, "POR"));
    CHECK(timeResends == 1);  // час у Nanit лише в пам'яті, тож шлемо негайно
    // Про мережу він теж забув — розповідаємо заново
    auto seen = settle();
    std::string netFrame = firstPrefix(seen, "NET,");
    CHECK(!netFrame.empty());
    CHECK_EQ(netFrame, "NET," + seqOf(netFrame) + ",sta,-61,192.168.0.106,-");
  }

  // ---------- зв'язок пропав ----------
  {
    CHECK(nanit.online);
    run(3500);  // понад 3 с без жодного кадру
    CHECK(!nanit.online);
    outFrames();
    fromNanit("SYS,10,4000,3,90,4100,0");
    run(20);
    CHECK(nanit.online);
    settle();
  }

  // ---------- PING щосекунди ----------
  {
    outFrames();
    run(3000);
    CHECK(countPrefix(outFrames(), "PING,") >= 3);  // Nanit вважає ESP зниклим через 5 с
  }

  // ---------- PING не губиться, коли буфер передачі зайнятий ----------
  // Мітка часу рухається лише при успішному записі: інакше PING тихо пропадав би, і
  // контроль зв'язку був би слабшим, ніж виглядає
  {
    outFrames();
    run(1000);  // хай PING пройде й мітка стане свіжою
    CHECK(countPrefix(outFrames(), "PING,") >= 1);

    Serial.txFree = 2;  // строк настане, але кадр не влазить
    run(1200);
    CHECK(Serial.out.empty());

    Serial.txFree = 256;
    run(10);  // мітка не рушила, тож PING іде з першого ж проходу
    CHECK(countPrefix(outFrames(), "PING,") == 1);
  }

  return report("link");
}
