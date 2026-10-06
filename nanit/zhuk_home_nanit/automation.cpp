// Автоматика будинку: вентиляція, штора, шлагбаум, світло, тривога CO, сцени.
// Пріоритети: безпека (CO) → сервісний режим → охорона → ручний режим → автоматика.
#include "zh.h"

static const uint32_t RED = 0xFF0000UL;

// ================= вентиляція =================
// Режим «увімкнено» не може триматися вічно: поки двигун крутиться, він глушить лінію
// від ESP, і команда «вимкнути» з вебінтерфейсу фізично не доходить — перевірено на
// макеті, жодна з восьми спроб не пройшла ні на повній швидкості, ні на 40 %. Без цього
// запобіжника єдиним способом зупинити двигун лишалося вимкнути живлення будинку.
//
// П'ять секунд — щоб показати роботу вентиляції й не тонути в шумі. Це тимчасова міра:
// коли завади двигуна буде вилікувано (конденсатор на клемах або нижча швидкість UART)
// і команда вимкнення почне доходити, строк можна повернути до хвилин.
static const uint32_t FAN_MANUAL_MAX = 5000UL;

// Період і довжина вікна тишини, у яке пролізає команда від ESP (див. fanUpdate).
// 60 мс із 240 — це чверть часу, і вікно ширше за найдовший кадр (25 байтів на 38400 —
// близько 7 мс). Перша спроба (40 мс із 320) майже вдвічі збігалася з періодом повтору
// команди на ESP, тож спроби падали в ту саму фазу циклу й промахувалися всі вісім разом.
static const uint32_t FAN_WINDOW_PERIOD = 240UL;
static const uint32_t FAN_WINDOW_QUIET = 80UL;  // третина часу: 60 мс давали 13 % промахів

static void applyMode(uint8_t devId, uint8_t m, uint8_t src);

static void fanUpdate(uint32_t now) {
  bool on;
  uint8_t reason = FR_MANUAL;
  static uint32_t manualSince;
  if (dev.fanMode == OM_ON && !sys.service && !env.coAlarm) {
    if (!manualSince) manualSince = now ? now : 1;
    if (now - manualSince >= FAN_MANUAL_MAX) {
      manualSince = 0;
      applyMode(DEV_FAN, OM_AUTO, SRC_TIMEOUT);  // подія MODE, як від будь-якої іншої зміни
    }
  } else {
    manualSince = 0;
  }
  if (env.coAlarm) {
    on = true;
    reason = FR_CO;
  } else if (sys.service) {
    on = pending(ovr.fanUntil, now);
    reason = FR_SVC;
  } else if (dev.fanMode != OM_AUTO) {
    on = dev.fanMode == OM_ON;
  } else {
    bool hot = env.dhtOk && env.t >= cfg.fanOn;
    bool wet = env.dhtOk && env.h >= cfg.humOn;
    bool gas = env.coValid && env.co >= cfg.coWarn;
    if (dev.fanOn) {  // гістерезис: вимикаємо лише коли все повернулось у норму
      bool cool = !env.dhtOk || (env.t <= cfg.fanOff && env.h + 4 < cfg.humOn);
      on = !(cool && (!env.coValid || env.co + 5 < cfg.coWarn));
    } else {
      on = hot || wet || gas;
    }
    reason = gas ? FR_CO : wet && !hot ? FR_HUM : FR_TEMP;
  }
  static uint8_t startedBy = FR_MANUAL;
  if (on != dev.fanOn) {
    dev.fanOn = on;
    if (on) startedBy = reason;  // причину ввімкнення пам'ятаємо до самого вимкнення
    evPush(EV_FAN, on, startedBy, env.t);
  }

  // Вікна тишини. Поки двигун під напругою, він глушить лінію від ESP, і команда
  // «вимкнути» не доходить — будинком неможливо керувати саме тоді, коли це найпотрібніше.
  // Тому періодично знімаємо з двигуна напругу (константи й причина їхніх значень — вище):
  // вентилятор інерційний і обертів майже не втрачає, зате в кожному вікні лінія чиста, а ESP
  // повторює команду кожні 150 мс — отже вона неминуче влучить у вікно. Для стану будинку
  // вентилятор при цьому лишається увімкненим: блимати в кадрах DEV було б брехнею про те,
  // що відбувається.
  bool quiet = on && now % FAN_WINDOW_PERIOD < FAN_WINDOW_QUIET;
  fanSet(on && !quiet);
}

// ================= штора =================
// Жест датчика вікна: потримали руку й прибрали — штора перемикається
// й переходить у ручний режим. Поки рука там, штора не рушить (захист механіки).
static void gestureUpdate(uint32_t now) {
  static uint32_t since;
  static bool armed;
  bool allowed = cal.wsGest && !sys.service && sec.state == SEC_DIS;
  if (!allowed) {
    since = 0;
    armed = false;
    return;
  }
  if (io.wsObstacle) {
    if (!since) since = now ? now : 1;
    if (now - since >= cal.wsHold) armed = true;
    return;
  }
  if (armed) {  // руку прибрали: перемикаємо положення штори
    dev.winManual = curtainPct() > 50 ? 0 : 100;
    applyMode(DEV_WIN, WM_MAN, SRC_GESTURE);
  }
  since = 0;
  armed = false;
}

static void windowUpdate() {
  uint8_t target;
  if (env.coAlarm) target = 100;
  else if (sys.service) target = ovr.curtain;
  else if (sec.state != SEC_DIS) target = 0;
  else if (dev.winMode == WM_MAN) target = dev.winManual;
  else target = env.bright ? 100 : 0;
  dev.winTarget = target;

  // Захист від защемлення: не зачиняємо, поки біля вікна перешкода.
  // Діє і під охороною: краще лишити штору відкритою, ніж зламати механізм.
  bool blocked = target < curtainPct() && io.wsObstacle && !env.coAlarm;
  if (blocked && !dev.winBlocked) evPush(EV_WIN_BLOCK);
  dev.winBlocked = blocked;
  if (blocked) curtainHold();  // саме hold, а не curtainSet(curtainPct()): див. actuators.cpp
  else curtainSet(target);
}

// ================= шлагбаум =================
static uint8_t gateReason = GR_CAR;
static uint8_t gatePrev = 0xFF;  // 0xFF — стан ще не знаємо: перший цикл лише запам'ятає його
static uint32_t leftAt;

static void gateStart(bool open, uint8_t reason) {
  gateSet(open);
  gateReason = reason;
  leftAt = 0;
}

static void gateUpdate(uint32_t now) {
  uint8_t gs = gateState();
  bool openish = gs == GS_OPEN || gs == GS_OPENING;
  if (!sys.service) {
    if (secLocked()) {
      if (openish) gateStart(false, GR_ARMED);
    } else if (prk.gateMode == GM_OPEN) {
      if (!openish) gateStart(true, GR_MANUAL);
    } else if (prk.gateMode == GM_CLOSE) {
      if (openish) gateStart(false, GR_MANUAL);
    } else if (prk.dist >= 0 && prk.dist <= cfg.openDist) {
      leftAt = 0;
      if (!openish) gateStart(true, GR_CAR);
    } else if (openish) {
      // Машина від'їхала: або видно далеко, або вже поза межами далекоміра
      if (!leftAt) leftAt = now ? now : 1;
      if (now - leftAt >= cfg.closeDelay * 1000UL) gateStart(false, GR_LEFT);
    }
  }
  gs = gateState();
  // Перший цикл після старту лише запам'ятовує стан: стріла, підняту ще до вимкнення
  // живлення, ніхто щойно не відкривав, і причина «приїхала машина» тут була б вигадкою
  if (gatePrev != 0xFF && gs != gatePrev && (gs == GS_OPEN || gs == GS_CLOSED))
    evPush(EV_GATE, gs, gateReason, prk.dist);
  gatePrev = gs;
  prk.gateState = gs;
}

uint16_t autoGateTest(bool open) {
  uint16_t steps = gateStepsTo(open);
  if (steps) gateStart(open, GR_TEST);
  return steps;
}

// ================= світло =================
static void lightUpdate(uint32_t now) {
  bool on;
  uint32_t color = dev.color;
  uint8_t level = dev.level;
  if (sec.state == SEC_ALARM) {  // тривога: блимає червоним
    on = (now / 400) & 1;
    color = RED;
    level = 100;
  } else if (sys.service) {
    on = pending(ovr.rgbUntil, now);
    color = ovr.rgb;
    level = 100;
  } else if (dev.lightMode != OM_AUTO) {
    on = dev.lightMode == OM_ON;
  } else {
    on = !env.bright && io.motionAt && now - io.motionAt < 60000UL;
  }
  dev.lightOn = on;
  rgbSet(color, level, on);
}

// ================= тривога CO =================
static void coUpdate() {
  if (!env.coAlarm && env.coValid && env.co >= cfg.coDanger) {
    env.coAlarm = true;
    env.coMuted = false;
    evPush(EV_CO_HIGH, env.co);
  } else if (env.coAlarm && (!env.coValid || env.co + 10 < cfg.coDanger)) {
    env.coAlarm = false;
    evPush(EV_CO_OK, env.co);
  }
  buzzerCo(env.coAlarm && !env.coMuted);
}

// ================= світлофор і дисплей паркінгу =================
static void parkingDisplay(uint32_t now) {
  uint8_t gs = prk.gateState;
  char tl = gs == GS_CLOSED ? 'r' : gs == GS_OPEN ? 'g' : 'y';
  if (pending(ovr.tlUntil, now)) tl = ovr.tl;
  prk.tl = tl;
  tlSet(tl);

  char seg[5];
  if (pending(ovr.segUntil, now)) memcpy(seg, ovr.seg, 5);
  else if (gs == GS_OPENING || gs == GS_CLOSING) strcpy_P(seg, PSTR("WAIt"));
  else if (gs == GS_OPEN) strcpy_P(seg, PSTR("OPEn"));
  else if (secLocked()) strcpy_P(seg, PSTR("BLOC"));
  else if (prk.dist < 0) strcpy_P(seg, PSTR("----"));
  else if (prk.dist < 50) {
    if (prk.dist <= cfg.stopDist) strcpy_P(seg, (now / 300) & 1 ? PSTR("StOP") : PSTR("    "));
    else snprintf_P(seg, sizeof(seg), PSTR("%4d"), prk.dist);  // паркувальний асистент
  } else {
    strcpy_P(seg, PSTR("StOP"));
  }
  memcpy(prk.seg, seg, 5);
  tmShow(seg, cal.tmBr);
}

// ================= інтерфейс модуля =================
void autoUpdate(uint32_t now) {
  coUpdate();
  fanUpdate(now);
  gestureUpdate(now);
  windowUpdate();
  gateUpdate(now);
  lightUpdate(now);
  parkingDisplay(now);
}

// Зміна режиму пристрою з повідомленням для журналу — звідки б вона не прийшла
static void applyMode(uint8_t devId, uint8_t m, uint8_t src) {  // оголошено вище
  uint8_t *slot = devId == DEV_FAN ? &dev.fanMode
                : devId == DEV_WIN ? &dev.winMode
                : devId == DEV_GATE ? &prk.gateMode
                                    : &dev.lightMode;
  if (*slot == m) return;
  *slot = m;
  evPush(EV_MODE, devId, m, src);
}

uint8_t autoMode(uint8_t devId, const char *mode, uint8_t src) {
  if (sys.service) return NAK_STATE;
  if ((devId == DEV_WIN || devId == DEV_GATE) && sec.state != SEC_DIS) return NAK_STATE;
  int8_t m = pgmFind(devModeList(devId), mode);
  if (m < 0) return NAK_ARG;
  applyMode(devId, m, src);
  return NAK_NONE;
}

uint8_t autoWin(uint8_t pos, uint8_t src) {
  if (sys.service || sec.state != SEC_DIS) return NAK_STATE;
  dev.winManual = pos;
  applyMode(DEV_WIN, WM_MAN, src);
  return NAK_NONE;
}

uint8_t autoLight(uint32_t color, uint8_t level) {
  if (sys.service || sec.state == SEC_ALARM) return NAK_STATE;
  dev.color = color;
  dev.level = level;
  return NAK_NONE;
}

uint8_t autoMute(uint8_t src) {
  if (!env.coAlarm || env.coMuted) return NAK_STATE;
  env.coMuted = true;
  evPush(EV_CO_MUTE, src);
  return NAK_NONE;
}

uint8_t autoScene(uint8_t scene, const char *pin, int16_t *detail) {
  if (scene == SC_HOME) {
    if (sec.state != SEC_DIS) {
      if (!pin || !*pin) return NAK_ARG;  // без коду — не витрачаємо спробу
      uint8_t r = secDisarm(SRC_SCENE, pin, detail);
      if (r) return r;
    }
    applyMode(DEV_LIGHT, OM_AUTO, SRC_SCENE);
    applyMode(DEV_WIN, WM_AUTO, SRC_SCENE);
    applyMode(DEV_GATE, GM_AUTO, SRC_SCENE);
    return NAK_NONE;
  }
  uint8_t r = secArm(scene == SC_NIGHT ? SM_NIGHT : SM_ARM, SRC_SCENE, pin, detail);
  if (r) return r;
  applyMode(DEV_LIGHT, OM_OFF, SRC_SCENE);
  if (scene == SC_AWAY) {
    applyMode(DEV_FAN, OM_AUTO, SRC_SCENE);
    applyMode(DEV_GATE, GM_AUTO, SRC_SCENE);
  }
  return NAK_NONE;
}
