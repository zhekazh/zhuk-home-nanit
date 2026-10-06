// Датчики. Жоден не блокує цикл надовго: найдовше — читання DHT11 (≈4 мс
// з вимкненими перериваннями раз на 2 с).
#include <NanitLib.h>
#include "zh.h"

// ================= DHT11 =================
// Власний драйвер: 20 мс стартового імпульсу не чекаємо в delay(), як бібліотека
// Adafruit, а відраховуємо між проходами циклу. Алгоритм читання бітів — як у неї.
static bool dhtStarting, dhtForced;
static uint32_t dhtAt;
static uint16_t dhtCount;
static uint8_t dhtErr, dhtFails;
static volatile uint8_t *dhtIn;
static uint8_t dhtMask;
static const uint16_t DHT_TIMEOUT = 0xFFFF;

static uint16_t dhtPulse(bool level) {
  uint16_t count = 0;
  uint8_t want = level ? dhtMask : 0;
  while ((*dhtIn & dhtMask) == want) {
    if (++count >= 4000) return DHT_TIMEOUT;  // ≈3 мс при 12 тактах на оберт циклу
  }
  return count;
}

static uint8_t dhtRead(int8_t *t, int8_t *h) {
  uint8_t d[5] = {0, 0, 0, 0, 0};
  uint8_t err = 0;
  pinMode(pins.dht, INPUT_PULLUP);
  delayMicroseconds(55);
  uint8_t sreg = SREG;
  cli();
  if (dhtPulse(LOW) == DHT_TIMEOUT || dhtPulse(HIGH) == DHT_TIMEOUT) err = 1;
  for (uint8_t i = 0; i < 40 && !err; i++) {
    uint16_t lo = dhtPulse(LOW);
    uint16_t hi = dhtPulse(HIGH);
    if (lo == DHT_TIMEOUT || hi == DHT_TIMEOUT) {
      err = 1;
      break;
    }
    d[i / 8] <<= 1;
    if (hi > lo) d[i / 8] |= 1;
  }
  SREG = sreg;
  if (err) return 1;
  if (((d[0] + d[1] + d[2] + d[3]) & 0xFF) != d[4]) return 2;
  *h = d[0];
  // Ознака мінуса в DHT11 лежить у старшому біті дробової частини (як у бібліотеці Adafruit)
  *t = (d[3] & 0x80) ? (int8_t)(-1 - (int8_t)d[2]) : (int8_t)d[2];
  return 0;
}

static void dhtUpdate(uint32_t now) {
  if (!dhtStarting) {
    if (!dhtForced && now - dhtAt < 2000) return;
    // Читання тримає переривання вимкненими ~4 мс: за цей час розтягнувся б імпульс
    // серво (штора смикнулась би) і загубилися б байти від ESP. Чекаємо тиші,
    // але не довше ніж 10 с, інакше клімат перестав би оновлюватись зовсім.
    if (now - dhtAt < 10000 && (servoActive() || Serial3.available() || Serial.available())) return;
    dhtForced = false;
    digitalWrite(pins.dht, LOW);
    pinMode(pins.dht, OUTPUT);
    dhtAt = now;
    dhtStarting = true;
    return;
  }
  if (now - dhtAt < 20) return;
  int8_t t, h;
  dhtErr = dhtRead(&t, &h);
  dhtStarting = false;
  dhtAt = now;
  dhtCount++;
  if (!dhtErr) {
    env.t = t + cal.dhtT;
    env.h = constrain(h + cal.dhtH, 0, 100);
    env.dhtOk = true;
    dhtFails = 0;
    hwOk(C_DHT);
  } else if (++dhtFails >= 3) {  // поодинокі збої не вважаємо несправністю
    env.dhtOk = false;
    hwFail(C_DHT, dhtErr == 1 ? HE_TIMEOUT : HE_CRC);
  }
}

void dhtForce() { dhtForced = true; }
uint16_t dhtResults() { return dhtCount; }
uint8_t dhtLastError() { return dhtErr; }

// ================= HC-SR04 =================
// Тривалість відлуння міряє переривання зміни піна (Echo = D10 на платі 3.1),
// тож цикл не чекає в pulseIn(). На старих платах лишається pulseIn() з тайм-аутом.
static const uint16_t NO_ECHO = 0xFFFF;
static volatile uint32_t snRise;
static volatile uint16_t snUs;
static volatile bool snDone;
static volatile uint8_t *snIn;
static uint8_t snMask;
static bool snIsr, snStarted;
static uint16_t snSamples[9];
static uint8_t snPos, snNoEcho;
static uint32_t snAt;

ISR(PCINT0_vect) {
  if (*snIn & snMask) {
    snRise = micros();
  } else if (snRise) {
    uint32_t d = micros() - snRise;
    snUs = d > 60000UL ? 60000U : d;
    snDone = true;
    snRise = 0;
  }
}

static void sonarPush(uint16_t cm) {
  snSamples[snPos] = cm;
  snPos = (snPos + 1) % 9;
  uint8_t n = constrain(cal.sonarN, 1, 9);
  uint16_t tmp[9];
  for (uint8_t i = 0; i < n; i++) tmp[i] = snSamples[(snPos + 9 - 1 - i) % 9];
  for (uint8_t i = 1; i < n; i++) {  // сортування вставками
    uint16_t x = tmp[i];
    int8_t j = i - 1;
    while (j >= 0 && tmp[j] > x) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = x;
  }
  uint16_t med = tmp[n / 2];
  uint8_t valid = 0;
  while (valid < n && tmp[valid] != NO_ECHO) valid++;
  prk.samples = n;
  prk.spread = valid ? min(255, tmp[valid - 1] - tmp[0]) : 0;
  if (med == NO_ECHO) {
    prk.dist = -1;
  } else {
    int16_t d = max(2, (int16_t)med + cal.sonarOff);
    prk.dist = d > (int16_t)cal.sonarMax ? -1 : d;
  }
  if (cm == NO_ECHO) {
    if (snNoEcho < 255) snNoEcho++;
    if (snNoEcho == 17) hwFail(C_SONAR, HE_NOECHO);  // ≈1 с без жодного відлуння
  } else {
    snNoEcho = 0;
    hwOk(C_SONAR);
  }
}

static uint16_t usToCm(uint32_t us) { return (us < 100 || us >= 30000UL) ? NO_ECHO : us / 58; }

static void sonarUpdate(uint32_t now) {
  if (now - snAt < 60) return;  // HC-SR04: не частіше ніж раз на 60 мс
  snAt = now;
  if (snIsr) {
    if (snStarted) {
      noInterrupts();
      bool done = snDone;
      uint16_t us = snUs;
      snDone = false;
      snRise = 0;
      interrupts();
      sonarPush(done ? usToCm(us) : NO_ECHO);
    }
    digitalWrite(pins.trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(pins.trig, LOW);
    snStarted = true;
  } else {
    digitalWrite(pins.trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(pins.trig, LOW);
    // На платах без переривання на Echo pulseIn блокує цикл, тому обмежуємо
    // очікування 8 мс: далі UART почав би втрачати байти
    uint32_t limit = min((cal.sonarMax + 20UL) * 58UL, 8000UL);
    sonarPush(usToCm(pulseIn(pins.echo, HIGH, limit)));
  }
}

// ================= цифрові датчики =================
static uint32_t bootAt, pirRiseAt, soundFirstAt, soundLastAt, wsChangeAt;
static bool pirPrev, pirReported, wsRawPrev;
// Прогріви латчимо прапорцем, а не міряємо «now - bootAt» щоразу: після переповнення
// millis() (49,7 доби) різниця знову стала б малою, і датчики вдруге пішли б «прогріватися».
// Для MQ-7 це знімало активну тривогу CO — coUpdate бачив !coValid, гасив зумер і виводив
// вентилятор зі штори з примусу, поки газ лишався на місці; PIR на 60 с сліпнув під охороною.
static bool pirWarmed, mqWarmed;
static uint8_t soundPeaks;

static void pirUpdate(uint32_t now) {
  bool raw = digitalRead(pins.pir);
  io.pir = raw;
  if (raw && !pirPrev) pirRiseAt = now;
  if (!raw) pirReported = false;
  pirPrev = raw;
  if (!pirWarmed) {
    if (now - bootAt < cal.pirWarm * 1000UL) return;  // датчик прогрівається
    pirWarmed = true;
  }
  if (raw && !pirReported && now - pirRiseAt >= cal.pirPulse) {
    pirReported = true;
    io.motionAt = now;
    io.edgeAt[SN_PIR] = now;
    secSensor(SN_PIR);
  }
}

// Мікрофон опитується не в головному циклі, а в перериванні Timer3 — кожні 100 мкс.
// Причина зміряна на макеті через TEST,sound,listen: компаратор LM393 на плескіт дає
// 7–8 імпульсів приблизно по 200 мкс, тобто менше ніж 0,2 % часу. Головний цикл читає пін
// раз на ≈4 мс і застає такий імпульс у одному випадку з двадцяти, тож «три сплески за
// три секунди» для охорони виходили лотереєю. Крок 100 мкс, а не 200: на макеті слабкий
// плескіт давав імпульси близько 100 мкс, і кожен другий такий сплеск губився.
// Ціна — близько 3 % часу процесора. Timer3 тут вільний: його ШІМ-виводи
// (D2, D3, D5) на платі 3.1 — датчик руху й кнопка, обидва цифрові. Вентилятор сидить
// на ШІМ Timer4, серво й RGB порту 4 — на Timer5, зумер забирає Timer2.
static volatile uint8_t *sndReg;
static uint8_t sndBit;
static volatile uint8_t sndEdges;  // фронтів з останнього читання, більше 255 не треба
// Другий лічильник, який ніхто не спустошує: soundUpdate щоцикл забирає sndEdges собі
// (це і є робота охорони), тож діагностика по ньому завжди бачила нуль
static volatile uint16_t sndEdgesTotal;
// Скільки разів датчик дав повноцінне спрацювання (peaks за win). Під охороною кожне
// таке — тривога, тож лічильник дає змогу зміряти хибні спрацювання, не ввімкнувши сирену
static uint16_t sndTrips;
static volatile bool sndIsrPrev;

// Канал B, а не A: вектор TIMER3_COMPA вже оголошує бібліотека Servo (на Mega вона
// готує обробники для таймерів 1, 3, 4 і 5, хоч при одному серво крутить лише Timer5),
// і власний ISR давав «multiple definition of __vector_32». Сам таймер при цьому вільний
ISR(TIMER3_COMPB_vect) {
  bool level = (*sndReg & sndBit) != 0;
  bool act = cal.soundInv ? level : !level;
  if (act && !sndIsrPrev) {
    if (sndEdges < 255) sndEdges++;
    sndEdgesTotal++;
  }
  sndIsrPrev = act;
}

static void soundBegin() {
  sndReg = portInputRegister(digitalPinToPort(pins.sound));
  sndBit = digitalPinToBitMask(pins.sound);
  TCCR3A = 0;                            // CTC: 16 МГц / 8 / 200 = 10 кГц, тобто 100 мкс
  TCCR3B = _BV(WGM32) | _BV(CS31);
  OCR3A = 199;                           // період лічильника
  OCR3B = 100;                           // а переривання беремо з каналу B — див. ISR
  TIMSK3 = _BV(OCIE3B);
}

uint16_t soundEdgesTotal() { return sndEdgesTotal; }
uint16_t soundTrips() { return sndTrips; }

// Скільки фронтів мікрофона зловило переривання з минулого разу (і обнулити лічильник)
uint8_t soundTakeEdges() {
  uint8_t n;
  uint8_t sreg = SREG;
  cli();
  n = sndEdges;
  sndEdges = 0;
  SREG = sreg;
  return n;
}

// Імпульси одного плескання йдуть щільною чергою; поки вони не розрідилися, це той самий
// сплеск. Без цього одне плескання миттю набирало б усі потрібні «три сплески за три секунди»
static const uint16_t SOUND_GAP = 150;

static void soundUpdate(uint32_t now) {
  io.sound = digitalRead(pins.sound);  // сирий рівень піна — для кадру IO й калібрування
  uint8_t edges = soundTakeEdges();    // а сплески рахує переривання, див. soundBegin()
  if (!edges) return;
  if (cal.soundSiren && !buzzerQuietFor(300)) return;  // не слухаємо власний зумер
  if (soundLastAt && now - soundLastAt < SOUND_GAP) {
    soundLastAt = now;                 // хвіст того самого сплеску
    return;
  }
  soundLastAt = now;
  io.edgeAt[SN_SOUND] = now;
  if (!soundPeaks || now - soundFirstAt > cal.soundWin) {
    soundPeaks = 0;
    soundFirstAt = now;
  }
  if (++soundPeaks >= cal.soundPeaks) {
    soundPeaks = 0;
    sndTrips++;
    secSensor(SN_SOUND);
  }
}

static void wsensUpdate(uint32_t now) {
  bool raw = digitalRead(pins.wsens);
  io.wsens = raw;
  if (raw != wsRawPrev) {
    wsRawPrev = raw;
    wsChangeAt = now;
  }
  bool obstacle = cal.wsInv ? raw : !raw;  // FC-51: LOW, коли є перешкода
  if (obstacle != io.wsObstacle && now - wsChangeAt >= cal.wsDeb) {
    io.wsObstacle = obstacle;
    if (obstacle) {
      io.edgeAt[SN_WSENS] = now;
      secSensor(SN_WSENS);
    }
  }
}

// ================= аналогові датчики =================
static uint32_t anAt, batAt;

static void analogUpdate(uint32_t now) {
  if (now - anAt < 100) return;
  anAt = now;
  // Освітленість: чим темніше, тим більше значення
  env.ldr = analogRead(pins.ldr);
  if (env.bright && env.ldr > cal.ldrTh + cal.ldrHyst) env.bright = false;
  else if (!env.bright && env.ldr + cal.ldrHyst < cal.ldrTh) env.bright = true;
  if (env.ldr > 3 && env.ldr < 1020) hwOk(C_LDR);

  // MQ-7: згладжуємо, рахуємо відносний рівень від «чистого повітря»
  uint16_t raw = analogRead(pins.mq7);
  env.coRaw = env.coRaw ? (env.coRaw * 3 + raw) / 4 : raw;
  if (!mqWarmed && now - bootAt >= cal.mqWarm * 1000UL) mqWarmed = true;
  env.coValid = mqWarmed;
  uint16_t base = min(cal.mqBase, 1000);
  long co = env.coRaw > base ? (long)(env.coRaw - base) * 100 / (1023 - base) : 0;
  env.co = min(co, 100L);
  env.coLvl = !env.coValid ? 0 : env.co >= cfg.coDanger ? 2 : env.co >= cfg.coWarn ? 1 : 0;
  if (raw > 3 && raw < 1020) hwOk(C_MQ7);

  if (now - batAt >= 5000 || !batAt) {
    batAt = now;
    env.mV = START_NANIT.getBatteryVoltage() * 1000;
    env.batt = START_NANIT.getBatteryPower();
  }
}

// ================= інтерфейс модуля =================
void sensorsBegin() {
  bootAt = millis();
  pinMode(pins.pir, INPUT);
  pinMode(pins.sound, INPUT);
  soundBegin();
  pinMode(pins.wsens, INPUT);
  pinMode(pins.dht, INPUT_PULLUP);
  dhtIn = portInputRegister(digitalPinToPort(pins.dht));
  dhtMask = digitalPinToBitMask(pins.dht);
  dhtAt = millis();  // перше читання через 2 с після старту

  pinMode(pins.trig, OUTPUT);
  digitalWrite(pins.trig, LOW);
  pinMode(pins.echo, INPUT);
  for (uint16_t &s : snSamples) s = NO_ECHO;
  if (digitalPinToPCICR(pins.echo) == &PCICR && digitalPinToPCICRbit(pins.echo) == 0) {
    snIn = portInputRegister(digitalPinToPort(pins.echo));
    snMask = digitalPinToBitMask(pins.echo);
    *digitalPinToPCMSK(pins.echo) |= _BV(digitalPinToPCMSKbit(pins.echo));
    PCICR |= _BV(PCIE0);
    snIsr = true;
  }

  wsRawPrev = digitalRead(pins.wsens);
  io.wsObstacle = cal.wsInv ? wsRawPrev : !wsRawPrev;
  env.ldr = analogRead(pins.ldr);
  env.bright = env.ldr < cal.ldrTh;
}

void sensorsUpdate(uint32_t now) {
  pirUpdate(now);
  soundUpdate(now);
  wsensUpdate(now);
  dhtUpdate(now);
  sonarUpdate(now);
  analogUpdate(now);
}
