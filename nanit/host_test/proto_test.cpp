// Тести протоколу й логіки прошивки на комп'ютері
#include "zh.h"
#include "EEPROM.h"
#include <vector>
#include <string>
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string framed(const std::string &body) {
  char t[8]; snprintf(t, sizeof t, "*%02X\n", crc8((const uint8_t *)body.data(), body.size()));
  return "$" + body + t;
}
static void esp(const std::string &body) { Serial3.in += framed(body); }
static void usb(const std::string &raw) { Serial.in += raw; }

static void loopOnce() {
  linkUpdate(fakeMillis); secUpdate(fakeMillis); autoUpdate(fakeMillis); diagUpdate(fakeMillis); settingsUpdate(fakeMillis);
}
static std::string captured;
static bool autoAck = true;
static void pump() {
  captured += Serial3.out; Serial3.out.clear();
  size_t p = 0;
  static size_t scanned = 0;
  if (scanned > captured.size()) scanned = 0;
  p = scanned;
  while (autoAck && (p = captured.find("$EV,", p)) != std::string::npos) {
    size_t e = captured.find(',', p + 4); if (e == std::string::npos) break;
    Serial3.in += framed("ACK," + captured.substr(p + 4, e - p - 4)); p = e;
  }
  scanned = captured.size();
}
static void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) { fakeMillis += 10; loopOnce(); pump(); } }

// Розбирає вихід порту на тіла кадрів, перевіряючи CRC
static std::vector<std::string> frames(HardwareSerial &port) {
  std::vector<std::string> v; std::string &o = (&port == &Serial3) ? captured : port.out; if (&port == &Serial3) { captured += Serial3.out; Serial3.out.clear(); } size_t p = 0;
  while ((p = o.find('$', p)) != std::string::npos) {
    size_t e = o.find('\n', p); if (e == std::string::npos) break;
    std::string f = o.substr(p + 1, e - p - 1); size_t star = f.rfind('*');
    std::string body = f.substr(0, star);
    unsigned crc = strtoul(f.substr(star + 1).c_str(), nullptr, 16);
    if (crc != crc8((const uint8_t *)body.data(), body.size())) { printf("bad CRC in output: %s\n", f.c_str()); fails++; }
    v.push_back(body); p = e;
  }
  o.clear(); return v;
}
static bool starts(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
// Імітує ESP: підтверджує всі події, повертає решту кадрів (без кадрів стану)
static std::vector<std::string> drain(std::vector<std::string> *events = nullptr) {
  std::vector<std::string> other;
  for (auto &f : frames(Serial3)) {
    if (starts(f, "EV,")) { if (events) events->push_back(f); }
    else if (!starts(f, "SEC,") && !starts(f, "ENV,") && !starts(f, "DEV,") && !starts(f, "PRK,") && !starts(f, "SYS,")) other.push_back(f);
  }
  return other;
}
static bool has(const std::vector<std::string> &v, const std::string &s) { for (auto &x : v) if (x == s) return true; return false; }
static bool hasPrefix(const std::vector<std::string> &v, const std::string &s) { for (auto &x : v) if (starts(x, s)) return true; return false; }

int main() {
  linkBegin(); settingsBegin(); secBegin(); diagBegin();
  sys.lastEspRx = 0;
  std::vector<std::string> ev;
  run(500);
  auto out = drain(&ev);
  CHECK(hasPrefix(out, "HI,1,0.1.0,"));
  CHECK(hasPrefix(ev, "EV,1,BOOT,POR"));
  CHECK(hasPrefix(ev, "EV,2,CFG,reset"));  // порожня EEPROM → типові значення

  // PING / пошкоджений CRC / USB без CRC
  esp("PING,41"); run(20); CHECK(has(drain(), "ACK,41"));
  Serial3.in += "$PING,42*00\n"; run(20); CHECK(!has(drain(), "ACK,42"));
  usb("$PING,7\n"); run(20); { auto u = frames(Serial); CHECK(has(u, "ACK,7")); }
  Serial3.in += "$PING,43\n"; run(20); CHECK(!has(drain(), "ACK,43"));  // з ESP без CRC не приймаємо

  // Список налаштувань
  esp("GET,44,cfg"); run(200); out = drain();
  CHECK(has(out, "VAL,cfg,fanOn,28") && has(out, "VAL,cfg,nightMask,6") && has(out, "ACK,44"));
  int vals = 0; for (auto &f : out) if (starts(f, "VAL,cfg,")) vals++; CHECK(vals == 14);
  CHECK(out.back() == "ACK,44");
  esp("GET,45,cal,rgb"); run(100); out = drain(); CHECK(has(out, "VAL,rgb,order,RGB") && has(out, "VAL,rgb,inv,0"));

  // Налаштування з перевіркою залежностей
  esp("SET,51,fanOff,29"); run(20); CHECK(has(drain(), "NAK,51,arg,fanOff"));
  esp("SET,52,fanOff,25"); run(20); ev.clear(); CHECK(has(drain(&ev), "ACK,52")); run(600); drain(&ev); CHECK(hasPrefix(ev, "EV,") && ev.size() >= 1 && ev[0].find("CFG,fanOff") != std::string::npos);
  CHECK(cfg.fanOff == 25);
  esp("SET,53,nope,1"); run(20); CHECK(has(drain(), "NAK,53,arg,nope"));
  esp("CAL,54,rgb,order,BGR"); run(20); CHECK(has(drain(), "ACK,54")); CHECK(cal.rgbOrder == 5);
  esp("CAL,55,servo,open,0"); run(20); CHECK(has(drain(), "NAK,55,arg,open"));  // закрито == відкрито
  run(2000); { FakeEEPROM &e = EEPROM; Cfg c; memcpy(&c, e.mem + 2, sizeof c); CHECK(c.fanOff == 25); }  // відкладений запис

  // Охорона: постановка, затримка, спрацювання, невірні коди
  esp("ARM,60,ARM"); run(20); ev.clear(); CHECK(has(drain(&ev), "ACK,60"));
  CHECK(sec.state == SEC_EXIT);
  esp("ARM,60,ARM"); run(20); CHECK(has(drain(), "ACK,60")); CHECK(sec.state == SEC_EXIT);  // повтор seq не виконується вдруге
  esp("ARM,61,ARM"); run(20); CHECK(has(drain(), "NAK,61,state"));
  run(30500); drain(&ev); CHECK(sec.state == SEC_ARM); CHECK(hasPrefix(ev, "EV,") );
  bool armedEv = false; for (auto &e : ev) if (e.find("ARMED,ARM") != std::string::npos) armedEv = true; CHECK(armedEv);
  CHECK(secStateLoad() == SEC_ARM);
  esp("MODE,62,gate,open"); run(20); CHECK(has(drain(), "NAK,62,state"));  // під охороною шлагбаум заблоковано
  secSensor(SN_PIR); CHECK(sec.state == SEC_ENTRY);
  esp("DISARM,63,1111"); run(20); CHECK(has(drain(), "NAK,63,pin,2"));
  esp("DISARM,64,2222"); run(20); CHECK(has(drain(), "NAK,64,pin,1"));
  esp("DISARM,65,3333"); run(20); CHECK(has(drain(), "NAK,65,lock,60"));
  esp("DISARM,66,1234"); run(20); out = drain(); CHECK(hasPrefix(out, "NAK,66,lock,"));  // навіть правильний код заблоковано
  run(16000); CHECK(sec.state == SEC_ALARM); CHECK(sec.trigger == SN_PIR);
  run(45000); esp("DISARM,67,1234"); run(20); CHECK(has(drain(), "ACK,67")); CHECK(sec.state == SEC_DIS);
  CHECK(secStateLoad() == SEC_DIS);

  // Нічний режим: рух не вмикає тривогу, вікно — одразу
  esp("ARM,70,NIGHT"); run(31000); drain(); CHECK(sec.state == SEC_NIGHT);
  secSensor(SN_PIR); CHECK(sec.state == SEC_NIGHT);
  secSensor(SN_WSENS); CHECK(sec.state == SEC_ALARM && sec.trigger == SN_WSENS);
  esp("DISARM,71,1234"); run(20); drain(); CHECK(sec.state == SEC_DIS);

  // Клавіатура: код# ставить і знімає
  for (char c : std::string("1234#")) secKey(c); CHECK(sec.state == SEC_EXIT);
  for (char c : std::string("1234#")) secKey(c); CHECK(sec.state == SEC_DIS);
  secKey('#'); CHECK(sec.wrong == 0);  // порожній # — не спроба

  // Зміна коду
  esp("PIN,72,1234,99"); run(20); CHECK(has(drain(), "NAK,72,arg"));
  esp("PIN,73,1234,5678"); run(20); CHECK(has(drain(), "ACK,73")); CHECK(!strcmp(secPin, "5678"));

  // Сервісний режим і перевірки
  esp("TEST,80,fan,run"); run(20); CHECK(has(drain(), "NAK,80,state"));
  esp("SVC,81,1"); run(20); CHECK(has(drain(), "ACK,81")); CHECK(sys.service);
  esp("ARM,82,ARM"); run(20); CHECK(has(drain(), "NAK,82,state"));  // у сервісному режимі охорона недоступна
  esp("TEST,83,fan,run"); run(100); out = drain(); CHECK(has(out, "ACK,83")); CHECK(dev.fanOn);
  run(3100); out = drain(); CHECK(has(out, "TR,83,fan,ok,3000")); CHECK(!dev.fanOn);
  esp("TEST,84,gate,open"); run(100); out = drain(); CHECK(has(out, "ACK,84") && has(out, "TR,84,gate,ok,512"));
  esp("TEST,85,tm1637,all"); run(100); out = drain(); CHECK(has(out, "TR,85,tm1637,ok"));
  esp("TEST,86,dht,nope"); run(20); CHECK(has(drain(), "NAK,86,arg"));
  esp("TEST,87,pir,wait"); run(100); out = drain(); CHECK(has(out, "ACK,87") && has(out, "TR,87,pir,wait,15"));
  io.edgeAt[SN_PIR] = fakeMillis + 1; run(100); out = drain(); CHECK(hasPrefix(out, "TR,87,pir,ok,"));
  esp("TEST,88,mq7,read"); run(50); out = drain(); CHECK(hasPrefix(out, "TR,88,mq7,ok,"));
  esp("SVC,89,0"); run(20); CHECK(has(drain(), "ACK,89")); CHECK(!sys.service);
  esp("SVC,90,1"); run(20); drain(); run(600100); CHECK(!sys.service);  // вимикається сам за 10 хв
  drain();

  // Невідомий тип, WATCH, HW-список
  esp("FOO,91"); run(20); CHECK(has(drain(), "NAK,91,type"));
  esp("GET,92,hw"); run(300); out = drain(); int hw = 0; for (auto &f : out) if (starts(f, "HW,")) hw++; CHECK(hw == 16 && out.back() == "ACK,92");
  CHECK(hasPrefix(out, "HW,board,ok,0,"));
  esp("WATCH,93,1"); run(1200); out = drain(); CHECK(hasPrefix(out, "IO,")); CHECK(hasPrefix(out, "HW,"));

  // Події повторюються, доки ESP не підтвердить
  frames(Serial3); autoAck = false;
  evPush(EV_SIREN_END); run(20); auto f1 = frames(Serial3); int n1 = 0; for (auto &f : f1) if (f.find("SIREN_END") != std::string::npos) n1++;
  run(600); auto f2 = frames(Serial3); int n2 = 0; for (auto &f : f2) if (f.find("SIREN_END") != std::string::npos) n2++;
  CHECK(n1 == 1 && n2 == 1);  // без підтвердження подія повторюється раз на 500 мс
  for (auto &f : f2) if (f.find("SIREN_END") != std::string::npos) esp("ACK," + f.substr(3, f.find(',', 3) - 3));
  run(1200); int n3 = 0; for (auto &f : frames(Serial3)) if (f.find("SIREN_END") != std::string::npos) n3++;
  CHECK(n3 == 0);  // після підтвердження повторів немає
  autoAck = true;

  // Кадри стану: DEV і PRK містять очікувані поля
  run(1100); std::string dv, pk; for (auto &f : frames(Serial3)) { if (starts(f, "DEV,")) dv = f; if (starts(f, "PRK,")) pk = f; }
  CHECK(starts(dv, "DEV,auto,")); CHECK(dv.find(",FFC98A,70") != std::string::npos);
  CHECK(starts(pk, "PRK,auto,")); CHECK(pk.find("OPEn") != std::string::npos || pk.find("StOP") != std::string::npos || pk.find("----") != std::string::npos);


  // --- виправлення після рев'ю ---
  // Пам'ять повторів розрахована на 8 команд: PING щосекунди не витісняє ARM
  esp("DISARM,200,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);
  esp("ARM,201,ARM"); run(20); CHECK(has(drain(), "ACK,201")); CHECK(sec.state == SEC_EXIT);
  esp("DISARM,202,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);
  for (int i = 203; i < 209; i++) { esp("PING," + std::to_string(i)); run(20); drain(); }
  esp("ARM,201,ARM"); run(20); CHECK(has(drain(), "ACK,201")); CHECK(sec.state == SEC_DIS);  // повтор не виконується

  // Повтор GET під час передачі списку не дає передчасного ACK
  esp("GET,210,cfg"); run(30); esp("GET,210,cfg"); run(300); out = drain();
  { int acks = 0, ackAt = -1, lastVal = -1;
    for (size_t i = 0; i < out.size(); i++) { if (out[i] == "ACK,210") { acks++; ackAt = i; } if (starts(out[i], "VAL,cfg,")) lastVal = i; }
    CHECK(acks == 1); CHECK(ackAt > lastVal);  // ACK лише один і лише після всього списку
  }

  // Сцена змінює режими пристроїв і повідомляє про це журналу
  ev.clear(); esp("MODE,211,light,on"); run(20); drain(&ev);
  esp("SCENE,212,away"); run(50); ev.clear(); drain(&ev);
  CHECK(dev.lightMode == OM_OFF);
  { bool m = false; for (auto &e : ev) if (e.find("MODE,light,off,scene") != std::string::npos) m = true; CHECK(m); }
  esp("SCENE,213,home,5678"); run(50); drain(); CHECK(sec.state == SEC_DIS && dev.lightMode == OM_AUTO);

  // Запобіжник вентилятора: режим «увімкнено» сам спадає в «авто» через 5 с.
  // Поки двигун крутиться, він глушить лінію від ESP, і команда «вимкнути» не доходить —
  // без цього таймера двигун зупиняло б лише вимкнення живлення будинку.
  esp("MODE,220,fan,on"); run(50); CHECK(has(drain(), "ACK,220"));
  CHECK(dev.fanMode == OM_ON && dev.fanOn);
  run(4000); CHECK(dev.fanMode == OM_ON);  // до строку тримається
  ev.clear(); run(1500); drain(&ev);
  CHECK(dev.fanMode == OM_AUTO);
  { bool m = false; for (auto &e : ev) if (e.find("MODE,fan,auto,timeout") != std::string::npos) m = true; CHECK(m); }

  // Постановка без коду, коли quickArm = 0, не витрачає спробу
  esp("SET,214,quickArm,0"); run(20); drain();
  uint8_t wrongBefore = sec.wrong;
  esp("ARM,215,ARM"); run(20); CHECK(has(drain(), "NAK,215,arg")); CHECK(sec.wrong == wrongBefore);
  esp("ARM,216,ARM,5678"); run(20); CHECK(has(drain(), "ACK,216")); CHECK(sec.state == SEC_EXIT);
  esp("DISARM,217,5678"); run(20); drain();
  esp("SET,218,quickArm,1"); run(20); drain();

  // Клавіша «*» не може потрапити в кадр як роздільник
  esp("SVC,219,1"); esp("WATCH,220,1"); run(50); drain();
  diagKey('*'); diagKey('5'); run(20); out = drain();
  CHECK(has(out, "KEY,_") && has(out, "KEY,5"));
  esp("SVC,221,0"); run(20); drain();


  // --- виправлення після рев'ю логіки ---
  // Захист від защемлення діє й під охороною
  esp("SVC,230,0"); run(20); drain();
  env.bright = true; io.wsObstacle = false; run(100); drain();
  CHECK(curtainPct() == 100);
  io.wsObstacle = true;
  esp("ARM,231,ARM"); run(100); out = drain();
  CHECK(has(out, "ACK,231"));
  CHECK(dev.winBlocked); CHECK(curtainPct() == 100);  // штора не їде на перешкоду
  io.wsObstacle = false; run(100); drain(); CHECK(curtainPct() == 0);

  run(31000); drain(); CHECK(sec.state == SEC_ARM);
  esp("DISARM,232,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);

  // Захист тримає позицію, а не сповзає вниз. Калібрування макета (open=165) — найгірший
  // випадок: один відсоток там 1,65°, і ціль у відсотках зсувалася щоцикл, доки штора не
  // доїжджала до кута, кратного кроку відсотка (24° нижче, тобто 15 % ходу).
  esp("CAL,233,servo,open,165"); run(50); drain();
  esp("WIN,234,75"); run(200); drain();
  uint8_t held = curtainPct();
  CHECK(held == 74);  // 75 % від 165° це 123°, а 123° назад у відсотки — 74
  env.bright = false; io.wsObstacle = true;
  esp("WIN,235,0"); run(3000); drain();
  CHECK(dev.winBlocked);
  CHECK(curtainPct() == held);        // не сповзло за три секунди
  CHECK(curtainAngle() == 123);       // і кут той самий
  io.wsObstacle = false; run(500); drain(); CHECK(curtainPct() == 0);
  esp("CAL,236,servo,open,90"); run(50); drain();

  // Прострочений строк не оживає після того, як millis() пішов далеко вперед. Блокування
  // клавіатури лишається в полі ненульовим, і різниця «at - now» перекочується в додатну
  // через 2³¹ мс — доти вірний код отримував NAK,lock ще ~25 діб.
  esp("PIN,240,1111,9999"); run(50); drain();
  esp("PIN,241,1111,9999"); run(50); drain();
  esp("PIN,242,1111,9999"); run(50); drain();  // три невірні — введення заблоковано на 60 с
  esp("PIN,243,5678,5678"); run(50); out = drain();
  CHECK(hasPrefix(out, "NAK,243,lock"));       // вірний код, але зараз справді заблоковано
  // Строк оживає, коли now перевищив його більше ніж на 2³¹ мс, тож стрибок мусить
  // перекрити ще й самі 60 с блокування
  fakeMillis += 0x80000000UL + 120000;
  run(100); drain();
  esp("PIN,244,5678,5678"); run(100); out = drain();
  CHECK(has(out, "ACK,244"));                  // строк давно минув — код мусить працювати

  // Зайнятий буфер передачі не витрачає спроб повтору події. Строк — 10 с, тобто двадцять
  // справжніх відправок; доти лічильник крутився з частотою циклу, і подія вилітала з черги
  // за 0,6 с, а ESP отримував замість неї EV_LOST.
  autoAck = false;  // ESP мовчить, тож подія лишається в польоті й повторюється
  run(50); drain();
  evPush(EV_CO_OK, 7);
  run(30); drain();    // перша відправка пішла в лінію — прибираємо її з виводу
  Serial3.txFree = 0;  // далі кадр у буфер не влазить: повтори не проходять
  run(1500);           // доти лічильник крутився з частотою циклу й подія вилітала за 0,6 с
  Serial3.txFree = 63;
  run(600);
  std::vector<std::string> ev3;
  drain(&ev3);
  bool again = false, lost = false;
  for (auto &f : ev3) {
    if (f.find("CO_OK") != std::string::npos) again = true;
    if (f.find("EV_LOST") != std::string::npos) lost = true;
  }
  CHECK(again);  // подія дочекалась вільного буфера й повторилась
  CHECK(!lost);  // і не була підмінена на EV_LOST
  autoAck = true; run(600); drain();

  // Шлагбаум зачиняється, коли машина від'їхала за межі далекоміра
  prk.dist = 5; run(200); drain(); CHECK(gateState() == GS_OPEN);
  prk.dist = -1; run(cfg.closeDelay * 1000 + 1500); drain();
  CHECK(gateState() == GS_CLOSED);

  // Сцена «Я вдома» без коду не витрачає спробу
  esp("ARM,233,ARM"); run(31000); drain();
  { uint8_t before = sec.wrong;
    esp("SCENE,234,home"); run(20); CHECK(has(drain(), "NAK,234,arg"));
    CHECK(sec.wrong == before); CHECK(sec.state == SEC_ARM); }
  esp("SCENE,235,home,5678"); run(50); drain(); CHECK(sec.state == SEC_DIS);

  // Стан модуля не показує «востаннє працював 49 діб тому»
  hwOk(C_DHT); esp("GET,236,hw"); run(300); out = drain();
  for (auto &f : out) if (starts(f, "HW,dht,")) CHECK(f == "HW,dht,ok,0,0");

  // Після 25 діб роботи правильний код усе одно знімає охорону.
  // Будинок, де ніхто жодного разу не помилився кодом: lockUntil лишився нулем
  esp("ARM,240,ARM"); run(31000); drain(); CHECK(sec.state == SEC_ARM);
  sec.lockUntil = 0; sec.wrong = 0;
  diagWatch(false, PORT_USB);
  fakeMillis = 2147483649UL; run(200); drain();
  { bool io_frames = false; for (auto &f : frames(Serial3)) if (starts(f, "IO,")) io_frames = true;
    CHECK(!io_frames); }  // спостереження вимкнене й не вмикається саме
  esp("DISARM,241,5678"); run(20); out = drain();
  CHECK(has(out, "ACK,241")); CHECK(sec.state == SEC_DIS);
  { std::string secFrame; for (auto &f : frames(Serial3)) if (starts(f, "SEC,")) secFrame = f;
    run(1100); for (auto &f : frames(Serial3)) if (starts(f, "SEC,")) secFrame = f;
    CHECK(secFrame == "SEC,DIS,-,0,-,0,0"); }  // поле «заблоковано» не з'їхало

  // Пам'ять повторів має строк давності: через 4 с той самий номер — уже нова
  // команда. Інакше ESP, що перезапустився й почав лічильник з 1, отримував би
  // збережені ACK, а команди не виконувались би
  esp("ARM,250,ARM"); run(20); drain(); CHECK(sec.state == SEC_EXIT);
  esp("DISARM,251,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);
  esp("ARM,250,ARM"); run(20); CHECK(has(drain(), "ACK,250"));
  CHECK(sec.state == SEC_DIS);  // поки свіжий — повтор, не виконується
  run(3400); drain();
  esp("ARM,250,ARM"); run(20); CHECK(has(drain(), "ACK,250"));
  CHECK(sec.state == SEC_DIS);  // строк мусить із запасом покривати вікно повторів ESP
                                // (12 спроб із паузою до 239 мс), тож це так само повтор
  run(1200); drain();
  esp("ARM,250,ARM"); run(20); CHECK(has(drain(), "ACK,250"));
  CHECK(sec.state == SEC_EXIT);  // за строком давності виконується як нова
  esp("DISARM,252,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);

  // Вісім слотів покривають вісім справжніх команд: пачка з шести SET (збереження форми)
  // разом із щосекундним PING не витісняє з кеша ранню команду
  esp("ARM,280,ARM"); run(20); CHECK(has(drain(), "ACK,280")); CHECK(sec.state == SEC_EXIT);
  esp("DISARM,281,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);
  for (int i = 0; i < 6; i++) {
    esp("SET," + std::to_string(282 + i) + ",siren,6" + std::to_string(i)); run(20); drain();
    esp("PING," + std::to_string(290 + i)); run(20); drain();
  }
  CHECK(cfg.siren == 65);
  esp("ARM,280,ARM"); run(20); CHECK(has(drain(), "ACK,280"));
  CHECK(sec.state == SEC_DIS);  // ARM усе ще в кеші: повтор не ставить будинок під охорону

  // Ручне довертання шлагбаума: лише в сервісному режимі й лише з числом
  esp("SVC,260,0"); run(20); drain();
  esp("CALDO,261,gate,step,40"); run(20); CHECK(has(drain(), "NAK,261,state"));
  esp("SVC,262,1"); run(20); drain();
  esp("CALDO,263,gate,step"); run(20); CHECK(has(drain(), "NAK,263,arg"));
  esp("CALDO,264,gate,step,9999"); run(20); CHECK(has(drain(), "NAK,264,arg"));
  esp("CALDO,265,gate,step,-40"); run(20); CHECK(has(drain(), "ACK,265"));
  esp("SVC,266,0"); run(20); drain();

  // Поки ESP мовчить, черга подій не накопичується: інакше втрачена подія
  // породжувала б EV_LOST, той — знову втрату, і так без кінця
  autoAck = false;
  run(6000);  // 5 с без кадрів від ESP — він вважається недоступним
  frames(Serial3);
  evPush(EV_SIREN_END);
  run(20);
  { int n = 0; for (auto &f : frames(Serial3)) if (f.find("SIREN_END") != std::string::npos) n++;
    CHECK(n == 1); }  // надіслано один раз, без черги
  run(3000);
  { int again = 0, lost = 0;
    for (auto &f : frames(Serial3)) {
      if (f.find("SIREN_END") != std::string::npos) again++;
      if (f.find("EV_LOST") != std::string::npos) lost++;
    }
    CHECK(again == 0); CHECK(lost == 0); }  // ні повторів, ні лавини EV_LOST
  // ESP повернувся — черга й повтори працюють як раніше
  esp("PING,270"); run(20); frames(Serial3);
  evPush(EV_SIREN_END); run(20);
  { int n = 0; for (auto &f : frames(Serial3)) if (f.find("SIREN_END") != std::string::npos) n++;
    CHECK(n == 1); }
  run(600);
  { int again = 0; for (auto &f : frames(Serial3)) if (f.find("SIREN_END") != std::string::npos) again++;
    CHECK(again == 1); }  // без підтвердження подія повторюється раз на 500 мс
  autoAck = true;
  esp("PING,271"); run(20); drain();

  // Сміття в EEPROM не має піднімати будинок під охорону. Старий формат був «стан плюс
  // XOR-байт», і випадковий вміст комірок мав шанс скластися в дійсний запис
  EEPROM.update(256, SEC_ARM);
  EEPROM.update(257, SEC_ARM ^ 0x5A);
  EEPROM.update(258, 0);
  CHECK(secStateLoad() == SEC_DIS);
  secStateSave(SEC_NIGHT); CHECK(secStateLoad() == SEC_NIGHT);
  EEPROM.update(257, SEC_ARM);  // підміна стану без перерахунку CRC
  CHECK(secStateLoad() == SEC_DIS);
  secStateSave(SEC_DIS);

  // Полярність виходу мікрофона задається калібруванням: модулі на LM393 бувають обох
  esp("CAL,266,sound,inv,1"); run(20); CHECK(has(drain(), "ACK,266"));
  CHECK(cal.soundInv == 1);
  esp("GET,267,cal,sound"); run(300); out = drain();
  CHECK(has(out, "VAL,sound,inv,1"));
  esp("CAL,268,sound,inv,2"); run(20); CHECK(has(drain(), "NAK,268,arg,inv"));
  esp("CAL,269,sound,inv,0"); run(20); drain(); CHECK(cal.soundInv == 0);

  // Положення стріли переживає вимкнення живлення: без цього прошивка після старту
  // вважала б підняту стрілу опущеною і довертала б її ще на хід у той самий бік
  { gatePosSave(cal.gateSteps);  // ціль пишеться перед рухом: якщо живлення зникне на ходу,
    CHECK(gatePosLoad() == (int16_t)cal.gateSteps);  // після старту вважаємо, що стріла доїхала
    gatePosSave(0); CHECK(gatePosLoad() == 0);
    // Запис іде по колу у вісім слотів, тож зношується не одна комірка. Після двадцяти
    // записів (кільце обернулося двічі, номер слота зріс) читається саме останнє значення
    for (int16_t v = 1; v <= 20; v++) gatePosSave(v);
    CHECK(gatePosLoad() == 20);
    // Зіпсоване все кільце — позиція невідома, стріла вважається зачиненою
    for (int a = 259; a < 259 + 8 * 4; a++) EEPROM.update(a, 0xFF);
    CHECK(gatePosLoad() == 0);
    // Позиція поза калібруванням теж не вірогідна
    gatePosSave(cal.gateSteps); cal.gateSteps /= 2;
    CHECK(gatePosLoad() == 0);
    cal.gateSteps *= 2; }

  // Стріла, піднята ще до перезапуску, не лишається висіти: автоматика її зачиняє
  esp("MODE,272,gate,auto"); run(20); drain();
  gateBegin(cal.gateSteps); prk.dist = -1;
  run(200); drain(); CHECK(gateState() == GS_OPEN);
  run(cfg.closeDelay * 1000 + 1500); drain();
  CHECK(gateState() == GS_CLOSED);

  // Перевірка датчика скасовується постановкою під охорону. Інакше під охороною людина
  // махала б рукою перед датчиком, secSensor() дав би ENTRY чи ALARM, а перевірка
  // звітувала б «ok» — саме від цього її й пускають лише в стані DIS
  esp("TEST,300,pir,wait"); run(100); out = drain();
  CHECK(has(out, "ACK,300") && has(out, "TR,300,pir,wait,15"));
  esp("ARM,301,ARM"); run(20); out = drain();
  CHECK(has(out, "ACK,301")); CHECK(has(out, "TR,300,pir,fail,state"));
  esp("DISARM,302,5678"); run(20); drain(); CHECK(sec.state == SEC_DIS);
  esp("TEST,303,pir,wait"); run(100); out = drain();  // стара перевірка не лишилась активною
  CHECK(has(out, "TR,303,pir,wait,15"));
  io.edgeAt[SN_PIR] = fakeMillis + 1; run(100); drain();

  // Кадр ENV несе coAlarm і coMuted: вивести їх із coLvl неможливо — тривога знімається
  // з гістерезисом і триває ще нижче порога, а про заглушення знала лише подія CO_MUTE
  env.dhtOk = true; env.t = 26; env.h = 44; env.coRaw = 324; env.ldr = 182; env.bright = true;
  env.coValid = true; env.co = cfg.coDanger; env.coLvl = 2;
  run(1100); CHECK(env.coAlarm);
  { std::string en; for (auto &f : frames(Serial3)) if (starts(f, "ENV,")) en = f;
    CHECK(en == "ENV,26,44,70,324,182,1,2,1,0"); }
  esp("MUTE,310"); run(1100); CHECK(env.coMuted);
  { std::string en; for (auto &f : frames(Serial3)) if (starts(f, "ENV,")) en = f;
    CHECK(en == "ENV,26,44,70,324,182,1,2,1,1"); }
  // Рівень упав нижче порога, але вище за гістерезис: тривога триває, хоч coLvl уже 0
  env.co = cfg.coDanger - 5; env.coLvl = 0; run(1100); CHECK(env.coAlarm);
  { std::string en; for (auto &f : frames(Serial3)) if (starts(f, "ENV,")) en = f;
    CHECK(en == "ENV,26,44,65,324,182,1,0,1,1"); }
  env.co = 0; run(1100); CHECK(!env.coAlarm); drain();

  // Налаштування humOn мусить переживати перезапуск: міграцію прив'язано до версії
  // запису, а не до самого значення, інакше вона спрацьовувала при кожному старті
  // й половина задекларованого діапазону (40–79) була недосяжна
  esp("SET,320,humOn,70"); run(20); CHECK(has(drain(), "ACK,320")); CHECK(cfg.humOn == 70);
  run(2000);  // відкладений запис у EEPROM
  settingsBegin(); CHECK(cfg.humOn == 70);
  // Запис попередньої версії читається як є, humOn піднімається один раз і запис стає п'ятим.
  // Структура тут — копія Store із settings.cpp: розійдуться — тест це й покаже
  { struct StoreV4 { uint8_t magic, version; Cfg cfg; Cal cal; char pin[7]; uint8_t crc; } v4;
    EEPROM.get(0, v4);
    v4.version = 4;
    v4.cfg.humOn = 66;
    v4.crc = crc8((const uint8_t *)&v4, offsetof(StoreV4, crc));
    EEPROM.put(0, v4);
    settingsBegin();
    CHECK(cfg.humOn == 85); CHECK(EEPROM.read(1) == 5);  // міграція й новий номер версії
    CHECK(!strcmp(secPin, "5678")); CHECK(cal.gateSteps == 512); }  // решта полів на місці
  esp("SET,321,humOn,70"); run(20); drain(); run(2000);
  settingsBegin(); CHECK(cfg.humOn == 70);  // удруге міграція не спрацьовує

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
