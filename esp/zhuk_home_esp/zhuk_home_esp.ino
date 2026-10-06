// Zhuk Home — ESP8266 Witty Cloud: вебінтерфейс і шлюз до контролера Nanit.
// Збірка: esp/build.sh (Generic ESP8266, 4 МБ, FS 1 МБ). Протокол — docs/protocol.md.
#include "zhe.h"

void setup() {
  // Буфер прийому збільшено навмисно: віддача сторінки блокує цикл на десятки мілісекунд,
  // а кадри від Nanit ідуть щосекунди — типових 128 байтів не вистачило б.
  Serial.setRxBufferSize(1024);
  Serial.begin(ZHE_NANIT_BAUD);
  logBegin();
  logf("Zhuk Home ESP %s, proto %d", ZHE_FW_VERSION, ZHE_PROTO);
  logf("reset: %s", ESP.getResetReason().c_str());

  fsBegin();
  cfgLoad();
  evStoreBegin();
  hubRestore();
  netBegin();
  webBegin();
  linkBegin();
  timeBegin();
}

void loop() {
  uint32_t now = millis();

  otaLoop();
  if (otaBusy()) return;  // під час заливки не чіпаємо ні мережу, ні Nanit

  linkLoop(now);  // кадри від Nanit першими: буфер прийому не повинен переповнитись
  netLoop(now);
  webLoop(now);
  timeLoop(now);
  evStoreLoop(now);

  // Дзеркало журналу в UART0 лишається на розсуд logbuf.cpp: воно працює, поки
  // не відкрита жодна сторінка, і його можна ввімкнути назавжди через cfg.logUart.
  static bool quieted;
  if (!quieted && net.up) {
    quieted = true;
    logToUart(cfg.logUart);
  }
}
