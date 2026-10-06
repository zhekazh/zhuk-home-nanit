// Зв'язок з ESP (Serial3) і з комп'ютером (USB, Serial) за протоколом docs/protocol.md.
#include "zh.h"

uint8_t bootResetCause();

// ================= Frame =================
static char hexDigit(uint8_t v) { return v < 10 ? '0' + v : 'A' + v - 10; }

Frame::Frame(const __FlashStringHelper *type) : len_(0) {
  PGM_P p = reinterpret_cast<PGM_P>(type);
  char c;
  while ((c = pgm_read_byte(p++))) put(c);
}

void Frame::put(char c) {
  if (len_ < sizeof(buf_) - 1) buf_[len_++] = c;
}

Frame &Frame::s(const char *v) {
  put(',');
  while (*v) put(*v++);
  return *this;
}

Frame &Frame::f(const __FlashStringHelper *v) {
  put(',');
  PGM_P p = reinterpret_cast<PGM_P>(v);
  char c;
  while ((c = pgm_read_byte(p++))) put(c);
  return *this;
}

Frame &Frame::p(const char *list, uint8_t idx) {
  char tmp[16];
  pgmItem(list, idx, tmp, sizeof(tmp));
  return s(tmp[0] ? tmp : "-");
}

Frame &Frame::n(long v) {
  char tmp[12];
  ltoa(v, tmp, 10);
  return s(tmp);
}

Frame &Frame::dash() { return s("-"); }

Frame &Frame::ch(char c) {
  put(',');
  put(c);
  return *this;
}

Frame &Frame::hex6(uint32_t v) {
  put(',');
  for (int8_t i = 5; i >= 0; i--) put(hexDigit((v >> (i * 4)) & 0xF));
  return *this;
}

uint16_t Frame::hash() const {  // CRC-16/CCITT: 8-бітного хеша не вистачало, кадри «застрягали»
  uint16_t h = 0xFFFF;
  for (uint8_t i = 0; i < len_; i++) {
    h ^= (uint16_t)buf_[i] << 8;
    for (uint8_t b = 0; b < 8; b++) h = (h & 0x8000) ? (h << 1) ^ 0x1021 : h << 1;
  }
  return h;
}

bool Frame::send(uint8_t port, bool force) {
  HardwareSerial &out = (port == PORT_ESP) ? Serial3 : Serial;
  int total = len_ + 5;
  int room = out.availableForWrite();
  if (!force && room < total && room < SERIAL_TX_BUFFER_SIZE - 1) return false;  // довгий кадр — лише в порожній буфер
  uint8_t c = crc8((const uint8_t *)buf_, len_);
  char tail[4] = {'*', hexDigit(c >> 4), hexDigit(c & 0xF), '\n'};
  out.write('$');
  out.write((const uint8_t *)buf_, len_);
  out.write((const uint8_t *)tail, 4);
  if (port == PORT_ESP && sys.dbg && Serial.availableForWrite() >= total) {
    Serial.write('$');
    Serial.write((const uint8_t *)buf_, len_);
    Serial.write((const uint8_t *)tail, 4);
  }
  return true;
}

// ================= події =================
struct Ev {
  uint16_t id;
  uint8_t code;
  int16_t a, b, c;
};
static const uint8_t EV_QUEUE = 8;
static Ev evq[EV_QUEUE];
static uint8_t evHead, evCount;
static uint16_t evNextId = 1, evLost;
static uint32_t evSentAt;
static uint8_t evTries;
static bool evInFlight;

static Frame evFrame(const Ev &e);

void evPush(uint8_t code, int16_t a, int16_t b, int16_t c) {
  if (!espOnline(millis())) {
    // ESP немає кому підтверджувати: у черзі подія висіла б 10 с, потім ставала
    // втраченою, а EV_LOST — новою подією в тій самій черзі, і так без кінця.
    // Тому надсилаємо один раз, без черги: на USB її видно через DBG.
    Ev e{evNextId, code, a, b, c};
    evNextId = evNextId == 65535 ? 1 : evNextId + 1;
    evFrame(e).send(PORT_ESP);
    return;
  }
  if (evCount == EV_QUEUE) {  // черга повна: відкидаємо найстарішу
    evHead = (evHead + 1) % EV_QUEUE;
    evCount--;
    evInFlight = false;
    evTries = 0;
    evLost++;
  }
  Ev &e = evq[(evHead + evCount) % EV_QUEUE];
  e.id = evNextId;
  evNextId = evNextId == 65535 ? 1 : evNextId + 1;
  e.code = code;
  e.a = a;
  e.b = b;
  e.c = c;
  evCount++;
}

static void evAck(uint16_t id) {
  if (evCount && evInFlight && evq[evHead].id == id) {
    evHead = (evHead + 1) % EV_QUEUE;
    evCount--;
    evInFlight = false;
    evTries = 0;
  }
}

static Frame evFrame(const Ev &e) {
  Frame fr(F("EV"));
  fr.n(e.id).p(N_EV, e.code);
  switch (e.code) {
    case EV_BOOT: fr.p(N_RESET, e.a); break;
    case EV_ARMING: fr.p(N_MODE, e.a).p(N_SRC, e.b); break;
    case EV_ARMED: fr.p(N_MODE, e.a); break;
    case EV_DISARM:
    case EV_CANCEL:
    case EV_PIN_SET:
    case EV_CO_MUTE: fr.p(N_SRC, e.a); break;
    case EV_ENTRY:
    case EV_ALARM: fr.p(N_SENSOR, e.a); break;
    case EV_PIN_BAD: fr.p(N_SRC, e.a).n(e.b); break;
    case EV_PIN_LOCK:
    case EV_CO_HIGH:
    case EV_CO_OK:
    case EV_LOST: fr.n(e.a); break;
    case EV_FAN: fr.n(e.a).p(N_FANR, e.b).n(e.c); break;
    case EV_GATE:
      fr.p(N_GATES, e.a).p(N_GATER, e.b);
      if (e.c >= 0) fr.n(e.c);
      else fr.dash();
      break;
    case EV_MODE: fr.p(N_DEV, e.a).p(devModeList(e.a), e.b).p(N_SRC, e.c); break;
    case EV_SVC: fr.n(e.a).p(N_SRC, e.b); break;
    case EV_CFG: {
      char name[12], value[8];
      uint8_t comp;
      if (e.a >= 0 && fieldInfo(e.a, &comp, name, sizeof(name), value, sizeof(value))) fr.s(name);
      else fr.f(F("reset"));
      break;
    }
    case EV_CAL:
    case EV_HW_OK: fr.p(N_COMP, e.a); break;
    case EV_HW_ERR: fr.p(N_COMP, e.a).p(N_HWERR, e.b); break;
    default: break;
  }
  return fr;
}

static void evUpdate(uint32_t now) {
  if (!espOnline(now)) {  // ESP зник: тримати чергу нікому, підтверджень не буде
    evCount = 0;
    evInFlight = false;
    evTries = 0;
    evLost = 0;
    return;
  }
  if (evLost) {  // черга може бути повна саме через мовчазний ESP — звільняємо місце
    if (evCount == EV_QUEUE) {
      evHead = (evHead + 1) % EV_QUEUE;
      evCount--;
      evInFlight = false;
      evTries = 0;
      evLost++;
    }
    uint16_t lost = evLost;
    evLost = 0;
    evPush(EV_LOST, lost > 32767 ? 32767 : lost);
  }
  if (!evCount) return;
  if (evInFlight && now - evSentAt < 500) return;
  // Лічимо справжні відправки, а не проходи циклу. Доти ++evTries стояв тут, а evSentAt
  // оновлювався лише при успішному send(): коли буфер Serial3 зайнятий, кадр не йшов, строк
  // 500 мс лишався простроченим, і лічильник крутився з частотою циклу. Подія «губилася» за
  // 595 мс замість десяти секунд, а ESP отримував EV_LOST на місці ALARM чи DISARM. На ESP
  // та сама логіка від початку написана правильно (esp/zhuk_home_esp/link.cpp).
  if (evTries >= 20) {  // двадцять відправок по 500 мс — 10 с без підтвердження
    evHead = (evHead + 1) % EV_QUEUE;
    evCount--;
    evInFlight = false;
    evTries = 0;
    evLost++;
    return;
  }
  if (evFrame(evq[evHead]).send(PORT_ESP)) {
    evSentAt = now;
    evInFlight = true;
    evTries++;
  }
}

// ================= кадри стану =================
enum : uint8_t { ST_SEC, ST_ENV, ST_DEV, ST_PRK, ST_SYS, ST_COUNT };
static uint32_t stSentAt[ST_COUNT], stCheckAt[ST_COUNT];
static uint16_t stHash[ST_COUNT];
static const bool ST_ON_CHANGE[ST_COUNT] = {true, false, true, true, false};

static Frame buildStatus(uint8_t type, uint32_t now) {
  switch (type) {
    case ST_SEC: {
      Frame fr(F("SEC"));
      fr.p(N_SEC, sec.state);
      if (sec.state == SEC_EXIT || sec.state == SEC_ENTRY || sec.state == SEC_ALARM) fr.p(N_MODE, sec.mode);
      else fr.dash();
      uint32_t left = 0;
      if (sec.state == SEC_EXIT || sec.state == SEC_ENTRY) left = secondsLeft(sec.until, now);
      if (sec.state == SEC_ALARM) left = secondsLeft(sec.sirenUntil, now);
      fr.n(left);
      if (sec.trigger < SN_COUNT) fr.p(N_SENSOR, sec.trigger);
      else fr.dash();
      fr.n(sec.wrong).n(secondsLeft(sec.lockUntil, now));
      return fr;
    }
    case ST_ENV: {
      Frame fr(F("ENV"));
      if (env.dhtOk) fr.n(env.t).n(env.h);
      else fr.dash().dash();
      if (env.coValid) fr.n(env.co);
      else fr.dash();
      // coAlarm окремо від coLvl: тривога знімається з гістерезисом (env.co + 10 < coDanger),
      // а coLvl стає 2 рівно на порозі — у проміжку будинок ще в тривозі, хоч приймач,
      // який виводив стан із coLvl, показував «усе гаразд». coMuted у кадрі, а не лише
      // в події CO_MUTE: інакше перезавантажена сторінка про заглушення не знає.
      fr.n(env.coRaw).n(env.ldr).n(env.bright).n(env.coLvl).n(env.coAlarm).n(env.coMuted);
      return fr;
    }
    case ST_DEV: {
      Frame fr(F("DEV"));
      fr.p(N_ONOFF, dev.fanMode).n(dev.fanOn).p(N_WINM, dev.winMode).n(curtainPct()).n(dev.winTarget);
      fr.n(dev.winBlocked).p(N_ONOFF, dev.lightMode).n(dev.lightOn).hex6(dev.color).n(dev.level);
      return fr;
    }
    case ST_PRK: {
      Frame fr(F("PRK"));
      fr.p(N_GATEM, prk.gateMode).p(N_GATES, prk.gateState);
      if (prk.dist >= 0) fr.n(prk.dist);
      else fr.dash();
      fr.ch(prk.tl ? prk.tl : '-');
      char seg[5];
      for (uint8_t i = 0; i < 4; i++) seg[i] = prk.seg[i] == ' ' || !prk.seg[i] ? '_' : prk.seg[i];
      seg[4] = 0;
      fr.s(seg);
      return fr;
    }
    default: {
      Frame fr(F("SYS"));
      fr.n(now / 1000).n(sys.ram).n(sys.loopMax).n(env.batt).n(env.mV);
      fr.n(sys.service ? secondsLeft(sys.svcUntil, now) : 0);
      return fr;
    }
  }
}

static void statusUpdate(uint32_t now) {
  for (uint8_t i = 0; i < ST_COUNT; i++) {
    bool periodic = now - stSentAt[i] >= 1000;
    bool check = ST_ON_CHANGE[i] && now - stSentAt[i] >= 100 && !pending(stCheckAt[i], now);
    if (!periodic && !check) continue;
    Frame fr = buildStatus(i, now);
    uint16_t h = fr.hash();
    if (!periodic && h == stHash[i]) {
      stCheckAt[i] = now + 100;
      continue;
    }
    if (fr.send(PORT_ESP)) {
      stSentAt[i] = now;
      stHash[i] = h;
    }
  }
}

static void sendHello(uint8_t port) {
  Frame(F("HI")).n(ZH_PROTO).f(F(ZH_FW_VERSION)).f(pins.v31 ? F("3.1") : F("2.0")).p(N_RESET, bootResetCause()).send(port, true);
}

// ================= відповіді й захист від повторів =================
struct Resp {
  uint8_t reason;  // Nak; RESP_LATER — відповідь надішле список
  int16_t detail;  // -1 — без деталі
  char key[12];
};
static const uint8_t RESP_LATER = 0xFE;

static Resp respOk() { return Resp{NAK_NONE, -1, ""}; }
static Resp respNak(uint8_t reason, int16_t detail = -1) { return Resp{reason, detail, ""}; }
static Resp respNakKey(uint8_t reason, const char *key) {
  Resp r{reason, -1, ""};
  strlcpy(r.key, key, sizeof(r.key));
  return r;
}

static void sendResp(uint8_t port, uint16_t seq, const Resp &r) {
  if (r.reason == NAK_NONE || r.reason == RESP_LATER) {
    Frame(F("ACK")).n(seq).send(port, true);
    return;
  }
  Frame fr(F("NAK"));
  fr.n(seq).p(N_NAK, r.reason);
  if (r.key[0]) fr.s(r.key);
  else if (r.detail >= 0) fr.n(r.detail);
  fr.send(port, true);
}

struct Seen {
  uint32_t at;        // коли команду виконано: давній номер — це вже нова команда, а не повтор
  uint16_t seq;
  uint8_t port, crc;  // crc тіла кадру: той самий номер з іншою командою — це нова команда
  Resp r;
};
static const uint8_t SEEN_SLOTS = 8;  // PING слота не займає (див. dispatch), тож це вісім справжніх команд
// Строк кешу мусить перевищувати все вікно повторів ESP: спроби × максимальна пауза, тобто
// 12 × 239 мс ≈ 2,87 с (самих пауз 11, ≈2,63 с). Інакше остання спроба приходила б уже після
// строку й виконувалася вдруге: повторний CALDO,gate,step довернув би стрілу двічі, а
// повторний PIN витратив би спробу з трьох і міг заблокувати введення на 60 с. Сам строк
// потрібен тому, що після перезапуску ESP лічильник починається з 1, і без давності ті самі
// номери отримували б старі відповіді без виконання команди.
static const uint16_t SEEN_TTL = 4000;
static Seen seen[SEEN_SLOTS];
static uint8_t seenNext;

// ================= списки VAL і HW =================
enum : uint8_t { LIST_NONE, LIST_CFG, LIST_CAL, LIST_HW, LIST_SCAN };
static struct {
  uint8_t kind, port, comp, idx, crc;
  uint16_t seq;
} list;

static void listUpdate() {
  if (list.kind == LIST_NONE) return;
  if (list.kind == LIST_SCAN) {
    if (list.idx < 12) {  // рівні пінів читаємо, не чіпаючи режим роботи пінів
      Frame fr(F("SC"));
      fr.n(list.idx + 1);
      for (uint8_t i = 0; i < 4; i++) {
        uint8_t pin = portPins[list.idx][i];
        if (pin == 0xFF) fr.dash();
        else fr.n(digitalRead(pin));
      }
      if (fr.send(list.port)) list.idx++;
      return;
    }
  } else if (list.kind == LIST_HW) {
    if (list.idx < C_COUNT) {
      if (hwFrame(list.idx, list.port, false)) list.idx++;
      return;
    }
  } else {
    while (list.idx < fieldCount()) {
      uint8_t comp;
      char name[12], value[8];
      fieldInfo(list.idx, &comp, name, sizeof(name), value, sizeof(value));
      bool wanted = list.kind == LIST_CFG ? comp == 0xFF : comp == list.comp;
      if (!wanted) {
        list.idx++;
        continue;
      }
      Frame fr(F("VAL"));
      if (comp == 0xFF) fr.f(F("cfg"));
      else fr.p(N_COMP, comp);
      if (fr.s(name).s(value).send(list.port)) list.idx++;
      return;
    }
  }
  Frame(F("ACK")).n(list.seq).send(list.port, true);
  list.kind = LIST_NONE;
}

// ================= команди =================
static bool is(const char *a, const char *pgm) { return strcmp_P(a, pgm) == 0; }

static bool parseHex6(const char *s, uint32_t *out) {
  if (strlen(s) != 6) return false;
  for (uint8_t i = 0; i < 6; i++)
    if (!isxdigit((unsigned char)s[i])) return false;  // «-», «+» і «0x» тут недоречні
  *out = strtoul(s, nullptr, 16);
  return true;
}

static Resp execute(uint8_t port, uint16_t seq, uint8_t argc, char **argv, uint8_t bodyCrc) {
  const char *t = argv[0];
  const char *a2 = argc > 2 ? argv[2] : "";
  const char *a3 = argc > 3 ? argv[3] : nullptr;
  int16_t detail = -1;
  long v;

  if (is(t, PSTR("PING"))) return respOk();

  if (is(t, PSTR("GET"))) {
    if (is(a2, PSTR("hi"))) {
      sendHello(port);
      return respOk();
    }
    if (list.kind != LIST_NONE) return respNak(NAK_BUSY);
    list.port = port;
    list.seq = seq;
    list.crc = bodyCrc;
    list.idx = 0;
    if (is(a2, PSTR("cfg"))) list.kind = LIST_CFG;
    else if (is(a2, PSTR("hw"))) list.kind = LIST_HW;
    else if (is(a2, PSTR("cal")) && a3) {
      int8_t comp = pgmFind(N_COMP, a3);
      if (comp < 0) return respNak(NAK_ARG);
      list.comp = comp;
      list.kind = LIST_CAL;
    } else {
      return respNak(NAK_ARG);
    }
    return Resp{RESP_LATER, -1, ""};
  }

  if (is(t, PSTR("MODE"))) {
    int8_t d = pgmFind(N_DEV, a2);
    if (d < 0 || !a3) return respNak(NAK_ARG);
    return respNak(autoMode(d, a3, SRC_WEB));
  }
  if (is(t, PSTR("WIN"))) {
    if (!parseNum(a2, 0, 100, &v)) return respNak(NAK_ARG);
    return respNak(autoWin(v, SRC_WEB));
  }
  if (is(t, PSTR("LIGHT"))) {
    uint32_t color;
    if (!parseHex6(a2, &color) || !parseNum(a3, 10, 100, &v)) return respNak(NAK_ARG);
    return respNak(autoLight(color, v));
  }
  if (is(t, PSTR("ARM"))) {
    int8_t m = pgmFind(N_MODE, a2);
    if (m != SM_ARM && m != SM_NIGHT) return respNak(NAK_ARG);
    uint8_t r = secArm(m, SRC_WEB, a3, &detail);
    return respNak(r, detail);
  }
  if (is(t, PSTR("DISARM"))) {
    if (argc < 3) return respNak(NAK_ARG);
    uint8_t r = secDisarm(SRC_WEB, a2, &detail);
    return respNak(r, detail);
  }
  if (is(t, PSTR("SCENE"))) {
    int8_t sc = pgmFind(N_SCENE, a2);
    if (sc < 0) return respNak(NAK_ARG);
    uint8_t r = autoScene(sc, a3, &detail);
    return respNak(r, detail);
  }
  if (is(t, PSTR("MUTE"))) return respNak(autoMute(SRC_WEB));
  if (is(t, PSTR("SET"))) {
    if (!a3) return respNak(NAK_ARG);
    uint8_t r = cfgSet(a2, a3);
    return r ? respNakKey(r, a2) : respOk();
  }
  if (is(t, PSTR("PIN"))) {
    if (!a3) return respNak(NAK_ARG);
    uint8_t r = secChangePin(a2, a3, &detail);
    return respNak(r, detail);
  }
  if (is(t, PSTR("SVC"))) {
    if (!parseNum(a2, 0, 1, &v)) return respNak(NAK_ARG);
    return respNak(svcSet(v, SRC_WEB));
  }
  if (is(t, PSTR("TEST"))) {
    int8_t comp = pgmFind(N_COMP, a2);
    if (comp < 0 || !a3) return respNak(NAK_ARG);
    return respNak(testStart(port, seq, comp, a3));
  }
  if (is(t, PSTR("CAL"))) {
    int8_t comp = pgmFind(N_COMP, a2);
    if (comp < 0 || argc < 5) return respNak(NAK_ARG);
    uint8_t r = calSet(comp, argv[3], argv[4]);
    return r ? respNakKey(r, argv[3]) : respOk();
  }
  if (is(t, PSTR("CALDO"))) {
    int8_t comp = pgmFind(N_COMP, a2);
    if (comp < 0 || !a3) return respNak(NAK_ARG);
    return respNak(calDo(comp, a3, argc > 4 ? argv[4] : nullptr));
  }
  if (is(t, PSTR("WATCH"))) {
    if (!parseNum(a2, 0, 1, &v)) return respNak(NAK_ARG);
    diagWatch(v, port);
    return respOk();
  }
  if (is(t, PSTR("TIME"))) {
    long tz, epoch;
    if (!parseNum(a2, 0, 2147483647L, &epoch) || !parseNum(a3, -720, 840, &tz)) return respNak(NAK_ARG);
    sys.unixBase = epoch;
    sys.unixAt = millis();
    sys.tzMin = tz;
    sys.timeValid = true;
    return respOk();
  }
  if (is(t, PSTR("NET"))) {
    int8_t st = pgmFind(N_NET, a2);
    if (st < 0 || argc < 5 || !parseNum(argv[3], -127, 0, &v)) return respNak(NAK_ARG);
    if (strlen(argv[4]) >= sizeof(sys.ip)) return respNak(NAK_ARG);  // не ховаємо обрізану адресу
    sys.net = st;
    sys.rssi = v;
    strlcpy(sys.ip, argv[4], sizeof(sys.ip));
    return respOk();
  }
  if (is(t, PSTR("SCAN")) && port == PORT_USB) {  // діагностика підключення модулів
    if (list.kind != LIST_NONE) return respNak(NAK_BUSY);
    list.port = port;
    list.seq = seq;
    list.crc = bodyCrc;
    list.idx = 0;
    list.kind = LIST_SCAN;
    return Resp{RESP_LATER, -1, ""};
  }
  if (is(t, PSTR("DBG")) && port == PORT_USB) {
    if (!parseNum(a2, 0, 2, &v)) return respNak(NAK_ARG);
    sys.dbg = v;
    return respOk();
  }
  return respNak(NAK_TYPE);
}

static void dispatch(uint8_t port, uint8_t argc, char **argv, uint32_t now, uint8_t bodyCrc) {
  if (port == PORT_ESP) sys.lastEspRx = now;
  if (is(argv[0], PSTR("ACK"))) {  // підтвердження події: лише від ESP, інакше USB-монітор
    long id;                        // міг би зняти з черги подію, якої ESP не бачив
    if (port == PORT_ESP && argc >= 2 && parseNum(argv[1], 1, 65535, &id)) evAck(id);
    return;
  }
  long seqL;
  if (argc < 2 || !parseNum(argv[1], 1, 65535, &seqL)) return;
  uint16_t seq = seqL;
  // Повтор того самого GET, поки список ще передається: мовчимо, ACK піде в кінці
  if (list.kind != LIST_NONE && list.seq == seq && list.port == port && list.crc == bodyCrc) return;
  uint32_t nowMs = millis();
  for (Seen &s : seen) {
    if (s.seq == seq && s.port == port && s.crc == bodyCrc &&
        nowMs - s.at <= SEEN_TTL) {  // повтор тієї самої команди
      sendResp(port, seq, s.r);
      return;
    }
  }
  Resp r = execute(port, seq, argc, argv, bodyCrc);
  // PING слота не займає: він чистий, і повторне виконання — той самий ACK. Інакше
  // щосекундний PING від ESP лишав би на історію лише вісім секунд, і пачка з шести SET
  // (збереження форми) витісняла б ранню команду ще в межах її строку.
  if (!is(argv[0], PSTR("PING"))) {
    Seen &slot = seen[seenNext];
    seenNext = (seenNext + 1) % SEEN_SLOTS;
    slot.at = nowMs;
    slot.seq = seq;
    slot.port = port;
    slot.crc = bodyCrc;
    slot.r = r;
  }
  if (r.reason != RESP_LATER) sendResp(port, seq, r);
}

// ================= прийом =================
struct Rx {
  char buf[FRAME_MAX];
  uint8_t len;
  bool active;
};
static Rx rx[2];

// Кадр, надісланий ESP, — на USB із префіксом «<». Без цього видно лише наш бік розмови.
static void echoFrame(const char *body) {
  uint8_t n = strlen(body);
  if (Serial.availableForWrite() < n + 4) return;
  Serial.write('<');
  Serial.write(' ');
  Serial.write((const uint8_t *)body, n);
  Serial.write('\n');
}

static void handleLine(uint8_t port, char *line, uint8_t len, uint32_t now) {
  char *star = strrchr(line, '*');
  if (star) {
    if (line + len - star != 3) return;
    char *end;
    uint8_t want = strtoul(star + 1, &end, 16);
    if (*end || crc8((const uint8_t *)line, star - line) != want) return;  // пошкоджений кадр
    *star = 0;
  } else if (port != PORT_USB) {
    return;  // без CRC приймаємо лише з USB
  }
  uint8_t bodyCrc = crc8((const uint8_t *)line, strlen(line));
  char *argv[8];
  uint8_t argc = 0;
  char *p = line;
  while (argc < 8) {
    argv[argc++] = p;
    p = strchr(p, ',');
    if (!p) break;
    *p++ = 0;
  }
  if (!argv[0][0]) return;
  if (port == PORT_ESP && sys.dbg >= 2) {
    char copy[FRAME_MAX];  // dispatch уже порізав рядок нулями, тож збираємо назад
    uint8_t n = 0;
    for (uint8_t i = 0; i < argc && n < sizeof(copy) - 1; i++) {
      if (i) copy[n++] = ',';
      for (const char *q = argv[i]; *q && n < sizeof(copy) - 1; q++) copy[n++] = *q;
    }
    copy[n] = 0;
    echoFrame(copy);
  }
  dispatch(port, argc, argv, now, bodyCrc);
}

// Текст, який ESP пише поза кадрами (його журнал і аварійний дамп при збої), — на USB.
// Свій монітор порту ESP має лише на GPIO2, тож без цього дзеркала його не видно взагалі.
static bool echoOpen;

static void echoByte(char c) {
  if (c == '\n') {
    if (echoOpen) {
      Serial.write('\n');
      echoOpen = false;
    }
    return;
  }
  if (Serial.availableForWrite() < 16) return;  // через чужий текст буфера не чекаємо
  if (!echoOpen) {
    Serial.write('~');
    Serial.write(' ');
    echoOpen = true;
  }
  Serial.write(c >= ' ' && c < 127 ? c : '.');  // сміття бутлоадера ESP не псує вивід
}

static void pollPort(uint8_t port, uint32_t now) {
  HardwareSerial &in = (port == PORT_ESP) ? Serial3 : Serial;
  Rx &r = rx[port];
  uint8_t budget = 64;
  while (budget-- && in.available()) {
    char c = in.read();
    if (c == '$') {
      r.active = true;
      r.len = 0;
      if (echoOpen) echoByte('\n');
    } else if (!r.active) {
      if (port == PORT_ESP && sys.dbg >= 2) echoByte(c);
      continue;
    } else if (c == '\r') {
      continue;
    } else if (c == '\n') {
      r.buf[r.len] = 0;
      r.active = false;
      handleLine(port, r.buf, r.len, now);
    } else if (r.len >= FRAME_MAX - 2) {
      r.active = false;  // задовгий кадр
    } else {
      r.buf[r.len++] = c;
    }
  }
}

// ================= інтерфейс модуля =================
static uint32_t helloAt;

void linkBegin() {
  // Розводимо кадри стану в часі, щоб вони не йшли одним пакетом раз на секунду
  for (uint8_t i = 0; i < ST_COUNT; i++) stSentAt[i] = millis() - 1000 + (uint32_t)i * 200;
  helloAt = millis() + 300;
  evPush(EV_BOOT, bootResetCause());
}

void linkPoll(uint32_t now) {
  pollPort(PORT_ESP, now);
  pollPort(PORT_USB, now);
}

void linkUpdate(uint32_t now) {
  linkPoll(now);
  if (helloAt && !pending(helloAt, now)) {
    helloAt = 0;
    sendHello(PORT_ESP);
  }
  evUpdate(now);
  listUpdate();
  statusUpdate(now);
}

bool espOnline(uint32_t now) { return sys.lastEspRx && now - sys.lastEspRx < 5000; }
