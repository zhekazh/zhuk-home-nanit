// Тести вузла між Nanit і сторінками: знімок стану, журнал подій у пам'яті,
// розбір команд від браузера. Саме тут сторінка вперше торкається протоколу,
// тож перевіряємо і те, що команда доходить, і те, що сміття не доходить.
#include <string>
#include <vector>

#include "check.h"
#include "zhe.h"

int checks = 0, fails = 0;

NanitState nanit;  // зазвичай живе в link.cpp, але його тут не збираємо

void logPrint(const char *) {}
void logf(const char *, ...) {}

// ---------- заглушки веба ----------
static std::vector<std::string> bcast;              // усім сторінкам
static std::vector<std::pair<int, std::string>> to;  // одній конкретній
void wsSend(const char *line) { bcast.push_back(line); }
// Подія йде лише привітаним сторінкам, тож тест мусить бачити її окремо від решти кадрів
static std::vector<std::string> evLive;
void wsSendEv(const char *line) {
  bcast.push_back(line);
  evLive.push_back(line);
}
void wsSendTo(uint8_t client, const char *line) { to.push_back({client, line}); }

// ---------- заглушка черги команд ----------
struct SentCmd {
  std::string type, args;
  uint16_t webId;
  uint8_t client;
};
static std::vector<SentCmd> sent;
static bool queueFull, tooLong;
bool linkSend(const char *type, const char *args, uint16_t webId, uint8_t client) {
  if (queueFull || tooLong) return false;
  sent.push_back({type, args ? args : "", webId, client});
  return true;
}
const char *linkWhy() { return tooLong ? "toolong" : "busy"; }

// ---------- заглушка журналу у файлі ----------
static std::vector<std::pair<uint32_t, std::string>> stored;
static std::vector<std::pair<uint32_t, std::string>> onDisk;
void evStorePush(uint32_t at, const char *raw) { stored.push_back({at, raw}); }
void evStoreFlush() {}
void evStoreBegin() {}
void evStoreLoop(uint32_t) {}
void evStoreEach(void (*fn)(uint32_t, const char *)) {
  for (auto &r : onDisk) fn(r.first, r.second.c_str());
}

// ---------- заглушка часу ----------
static uint32_t fakeUnix = 1790000000;
uint32_t timeNow() { return fakeUnix; }

// ---------- помічники ----------
// Кадр від Nanit приходить у вузол уже розібраним на поля — повторюємо це тут
static void fromNanit(const std::string &raw) {
  char buf[128], keep[128];
  strlcpy(buf, raw.c_str(), sizeof buf);
  strlcpy(keep, raw.c_str(), sizeof keep);
  char *argv[8];
  uint8_t argc = 0;
  char *p = buf;
  while (argc < 8) {
    argv[argc++] = p;
    p = strchr(p, ',');
    if (!p) break;
    *p++ = 0;
  }
  hubFrame(argv, argc, keep);
}

// Привітання віддається кроками: курсор тримає web.cpp і кличе крок по кілька разів за
// прохід циклу, щоб не ставити всі 86 повідомлень у буфер одним заходом
static uint32_t greetSeq[8];  // курсор подій на клієнта: у прошивці його тримає web.cpp
static uint16_t greet(uint8_t client) {
  uint16_t s = 0;
  greetSeq[client] = 0;
  while (s < 500 && hubGreetStep(client, s, &greetSeq[client])) s++;
  return s;
}

static std::vector<std::string> toClient(int c) {
  std::vector<std::string> v;
  for (auto &x : to) if (x.first == c) v.push_back(x.second);
  return v;
}
static bool has(const std::vector<std::string> &v, const std::string &s) {
  for (auto &x : v) if (x == s) return true;
  return false;
}
static int countPrefix(const std::vector<std::string> &v, const std::string &p) {
  int n = 0;
  for (auto &x : v) if (x.compare(0, p.size(), p) == 0) n++;
  return n;
}

int main() {
  net.mode = NM_STA;
  nanit.online = true;

  // ---------- відновлення журналу з файлу ----------
  onDisk = {{1789990000, "EV,1,SEC_ARM,-"}, {1789990100, "EV,2,PIR,-"}};
  hubRestore();

  // ---------- кадри від Nanit ----------
  fromNanit("SEC,DIS,-,0,0");
  fromNanit("ENV,240,540,12,0");
  CHECK(has(bcast, "SEC,DIS,-,0,0"));  // кадр іде сторінкам тим самим текстом
  CHECK(has(bcast, "ENV,240,540,12,0"));

  // Подія лягає і в пам'ять, і в чергу на запис у файл
  fromNanit("EV,7,WIN_BLOCK,-");
  CHECK(stored.size() == 1);
  if (stored.size() == 1) {
    CHECK(stored[0].first == fakeUnix);
    CHECK_EQ(stored[0].second, "EV,7,WIN_BLOCK,-");
  }

  // ---------- знімок новій сторінці ----------
  {
    to.clear();
    greet(4);
    auto v = toClient(4);
    CHECK(has(v, "SEC,DIS,-,0,0"));
    CHECK(has(v, "ENV,240,540,12,0"));
    // Події віддаються з часом: інакше після F5 усі мітки стали б часом відкриття вкладки
    CHECK(has(v, "EVT,1789990000,EV,1,SEC_ARM,-"));
    CHECK(has(v, "EVT,1789990100,EV,2,PIR,-"));
    CHECK(has(v, "EVT,1790000000,EV,7,WIN_BLOCK,-"));
    CHECK(countPrefix(v, "EVT,") == 3);
  }

  // ---------- журнал у пам'яті не росте ----------
  {
    for (int i = 0; i < 45; i++) {
      char b[48];
      snprintf(b, sizeof b, "EV,%d,PIR,-", 100 + i);
      fromNanit(b);
    }
    to.clear();
    greet(4);
    auto v = toClient(4);
    CHECK(countPrefix(v, "EVT,") == 40);  // EV_KEEP: найдавніші витіснені
    CHECK(has(v, "EVT,1790000000,EV,144,PIR,-"));
    CHECK(!has(v, "EVT,1790000000,EV,104,PIR,-"));
  }

  // ---------- нова подія просто посеред привітання ----------
  // Курсор подій — монотонний номер, а не індекс у кільці. Доти початок відліку рахувався
  // від evHead щоразу: щойно кільце прокручувалось під час привітання, індекси зсувались і
  // клієнт назавжди пропускав рівно одну подію на кожну, що надійшла за цей час.
  {
    for (int i = 0; i < 40; i++) fromNanit("EV," + std::to_string(1000 + i) + ",PIR,-");
    to.clear(); bcast.clear(); evLive.clear();
    greetSeq[3] = 0;
    uint16_t s = 0;
    for (; s < 6 + 5; s++) CHECK(hubGreetStep(3, s, &greetSeq[3]));  // знімок і п'ять подій
    fromNanit("EV,1040,PIR,-");  // кільце прокрутилось, поки клієнт ще вітається
    // Подія пішла окремим каналом для привітаних сторінок (кого саме пропустити — вирішує
    // web.cpp за фазою привітання; тут перевіряємо лише, що вузол не змішує її з рештою кадрів)
    CHECK(evLive.size() == 1);
    while (s < 500 && hubGreetStep(3, s, &greetSeq[3])) s++;

    std::vector<long> ids;
    for (auto &x : toClient(3))
      if (x.compare(0, 4, "EVT,") == 0) {
        size_t a = x.find(",EV,");
        if (a != std::string::npos) ids.push_back(atol(x.c_str() + a + 4));
      }
    // Клієнт устиг забрати 1000 ще до того, як її витіснила 1040, тож у нього всі 41 підряд
    CHECK(ids.size() == 41);
    bool seq = ids.size() == 41;
    for (size_t i = 0; i < ids.size(); i++) seq = seq && ids[i] == 1000 + (long)i;
    CHECK(seq);  // 1000…1040 без пропуску й без дубля
  }

  // ---------- привітання перериване ----------
  // web.cpp віддає його порціями по кілька рядків за прохід циклу, тож крок мусить
  // продовжувати з того самого місця, а не починати спочатку
  {
    to.clear();
    uint16_t steps = greet(5);
    CHECK(steps == 6 + 40);           // шість кадрів знімка (і порожні теж) плюс EV_KEEP
    CHECK(!hubGreetStep(5, steps, &greetSeq[5]));   // за межами кроків більше немає
    auto whole = toClient(5);

    to.clear();
    bool all = true;
    greetSeq[6] = 0;
    for (uint16_t a = 0; a < steps; a += 8)
      for (uint16_t k = a; k < a + 8 && k < steps; k++) all = all && hubGreetStep(6, k, &greetSeq[6]);
    CHECK(all);
    auto parts = toClient(6);
    CHECK(parts == whole);  // порціями чи одним заходом — те саме й у тому ж порядку

    // Порядок незмінний: спершу знімок стану, потім події. Порожній кадр знімка нічого не
    // шле, але крок займає — курсор не залежить від того, що Nanit уже надіслав.
    CHECK(parts.size() == 2 + 40);
    if (parts.size() == 2 + 40) {
      CHECK(parts[0].compare(0, 4, "SEC,") == 0);
      CHECK(parts[1].compare(0, 4, "ENV,") == 0);
      CHECK(parts[2].compare(0, 4, "EVT,") == 0);
    }
  }

  // ---------- команда від сторінки ----------
  {
    sent.clear();
    to.clear();
    hubWebText(2, "5,MODE,fan,on");
    CHECK(sent.size() == 1);
    if (sent.size() == 1) {
      CHECK_EQ(sent[0].type, "MODE");
      CHECK_EQ(sent[0].args, "fan,on");   // тип і номер ставить уже link.cpp
      CHECK(sent[0].webId == 5);
      CHECK(sent[0].client == 2);
    }
    CHECK(to.empty());  // відповідь прийде пізніше, через hubCmdDone

    sent.clear();
    hubWebText(2, "6,PING");  // команда без аргументів
    CHECK(sent.size() == 1);
    if (sent.size() == 1) CHECK_EQ(sent[0].args, "");
  }

  // ---------- сміття від сторінки ----------
  {
    sent.clear();
    to.clear();
    hubWebText(2, "MODE,fan,on");    // без номера запиту
    hubWebText(2, "0,MODE,fan,on");  // номер 0 зарезервований
    hubWebText(2, "5,");             // порожній тип
    // «$» починає новий кадр, «*» відкриває CRC: обидва розсипали б кадр до Nanit.
    // Раніше перевірявся лише тип, і в аргументах вони проходили далі.
    hubWebText(2, "5,MO$DE,fan,on");
    hubWebText(2, "5,MO*DE,fan,on");
    hubWebText(2, "5,MODE,fan$on");
    hubWebText(2, "5,MODE,fan*on");
    // Кадр несе лише друковані ASCII, тож керуючі байти теж мусять відлітати: текст
    // WebSocket їх пропускає, а в кадрі «\n» обриває його перед «*CRC», і Nanit відкидає
    // обрізок по контрольній сумі — сторінка чекала б відповіді дві з половиною секунди
    hubWebText(2, "5,MODE,fan\non");
    hubWebText(2, "5,MODE,fan,on\n");
    hubWebText(2, "5,MODE,fan\ron");
    hubWebText(2, "5,MODE,fan\ton");
    hubWebText(2, "5,MODE,fan\x01on");
    hubWebText(2, "5,MODE,\x7Fon");        // DEL — теж не друкований
    hubWebText(2, "5,MODE,увімкнути");     // кирилиця в UTF-8: байти понад 0x7E
    CHECK(sent.empty());
    CHECK(to.empty());  // мовчимо: відповідати нема на що
  }

  // ---------- коли команду виконати неможливо ----------
  {
    sent.clear();
    to.clear();
    nanit.online = false;
    hubWebText(3, "8,MODE,fan,on");
    CHECK(sent.empty());
    CHECK(has(toClient(3), "RSP,8,NAK,-,offline"));

    to.clear();
    nanit.online = true;
    net.mode = NM_AP;  // у власній точці доступу сторінка лише налаштовує Wi-Fi
    hubWebText(3, "9,MODE,fan,on");
    CHECK(sent.empty());
    CHECK(has(toClient(3), "RSP,9,NAK,-,ap"));

    to.clear();
    net.mode = NM_STA;
    queueFull = true;
    hubWebText(3, "10,MODE,fan,on");
    CHECK(has(toClient(3), "RSP,10,NAK,-,busy"));
    queueFull = false;

    // «Задовга команда» — не «зайнято»: перше не зникне саме, і сторінка мусить сказати
    // людині різне
    to.clear();
    tooLong = true;
    hubWebText(3, "11,MODE,fan,on");
    CHECK(sent.empty());
    CHECK(has(toClient(3), "RSP,11,NAK,-,toolong"));
    tooLong = false;

    // Форма власних відмов ESP така сама, як у NAK від Nanit: «RSP,<id>,NAK,<seq>,<причина>»
    // з «-» на місці номера, якого в неї немає. Сторінка читає причину з того самого поля
    // для обох випадків, і без «-» показувала б «Неприпустиме значення» замість причини.
    for (const std::string &r : {std::string("ap"), std::string("offline"),
                                 std::string("busy"), std::string("toolong")}) {
      to.clear();
      net.mode = r == "ap" ? NM_AP : NM_STA;
      nanit.online = r != "offline";
      queueFull = r == "busy";
      tooLong = r == "toolong";
      hubWebText(3, "12,MODE,fan,on");
      auto v = toClient(3);
      CHECK(v.size() == 1);
      if (v.size() == 1) {
        // п'ять полів, п'яте — причина: саме його читає сторінка
        std::vector<std::string> f;
        size_t b = 0, e;
        while ((e = v[0].find(',', b)) != std::string::npos) {
          f.push_back(v[0].substr(b, e - b));
          b = e + 1;
        }
        f.push_back(v[0].substr(b));
        CHECK(f.size() == 5);
        if (f.size() == 5) {
          CHECK_EQ(f[2], "NAK");
          CHECK_EQ(f[3], "-");
          CHECK_EQ(f[4], r);
        }
      }
    }
    net.mode = NM_STA;
    nanit.online = true;
    queueFull = false;
    tooLong = false;
  }

  // ---------- відповідь повертається тій сторінці, що просила ----------
  {
    to.clear();
    bcast.clear();
    hubCmdDone(12, 1, "ACK,340");
    CHECK(has(toClient(1), "RSP,12,ACK,340"));
    CHECK(toClient(0).empty());  // сусідній вкладці чужа відповідь не потрібна
    CHECK(bcast.empty());        // і розсилати її теж нікуди

    to.clear();
    hubCmdDone(0, 1, "ACK,341");  // команда самого ESP: сторінці про неї знати не треба
    CHECK(to.empty());
  }

  return report("hub");
}
