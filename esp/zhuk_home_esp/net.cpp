// Wi-Fi: підключення до мережі, власна точка доступу з перехопленням DNS, імена в мережі.
#include <DNSServer.h>
#include <ESP8266LLMNR.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "zhe.h"

extern "C" {
#include "user_interface.h"  // wifi_station_get_connect_status: точна причина відмови
}

Net net;

static DNSServer dns;
static bool apOn;        // точка доступу піднята
static bool mdnsOn;      // імена зареєстровані
static uint32_t tryAt;   // коли почалась спроба підключення
static uint32_t apTryAt; // коли з точки доступу останній раз пробували мережу
static uint32_t rssiAt;  // коли останній раз читали рівень сигналу
static char host[25];
static char apName[33];

static const uint32_t STA_TIMEOUT = 25000;  // стільки чекаємо мережу на одну спробу
// Щоб спробувати мережу, точку доступу треба згорнути: ESP8266 тримає AP і STA лише на
// одному каналі. Поки до точки хтось підключений — найімовірніше, людина саме вписує пароль
// Wi-Fi — рвати її раз на пів хвилини не можна. А поки нікого немає, рвати нічого, і коротка
// втрата роутера коштує 30 с замість двох хвилин без вебінтерфейсу й з новою адресою DHCP.
static const uint32_t STA_RETRY = 120000;      // з клієнтом у точці доступу
static const uint32_t STA_RETRY_IDLE = 30000;  // поки точка доступу нікому не потрібна
static const uint8_t STA_TRIES = 2;         // після OTA роутер інколи відповідає не з першого разу
static uint8_t staTries;

const char *netHost() { return host; }

// ---- імена: zhukhome.local (mDNS) і zhukhome (LLMNR, Windows) ----
static void namesBegin() {
  if (mdnsOn) return;
  MDNS.begin(host);
  MDNS.addService("http", "tcp", 80);
  LLMNR.begin(host);
  mdnsOn = true;
  logf("net: http://%s.local", host);
}

// Точку доступу перед спробою обов'язково згортаємо: ESP8266 тримає AP і STA лише на
// одному каналі, тож із піднятою точкою підключення до роутера на іншому каналі
// не вдається ніколи — модуль замикає себе у власній мережі.
static void apStop();

static void staStart() {
  if (apOn) apStop();
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  tryAt = millis();
  staTries++;
  logf("net: під'єднуюсь до %s (спроба %u)", cfg.ssid, staTries);
}

// SDK знає, чому саме не вийшло: без цього «мережа не відповіла» нічого не пояснює.
static const char *staWhy() {
  switch (wifi_station_get_connect_status()) {
    case STATION_WRONG_PASSWORD: return "pass";
    case STATION_NO_AP_FOUND: return "nossid";
    case STATION_CONNECT_FAIL: return "fail";
    case STATION_IDLE: return "idle";
    case STATION_CONNECTING: return "slow";
    case STATION_GOT_IP: return "ok";
    default: return "-";
  }
}

// Один огляд ефіру при невдачі: видно, чи мережу взагалі чути й наскільки слабко.
// Сканування блокує цикл на пару секунд, тому робимо його лише в цей момент.
static void scanReport() {
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    logPrint("net: в ефірі жодної мережі");
    return;
  }
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool mine = WiFi.SSID(i) == cfg.ssid;
    if (mine) found = true;
    if (i < 5 || mine)
      logf("net: %s%s %d дБм кан.%d", mine ? "* " : "  ", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
           WiFi.channel(i));
  }
  if (!found) logf("net: мережі %s в ефірі немає (бачу %d інших)", cfg.ssid, n);
  WiFi.scanDelete();
}

static void apStart() {
  IPAddress ip(192, 168, 4, 1);
  WiFi.mode(WIFI_AP);  // тільки точка доступу: змішаний режим прив'язав би STA до її каналу
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName);  // без пароля: у цьому режимі сторінка дає лише налаштування
  apOn = true;
  net.mode = NM_AP;
  net.up = true;
  net.rssi = 0;
  net.since = millis();
  strlcpy(net.ssid, apName, sizeof(net.ssid));
  strlcpy(net.ip, "192.168.4.1", sizeof(net.ip));
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", ip);  // будь-яке ім'я веде на нашу сторінку
  apTryAt = millis();
  logf("net: точка доступу %s, 192.168.4.1", apName);
}

static void apStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  logPrint("net: точку доступу згорнуто");
}

static void staUp() {
  staTries = 0;
  strlcpy(net.why, "ok", sizeof(net.why));
  net.mode = NM_STA;
  net.up = true;
  net.since = millis();
  strlcpy(net.ssid, cfg.ssid, sizeof(net.ssid));
  strlcpy(net.ip, WiFi.localIP().toString().c_str(), sizeof(net.ip));
  net.rssi = WiFi.RSSI();
  logf("net: %s, %s, %d дБм", net.ssid, net.ip, net.rssi);
  if (apOn) apStop();
  namesBegin();
}

void netBegin() {
  memset(&net, 0, sizeof(net));
  net.mode = NM_BOOT;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apName, sizeof(apName), "ZhukHome-%02X%02X", mac[4], mac[5]);
  strlcpy(host, cfg.host[0] ? cfg.host : "zhukhome", sizeof(host));

  WiFi.persistent(false);  // основне сховище — наш файл; у флеш SDK пишемо лише копію
  WiFi.setAutoReconnect(true);
  // Без цього модем спить між маячками роутера й відповідає із затримкою до 300 мс:
  // натискання у вебінтерфейсі відчувалося як гальмування. Ціна — близько 70 мА постійно.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.hostname(host);
  if (cfg.ssid[0]) {
    staStart();
    return;
  }
  // Файл конфігу міг пропасти разом із LittleFS — наприклад, коли змінилась розкладка
  // флешу. Копія в пам'яті SDK від розкладки не залежить, тож пробуємо її, перш ніж
  // здаватись і піднімати точку доступу.
  WiFi.mode(WIFI_STA);
  if (WiFi.SSID().length()) {
    strlcpy(cfg.ssid, WiFi.SSID().c_str(), sizeof(cfg.ssid));
    // Пароль теж, інакше cfg.pass лишається порожнім: перша ж наступна спроба піде як
    // WiFi.begin(ssid, "") і дасть WRONG_PASSWORD, а перше збереження з веба запише
    // порожній пароль у файл — і тоді мережа не піднімається більше ніколи, бо cfg.ssid
    // уже не порожній і цей запасний шлях не спрацьовує.
    strlcpy(cfg.pass, WiFi.psk().c_str(), sizeof(cfg.pass));
    logf("net: конфігу немає, беру мережу з пам'яті SDK: %s", cfg.ssid);
    WiFi.begin();  // пароль SDK підставить сам
    tryAt = millis();
    staTries++;
    return;
  }
  logPrint("net: мережу не налаштовано");
  apStart();
}

// Копія облікових даних у флеш SDK: страховка на випадок втрати файла конфігу.
void netRemember() {
  WiFi.persistent(true);
  WiFi.begin(cfg.ssid, cfg.pass);
  WiFi.persistent(false);
}

void netLoop(uint32_t now) {
  if (apOn) dns.processNextRequest();
  if (mdnsOn) MDNS.update();

  bool connected = WiFi.status() == WL_CONNECTED;

  switch (net.mode) {
    case NM_BOOT:
      if (connected) staUp();
      else if (now - tryAt > STA_TIMEOUT) {
        strlcpy(net.why, staWhy(), sizeof(net.why));
        logf("net: не підключився (%s)", net.why);
        if (staTries < STA_TRIES) {
          staStart();  // друга спроба без точки доступу: вона може забрати канал
          break;
        }
        scanReport();
        apStart();
      }
      break;

    case NM_STA:
      if (!connected) {  // роутер зник: SDK відновлює сам, ми лише чекаємо
        logPrint("net: зв'язок втрачено");
        net.up = false;
        net.mode = NM_BOOT;
        tryAt = now;
        // staTries обнуляти тут не треба: staUp() зробив це, щойно мережа піднялась, тож
        // перепідключення дістає свої дві спроби без точки доступу від нуля.
        break;
      }
      if (now - rssiAt > 5000) {  // rssi показуємо на сторінці, оновлюємо рідко
        net.rssi = WiFi.RSSI();
        rssiAt = now;
      }
      break;

    case NM_AP:
      if (connected) {  // мережа з'явилась сама
        staUp();
        break;
      }
      // Строк перевіряємо на місці, а не запам'ятовуємо дату наступної спроби: клієнт
      // міг підключитись уже після того, як точка піднялась, і тоді рвати її рано.
      if (cfg.ssid[0] &&
          now - apTryAt >= (WiFi.softAPgetStationNum() ? STA_RETRY : STA_RETRY_IDLE)) {
        apTryAt = now;
        staTries = 0;
        staStart();  // згортає точку доступу сам: інакше канали конфліктують
        net.mode = NM_BOOT;  // спроба триває; не вдасться — знову точка доступу
        tryAt = now;
      }
      break;
  }

  // у точці доступу імена теж потрібні: http://zhukhome.local працює й там
  if (net.up && !mdnsOn) namesBegin();
}
