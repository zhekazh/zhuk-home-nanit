// Виконавчі пристрої. Усі рухи неблокуючі: кожен виклик actUpdate() робить
// щонайбільше один крок.
#include <Servo.h>
#include "zh.h"

// ================= вентилятор =================
static int16_t fanApplied = -1;  // останнє застосоване значення: шпаруватість і напрям

void fanSet(bool on) {
  uint8_t duty = on ? (uint16_t)cal.fanSpeed * 255 / 100 : 0;
  int16_t key = duty | (cal.fanRev ? 0x100 : 0);
  if (key == fanApplied) return;
  fanApplied = key;
  uint8_t drive = cal.fanRev ? pins.motB : pins.motA;
  uint8_t low = cal.fanRev ? pins.motA : pins.motB;
  digitalWrite(low, LOW);
  // На платах, старших за 3.1, двигун сидить на пінах Timer2, який забирає собі
  // tone() для зумера. Тому там керуємо без ШІМ, інакше перший звук глушить двигун.
  if (pins.v31) analogWrite(drive, duty);
  else digitalWrite(drive, duty >= 128);
  // Дозвіл драйверу даємо після рівнів і знімаємо, коли двигун не потрібен: вимкнений
  // драйвер не тримає обмотки під напругою й не шумить у лінію зв'язку з ESP.
  digitalWrite(pins.motEn, on ? HIGH : LOW);
}

// ================= RGB-світло =================
// Піни порту 4 (D44–D46) — ШІМ таймера Timer5, який на час руху забирає бібліотека
// Servo. Тоді світимо без ШІМ (увімкнено/вимкнено за половиною яскравості).
static uint8_t rgbValue[3];  // R, G, B з урахуванням яскравості
static bool servoOn;

static void rgbApply() {
  char order[4];
  pgmItem(N_ORDER, cal.rgbOrder, order, sizeof(order));
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t v = order[i] == 'R' ? rgbValue[0] : order[i] == 'G' ? rgbValue[1] : rgbValue[2];
    if (cal.rgbInv) v = 255 - v;
    if (servoOn) digitalWrite(pins.rgb[i], v >= 128);
    else analogWrite(pins.rgb[i], v);
  }
}

void rgbSet(uint32_t color, uint8_t level, bool on) {
  uint8_t v[3];
  for (uint8_t i = 0; i < 3; i++) v[i] = on ? (uint16_t)((color >> (16 - 8 * i)) & 0xFF) * level / 100 : 0;
  static uint8_t appliedOrder = 0xFF, appliedInv = 0xFF;
  if (!memcmp(v, rgbValue, 3) && appliedOrder == cal.rgbOrder && appliedInv == cal.rgbInv) return;
  memcpy(rgbValue, v, 3);
  appliedOrder = cal.rgbOrder;
  appliedInv = cal.rgbInv;
  rgbApply();
}

// ================= штора (сервопривід) =================
static Servo servo;
static int16_t cAngle10 = -1;  // поточний кут ×10; -1 — ще невідомий
static int16_t cTarget10 = -1;  // цільовий кут ×10; -1 — ціль ще не задавали
static uint32_t cStepAt, cStillAt;

static int16_t angleFor(uint8_t pct) {
  int16_t a = cal.servoClosed + ((int16_t)cal.servoOpen - cal.servoClosed) * pct / 100;
  return cal.servoRev ? 180 - a : a;
}

// Поки ціль не задано, вона означає «закрито» — так було й коли ціль зберігалася у відсотках
static int16_t targetAngle10() { return cTarget10 < 0 ? angleFor(0) * 10 : cTarget10; }

// Після detach() бібліотека Servo лишає Timer5 у своєму режимі. Повертаємо
// налаштування ядра Arduino (8-бітний ШІМ, дільник 64), щоб RGB знову мав ШІМ.
static void timer5Restore() {
  TIMSK5 &= ~_BV(OCIE5A);
  TCCR5B = 0;
  TCCR5A = _BV(WGM50);
  TCNT5 = 0;
  TCCR5B = _BV(CS51) | _BV(CS50);
}

static void curtainUpdate(uint32_t now) {
  if (now - cStepAt < 20) return;
  cStepAt = now;
  int16_t target = targetAngle10();
  if (cAngle10 < 0) {  // після старту одразу ставимо в потрібне положення
    cAngle10 = target;
    cStillAt = now;
  }
  if (cAngle10 != target) {
    int16_t step = max(1, (int16_t)cal.servoSpeed / 5);  // °/с → десяті градуса за 20 мс
    if (abs(target - cAngle10) <= step) cAngle10 = target;
    else cAngle10 += target > cAngle10 ? step : -step;
    cStillAt = now;
  }
  // Один поріг на все рішення. Два різні вікна (тримати, поки минуло менше за 600 мс, і
  // відчіпляти від 500) перетиналися: у проходах на 520, 540, 560 і 580 мс серво встигало
  // відчепитися й одразу причепитися знову. Кожна така пара тягне timer5Restore() і
  // rgbApply(), тобто RGB перемикається між апаратним ШІМ і «увімкнено за половиною
  // яскравості» — на малій яскравості це видимий сплеск, а серво може дьорнутися недоімпульсом.
  bool needServo = cAngle10 != target || now - cStillAt < 500;
  if (needServo) {
    if (!servoOn) {
      servo.attach(pins.servo);
      servoOn = true;
      rgbApply();
    }
    servo.write(cAngle10 / 10);
  } else if (servoOn) {  // доїхали й потримали: відпускаємо серво
    servo.detach();
    digitalWrite(pins.servo, LOW);  // detach() лишає пін як є — можливий вічний HIGH
    servoOn = false;
    timer5Restore();
    rgbApply();
  }
}

// Ціль живе в десятих градуса, а не у відсотках. Перетворення «кут → % → кут» губить
// залишок цілочисельного ділення, тому curtainSet(curtainPct()) щоразу давав ціль на
// градус-два нижче поточного кута. Захист від защемлення кликав його щоцикл, і штора
// сповзала до найближчого кута, кратного кроку відсотка: при open=165 це 24° (15 % ходу),
// а з позиції нижче 24° вона доїжджала в упор — рівно те, від чого захист і боронить.
void curtainSet(uint8_t pct) { cTarget10 = angleFor(min(pct, 100)) * 10; }

// Спинити штору там, де вона зараз: захист від защемлення тримає позицію без round-trip
void curtainHold() { cTarget10 = cAngle10 < 0 ? targetAngle10() : cAngle10; }

// Поки серво під'єднане, його імпульс не можна розтягувати довгими cli()
bool servoActive() { return servoOn; }

uint8_t curtainAngle() { return (cAngle10 < 0 ? targetAngle10() : cAngle10) / 10; }

uint8_t curtainPct() {
  int16_t a = curtainAngle();
  if (cal.servoRev) a = 180 - a;
  int16_t span = (int16_t)cal.servoOpen - cal.servoClosed;
  if (!span) return 0;
  long p = (long)(a - cal.servoClosed) * 100 / span;
  return constrain(p, 0, 100);
}

bool curtainMoving() { return cAngle10 >= 0 && cAngle10 != targetAngle10(); }

// ================= шлагбаум (28BYJ-48 + ULN2003) =================
// Повнокрокове керування: дві обмотки на крок (у ud_v3.1 було хвильове, по одній, —
// момент удвічі менший, і двигун губив кроки під вагою стріли). Відкриття — обмотки в
// зворотному порядку (IN4→IN1). Датчика положення немає, тому позицію в кроках
// зберігаємо в EEPROM (gatePosSave) і читаємо при старті: інакше після вимкнення
// живлення з піднятою стрілою прошивка вважала б її опущеною.
static int16_t gPos, gTarget;
static uint8_t gPhase;
static uint32_t gStepAt;
static bool gCoils;

static void gateCoils(int8_t phase) {
  // Дві обмотки на крок (full step), а не одна: момент майже вдвічі більший,
  // інакше 28BYJ-48 під навантаженням стріли губить кроки, і нуль повзе
  for (uint8_t k = 0; k < 4; k++)
    digitalWrite(pins.gate[k], phase >= 0 && (k == phase || k == ((phase + 1) & 3)));
  gCoils = phase >= 0;
}

static void gateUpdate(uint32_t now) {
  if (gPos == gTarget) {
    // Знеструмлюємо не одразу: ротору треба дійти до останньої позиції,
    // інакше за кілька циклів відкриття накопичиться втрата кроків
    if (gCoils && now - gStepAt >= 2u * cal.gateDelay) {
      gateCoils(-1);
      gatePosSave(gPos);  // рух завершено — позиція відома точно
    }
    return;
  }
  if (now - gStepAt < cal.gateDelay) return;
  gStepAt = now;
  int8_t dir = gTarget > gPos ? 1 : -1;
  bool backward = (dir > 0) != (cal.gateRev != 0);
  gPhase = backward ? (gPhase + 3) & 3 : (gPhase + 1) & 3;
  gateCoils(gPhase);
  gPos += dir;
}

// Позиція з EEPROM. Стріла нікуди не рушить: ціль дорівнює тому, де вона стоїть
void gateBegin(int16_t pos) { gPos = gTarget = pos; }

void gateSet(bool open) {
  gTarget = open ? cal.gateSteps : 0;
  if (gTarget != gPos) gatePosSave(gTarget);  // ціль перед рухом: див. gatePosSave
}

void gateHome() {
  gPos = 0;
  gTarget = 0;
  gatePosSave(0);
}

void gateNudge(int16_t delta) {  // ручне довертання при налаштуванні
  gTarget = gPos + delta;
  if (gTarget != gPos) gatePosSave(gTarget);
}

uint16_t gateStepsTo(bool open) { return abs((open ? (int16_t)cal.gateSteps : 0) - gPos); }

uint8_t gateState() {
  if (gPos == gTarget) return gTarget > 0 ? GS_OPEN : GS_CLOSED;
  return gTarget > gPos ? GS_OPENING : GS_CLOSING;
}

// ================= світлофор =================
static char tlApplied = '?';

void tlSet(char c) {
  // На порт світлофора можна поставити RGB-модуль замість трьох окремих світлодіодів
  // (KY-016 і подібні). Тоді середній вивід — синій канал, і «жовтий» світив би синім,
  // бо жовтого кристала там немає: він виходить із червоного разом із зеленим.
  //
  // Рівну суміш око бачить салатовою — зелений кристал яскравіший за червоний. Треба
  // приглушити зелений, але виводи порту 3 (D25, D30, D31) не мають апаратного ШІМ,
  // тож робимо програмний: зелений світить `cal.tlRgb` проходів циклу з шістнадцяти.
  // Цикл Nanit — кілька мілісекунд, тобто близько 80 Гц; мерехтіння непомітне.
  if (cal.tlRgb && c == 'y') {
    static uint8_t phase;
    tlApplied = 0;  // суміш перемальовується щопроходу, кеш тут не діє
    digitalWrite(pins.tlR, HIGH);
    digitalWrite(pins.tlY, LOW);
    digitalWrite(pins.tlG, (phase++ & 0x0F) < cal.tlRgb);
    return;
  }
  if (c == tlApplied) return;
  tlApplied = c;
  if (cal.tlRgb) {
    digitalWrite(pins.tlR, c == 'r');
    digitalWrite(pins.tlY, LOW);
    digitalWrite(pins.tlG, c == 'g');
    return;
  }
  digitalWrite(pins.tlR, c == 'r');
  digitalWrite(pins.tlY, c == 'y');
  digitalWrite(pins.tlG, c == 'g');
}

// ================= зумер (пасивний) =================
struct Note {
  uint16_t hz;  // 0 — пауза, 1 — частота звуку клавіш із калібрування
  uint16_t ms;
};
static const Note T_KEY[] PROGMEM = {{1, 40}};
static const Note T_OK[] PROGMEM = {{1047, 120}, {1319, 120}, {1568, 180}};
static const Note T_DENY[] PROGMEM = {{220, 250}, {0, 80}, {220, 250}};
static const Note T_TICK[] PROGMEM = {{2000, 60}};
static const Note T_ENTRY[] PROGMEM = {{2500, 120}};
static const Note T_ARMED[] PROGMEM = {{1568, 120}, {1047, 220}};
static const Note *const TONES[] = {T_KEY, T_OK, T_DENY, T_TICK, T_ENTRY, T_ARMED};
static const uint8_t TONE_LEN[] = {1, 3, 3, 1, 1, 2};
// Кількість елементів, а не sizeof: правильне число лише поки елементи однобайтові
static const uint8_t TONE_COUNT = sizeof(TONE_LEN) / sizeof(TONE_LEN[0]);

static const Note *bzTone;
static uint8_t bzLen, bzIdx;
static uint32_t bzNoteEnd, bzLastSound, bzSirenTestUntil;
static bool bzSiren, bzCo;
static uint16_t bzHz;  // частота, що звучить зараз; 0 — тиша

static void bzOut(uint16_t hz) {
  if (hz) bzLastSound = millis();
  if (hz == bzHz) return;
  bzHz = hz;
  if (hz) tone(pins.buzzer, hz);
  else noTone(pins.buzzer);
}

void buzzerPlay(uint8_t t) {
  if (t >= TONE_COUNT || bzSiren || bzCo) return;  // сирена важливіша
  bzTone = TONES[t];
  bzLen = TONE_LEN[t];
  bzIdx = 0;
  bzNoteEnd = millis();
}

void buzzerMelody() { buzzerPlay(BZ_OK); }
void buzzerSiren(bool on) { bzSiren = on; }
void buzzerCo(bool on) { bzCo = on; }
void buzzerSirenTest(uint16_t ms) { bzSirenTestUntil = millis() + ms; }
bool buzzerQuietFor(uint16_t ms) { return !bzHz && millis() - bzLastSound >= ms; }

static void buzzerUpdate(uint32_t now) {
  if (bzSiren || pending(bzSirenTestUntil, now)) {  // сирена: 600→1400→600 Гц за секунду
    uint16_t ph = now / 20 * 20 % 1000;
    bzOut(600 + (ph < 500 ? ph : 1000 - ph) * 8 / 5);
    bzTone = nullptr;
    return;
  }
  if (bzCo) {  // тривога CO: три сигнали, пауза
    uint16_t ph = now % 2400;
    bzOut(ph < 900 && ph % 300 < 200 ? 2700 : 0);
    bzTone = nullptr;
    return;
  }
  if (!bzTone) {
    bzOut(0);
    return;
  }
  if (pending(bzNoteEnd, now)) return;
  if (bzIdx >= bzLen) {
    bzTone = nullptr;
    bzOut(0);
    return;
  }
  Note n;
  memcpy_P(&n, &bzTone[bzIdx++], sizeof(n));
  bzOut(n.hz == 1 ? cal.buzTone : n.hz);
  bzNoteEnd = now + n.ms;
}

// ================= дисплей TM1637 =================
// Власний драйвер замість бібліотеки: так видно, чи дисплей підтверджує (ACK)
// кожен байт. Лінії працюють як відкритий колектор: «0» — вихід LOW, «1» —
// відпущений вхід, який підтягують резистори модуля.
static char tmShown[5];
static uint8_t tmShownBr = 0xFF;
static bool tmLastAck = true;
static uint32_t tmRefreshAt;

static void tmDelay() { delayMicroseconds(10); }

static void tmStart() {
  pinMode(pins.tmDio, OUTPUT);
  tmDelay();
}

static void tmStop() {
  pinMode(pins.tmDio, OUTPUT);
  tmDelay();
  pinMode(pins.tmClk, INPUT);
  tmDelay();
  pinMode(pins.tmDio, INPUT);
  tmDelay();
}

static bool tmWrite(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    pinMode(pins.tmClk, OUTPUT);
    tmDelay();
    pinMode(pins.tmDio, (b & 1) ? INPUT : OUTPUT);
    tmDelay();
    pinMode(pins.tmClk, INPUT);
    tmDelay();
    b >>= 1;
  }
  pinMode(pins.tmClk, OUTPUT);
  pinMode(pins.tmDio, INPUT_PULLUP);  // без підтяжки від'єднаний дисплей «підтверджує» запис
  tmDelay();
  pinMode(pins.tmClk, INPUT);
  tmDelay();
  bool ack = digitalRead(pins.tmDio) == LOW;
  digitalWrite(pins.tmDio, LOW);  // знімаємо підтяжку до перемикання на вихід
  if (ack) pinMode(pins.tmDio, OUTPUT);
  tmDelay();
  pinMode(pins.tmClk, OUTPUT);
  tmDelay();
  return ack;
}

static uint8_t tmGlyph(char c) {
  switch (c) {
    case '0': case 'O': return 0x3F;
    case '1': case 'I': return 0x06;
    case '2': return 0x5B;
    case '3': return 0x4F;
    case '4': return 0x66;
    case '5': case 'S': return 0x6D;
    case '6': return 0x7D;
    case '7': return 0x07;
    case '8': case 'B': return 0x7F;
    case '9': return 0x6F;
    case 'A': return 0x77;
    case 'C': return 0x39;
    case 'E': return 0x79;
    case 'L': return 0x38;
    case 'P': return 0x73;
    case 'W': return 0x6A;
    case 'n': return 0x54;
    case 't': return 0x78;
    case '-': return 0x40;
    default: return 0x00;
  }
}

bool tmShow(const char *text, uint8_t brightness) {
  uint32_t now = millis();
  bool same = !strncmp(text, tmShown, 4) && brightness == tmShownBr;
  if (same && pending(tmRefreshAt, now)) return tmLastAck;
  tmRefreshAt = now + 5000;  // раз на 5 с оновлюємо й перевіряємо дисплей
  strncpy(tmShown, text, 4);
  tmShown[4] = 0;
  tmShownBr = brightness;
  bool ok = true;
  tmStart();
  ok &= tmWrite(0x40);  // запис даних з автоінкрементом адреси
  tmStop();
  tmStart();
  ok &= tmWrite(0xC0);  // з першого розряду
  for (uint8_t i = 0; i < 4; i++) ok &= tmWrite(tmGlyph(text[i]));
  tmStop();
  tmStart();
  ok &= tmWrite(0x88 | (brightness & 7));  // дисплей увімкнено, яскравість
  tmStop();
  if (ok) hwOk(C_TM1637);
  else hwFail(C_TM1637, HE_NACK);
  tmLastAck = ok;
  return ok;
}

bool tmAck() { return tmLastAck; }

// Наступний tmShow() запише дисплей, навіть якщо напис не змінився
void tmForce() { tmRefreshAt = 0; }

// ================= інтерфейс модуля =================
void actBegin() {
  // Порядок важливий: спершу керуючі піни в нуль, і лише потім дозвіл драйверу. Якщо
  // ввімкнути драйвер раніше, він побачить на своїх входах невизначені рівні, поки піни
  // ще входи з високим імпедансом, — і двигун смикався при кожному старті плати.
  pinMode(pins.motA, OUTPUT);
  pinMode(pins.motB, OUTPUT);
  digitalWrite(pins.motA, LOW);
  digitalWrite(pins.motB, LOW);
  pinMode(pins.motEn, OUTPUT);
  digitalWrite(pins.motEn, LOW);  // драйвер увімкнеться лише на час роботи вентилятора
  fanSet(false);
  for (uint8_t k = 0; k < 4; k++) pinMode(pins.gate[k], OUTPUT);
  gateCoils(-1);
  gateBegin(gatePosLoad());  // settingsBegin уже прочитав cal.gateSteps
  pinMode(pins.tlR, OUTPUT);
  pinMode(pins.tlY, OUTPUT);
  pinMode(pins.tlG, OUTPUT);
  tlSet('r');
  for (uint8_t i = 0; i < 3; i++) pinMode(pins.rgb[i], OUTPUT);
  rgbApply();
  pinMode(pins.buzzer, OUTPUT);
  digitalWrite(pins.tmClk, LOW);
  digitalWrite(pins.tmDio, LOW);
  pinMode(pins.tmClk, INPUT);
  pinMode(pins.tmDio, INPUT);
}

void actUpdate(uint32_t now) {
  curtainUpdate(now);
  gateUpdate(now);
  buzzerUpdate(now);
}
