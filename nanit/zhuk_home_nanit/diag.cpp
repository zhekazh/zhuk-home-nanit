// Діагностика: сервісний режим, перевірки модулів (TEST → TR), стан модулів (HW),
// спостереження за сирими сигналами (WATCH → IO, HW, KEY), статистика циклу.
#include "zh.h"

// ================= стан модулів =================
static uint32_t hwOkAt[C_COUNT];
static uint8_t hwErrs[C_COUNT];
static uint16_t hwBad;  // біт на модуль: зараз несправний

void hwOk(uint8_t comp) {
  hwOkAt[comp] = millis() | 1;  // 0 означає «ще не працював»
  if (hwBad & (1u << comp)) {
    hwBad &= ~(1u << comp);
    evPush(EV_HW_OK, comp);
  }
}

void hwFail(uint8_t comp, uint8_t code) {
  if (hwErrs[comp] < 255) hwErrs[comp]++;
  if (!(hwBad & (1u << comp))) {
    hwBad |= 1u << comp;
    evPush(EV_HW_ERR, comp, code);
  }
}

bool hwFrame(uint8_t comp, uint8_t port, bool force) {
  uint8_t st = (hwBad & (1u << comp)) ? HW_ERR : hwOkAt[comp] ? HW_OK : HW_NONE;
  Frame fr(F("HW"));
  fr.p(N_COMP, comp).p(N_HWST, st).n(hwErrs[comp]);
  if (hwOkAt[comp]) {
    uint32_t ago = millis() - hwOkAt[comp];  // мітку зсунуто на 1 мс, тож буває «майбутнє»
    fr.n((int32_t)ago < 0 ? 0 : ago / 1000);
  } else {
    fr.dash();
  }
  return fr.send(port, force);
}

// ================= сервісний режим =================
static void testAbort();

uint8_t svcSet(bool on, uint8_t src) {
  if (on == sys.service) {
    if (on) sys.svcUntil = millis() + 600000UL;  // повторне ввімкнення продовжує режим
    return NAK_NONE;
  }
  if (on && sec.state != SEC_DIS) return NAK_STATE;
  sys.service = on;
  if (on) {
    sys.svcUntil = millis() + 600000UL;
    ovr.curtain = curtainPct();
  } else {
    ovr.fanUntil = ovr.tlUntil = ovr.rgbUntil = ovr.segUntil = 0;
    testAbort();  // перевірка, що потребує сервісного режиму, далі не має сенсу
  }
  evPush(EV_SVC, on, src);
  return NAK_NONE;
}

// ================= перевірки =================
static const char A_BOARD[] PROGMEM = "screen";
static const char A_SERVO[] PROGMEM = "close|half|open";
static const char A_RUN[] PROGMEM = "run";
static const char A_GATE[] PROGMEM = "open|close";
static const char A_TL[] PROGMEM = "r|y|g|cycle";
static const char A_RGB[] PROGMEM = "r|g|b";
static const char A_WAIT[] PROGMEM = "wait";
static const char A_SOUND[] PROGMEM = "wait|listen";
static const char A_BUZ[] PROGMEM = "beep|melody|siren|pair";
static const char A_READ[] PROGMEM = "read";
static const char A_TM[] PROGMEM = "all|stop";

static const char *actionsFor(uint8_t comp) {
  switch (comp) {
    case C_BOARD: return A_BOARD;
    case C_SERVO: return A_SERVO;
    case C_FAN: return A_RUN;
    case C_GATE: return A_GATE;
    case C_TLIGHT: return A_TL;
    case C_RGB: return A_RGB;
    case C_SOUND: return A_SOUND;
    case C_PIR:
    case C_WSENS:
    case C_LDR: return A_WAIT;
    case C_BUZZER: return A_BUZ;
    case C_DHT:
    case C_MQ7:
    case C_SONAR: return A_READ;
    case C_TM1637: return A_TM;
    default: return nullptr;  // клавіатура перевіряється через KEY
  }
}

static struct {
  bool active, needSvc, needDis;  // чого перевірка вимагала на старті: сервісного режиму, знятої охорони
  uint8_t comp, action, port, phase;
  uint16_t seq, v;
  uint32_t start, deadline;
} test;

// ---------- прослуховування мікрофона ----------
// Штатно пін мікрофона читається раз на цикл, тобто приблизно раз на 4 мс, і цього
// не досить: зміряно на макеті, що плескіт дає лише 2–3 імпульси по 100–200 мкс кожен
// (менше десятої відсотка часу), тож штатне опитування ловить його з шансом близько 5 %.
// Тут пін читається тісним циклом — 3 мс за кожен цикл прошивки, близько 78 000 відліків
// за секунду (буфер Serial3 — 64 Б, на 38400 це 16 мс, тож канал до ESP не страждає).
static struct {
  uint32_t samples, activeSamples;
  uint16_t edges;
  bool prev;
} snd;

// Власна мітка початку: «now» у циклі беруть до розбору команди, тож момент старту
// перевірки буває пізнішим за нього, і різниця як беззнакова стає величезною —
// слухання завершувалося на першому ж проході, не набравши й тисячі відліків
static uint32_t listenStart;

static void listenReset() {
  snd = {0, 0, 0, false};
  listenStart = millis();
}

static void listenTick() {
  uint32_t t0 = micros();
  while (micros() - t0 < 3000) {
    bool a = cal.soundInv ? digitalRead(pins.sound) : !digitalRead(pins.sound);
    if (a) {
      snd.activeSamples++;
      if (!snd.prev) snd.edges++;
    }
    snd.prev = a;
    snd.samples++;
  }
}

// Частка часу, коли вихід був активний, у сотих відсотка (0..10000). Саму частку, а не
// лише фронти: плескіт дає 2–3 фронти по 100–200 мкс, тобто менше десятої відсотка,
// і в десятих відсотка таке вимірювання виглядало б як рівний нуль
static uint16_t listenBp() {
  return snd.samples ? (uint16_t)(snd.activeSamples * 10000UL / snd.samples) : 0;
}

static Frame trFrame(const __FlashStringHelper *result) {
  Frame fr(F("TR"));
  fr.n(test.seq).p(N_COMP, test.comp).f(result);
  return fr;
}

static void testDone(Frame &fr, bool ok) {
  fr.send(test.port, true);
  if (ok) hwOk(test.comp);
  test.active = false;
}

static void testFailState() {
  Frame fr = trFrame(F("fail"));
  fr.f(F("state"));
  testDone(fr, false);
}

static void testAbort() {  // сервісний режим вимкнено
  if (test.active && test.needSvc) testFailState();
}

// Перевірки датчиків дозволені лише в стані DIS саме щоб не ввімкнути тривогу. Постановка
// під охорону під час 15-секундного очікування знімала цей захист: людина махає рукою,
// secSensor() дає ENTRY або ALARM, а перевірка ще й звітує «ok».
void diagArming() {
  if (test.active && test.needDis) testFailState();
}

uint8_t testStart(uint8_t port, uint16_t seq, uint8_t comp, const char *action) {
  const char *list = actionsFor(comp);
  int8_t a = list ? pgmFind(list, action) : -1;
  if (a < 0) return NAK_ARG;
  bool needService = comp == C_SERVO || comp == C_FAN || comp == C_GATE || comp == C_TLIGHT || comp == C_RGB;
  bool needDisarmed = comp == C_PIR || comp == C_SOUND || comp == C_WSENS;
  if ((needService && !sys.service) || (needDisarmed && sec.state != SEC_DIS)) return NAK_STATE;
  if (test.active) return NAK_BUSY;
  uint32_t now = millis();
  // Тайм-аут рахуємо з калібрування: повільне серво чи дрібний крок шлагбаума
  // їдуть довго, і це не привід називати справний модуль несправним
  uint32_t limit = 15000;
  if (comp == C_SERVO) limit = 3000 + 180000UL / max(1, (int)cal.servoSpeed);
  else if (comp == C_GATE) limit = 3000 + (uint32_t)cal.gateSteps * cal.gateDelay * 2;
  test = {true, needService, needDisarmed, comp, (uint8_t)a, port, 0, seq, 0, now, now + limit};
  return NAK_NONE;
}

static void testUpdate(uint32_t now) {
  if (!test.active) return;
  uint32_t el = now - test.start;
  // «listen» — не перевірка справності, а вимірювання: скільком імпульсам мікрофона
  // прошивка встигає бути свідком. Зручно й для потенціометра: у тиші має бути нуль
  if (test.comp == C_SOUND && test.action == 1) {
    if (test.phase == 0) {
      listenReset();
      test.v = soundEdgesTotal();  // від чого рахувати фронти переривання
      test.phase = 1;
    }
    listenTick();
    if (millis() - listenStart >= 1000) {
      Frame fr = trFrame(F("ok"));
      // Останнє поле — фронти, які зловило переривання: пряме порівняння з тісним циклом
      fr.n(snd.edges).n(listenBp()).n((uint16_t)(snd.samples / 100)).n(soundEdgesTotal() - test.v).n(soundTrips());
      testDone(fr, true);
    }
    return;
  }
  switch (test.comp) {
    case C_BOARD: {
      dispTestPattern(3000);
      Frame fr = trFrame(F("ok"));
      testDone(fr, true);
      break;
    }
    case C_SERVO: {
      if (test.phase == 0) {
        ovr.curtain = test.action * 50;  // close / half / open; автоматика застосує в наступному циклі
        test.phase = 1;
      } else if (!curtainMoving()) {
        Frame fr = trFrame(F("ok"));
        fr.n(curtainAngle());
        testDone(fr, true);
      } else if (!pending(test.deadline, now)) {
        Frame fr = trFrame(F("fail"));
        fr.f(F("timeout"));
        testDone(fr, false);
      }
      break;
    }
    case C_FAN:
      if (test.phase == 0) {
        ovr.fanUntil = now + 3000;
        test.phase = 1;
      } else if (el >= 3000) {
        Frame fr = trFrame(F("ok"));
        fr.n(3000);
        testDone(fr, true);
      }
      break;
    case C_GATE:
      if (test.phase == 0) {
        test.v = autoGateTest(test.action == 0);
        test.phase = 1;
      }
      if (gateState() == GS_OPEN || gateState() == GS_CLOSED) {
        Frame fr = trFrame(F("ok"));
        fr.n(test.v);
        testDone(fr, true);
      } else if (!pending(test.deadline, now)) {
        Frame fr = trFrame(F("fail"));
        fr.f(F("timeout"));
        testDone(fr, false);
      }
      break;
    case C_TLIGHT: {
      static const char TL[] = {'r', 'y', 'g'};
      if (test.action < 3) {
        ovr.tl = TL[test.action];
        ovr.tlUntil = now + 2500;
      } else if (el < 2400) {  // по черзі, по 800 мс
        ovr.tl = TL[el / 800];
        ovr.tlUntil = now + 100;
        break;
      }
      Frame fr = trFrame(F("ok"));
      testDone(fr, true);
      break;
    }
    case C_RGB: {
      static const uint32_t RGB[] = {0xFF0000UL, 0x00FF00UL, 0x0000FFUL};
      ovr.rgb = RGB[test.action];
      ovr.rgbUntil = now + 2500;
      Frame fr = trFrame(F("ok"));
      testDone(fr, true);
      break;
    }
    case C_PIR:
    case C_SOUND:
    case C_WSENS:
    case C_LDR: {
      if (test.phase == 0) {
        Frame fr = trFrame(F("wait"));
        fr.n(15).send(test.port, true);
        test.v = env.ldr;
        test.phase = 1;
        break;
      }
      uint8_t sn = test.comp == C_PIR ? SN_PIR : test.comp == C_SOUND ? SN_SOUND : SN_WSENS;
      bool hit = test.comp == C_LDR ? abs((int16_t)env.ldr - (int16_t)test.v) > 150
                                    : (int32_t)(io.edgeAt[sn] - test.start) > 0;
      if (hit) {
        Frame fr = trFrame(F("ok"));
        if (test.comp == C_LDR) fr.n(test.v).n(env.ldr);
        else fr.n(io.edgeAt[sn] - test.start);
        testDone(fr, true);
      } else if (el > 15000) {
        Frame fr = trFrame(F("fail"));
        fr.f(F("timeout"));
        testDone(fr, false);
      }
      break;
    }
    case C_BUZZER: {
      // «pair» — зумер грає, а мікрофон слухає: єдина пара модулів макета, яка може
      // підтвердити одне одного без людини. Слухаємо тісним циклом, бо штатного
      // опитування раз на 4 мс не досить, щоб зловити імпульси від тону
      if (test.action == 3) {
        if (test.phase == 0) {
          listenReset();
          test.v = soundEdgesTotal();
          buzzerSirenTest(600);  // найгучніше, що вміє зумер: тон нижчої гучності мікрофон не бере
          test.phase = 1;
        }
        listenTick();
        if (millis() - listenStart >= 700) {
          uint16_t isrEdges = soundEdgesTotal() - test.v;
          bool heard = snd.edges > 0 || isrEdges > 0;
          Frame fr = trFrame(heard ? F("ok") : F("fail"));
          if (heard) fr.n(snd.edges).n(listenBp()).n(isrEdges);
          else fr.f(F("silent"));
          testDone(fr, heard);
        }
        break;
      }
      if (test.action == 0) buzzerPlay(BZ_KEY);
      else if (test.action == 1) buzzerMelody();
      else buzzerSirenTest(1000);
      Frame fr = trFrame(F("ok"));
      testDone(fr, true);
      break;
    }
    case C_DHT:
      if (test.phase == 0) {
        test.v = dhtResults();
        dhtForce();
        test.phase = 1;
      } else if (dhtResults() != test.v) {
        uint8_t err = dhtLastError();
        if (!err) {
          Frame fr = trFrame(F("ok"));
          fr.n(env.t).n(env.h).n(el);
          testDone(fr, true);
        } else {
          Frame fr = trFrame(F("fail"));
          fr.f(err == 1 ? F("timeout") : F("crc"));
          testDone(fr, false);
        }
      } else if (el > 3000) {
        Frame fr = trFrame(F("fail"));
        fr.f(F("timeout"));
        testDone(fr, false);
      }
      break;
    case C_MQ7: {
      Frame fr = trFrame(F("ok"));
      fr.n(env.coRaw).n((uint32_t)env.coRaw * 5000 / 1023);
      testDone(fr, true);
      break;
    }
    case C_SONAR:
      if (prk.dist < 0) {
        Frame fr = trFrame(F("fail"));
        fr.f(F("noecho"));
        testDone(fr, false);
      } else {
        Frame fr = trFrame(F("ok"));
        fr.n(prk.dist).n(prk.samples).n(prk.spread);
        testDone(fr, true);
      }
      break;
    case C_TM1637:
      if (test.phase == 0) {
        strcpy_P(ovr.seg, test.action == 0 ? PSTR("8888") : PSTR("StOP"));
        ovr.segUntil = now + 3000;
        tmForce();  // інакше той самий напис міг би не дійти до дисплея
        test.phase = 1;
      } else {  // автоматика вже записала напис на дисплей
        bool ok = tmAck();
        Frame fr = trFrame(ok ? F("ok") : F("fail"));
        if (!ok) fr.f(F("nack"));
        testDone(fr, ok);
      }
      break;
    default:
      test.active = false;
      break;
  }
}

uint8_t calDo(uint8_t comp, const char *action, const char *arg) {
  if (comp == C_GATE && !strcmp_P(action, PSTR("home"))) {
    if (!sys.service) return NAK_STATE;
    gateHome();
  } else if (comp == C_GATE && !strcmp_P(action, PSTR("step"))) {
    long d;
    if (!sys.service) return NAK_STATE;
    if (!arg || !parseNum(arg, -2048, 2048, &d)) return NAK_ARG;
    gateNudge(d);
    return NAK_NONE;  // позицію пише сам gateNudge, калібрування не змінилось
  } else if (comp == C_MQ7 && !strcmp_P(action, PSTR("base"))) {
    cal.mqBase = env.coRaw;
  } else if (comp == C_LDR && !strcmp_P(action, PSTR("take"))) {
    cal.ldrTh = env.ldr;
  } else {
    return NAK_ARG;
  }
  settingsTouch();
  evPush(EV_CAL, comp);
  return NAK_NONE;
}

// ================= спостереження (WATCH) =================
static uint32_t watchUntil, ioSentAt, hwSentAt;
static uint8_t ioLast = 0xFF, hwNext, watchPort = PORT_ESP;

void diagWatch(bool on, uint8_t port) {
  watchUntil = on ? millis() + 10000UL : 0;
  watchPort = port;  // діагностика йде тому порту, що її попросив, а не завжди на ESP
}

void diagKey(char k) {
  if (pending(watchUntil, millis())) {
    char s[2] = {k == '*' ? '_' : k, 0};  // «*» розділяє кадр і CRC, тому передаємо як «_»
    Frame(F("KEY")).s(s).send(watchPort, true);
  }
}

static void watchUpdate(uint32_t now) {
  if (!pending(watchUntil, now)) return;
  uint8_t bits = io.pir | (io.sound << 1) | (io.wsens << 2);
  if (bits != ioLast || now - ioSentAt >= 1000) {
    if (Frame(F("IO")).n(io.pir).n(io.sound).n(io.wsens).send(watchPort)) {
      ioLast = bits;
      ioSentAt = now;
    }
  }
  if (now - hwSentAt >= 60 && hwFrame(hwNext, watchPort, false)) {  // усі модулі приблизно за секунду
    hwSentAt = now;
    hwNext = (hwNext + 1) % C_COUNT;
  }
}

// ================= статистика циклу =================
static uint32_t loopMaxUs, statAt;

void diagLoopTime(uint32_t us) {
  if (us > loopMaxUs) loopMaxUs = us;
}

// ================= інтерфейс модуля =================
void diagBegin() { hwOk(C_BOARD); }

void diagUpdate(uint32_t now) {
  if (sys.service && !pending(sys.svcUntil, now)) svcSet(false, SRC_TIMEOUT);
  testUpdate(now);
  watchUpdate(now);
  if (now - statAt >= 1000) {
    statAt = now;
    sys.loopMax = min(255UL, (loopMaxUs + 999) / 1000);
    loopMaxUs = 0;
    sys.ram = freeRam();
  }
}
