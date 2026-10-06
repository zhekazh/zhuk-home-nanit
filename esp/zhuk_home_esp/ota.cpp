// Оновлення прошивки по Wi-Fi: espota (для розробки) і POST /update (з браузера).
// Після першої заливки по USB модуль можна не знімати з порту 11.
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>

#include "zhe.h"

static ESP8266HTTPUpdateServer httpUpdate;
static bool busy;

bool otaBusy() { return busy; }

extern ESP8266WebServer server;

// Пароль веб-заливки змінюємо на місці: httpUpdate.setup() удруге кликати не можна — він
// щоразу реєструє ще один обробник на тому самому шляху. Без цього зміна службового пароля
// закривала /wifi, /reboot і /scan одразу (allowService читає cfg.ota живим) і починала
// показувати на сторінці «пароль заданий», тоді як POST /update лишався відкритим для всіх
// до перезапуску — тобто стан брехав саме про найруйнівнішу точку. Порожній пароль
// бібліотека трактує як «без автентифікації», тож знімання працює тим самим викликом.
void otaSetPassword() { httpUpdate.updateCredentials("zhuk", cfg.ota); }

void otaAttach() {
  // Порожній логін означає «без пароля»: у домашній мережі так зручніше,
  // а поле лишається на випадок, коли мережа спільна.
  httpUpdate.setup(&server, "/update", "zhuk", cfg.ota);

  ArduinoOTA.setHostname(netHost());
  // espota пароль бере лише до begin(): бібліотека змінює його тільки поки !_initialized,
  // тому після зміни пароля з веба модуль перезапускається (див. handlePass)
  if (cfg.ota[0]) ArduinoOTA.setPassword(cfg.ota);
  // Хук самого Updater'а покриває обидва шляхи — і ArduinoOTA, і веб-оновлення через
  // /update: обидва пишуть через нього, а ArduinoOTA.onStart для /update не викликається
  Update.onStart([]() {
    busy = true;
    // Тіло запиту ядро читає циклом «поки байтів немає, а клієнт під'єднаний — yield()», без
    // жодного строку (ESP8266WebServer, Parsing-impl.h). Якщо телефон вийшов із зони посеред
    // передачі, FIN не приходить, сокет лишається ESTABLISHED — і ESP назавжди крутиться в
    // цьому циклі всередині handleClient(): loop() не повертається, зв'язок із Nanit стоїть
    // (а Nanit за 5 с чистить свою чергу подій), і навіть watchdog не врятує, бо yield() його
    // годує. Keepalive робить мовчання видимим: за ~20 с сокет вважається мертвим, цикл
    // виходить, Updater дає помилку, busy знімається сам. На espota клієнта тут немає —
    // connected() на порожньому WiFiClient безпечно віддає 0.
    if (server.client().connected()) server.client().keepAlive(5, 5, 3);
    wsCloseAll();    // звільняємо пам'ять, яку тримають відкриті сторінки
    evStoreFlush();  // журнал на диск, поки прошивка ще та сама
    logPrint("ota: заливка почалась, сторінки від'єднано");
  });
  ArduinoOTA.onStart([]() { busy = true; });
  ArduinoOTA.onEnd([]() { logPrint("ota: готово, перезапуск"); });
  ArduinoOTA.onError([](ota_error_t e) {
    busy = false;
    logf("ota: помилка %u", (unsigned)e);
  });
  ArduinoOTA.begin(false);  // імена реєструє net.cpp, другий mDNS тут не потрібен
  logPrint("ota: espota і POST /update готові");
}

// Веб-заливка свого onError не має: ESP8266HTTPUpdateServer лише пише через Updater,
// тож після відмови — битий .bin, обрив з'єднання, невірний пароль — busy лишався б
// піднятим назавжди. А loop() при busy одразу робить return: стояли б і вебсервер, і
// WebSocket, і зв'язок із Nanit, і PING. Питаємо сам Updater — він один знає про обидва
// шляхи. Успішній заливці це не шкодить: вона вся відбувається всередині одного
// handleClient(), а після неї модуль перезапускається сам.
void otaLoop() {
  ArduinoOTA.handle();
  if (busy && !Update.isRunning()) busy = false;
}
