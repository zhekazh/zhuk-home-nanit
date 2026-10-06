// Zhuk Home — прошивка ESP8266 Witty Cloud: шлюз між Nanit і браузером.
// Обмін із Nanit по UART — docs/protocol.md; те, що модуль віддає в мережу
// (HTTP на :80 і WebSocket на :81), — docs/api.md.
#pragma once
#include <Arduino.h>

#define ZHE_FW_VERSION "0.2.1"
#define ZHE_PROTO 1             // версія протоколу Nanit ↔ ESP
#define ZHE_NANIT_BAUD 38400UL  // Serial (UART0) — до Nanit; чому не 57600 — див. zh.h
#define FRAME_MAX 96            // найдовший кадр разом із $, *CRC і \n
// Київ: UTC+2, літній час з останньої неділі березня по останню неділю жовтня.
// Те саме, що TZ_Europe_Kiev у ядрі, але звичайним рядком — воно лежить у PROGMEM.
#define TZ_DEFAULT "EET-2EEST,M3.5.0/3,M10.5.0/4"

// ---------- журнал налагодження ----------
// Кільцевий буфер у RAM: веб-консоль показує його новому клієнту цілком.
// Дублювання в UART0 (там же протокол) — лише поки не піднялась мережа
// і при вимозі з веба: рядки без «$» Nanit відкидає, а з $DBG,n,2 ретранслює на USB.
#define LOG_LINES 40
#define LOG_LEN 96

void logBegin();
void logPrint(const char *s);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint8_t logCount();
const char *logAt(uint8_t i);  // 0 — найстаріший наявний рядок
uint32_t logSeq();             // скільки рядків написано від старту
// Наступний рядок історії за номером `at`; nullptr — курсор дійшов до кінця журналу.
// Саме номер, а не індекс: поки сторінка дістає історію порціями, кільце може прокрутитись.
const char *logSince(uint32_t *at);
void logToUart(bool on);

// ---------- налаштування у LittleFS ----------
// Пишуться з веба, тому не в EEPROM: файл переживає оновлення прошивки.
struct Cfg {
  char ssid[33];
  char pass[65];
  char host[25];  // ім'я в мережі: http://<host>.local
  char ota[25];   // пароль OTA й службових дій (/wifi, /reboot, /reset, /update); порожній — без пароля
  char tz[40];    // правило зони POSIX; зсув і дати переходу рахує бібліотека
  char ntp[33];
  bool logUart;
};
extern Cfg cfg;

bool fsBegin();
void cfgDefaults();
bool cfgLoad();
bool cfgSave();
void cfgSetStr(char *dst, size_t size, const char *src);  // копія без керуючих символів

// ---------- мережа ----------
enum NetMode : uint8_t { NM_BOOT, NM_STA, NM_AP };

struct Net {
  uint8_t mode;
  int8_t rssi;
  char ip[16];
  char ssid[33];  // до чого підключені або ім'я власної точки
  bool up;        // є адреса й можна віддавати сторінки
  char why[8];    // чому не вийшло підключитись: pass, nossid, fail, slow
  uint32_t since;
};
extern Net net;

void netBegin();
void netLoop(uint32_t now);
void netRemember();  // продублювати мережу у флеш SDK, щоб пережити втрату конфігу
const char *netHost();

// ---------- зв'язок із Nanit ----------
struct NanitState {
  bool online;       // кадри йдуть не рідше ніж раз на 3 с
  uint32_t lastRx;   // коли прийшов останній правильний кадр
  uint32_t frames;   // скільки кадрів прийнято
  uint32_t bad;      // скільки відкинуто через CRC
  uint8_t proto;
  char fw[12];
  char board[6];
  char reset[6];
};
extern NanitState nanit;

void linkBegin();
void linkLoop(uint32_t now);
// Поставити команду в чергу. args — без типу й номера: "fan,on". webId — номер запиту
// сторінки, якому повернути відповідь; 0 — команда самого ESP.
// false — черга повна або команда не вміщається в кадр.
bool linkSend(const char *type, const char *args, uint16_t webId, uint8_t client);
const char *linkWhy();  // причина останньої відмови linkSend: busy або toolong

// ---------- журнал подій у файлі (evstore.cpp) ----------
// Кільцевий файл постійного розміру, записи скидаються порціями: флеш витримує близько
// 100 000 стирань на сектор, тож запис на кожну подію був би марнотратством ресурсу.
#define EV_LEN 72

void evStoreBegin();
void evStorePush(uint32_t at, const char *raw);
void evStoreFlush();  // перед перезапуском, оновленням і скиданням — обов'язково
void evStoreLoop(uint32_t now);
void evStoreEach(void (*fn)(uint32_t at, const char *raw));

// ---------- вузол: знімок стану, журнал подій, маршрутизація ----------
void hubFrame(char **argv, uint8_t argc, const char *raw);  // кадр від Nanit
void hubCmdDone(uint16_t webId, uint8_t client, const char *raw);  // ACK/NAK на команду сторінки
// Один крок привітання нової сторінки; false — кроки скінчились. Чому не одним заходом —
// див. hub.cpp і webLoop().
bool hubGreetStep(uint8_t client, uint16_t step, uint32_t *evSeq);  // evSeq — курсор подій клієнта
void hubWebText(uint8_t client, const char *text);          // рядок від сторінки
void hubRestore();                                          // події з файлу в пам'ять

// ---------- час ----------
// Nanit не має годинника: час у будинок приносить ESP (див. timesync.cpp)
void timeBegin();
void timeLoop(uint32_t now);
bool timeValid();
uint32_t timeNow();   // unix-час UTC, 0 — ще не синхронізовано
void timeResend();    // Nanit перезапустився й забув час — надіслати негайно
int16_t timeOffset();  // поточний зсув від UTC у хвилинах, з урахуванням літнього часу
void timeSetZone(const char *rule);

// ---------- веб ----------
void webBegin();
void webLoop(uint32_t now);
void wsSend(const char *line);               // рядок усім відкритим сторінкам
void wsSendEv(const char *line);             // подія — лише привітаним сторінкам
void wsSendTo(uint8_t client, const char *line);
// Рядок журналу всім, крім сторінок, які саме дістають історію: до них він прийде своєю
// чергою за номером, інакше був би дублем поперед старіших рядків.
void wsSendLog(const char *line);
void wsCloseAll();                            // від'єднати сторінки (перед OTA)
bool wsHasClients();

// ---------- оновлення ----------
void otaAttach();  // веб-оновлення живе в тому ж сервері, що й сторінка
void otaLoop();
bool otaBusy();  // під час заливки все інше мовчить
void otaSetPassword();  // перечитати cfg.ota для POST /update без перезапуску
