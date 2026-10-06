// Охорона: режими, затримки на вихід і вхід, тривога, код з клавіатури й мережі.
//
// Клавіатура:
//   код#  — поставити на охорону, якщо знято; інакше зняти (або скасувати постановку)
//   A     — під охорону, B — нічний режим (з кодом перед літерою, якщо quickArm = 0)
//   C     — вимкнути звук тривоги CO
//   *     — стерти введене
#include "zh.h"

static char kbuf[7];
static uint8_t klen;
static uint32_t kLast, tickAt;

static uint8_t maskFor(uint8_t mode) { return mode == SM_NIGHT ? cfg.nightMask : cfg.armMask; }

// Перевірка коду з підрахунком спроб: після трьох невірних — блокування на 60 с
static uint8_t pinCheck(const char *pin, uint8_t src, int16_t *detail) {
  uint32_t now = millis();
  if (pending(sec.lockUntil, now)) {
    *detail = secondsLeft(sec.lockUntil, now);
    return NAK_LOCK;
  }
  if (pin && strcmp(pin, secPin) == 0) {
    sec.wrong = 0;
    return NAK_NONE;
  }
  buzzerPlay(BZ_DENY);
  if (++sec.wrong >= 3) {
    sec.wrong = 0;
    sec.lockUntil = now + 60000UL;
    evPush(EV_PIN_LOCK, 60);
    *detail = 60;
    return NAK_LOCK;
  }
  evPush(EV_PIN_BAD, src, sec.wrong);
  *detail = 3 - sec.wrong;
  return NAK_PIN;
}

static void startArming(uint8_t mode, uint8_t src) {
  diagArming();  // перевірка датчика, що вимагала знятої охорони, далі дала б тривогу, а не «ok»
  sec.state = SEC_EXIT;
  sec.mode = mode;
  sec.until = millis() + cfg.exitDelay * 1000UL;
  sec.trigger = SN_NONE;
  tickAt = 0;
  evPush(EV_ARMING, mode, src);
  buzzerPlay(BZ_OK);
}

static void raiseAlarm(uint8_t sensor) {
  sec.state = SEC_ALARM;
  sec.trigger = sensor;
  sec.sirenUntil = millis() + cfg.siren * 1000UL;
  buzzerSiren(true);
  evPush(EV_ALARM, sensor);
}

uint8_t secArm(uint8_t mode, uint8_t src, const char *pin, int16_t *detail) {
  if (sys.service || sec.state != SEC_DIS) return NAK_STATE;
  if (!cfg.quickArm) {
    if (!pin || !*pin) return NAK_ARG;  // без коду — не витрачаємо спробу
    uint8_t r = pinCheck(pin, src, detail);
    if (r) return r;
  }
  startArming(mode, src);
  return NAK_NONE;
}

uint8_t secDisarm(uint8_t src, const char *pin, int16_t *detail) {
  if (sec.state == SEC_DIS) return NAK_STATE;
  uint8_t r = pinCheck(pin, src, detail);
  if (r) return r;
  bool cancel = sec.state == SEC_EXIT;
  sec.state = SEC_DIS;
  sec.mode = SM_NONE;
  sec.trigger = SN_NONE;
  sec.sirenUntil = 0;
  buzzerSiren(false);
  secStateSave(SEC_DIS);
  evPush(cancel ? EV_CANCEL : EV_DISARM, src);
  buzzerPlay(BZ_OK);
  return NAK_NONE;
}

uint8_t secChangePin(const char *oldPin, const char *newPin, int16_t *detail) {
  uint8_t len = strlen(newPin);
  if (len < 4 || len > 6) return NAK_ARG;
  for (uint8_t i = 0; i < len; i++)
    if (!isdigit(newPin[i])) return NAK_ARG;
  uint8_t r = pinCheck(oldPin, SRC_WEB, detail);
  if (r) return r;
  strcpy(secPin, newPin);
  settingsTouch();
  evPush(EV_PIN_SET, SRC_WEB);
  return NAK_NONE;
}

void secSensor(uint8_t sensor) {
  uint8_t bit = 1 << sensor;
  if (sec.state == SEC_ARM || sec.state == SEC_NIGHT) {
    if (!(maskFor(sec.mode) & bit)) return;
    if (sensor == SN_PIR && cfg.entryDelay) {  // рух дає час ввести код
      sec.state = SEC_ENTRY;
      sec.trigger = sensor;
      sec.until = millis() + cfg.entryDelay * 1000UL;
      tickAt = 0;
      evPush(EV_ENTRY, sensor);
    } else {
      raiseAlarm(sensor);
    }
  } else if (sec.state == SEC_ENTRY && sensor != SN_PIR && (maskFor(sec.mode) & bit)) {
    raiseAlarm(sensor);
  }
}

bool secLocked() {
  return sec.state == SEC_ARM || sec.state == SEC_NIGHT || sec.state == SEC_ENTRY || sec.state == SEC_ALARM;
}

uint8_t secKeyLen() { return klen; }

// У сервісному режимі клавіші сюди не доходять: keyPressed() у .ino віддає їх diagKey()
void secKey(char k) {
  kLast = millis();
  if (cal.buzKeys) buzzerPlay(BZ_KEY);
  if (k >= '0' && k <= '9') {
    if (klen < 6) kbuf[klen++] = k;
    return;
  }
  kbuf[klen] = 0;
  const char *code = klen ? kbuf : nullptr;
  int16_t detail;
  switch (k) {
    case '#':
      if (!code) break;  // порожній # не рахуємо невірною спробою
      if (sec.state == SEC_DIS) {
        if (pinCheck(code, SRC_KP, &detail) == NAK_NONE) startArming(SM_ARM, SRC_KP);
      } else {
        secDisarm(SRC_KP, code, &detail);
      }
      break;
    case 'A':
    case 'B':
      if (!cfg.quickArm && !code) buzzerPlay(BZ_DENY);
      else secArm(k == 'A' ? SM_ARM : SM_NIGHT, SRC_KP, code, &detail);
      break;
    case 'C':
      autoMute(SRC_KP);
      break;
    default:
      break;
  }
  klen = 0;
}

void secBegin() {
  uint8_t saved = secStateLoad();  // охорона переживає вимкнення живлення
  if (saved != SEC_DIS) {
    sec.state = saved;
    sec.mode = saved == SEC_NIGHT ? SM_NIGHT : SM_ARM;
    evPush(EV_ARMED, sec.mode);
  }
}

void secUpdate(uint32_t now) {
  if (klen && now - kLast > 10000) klen = 0;  // недонабраний код забуваємо за 10 с

  switch (sec.state) {
    case SEC_EXIT:
      if (!pending(sec.until, now)) {
        sec.state = sec.mode == SM_NIGHT ? SEC_NIGHT : SEC_ARM;
        secStateSave(sec.state);
        evPush(EV_ARMED, sec.mode);
        buzzerPlay(BZ_ARMED);
      } else if (now - tickAt >= (secondsLeft(sec.until, now) <= 5 ? 500UL : 1000UL)) {
        tickAt = now;
        buzzerPlay(BZ_TICK);
      }
      break;
    case SEC_ENTRY:
      if (!pending(sec.until, now)) raiseAlarm(sec.trigger);
      else if (now - tickAt >= 500) {
        tickAt = now;
        buzzerPlay(BZ_ENTRY);
      }
      break;
    case SEC_ALARM:
      if (sec.sirenUntil && !pending(sec.sirenUntil, now)) {
        sec.sirenUntil = 0;
        buzzerSiren(false);
        evPush(EV_SIREN_END);
      }
      break;
    default:
      break;
  }
}
