// Вузол між Nanit і сторінками: тримає останній знімок стану, журнал подій
// і перекладає команди сторінки в кадри протоколу.
//
// Кадри йдуть у браузер тим самим текстом, що прийшли від Nanit, лише без «*CRC»:
// сторінка розбирає їх так само, як це робив би ще один Nanit. Своєї мови ESP не вводить.
// Сам рівень — кадри, команди, привітання, власні причини відмов — описує docs/api.md.
#include "zhe.h"

// ---------- знімок ----------
// П'ять кадрів стану плюс привітання: нова сторінка одразу бачить будинок цілком,
// не чекаючи секунди до наступної розсилки.
enum : uint8_t { S_HI, S_SEC, S_ENV, S_DEV, S_PRK, S_SYS, S_COUNT };
static const char *const S_TYPE[S_COUNT] = {"HI", "SEC", "ENV", "DEV", "PRK", "SYS"};
static char snap[S_COUNT][FRAME_MAX];

// ---------- журнал ----------
// Події тримаємо в RAM разом із часом, коли вони прийшли. Час потрібен саме тут: Nanit
// годинника не має й нічого про час не знає, а сторінка після перезавантаження вже не
// здогадається, коли сталася подія, — усі записи отримали б час відкриття вкладки.
#define EV_KEEP 40

struct EvRec {
  uint32_t at;  // unix-час; 0 — сталося до синхронізації з NTP
  char raw[EV_LEN];
};

static EvRec evLog[EV_KEEP];
static uint32_t evSeqNext;  // номер наступної події; кільце індексується seq % EV_KEEP
static uint8_t evCount;     // скільки записів наявні (до EV_KEEP)

// Подія лягає і в пам'ять (звідти її бере нова сторінка), і в чергу на запис у файл
static void evKeep(uint32_t at, const char *raw) {
  EvRec &r = evLog[evSeqNext % EV_KEEP];
  r.at = at;
  strlcpy(r.raw, raw, EV_LEN);
  evSeqNext++;
  if (evCount < EV_KEEP) evCount++;
}

static void evPush(const char *raw) {
  uint32_t at = timeNow();
  evKeep(at, raw);
  evStorePush(at, raw);
}

// Після перезапуску ESP пам'ять порожня — відновлюємо останні події з файлу, щоб журнал
// у вебінтерфейсі не починався з нуля щоразу, коли модуль перезавантажили чи оновили.
void hubRestore() {
  evStoreEach([](uint32_t at, const char *raw) { evKeep(at, raw); });
  logf("журнал: відновлено %u подій", evCount);
}

void hubFrame(char **argv, uint8_t argc, const char *raw) {
  (void)argc;
  for (uint8_t i = 0; i < S_COUNT; i++) {
    if (!strcmp(argv[0], S_TYPE[i])) {
      strlcpy(snap[i], raw, FRAME_MAX);
      break;
    }
  }
  if (!strcmp(argv[0], "EV")) {
    evPush(raw);
    // Живу подію отримують лише привітані сторінки: та, що саме дістає історію, побачить її
    // кадром EVT на своєму місці. Інакше подія приходила б двічі, ще й поперед старіших —
    // рівно те, від чого для рядків консолі зроблено wsSendLog.
    wsSendEv(raw);
    return;
  }
  wsSend(raw);
}

void hubCmdDone(uint16_t webId, uint8_t client, const char *raw) {
  if (!webId) return;  // команда самого ESP: сторінці про неї знати не треба
  char line[FRAME_MAX + 16];
  snprintf(line, sizeof(line), "RSP,%u,%s", webId, raw);
  wsSendTo(client, line);  // саме тій сторінці: чужий RSP збив би її з відповіддю
}

// Привітання нової сторінки — це до 86 повідомлень: 40 рядків журналу модуля (їх віддає
// web.cpp), 6 кадрів знімка й до 40 подій. Одним заходом це ≈7 КБ у буфер TCP і стрибок
// купи в модулі, де вільних близько 29 КБ — саме через брак цієї пам'яті рвалася OTA
// (див. комментар про wsCloseAll у web.cpp). Тому привітання розкладене на кроки: курсор
// на кожного клієнта тримає web.cpp і кличе цю функцію по кілька разів за прохід циклу.
//
// Порядок кроків незмінний: спершу знімок стану, потім події. Сторінка мусить знати, у
// якому стані будинок, перш ніж розкладати його історію, — інакше журнал приходив би
// раніше за те, до чого він стосується. Крок із порожнім кадром знімка нічого не шле, але
// номер усе одно займає: курсор не мусить залежати від того, що Nanit уже надіслав.
// false — кроки скінчились.
bool hubGreetStep(uint8_t client, uint16_t step, uint32_t *evSeq) {
  if (step < S_COUNT) {
    if (snap[step][0]) wsSendTo(client, snap[step]);
    return true;
  }
  // Події віддаємо за НОМЕРОМ, а не за індексом у кільці. Доти курсором був номер кроку, а
  // початок відліку рахувався щоразу від evHead: щойно під час привітання надходила нова
  // подія, кільце прокручувалось, усі індекси зсувались — і клієнт назавжди пропускав рівно
  // одну подію на кожну, що надійшла за цей час (а при незаповненому кільці отримував
  // дубль). Для рядків консолі цю саму пастку вже закрито номерами (logbuf.cpp).
  if (!evCount) return false;
  uint32_t oldest = evSeqNext - evCount;
  if (*evSeq < oldest) *evSeq = oldest;  // те, що вже витіснене з кільця, наздогнати годі
  if (*evSeq >= evSeqNext) return false;
  // Кадр «EVT,<unix>,EV,…» — та сама подія, але з часом: сторінка ставить її в журнал
  // на своє місце замість того, щоб вважати щойно отриманою.
  const EvRec &e = evLog[*evSeq % EV_KEEP];
  char line[EV_LEN + 24];
  snprintf(line, sizeof(line), "EVT,%lu,%s", (unsigned long)e.at, e.raw);
  wsSendTo(client, line);
  (*evSeq)++;
  return true;
}

// ---------- команди від сторінки ----------
// Формат: «<id>,<ТИП>[,аргументи]» — той самий кадр без номера команди й CRC.
// Номер для Nanit ставить link.cpp, а <id> служить лише для відповіді цій сторінці.
void hubWebText(uint8_t client, const char *text) {
  // Кадр до Nanit — лише ASCII (docs/protocol.md), і службові знаки в ньому зайняті: «$»
  // починає кадр, «*» відкриває CRC, «\r\n» його завершує. Тож у полях лишаються тільки
  // друковані знаки 0x20–0x7E, і фільтр іде за діапазоном, а не за списком із двох
  // символів. Перенесення рядка, яке текст WebSocket пропускає, розривало кадр перед
  // «*CRC», Nanit відкидав обрізок по контрольній сумі, ESP робив дванадцять спроб за
  // ≈2,6 с — і сторінка діставала «без відповіді». Перевіряємо весь рядок, а не саму назву
  // команди: аргументи йдуть у той самий кадр, і раніше вони цю перевірку обходили.
  for (const char *p = text; *p; p++) {
    uint8_t c = (uint8_t)*p;
    if (c < 0x20 || c > 0x7E || c == '$' || c == '*') return;
  }

  char buf[96];
  strlcpy(buf, text, sizeof(buf));

  char *comma = strchr(buf, ',');
  if (!comma) return;
  *comma = 0;
  uint16_t id = atoi(buf);
  char *type = comma + 1;
  char *args = strchr(type, ',');
  if (args) *args++ = 0;

  if (!id || !*type) return;

  // Власні відмови ESP мають ту саму форму, що й NAK від Nanit: «RSP,<id>,NAK,<seq>,<причина>»,
  // де на місці номера команди стоїть «-» — його в цієї відмови немає, бо до Nanit вона й не
  // дійшла. Сторінка читає причину з того самого поля для обох випадків: без «-» вона брала б
  // за причину порожнє місце й показувала людині «Неприпустиме значення» замість того, що
  // керування вимкнене в режимі точки доступу. «-» за відсутнє значення — те саме, що в
  // кадрах протоколу.
  char reply[64];
  if (net.mode == NM_AP) {  // у власній точці доступу сторінка лише налаштовує Wi-Fi
    snprintf(reply, sizeof(reply), "RSP,%u,NAK,-,ap", id);
    wsSendTo(client, reply);
    return;
  }
  if (!nanit.online) {
    snprintf(reply, sizeof(reply), "RSP,%u,NAK,-,offline", id);
    wsSendTo(client, reply);
    return;
  }
  if (!linkSend(type, args, id, client)) {  // busy — черга повна, toolong — не влазить у кадр
    snprintf(reply, sizeof(reply), "RSP,%u,NAK,-,%s", id, linkWhy());
    wsSendTo(client, reply);
  }
}
