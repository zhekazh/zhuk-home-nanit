// Zhuk Home — прошивка контролера Nanit.
// Спільні типи, стан будинку й інтерфейси модулів. Протокол описано в docs/protocol.md.
#pragma once
#include <Arduino.h>

#define ZH_FW_VERSION "0.1.0"
#define ZH_PROTO 1
// 38400, а не 57600: на 16 МГц дільник UART Mega дає для 57600 похибку 2,1 % — найгіршу
// зі стандартних швидкостей, і кадри від ESP сипалися на CRC (кожна команда доходила з
// повтору, +300 мс). На 38400 похибка 0,2 %, а трафіку тут менше за 300 Б/с.
#define ZH_ESP_BAUD 38400UL
#define ZH_USB_BAUD 115200UL
#define ZH_WATCHDOG 1  // 0 — вимкнути сторожовий таймер
#define FRAME_MAX 96   // найдовший кадр разом із $, *CRC і \n

// ---------- переліки (порядок збігається зі списками назв N_*) ----------
enum SecState : uint8_t { SEC_DIS, SEC_EXIT, SEC_ARM, SEC_NIGHT, SEC_ENTRY, SEC_ALARM };
enum SecMode : uint8_t { SM_NONE, SM_ARM, SM_NIGHT };
enum SensorId : uint8_t { SN_PIR, SN_SOUND, SN_WSENS, SN_COUNT, SN_NONE = 0xFF };
enum Src : uint8_t { SRC_KP, SRC_WEB, SRC_SCENE, SRC_GESTURE, SRC_TIMEOUT };
enum OnOffMode : uint8_t { OM_AUTO, OM_ON, OM_OFF };
enum WinMode : uint8_t { WM_AUTO, WM_MAN };
enum GateMode : uint8_t { GM_AUTO, GM_OPEN, GM_CLOSE };
enum GateState : uint8_t { GS_CLOSED, GS_OPENING, GS_OPEN, GS_CLOSING };
enum Dev : uint8_t { DEV_FAN, DEV_WIN, DEV_GATE, DEV_LIGHT };
enum Scene : uint8_t { SC_AWAY, SC_HOME, SC_NIGHT };
enum Comp : uint8_t {
  C_BOARD, C_SERVO, C_FAN, C_GATE, C_TLIGHT, C_RGB, C_PIR, C_SOUND,
  C_BUZZER, C_WSENS, C_DHT, C_MQ7, C_LDR, C_KEYPAD, C_SONAR, C_TM1637, C_COUNT
};
enum Port : uint8_t { PORT_ESP, PORT_USB };
enum EvCode : uint8_t {
  EV_BOOT, EV_ARMING, EV_ARMED, EV_DISARM, EV_CANCEL, EV_ENTRY, EV_ALARM, EV_SIREN_END,
  EV_PIN_BAD, EV_PIN_LOCK, EV_PIN_SET, EV_CO_HIGH, EV_CO_OK, EV_CO_MUTE, EV_FAN, EV_GATE,
  EV_WIN_BLOCK, EV_MODE, EV_SVC, EV_CFG, EV_CAL, EV_HW_ERR, EV_HW_OK, EV_LOST
};
enum Nak : uint8_t { NAK_NONE, NAK_TYPE, NAK_ARG, NAK_STATE, NAK_PIN, NAK_LOCK, NAK_BUSY };
enum HwStatus : uint8_t { HW_NONE, HW_OK, HW_WARN, HW_ERR };
enum HwErr : uint8_t { HE_TIMEOUT, HE_CRC, HE_NOECHO, HE_NACK };
enum FanReason : uint8_t { FR_TEMP, FR_HUM, FR_CO, FR_MANUAL, FR_SVC };
enum GateReason : uint8_t { GR_CAR, GR_LEFT, GR_MANUAL, GR_ARMED, GR_TEST };
enum ResetCause : uint8_t { RC_POR, RC_EXT, RC_WDT, RC_BOR };
enum NetState : uint8_t { NET_OFF, NET_STA, NET_AP };
enum Tone : uint8_t { BZ_KEY, BZ_OK, BZ_DENY, BZ_TICK, BZ_ENTRY, BZ_ARMED };

// Списки назв у флеші, розділені «|» (util.cpp)
extern const char N_SEC[] PROGMEM;
extern const char N_MODE[] PROGMEM;
extern const char N_SENSOR[] PROGMEM;
extern const char N_SRC[] PROGMEM;
extern const char N_ONOFF[] PROGMEM;
extern const char N_WINM[] PROGMEM;
extern const char N_GATEM[] PROGMEM;
extern const char N_GATES[] PROGMEM;
extern const char N_DEV[] PROGMEM;
extern const char N_SCENE[] PROGMEM;
extern const char N_COMP[] PROGMEM;
extern const char N_EV[] PROGMEM;
extern const char N_NAK[] PROGMEM;
extern const char N_HWST[] PROGMEM;
extern const char N_HWERR[] PROGMEM;
extern const char N_FANR[] PROGMEM;
extern const char N_GATER[] PROGMEM;
extern const char N_RESET[] PROGMEM;
extern const char N_NET[] PROGMEM;
extern const char N_ORDER[] PROGMEM;

// ---------- утиліти (util.cpp) ----------
uint8_t crc8(const uint8_t *data, size_t len);
bool pgmItem(const char *list, uint8_t idx, char *out, uint8_t size);
int8_t pgmFind(const char *list, const char *s);
bool parseNum(const char *s, long lo, long hi, long *out);
const char *devModeList(uint8_t devId);  // список назв режимів пристрою (Dev)
int freeRam();
// Чи ще не настав момент `at` (коректно після переповнення millis).
// Нуль означає «строку не задано», інакше після 24,9 доби роботи нульові поля
// почали б виглядати як момент у майбутньому.
//
// Верхня межа — та сама пастка, але для полів, які після спрацювання лишаються ненульовими
// (`sec.lockUntil`, `watchUntil`, `bzNoteEnd`, `tmRefreshAt`, `screenUntil`): щойно `at`
// минуло понад 2³¹ мс тому, різниця перекочується в додатну, і прострочений строк «оживає»
// ще на 24,85 доби. Одне блокування клавіатури, спрацьоване колись давно, після 24,86 доби
// роботи почало відмовляти вірному коду — будинок було не зняти з охорони ні з клавіатури,
// ні зі сторінки, а поле `lock` кадру SEC віддавало 2147478 замість 0–60. Найдовший строк
// у прошивці — сервісний режим на 10 хвилин, тож усе, що далі за годину, — це перекочування
// лічильника, а не строк.
inline bool pending(uint32_t at, uint32_t now) {
  uint32_t left = at - now;
  return at && left && left <= 3600000UL;
}
inline uint32_t secondsLeft(uint32_t at, uint32_t now) { return pending(at, now) ? (at - now + 999) / 1000 : 0; }

// ---------- піни (pins.cpp) ----------
struct Pins {
  bool v31;  // плата 3.1 і новіша
  uint8_t servo, motA, motB, motEn;
  uint8_t gate[4];
  uint8_t tlR, tlY, tlG;
  uint8_t rgb[3];  // P4_2, P4_3, P4_4
  uint8_t pir, sound, buzzer, wsens;
  uint8_t dht, mq7, ldr;
  uint8_t kpRow[4], kpCol[4];
  uint8_t tmClk, tmDio, trig, echo;
};
extern Pins pins;
extern uint8_t portPins[12][4];  // усі сигнальні піни портів
void pinsBegin();

// ---------- налаштування й калібрування (settings.cpp) ----------
struct Cfg {
  uint8_t fanOn, fanOff, humOn, coWarn, coDanger;
  uint8_t openDist, closeDelay, stopDist;
  uint8_t exitDelay, entryDelay;
  uint16_t siren;
  uint8_t quickArm, armMask, nightMask;
};
struct Cal {
  uint8_t servoClosed, servoOpen, servoSpeed, servoRev;
  uint8_t fanSpeed, fanRev;
  uint16_t gateSteps;
  uint8_t gateDelay, gateRev;
  uint8_t rgbOrder, rgbInv;
  uint8_t tlRgb;  // світлофор — не три світлодіоди, а RGB-модуль: жовтий = червоний + зелений
  uint8_t pirWarm;
  uint16_t pirPulse;
  uint8_t soundPeaks;
  uint16_t soundWin;
  uint8_t soundSiren, soundInv;
  uint16_t buzTone;
  uint8_t buzKeys;
  uint16_t wsDeb;
  uint8_t wsInv, wsGest;
  uint16_t wsHold;
  int8_t dhtT, dhtH;
  uint16_t mqWarm, mqBase;
  uint16_t ldrTh;
  uint8_t ldrHyst;
  uint8_t kpSwap, kpDeb;
  int8_t sonarOff;
  uint8_t sonarN;
  uint16_t sonarMax;
  uint8_t tmBr;
};
extern Cfg cfg;
extern Cal cal;
extern char secPin[7];

void settingsBegin();
void settingsUpdate(uint32_t now);
void settingsTouch();  // зберегти в EEPROM трохи згодом
uint8_t cfgSet(const char *key, const char *val);                // Nak
uint8_t calSet(uint8_t comp, const char *key, const char *val);  // Nak
uint8_t fieldCount();
// Поле таблиці за номером: модуль (0xFF — налаштування), назва, значення
bool fieldInfo(uint8_t i, uint8_t *comp, char *name, uint8_t nameSize, char *value, uint8_t valueSize);
void secStateSave(uint8_t state);
uint8_t secStateLoad();
void gatePosSave(int16_t pos);
int16_t gatePosLoad();

// ---------- стан будинку (state.cpp) ----------
struct SecData {
  uint8_t state, mode, trigger, wrong;
  uint32_t until, sirenUntil, lockUntil;
};
struct EnvData {
  int8_t t, h;
  bool dhtOk;
  uint8_t co;
  bool coValid;
  uint16_t coRaw, ldr;
  bool bright;
  uint8_t coLvl;
  bool coAlarm, coMuted;
  uint8_t batt;
  uint16_t mV;
};
struct IoData {
  bool pir, sound, wsens;  // сирі рівні пінів
  bool wsObstacle;         // перешкода біля вікна після антибрязкоту
  uint32_t motionAt;
  uint32_t edgeAt[SN_COUNT];  // останнє спрацювання кожного датчика
};
struct DevData {
  uint8_t fanMode;
  bool fanOn;
  uint8_t winMode, winManual, winTarget;
  bool winBlocked;
  uint8_t lightMode;
  bool lightOn;
  uint32_t color;
  uint8_t level;
};
struct PrkData {
  uint8_t gateMode, gateState;
  int16_t dist;  // см, -1 — немає даних
  uint8_t spread, samples;
  char tl;       // r, y, g або 0
  char seg[5];
};
struct SysData {
  bool service;
  uint32_t svcUntil;
  uint8_t reset;
  uint16_t ram;
  uint8_t loopMax;
  uint8_t net;
  int8_t rssi;
  char ip[16];
  uint32_t unixBase, unixAt;
  int16_t tzMin;
  bool timeValid;
  uint8_t dbg;  // 0 — тихо, 1 — кадри на USB, 2 — ще й текст, що ESP пише поза кадрами
  uint32_t lastEspRx;
};
// Керування з перевірок обладнання
struct Ovr {
  uint32_t fanUntil, tlUntil, rgbUntil, segUntil, screenUntil;
  char tl;
  uint32_t rgb;
  char seg[5];
  uint8_t curtain;
};
extern SecData sec;
extern EnvData env;
extern IoData io;
extern DevData dev;
extern PrkData prk;
extern SysData sys;
extern Ovr ovr;

// ---------- зв'язок (link.cpp) ----------
class Frame {
 public:
  explicit Frame(const __FlashStringHelper *type);
  Frame &s(const char *v);
  Frame &f(const __FlashStringHelper *v);
  Frame &p(const char *list, uint8_t idx);
  Frame &n(long v);
  Frame &dash();
  Frame &ch(char c);
  Frame &hex6(uint32_t v);
  bool send(uint8_t port, bool force = false);
  uint16_t hash() const;

 private:
  void put(char c);
  char buf_[FRAME_MAX - 4];
  uint8_t len_;
};
void linkBegin();
void linkUpdate(uint32_t now);
void linkPoll(uint32_t now);  // лише прийом
void evPush(uint8_t code, int16_t a = 0, int16_t b = 0, int16_t c = 0);
bool espOnline(uint32_t now);

// ---------- датчики (sensors.cpp) ----------
void sensorsBegin();
void sensorsUpdate(uint32_t now);
uint16_t soundEdgesTotal();   // усі фронти від старту, не спустошується
uint16_t soundTrips();        // повноцінних спрацювань від старту (під охороною = тривоги)
uint8_t soundTakeEdges();  // фронти мікрофона, зафіксовані перериванням Timer3
void dhtForce();
uint16_t dhtResults();   // лічильник завершених читань
uint8_t dhtLastError();  // 0 — успіх, 1 — немає відповіді, 2 — помилка CRC

// ---------- виконавчі пристрої (actuators.cpp) ----------
void actBegin();
void actUpdate(uint32_t now);
void fanSet(bool on);
void curtainSet(uint8_t pct);
void curtainHold();  // спинити штору в поточному куті
uint8_t curtainPct();
uint8_t curtainAngle();
bool curtainMoving();
bool servoActive();
void gateBegin(int16_t pos);
void gateSet(bool open);
void gateHome();
void gateNudge(int16_t delta);
uint8_t gateState();
uint16_t gateStepsTo(bool open);
void tlSet(char c);
void rgbSet(uint32_t color, uint8_t level, bool on);
void buzzerPlay(uint8_t tone);
void buzzerMelody();
void buzzerSiren(bool on);
void buzzerCo(bool on);
void buzzerSirenTest(uint16_t ms);
bool buzzerQuietFor(uint16_t ms);  // зумер мовчить щонайменше ms
bool tmShow(const char *text, uint8_t brightness);
bool tmAck();
void tmForce();

// ---------- клавіатура (keypad.cpp) ----------
void kpBegin();
char kpUpdate(uint32_t now);

// ---------- охорона (security.cpp) ----------
void secBegin();
void secUpdate(uint32_t now);
uint8_t secArm(uint8_t mode, uint8_t src, const char *pin, int16_t *detail);
uint8_t secDisarm(uint8_t src, const char *pin, int16_t *detail);
uint8_t secChangePin(const char *oldPin, const char *newPin, int16_t *detail);
void secSensor(uint8_t sensor);
void secKey(char k);
uint8_t secKeyLen();
bool secLocked();  // охорона ввімкнена: вікно й шлагбаум зачинені

// ---------- автоматика (automation.cpp) ----------
void autoUpdate(uint32_t now);
uint8_t autoMode(uint8_t devId, const char *mode, uint8_t src);
uint8_t autoWin(uint8_t pos, uint8_t src);
uint8_t autoLight(uint32_t color, uint8_t level);
uint8_t autoScene(uint8_t scene, const char *pin, int16_t *detail);
uint8_t autoMute(uint8_t src);
uint16_t autoGateTest(bool open);

// ---------- діагностика й сервісний режим (diag.cpp) ----------
void diagBegin();
void diagUpdate(uint32_t now);
void diagLoopTime(uint32_t us);
uint8_t svcSet(bool on, uint8_t src);
void diagArming();  // почалася постановка під охорону: перевірки, що вимагали DIS, скасувати
uint8_t testStart(uint8_t port, uint16_t seq, uint8_t comp, const char *action);
uint8_t calDo(uint8_t comp, const char *action, const char *arg);
void diagWatch(bool on, uint8_t port);
void diagKey(char k);
void hwOk(uint8_t comp);
void hwFail(uint8_t comp, uint8_t code);
bool hwFrame(uint8_t comp, uint8_t port, bool force);

// ---------- екран Nanit (display.cpp) ----------
void dispBegin();
void dispUpdate(uint32_t now);
void dispTestPattern(uint16_t ms);
