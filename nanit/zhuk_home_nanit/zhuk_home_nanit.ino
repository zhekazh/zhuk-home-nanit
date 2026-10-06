/*
  Zhuk Home — прошивка контролера Nanit для макета розумного будинку.
  Власне бачення проєкту NanitRobot/ud_v3.1: https://github.com/NanitRobot/ud_v3.1

  Nanit керує будинком сам, ESP8266 на порту 11 лише показує вебінтерфейс.
  Протокол обміну: docs/protocol.md. Ті самі команди можна набирати в моніторі
  порту (115200 бод, кінець рядка — NL), наприклад: $PING,1  $GET,2,cfg  $DBG,3,1

  Плата: Arduino Mega or Mega 2560. Потрібні бібліотеки: NanitLib (з залежностями), Servo.
*/
#if !defined(__AVR_ATmega2560__)
#error "Оберіть плату: Tools > Board > Arduino AVR Boards > Arduino Mega or Mega 2560, Processor: ATmega2560"
#endif

#include <NanitLib.h>
#include <avr/wdt.h>
#include "zh.h"

static void keyPressed(char k) {
  hwOk(C_KEYPAD);  // єдине місце: сюди приходить кожне натискання, у сервісному режимі теж
  if (sys.service) {
    if (cal.buzKeys) buzzerPlay(BZ_KEY);
    if (k == 'C') autoMute(SRC_KP);  // заглушити тривогу CO можна й у сервісному режимі
    diagKey(k);  // решта клавіш іде на перевірку, а не в охорону
  } else {
    secKey(k);
  }
}

void setup() {
  Serial.begin(ZH_USB_BAUD);
  Serial3.begin(ZH_ESP_BAUD);
  pinsBegin();
  linkBegin();  // першою в черзі подій має бути BOOT
  settingsBegin();
  Nanit_Base_Start();  // екран і службова ініціалізація NanitLib
  actBegin();
  sensorsBegin();
  kpBegin();
  secBegin();
  dispBegin();
  diagBegin();
  Serial.println(F("# Zhuk Home " ZH_FW_VERSION " ready"));
#if ZH_WATCHDOG
  wdt_enable(WDTO_4S);
#endif
}

void loop() {
  uint32_t started = micros();
  uint32_t now = millis();
#if ZH_WATCHDOG
  wdt_reset();
#endif
  linkUpdate(now);
  sensorsUpdate(now);
  char k = kpUpdate(now);
  if (k) keyPressed(k);
  secUpdate(now);
  autoUpdate(now);
  actUpdate(now);
  diagUpdate(now);
  settingsUpdate(now);
  linkPoll(now);  // ще раз прийняти байти перед повільним екраном
  dispUpdate(now);
  diagLoopTime(micros() - started);
}
