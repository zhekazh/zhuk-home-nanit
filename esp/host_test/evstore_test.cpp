// Тести кільцевого журналу подій у файлі. Перезапуск ESP тут — справжній перезапуск
// процесу: статики модуля мають відновитися з самого файлу, а не з пам'яті. Тому
// бінарник запускається кілька разів із назвою етапу, а тека передається аргументом.
#include <string>
#include <vector>

#include "LittleFS.h"
#include "check.h"
#include "zhe.h"

int checks = 0, fails = 0;

void logPrint(const char *) {}
void logf(const char *, ...) {}

// ---------- читання журналу ----------
struct Rec {
  uint32_t at;
  std::string raw;
};
static std::vector<Rec> got;
static void collect(uint32_t at, const char *raw) { got.push_back({at, std::string(raw)}); }
static std::vector<Rec> readAll() {
  got.clear();
  evStoreEach(collect);
  return got;
}

static const uint32_t FILE_BYTES = 120 * 80;  // 120 слотів по 80 Б

static uint32_t fileSize() {
  File f = LittleFS.open("/events", "r");
  return f ? f.size() : 0;
}

static std::string ev(int n) {
  char b[32];
  snprintf(b, sizeof b, "EV,%d,PIR,-", n);
  return b;
}

// ---------- етапи ----------
static void phaseFresh() {
  evStoreBegin();
  CHECK(fileSize() == FILE_BYTES);  // файл створюється повного розміру одразу
  CHECK(readAll().empty());

  evStorePush(1000, "EV,1,SEC_ARM,-");
  evStorePush(1001, "EV,2,PIR,-");
  evStorePush(1002, "EV,3,SEC_DIS,-");
  // Записи чекають у пам'яті: флеш витримує близько 100 000 стирань на сектор,
  // тож запис на кожну подію був би марнотратством ресурсу
  CHECK(readAll().empty());

  evStoreFlush();
  auto v = readAll();
  CHECK(v.size() == 3);
  if (v.size() == 3) {
    CHECK(v[0].at == 1000);
    CHECK_EQ(v[0].raw, "EV,1,SEC_ARM,-");
    CHECK_EQ(v[2].raw, "EV,3,SEC_DIS,-");
  }
  CHECK(fileSize() == FILE_BYTES);  // файл не росте
}

static void phaseRestart() {
  evStoreBegin();  // новий процес: позицію запису треба знайти в самому файлі
  auto v = readAll();
  CHECK(v.size() == 3);
  if (v.size() == 3) {
    CHECK_EQ(v[0].raw, "EV,1,SEC_ARM,-");
    CHECK_EQ(v[2].raw, "EV,3,SEC_DIS,-");
  }

  // Дописуємо далі — старі записи мають лишитись на місці
  evStorePush(2000, "EV,4,WIN_BLOCK,-");
  evStoreFlush();
  v = readAll();
  CHECK(v.size() == 4);
  if (v.size() == 4) {
    CHECK_EQ(v[0].raw, "EV,1,SEC_ARM,-");
    CHECK_EQ(v[3].raw, "EV,4,WIN_BLOCK,-");
  }
}

static void phaseWrap() {
  evStoreBegin();
  CHECK(readAll().empty());

  // Черга в пам'яті на 12 записів: коли повна, скидається сама
  for (int i = 1; i <= 130; i++) evStorePush(10000 + i, ev(i).c_str());
  evStoreFlush();

  auto v = readAll();
  CHECK(v.size() == 120);  // 120 слотів: найдавніші витіснені
  if (v.size() == 120) {
    CHECK_EQ(v[0].raw, ev(11));    // перші десять уже затерті
    CHECK_EQ(v[119].raw, ev(130));  // найновіша — остання
    bool ordered = true;
    for (size_t i = 1; i < v.size(); i++)
      if (v[i].at <= v[i - 1].at) ordered = false;
    CHECK(ordered);  // читання йде від найстарішої до найновішої
  }
  CHECK(fileSize() == FILE_BYTES);  // після 130 записів файл того самого розміру
}

static void phaseAfterWrap() {
  evStoreBegin();
  auto v = readAll();
  CHECK(v.size() == 120);
  if (v.size() == 120) {
    // Позиція запису шукається як найстаріший запис, тож порядок переживає перезапуск
    CHECK_EQ(v[0].raw, ev(11));
    CHECK_EQ(v[119].raw, ev(130));
  }
  // Наступний запис має лягти на місце найстарішого, а не в кінець файлу
  evStorePush(20000, "EV,999,CO_ALARM,-");
  evStoreFlush();
  v = readAll();
  CHECK(v.size() == 120);
  if (v.size() == 120) {
    CHECK_EQ(v[0].raw, ev(12));
    CHECK_EQ(v[119].raw, "EV,999,CO_ALARM,-");
  }
  CHECK(fileSize() == FILE_BYTES);
}

// Подія до синхронізації з NTP несе час 0, і перша така приходить при кожному старті
// (BOOT). Голову кільця за часом шукати не можна: вона стала б саме на цю подію.
static void phaseZero() {
  evStoreBegin();
  CHECK(readAll().empty());

  evStorePush(0, "EV,1,BOOT,-");  // NTP ще не відповів
  evStorePush(5000, "EV,2,SEC_ARM,-");
  evStorePush(5001, "EV,3,PIR,-");
  evStoreFlush();

  auto v = readAll();
  CHECK(v.size() == 3);
  if (v.size() == 3) {
    CHECK(v[0].at == 0);
    CHECK_EQ(v[0].raw, "EV,1,BOOT,-");
    CHECK_EQ(v[2].raw, "EV,3,PIR,-");
  }
}

static void phaseZeroRestart() {
  evStoreBegin();  // новий процес: голову шукаємо в самому файлі
  auto v = readAll();
  CHECK(v.size() == 3);
  if (v.size() == 3) CHECK_EQ(v[0].raw, "EV,1,BOOT,-");

  // Наступний запис мусить лягти після наявних, а не затерти подію без часу
  evStorePush(5002, "EV,4,WIN_BLOCK,-");
  evStoreFlush();
  v = readAll();
  CHECK(v.size() == 4);
  if (v.size() == 4) {
    CHECK_EQ(v[0].raw, "EV,1,BOOT,-");
    CHECK_EQ(v[3].raw, "EV,4,WIN_BLOCK,-");
  }
}

// Те саме, але кільце вже обернулося: подія без часу лежить у слоті 19 і нікуди не ділась
static void phaseZeroWrap() {
  evStoreBegin();
  CHECK(readAll().empty());

  for (int i = 1; i <= 125; i++) {
    if (i == 20) evStorePush(0, "EV,20,BOOT,-");
    else evStorePush(20000 + i, ev(i).c_str());
  }
  evStoreFlush();

  auto v = readAll();
  CHECK(v.size() == 120);
  if (v.size() == 120) {
    CHECK_EQ(v[0].raw, ev(6));  // перші п'ять витіснені
    CHECK_EQ(v[119].raw, ev(125));
  }
  CHECK(fileSize() == FILE_BYTES);
}

static void phaseZeroAfterWrap() {
  evStoreBegin();
  auto v = readAll();
  CHECK(v.size() == 120);
  if (v.size() == 120) {
    CHECK_EQ(v[0].raw, ev(6));           // найстаріший, а не подія без часу зі слота 19
    CHECK(v[14].at == 0);                // двадцятий запис: шостий став першим
    CHECK_EQ(v[14].raw, "EV,20,BOOT,-");
    CHECK_EQ(v[119].raw, ev(125));
  }

  // Затирається найстаріший запис, а не той, що стоїть одразу за подією без часу
  evStorePush(20200, "EV,200,CO_ALARM,-");
  evStoreFlush();
  v = readAll();
  CHECK(v.size() == 120);
  if (v.size() == 120) {
    CHECK_EQ(v[0].raw, ev(7));
    CHECK_EQ(v[13].raw, "EV,20,BOOT,-");
    CHECK_EQ(v[119].raw, "EV,200,CO_ALARM,-");
  }
}

// Файл від старішої прошивки: слот був «<unix>\t<кадр>», без номера запису. Розібрати
// його нічим, тож журнал починаємо заново — втратити один раз десятки записів
// налагодження краще, ніж мовчки віддавати сторінці кашу.
static void phaseLegacy() {
  {
    File f = LittleFS.open("/events", "w");
    CHECK((bool)f);
    for (int i = 0; i < 120; i++) {
      char slot[81];
      int n = snprintf(slot, sizeof slot, "%d\tEV,%d,PIR,-", 30000 + i, i + 1);
      memset(slot + n, ' ', 79 - n);
      slot[79] = '\n';
      f.write((const uint8_t *)slot, 80);
    }
    f.close();
  }

  evStoreBegin();
  CHECK(fileSize() == FILE_BYTES);
  CHECK(readAll().empty());  // старі слоти не розбираються, файл створено заново

  evStorePush(31000, "EV,1,SEC_ARM,-");
  evStoreFlush();
  auto v = readAll();
  CHECK(v.size() == 1);
  if (v.size() == 1) CHECK_EQ(v[0].raw, "EV,1,SEC_ARM,-");
}

// Скидання за часом: раз на п'ять хвилин, якщо є що зберігати
static void phaseTimer() {
  evStoreBegin();
  evStorePush(3000, "EV,5,PIR,-");
  fakeMillis += 299000;
  evStoreLoop(fakeMillis);
  CHECK(readAll().empty());  // ще рано
  fakeMillis += 2000;
  evStoreLoop(fakeMillis);
  CHECK(readAll().size() == 1);

  // Задовгий запис обрізається до розміру слота й не псує сусідній
  std::string longRaw = "EV,6," + std::string(EV_LEN - 10, 'Z');
  evStorePush(4000, longRaw.c_str());
  evStorePush(4001, "EV,7,PIR,-");
  evStoreFlush();
  auto v = readAll();
  CHECK(v.size() == 3);
  if (v.size() == 3) {
    CHECK(v[1].at == 4000);
    CHECK(v[1].raw.size() < EV_LEN);        // обрізано під слот
    CHECK(v[1].raw.compare(0, 5, "EV,6,") == 0);
    CHECK_EQ(v[2].raw, "EV,7,PIR,-");       // сусідній слот цілий
  }
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("вжиток: evstore_test <тека> <етап>\n");
    return 2;
  }
  littlefsRoot(argv[1]);
  std::string phase = argv[2];
  if (phase == "fresh") phaseFresh();
  else if (phase == "restart") phaseRestart();
  else if (phase == "wrap") phaseWrap();
  else if (phase == "after-wrap") phaseAfterWrap();
  else if (phase == "timer") phaseTimer();
  else if (phase == "zero") phaseZero();
  else if (phase == "zero-restart") phaseZeroRestart();
  else if (phase == "zero-wrap") phaseZeroWrap();
  else if (phase == "zero-after-wrap") phaseZeroAfterWrap();
  else if (phase == "legacy") phaseLegacy();
  else {
    printf("невідомий етап: %s\n", phase.c_str());
    return 2;
  }
  return report(("evstore/" + phase).c_str());
}
