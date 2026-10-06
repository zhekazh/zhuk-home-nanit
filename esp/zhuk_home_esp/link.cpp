// Зв'язок із Nanit по UART0 за протоколом docs/protocol.md: кадри з CRC, номери команд,
// повтори, підтвердження подій. Своїх рішень тут немає — лише транспорт.
#include "zhe.h"

NanitState nanit;

// ---------- кадри ----------
// CRC-8/SMBUS, той самий, що на Nanit: поліном 0x07, початкове 0x00. crc8("123456789") = 0xF4
static uint8_t crc8(const char *s, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= (uint8_t)*s++;
    for (uint8_t i = 0; i < 8; i++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

static char hexDigit(uint8_t v) { return v < 10 ? '0' + v : 'A' + v - 10; }

// Кадр іде в порт лише коли він поміщається цілком: інакше Nanit побачить обрізок,
// відкине його по CRC, і команду доведеться повторювати.
static bool writeFrame(const char *body) {
  size_t len = strlen(body);
  if ((size_t)Serial.availableForWrite() < len + 5) return false;
  uint8_t c = crc8(body, len);
  Serial.write('$');
  Serial.write((const uint8_t *)body, len);
  Serial.write('*');
  Serial.write(hexDigit(c >> 4));
  Serial.write(hexDigit(c & 0xF));
  Serial.write('\n');
  return true;
}

// ---------- черга команд ----------
// Одна команда в польоті: Nanit відповідає за 300 мс, а відповіді розрізняються лише
// номером — із кількома одночасними довелось би вгадувати, чий це ACK.
static const uint8_t QMAX = 6;

struct Cmd {
  char body[72];    // тип, номер і аргументи — те, що піде між $ і *
  uint16_t seq;
  uint16_t webId;   // номер запиту сторінки, щоб повернути їй відповідь
  uint8_t client;   // якій саме сторінці: інакше чужі відповіді плутали б її
  uint8_t tries;
  uint32_t firstAt;  // коли пішла перша спроба: вікно повторів обмежене й у часі
};

static Cmd q[QMAX];
static uint8_t qHead, qCount;
static uint32_t sentAt;
static bool inFlight;
static uint16_t seqNext = 1;
static uint32_t pingAt, netAt;
static uint16_t pingSeq;  // номер поточного PING: поки він не пішов, повтор іде з тим самим
static uint8_t netSent;   // який режим мережі Nanit уже знає
static uint16_t ackWant;  // подія, яку треба підтвердити; 0 — нема чого підтверджувати
static uint16_t evLastId;  // остання підтверджена подія: повтори не мусять дублювати журнал
// Чому останній linkSend відмовив. Сторінці різниця потрібна: «зайнято» зникає само за
// секунди, а «задовга команда» не зникне ніколи. Так само окремим словом віддає причину
// staWhy() у net.cpp.
static const char *sendWhy = "busy";

static uint16_t takeSeq() {
  uint16_t s = seqNext;
  seqNext = seqNext == 65535 ? 1 : seqNext + 1;
  return s;
}

// Довжина черги: у прошивці її ніхто не питає, тому зі zhe.h оголошення прибрано, а
// компонувальник цю функцію відкидає (у esp/build/zhuk_home_esp.ino.map вона в розділі
// «Discarded input sections», тобто у прошивці її й не було). Лишається вона заради
// esp/host_test/link_test.cpp: черга — статик цього модуля, і без такого спостерігача
// половину перевірок транспорту довелось би викинути. Тест оголошує її сам.
uint8_t linkQueued() { return qCount; }

const char *linkWhy() { return sendWhy; }

bool linkSend(const char *type, const char *args, uint16_t webId, uint8_t client) {
  if (qCount >= QMAX) {
    logf("link: черга повна, %s втрачено", type);
    sendWhy = "busy";
    return false;
  }
  Cmd &c = q[(qHead + qCount) % QMAX];
  c.seq = takeSeq();
  c.webId = webId;
  c.client = client;
  c.tries = 0;
  int n;
  if (args && *args) n = snprintf(c.body, sizeof(c.body), "%s,%u,%s", type, c.seq, args);
  else n = snprintf(c.body, sizeof(c.body), "%s,%u", type, c.seq);
  // Обрізок пішов би до Nanit із ПРАВИЛЬНИМ CRC — сума рахується вже по обрізку, тож кадр
  // виглядав би цілим, а команда була б іншою. Краще відмовити сторінці одразу.
  if (n < 0 || (size_t)n >= sizeof(c.body)) {
    logf("link: %s не вміщається в кадр, відкинуто", type);
    sendWhy = "toolong";
    return false;
  }
  qCount++;
  return true;
}

static void cmdDone(const char *raw) {
  Cmd &c = q[qHead];
  hubCmdDone(c.webId, c.client, raw);
  qHead = (qHead + 1) % QMAX;
  qCount--;
  inFlight = false;
}

static void queueLoop(uint32_t now) {
  if (!qCount) return;
  Cmd &c = q[qHead];
  if (!inFlight) {
    if (writeFrame(c.body)) {
      c.tries = 1;
      c.firstAt = now;
      sentAt = now;
      inFlight = true;
    }
    return;
  }
  // Повторюємо частіше, ніж обіцяні протоколом 300 мс відповіді: Nanit пам'ятає останні
  // команди й на повтор віддає збережений ACK, не виконуючи дію вдруге. Рідка втрата кадру
  // коштує тоді 150 мс, а не третину секунди.
  //
  // Розкид у паузі принципово важливий: вентилятор глушить лінію й пропускає кадри лише
  // у вікнах тишини, а рівна пауза потрапляла в ту саму фазу вікна — усі спроби
  // промахувалися разом. Випадкові 0–90 мс розмивають фазу, і котрась спроба влучає.
  if (now - sentAt < 150 + (uint32_t)(sentAt % 90)) return;
  // Дванадцять спроб, а не три: поки працює двигун вентилятора, лінія ESP→Nanit шумить,
  // і команда «вимкнути» зобов'язана пробитися — інакше вентилятор лишається крутитися,
  // а вебінтерфейс безсилий. У вікно тишини кадр влучає приблизно в третині випадків,
  // тож дванадцять спроб дають відмову менш ніж раз на сотню команд. Дві з половиною
  // секунди наполегливості дешевші за таку відмову.
  // Межа не лише в спробах, а й у часі. Правило «строк кешу Nanit > спроби × максимальна
  // пауза» (SEEN_TTL 4 с проти 11 пауз ≈ 2,63 с) тримається тільки поки цикл не стоїть — а він
  // стоїть: WiFi.scanNetworks() блокує ~2 с (esp_suspend), повільний WebSocket-клієнт — до 5 с
  // (WEBSOCKETS_TCP_TIMEOUT). Тоді дванадцята спроба приходить уже після строку, слот у кеші
  // Nanit прострочений, і та сама команда виконується ВДРУГЕ: CALDO,gate,step довертає стрілу
  // на другий крок, PIN витрачає другу спробу з трьох і може заблокувати введення на 60 с.
  // Тож кидаємо команду раніше, ніж Nanit забуде її номер: 3,5 с проти строку 4 с.
  if (c.tries >= 12 || now - c.firstAt >= 3500) {
    logf("link: %s без відповіді", c.body);
    cmdDone("TIMEOUT");
    return;
  }
  if (writeFrame(c.body)) {  // повтор іде з тим самим номером: Nanit не виконає команду двічі
    c.tries++;
    sentAt = now;
  }
}

// ---------- прийом ----------
static char rxBuf[FRAME_MAX];
static uint8_t rxLen;
static bool rxActive;

static void handleFrame(char *body, uint8_t len, uint32_t now) {
  char *star = strrchr(body, '*');
  if (!star || body + len - star != 3) {
    nanit.bad++;
    return;
  }
  char *end;
  uint8_t want = strtoul(star + 1, &end, 16);
  if (*end || crc8(body, star - body) != want) {  // пошкоджений кадр: Nanit повторить сам
    nanit.bad++;
    return;
  }
  *star = 0;

  nanit.lastRx = now ? now : 1;
  nanit.frames++;

  // Розбір на поля робимо поверх буфера, але сирий рядок потрібен сторінці цілим
  char raw[FRAME_MAX];
  strlcpy(raw, body, sizeof(raw));

  char *argv[8];
  uint8_t argc = 0;
  char *p = body;
  while (argc < 8) {
    argv[argc++] = p;
    p = strchr(p, ',');
    if (!p) break;
    *p++ = 0;
  }

  if (!strcmp(argv[0], "ACK") || !strcmp(argv[0], "NAK")) {
    if (inFlight && argc >= 2 && (uint16_t)atoi(argv[1]) == q[qHead].seq) cmdDone(raw);
    else hubFrame(argv, argc, raw);  // ACK на PING або на команду, якої ми вже не чекаємо
    return;
  }

  if (!strcmp(argv[0], "EV") && argc >= 2) {
    // Підтвердження мусить дійти: якщо буфер передачі саме зайнятий, одноразова спроба
    // пропала б, і Nanit повторював би подію кожні 500 мс усі десять секунд. Тому
    // запам'ятовуємо номер і надсилаємо його з кожного проходу циклу, доки не вийде.
    uint16_t id = atoi(argv[1]);
    ackWant = id;
    if (id == evLastId) return;  // той самий повтор: у журнал він уже потрапив
    evLastId = id;
  }

  if (!strcmp(argv[0], "HI") && argc >= 5) {
    nanit.proto = atoi(argv[1]);
    strlcpy(nanit.fw, argv[2], sizeof(nanit.fw));
    strlcpy(nanit.board, argv[3], sizeof(nanit.board));
    strlcpy(nanit.reset, argv[4], sizeof(nanit.reset));
    logf("nanit: fw %s, плата %s, старт %s, proto %u", nanit.fw, nanit.board, nanit.reset,
         nanit.proto);
    netSent = 0xFF;  // Nanit перезапустився: розкажемо йому про мережу заново
    timeResend();    // і про час: він у нього тільки в пам'яті
  }

  hubFrame(argv, argc, raw);
}

static void rxLoop(uint32_t now) {
  uint16_t budget = 256;  // за прохід читаємо обмежено, щоб не тримати цикл
  while (budget-- && Serial.available()) {
    char c = Serial.read();
    if (c == '$') {
      rxActive = true;
      rxLen = 0;
    } else if (!rxActive || c == '\r') {
      continue;
    } else if (c == '\n') {
      rxBuf[rxLen] = 0;
      rxActive = false;
      handleFrame(rxBuf, rxLen, now);
    } else if (rxLen >= FRAME_MAX - 2) {
      rxActive = false;  // задовгий кадр
    } else {
      rxBuf[rxLen++] = c;
    }
  }
}

// ---------- інтерфейс модуля ----------
static uint32_t hiAskedAt;

void linkBegin() {
  memset(&nanit, 0, sizeof(nanit));
  strlcpy(nanit.fw, "-", sizeof(nanit.fw));
  netSent = 0xFF;
  hiAskedAt = millis();
  linkSend("GET", "hi", 0, 0);  // якщо Nanit уже працює, його HI ми проспали
}

// Версію Nanit перепитуємо, доки не дізнаємось. Єдиного GET,hi при старті не досить:
// живлення в ESP і Nanit спільне, тож обидва встають разом, і якщо Nanit цієї миті ще не
// відповідає (а після просадки живлення саме так і буває), відповідь не приходить — і
// nanit.fw лишався «-» аж до перезапуску ESP. Сторінка тоді показувала порожню версію в
// кадрі NANIT, і те саме стосується причини старту й номера протоколу, на якому тримається
// запобіжник розділу 12.
static void askHi(uint32_t now) {
  if (!nanit.online || nanit.fw[0] != '-' || now - hiAskedAt < 30000) return;
  hiAskedAt = now;
  linkSend("GET", "hi", 0, 0);
}

void linkLoop(uint32_t now) {
  rxLoop(now);

  if (ackWant) {
    char ack[16];
    snprintf(ack, sizeof(ack), "ACK,%u", ackWant);
    if (writeFrame(ack)) ackWant = 0;
  }

  bool online = nanit.lastRx && now - nanit.lastRx < 3000;
  if (online != nanit.online) {
    nanit.online = online;
    logPrint(online ? "nanit: на зв'язку" : "nanit: не відповідає");
    if (online) netSent = 0xFF;
  }

  // Nanit вважає ESP зниклим через 5 с без кадрів, тому цокаємо щосекунди
  if (now - pingAt >= 1000) {
    // Мітку рухаємо лише коли кадр справді пішов: інакше PING, який не вліз у зайнятий
    // буфер передачі, тихо губився б, і контроль зв'язку був би слабшим, ніж виглядає.
    // Номер при цьому беремо один раз на PING — інакше кожен прохід циклу спалював би
    // по номеру, і лічильник обійшов би 65535 за хвилину тиші в порту.
    if (!pingSeq) pingSeq = takeSeq();
    char b[16];
    snprintf(b, sizeof(b), "PING,%u", pingSeq);
    if (writeFrame(b)) {
      pingAt = now;
      pingSeq = 0;
    }
  }

  // Стан мережі — на екран Nanit: при зміні одразу, інакше раз на 30 с
  if (nanit.online && (netSent != net.mode || now - netAt >= 30000)) {
    const char *mode = net.mode == NM_STA ? "sta" : (net.mode == NM_AP ? "ap" : "off");
    char args[40];
    snprintf(args, sizeof(args), "%s,%d,%s,%s", mode, net.rssi, net.ip[0] ? net.ip : "-",
             net.why[0] ? net.why : "-");
    if (linkSend("NET", args, 0, 0)) {
      netSent = net.mode;
      netAt = now;
    }
  }

  askHi(now);
  queueLoop(now);
}
