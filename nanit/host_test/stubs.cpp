// Заглушки заліза: датчики, виконавчі пристрої, екран
#include "zh.h"
#include "EEPROM.h"
uint32_t fakeMillis = 1000;
HardwareSerial Serial, Serial3;
FakeEEPROM EEPROM;
Pins pins;
uint8_t portPins[12][4];
uint8_t bootResetCause() { return RC_POR; }
static int gatePos, gateTgt; bool fanIsOn; char tlNow; bool tmOkFlag = true;
void fanSet(bool on) { fanIsOn = on; }
// Серво шторки: рух миттєвий (часу тут немає), але одиниці ті самі, що в прошивці — кут у
// десятих градуса, і перетворення «відсоток → кут → відсоток» відтворене точно. Спрощений
// стаб (curPct = pct) саме цим і був сліпий: сповзання штори під захистом від защемлення
// живе в залишку цілочисельного ділення, якого при тотожності немає.
static int16_t cAngle10 = -1;
static int16_t angleFor10(uint8_t pct) {
  int16_t a = cal.servoClosed + ((int16_t)cal.servoOpen - cal.servoClosed) * min(pct, 100) / 100;
  return (cal.servoRev ? 180 - a : a) * 10;
}
void curtainSet(uint8_t p) { cAngle10 = angleFor10(p); }
void curtainHold() {}  // штора вже стоїть там, де стоїть
uint8_t curtainAngle() { return (cAngle10 < 0 ? angleFor10(0) : cAngle10) / 10; }
uint8_t curtainPct() {
  int16_t a = curtainAngle();
  if (cal.servoRev) a = 180 - a;
  int16_t span = (int16_t)cal.servoOpen - cal.servoClosed;
  if (!span) return 0;
  long p = (long)(a - cal.servoClosed) * 100 / span;
  return constrain(p, 0, 100);
}
bool curtainMoving() { return false; }
uint8_t soundTakeEdges() { return 0; }  // переривання мікрофона — лише на залізі
uint16_t soundEdgesTotal() { return 0; }
uint16_t soundTrips() { return 0; }
void gateBegin(int16_t pos) { gatePos = gateTgt = pos; }
void gateSet(bool open) { gateTgt = open ? 512 : 0; gatePos = gateTgt; }
void gateHome() { gatePos = gateTgt = 0; }
void gateNudge(int16_t d) { gateTgt = gatePos + d; gatePos = gateTgt; }
uint8_t gateState() { return gatePos ? GS_OPEN : GS_CLOSED; }
uint16_t gateStepsTo(bool open) { return abs((open ? 512 : 0) - gatePos); }
void tlSet(char c) { tlNow = c; }
void rgbSet(uint32_t, uint8_t, bool) {}
void buzzerPlay(uint8_t) {}
void buzzerMelody() {}
void buzzerSiren(bool) {}
void buzzerCo(bool) {}
void buzzerSirenTest(uint16_t) {}
bool buzzerQuietFor(uint16_t) { return true; }
bool tmShow(const char *, uint8_t) { return tmOkFlag; }
bool tmAck() { return tmOkFlag; }
void tmForce() {}
void dhtForce() {}
uint16_t dhtResults() { return 0; }
uint8_t dhtLastError() { return 0; }
void dispTestPattern(uint16_t) {}

int freeRam() { return 5000; }
