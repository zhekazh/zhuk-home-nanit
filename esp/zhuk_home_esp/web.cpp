// HTTP-сервер і WebSocket. На «/» — сторінка інтерфейсу, вшита у прошивку стиснутою;
// на «/setup» — проста службова: стан мережі, кадри від Nanit, консоль і форма Wi-Fi.
// Усі точки, формати відповідей і що закриває службовий пароль — docs/api.md.
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

#include <WebSocketsServer.h>

#include "web_app.h"
#include "zhe.h"

ESP8266WebServer server(80);  // не static: ota.cpp чіпляє до нього /update

// Писати в клієнта, чий буфер відправки повний, не можна: WebSockets::write чекає місця в
// циклі аж до WEBSOCKETS_TCP_TIMEOUT, а це 5 секунд (WebSockets.h:124) — і весь цей час
// стоїть loop(). За п'ять секунд Nanit рахує ESP зниклим і чистить свою чергу подій, тож ті
// події не потраплять у журнал уже ніколи; заразом переповнюється буфер прийому UART, а
// вікно повторів команди виходить за строк кешу Nanit. У фазі привітання, де за прохід іде
// до восьми повідомлень, затримка ще й множилася на вісім. Досить одного телефона з поганим
// сигналом або згорнутої вкладки, у якої не розсосалось вікно TCP.
//
// Бібліотека тримає TCP-клієнта в protected-полі, тож дістаємо його підкласом — це дешевше
// за власну копію стану з'єднань.
class WsServer : public WebSocketsServer {
 public:
  explicit WsServer(uint16_t port) : WebSocketsServer(port) {}
  // Скільки байтів клієнт справді може прийняти зараз (tcp_sndbuf у lwIP). 0 — слот вільний,
  // клієнт мертвий або вікно відправки заповнене.
  int txRoom(uint8_t num) {
    if (num >= WEBSOCKETS_SERVER_CLIENT_MAX) return 0;
    WSclient_t &c = _clients[num];
    if (c.status != WSC_CONNECTED || !c.tcp || !c.tcp->connected()) return 0;
    return c.tcp->availableForWrite();
  }
};

static WsServer ws(81);
static uint8_t clients;

// Клієнт, який не забирає дані, тримає слот і гальмує всіх. Його кадри пропускаємо (кадр
// стану однаково прийде наступної секунди), а якщо вікно не розсмоктується кілька секунд —
// від'єднуємо: сторінка перепідключиться сама, і будинок не стоїть через один телефон.
static uint32_t stuckSince[WEBSOCKETS_SERVER_CLIENT_MAX];
static const uint32_t STUCK_DROP = 3000;
static const size_t WS_OVERHEAD = 8;  // заголовок кадру WebSocket

static bool wsReady(uint8_t num, size_t len) {
  int room = ws.txRoom(num);
  if (room <= 0) return false;  // вільний слот, мертвий клієнт або зовсім немає місця
  if ((size_t)room >= len + WS_OVERHEAD) {
    stuckSince[num] = 0;
    return true;
  }
  uint32_t now = millis();
  if (!stuckSince[num]) {
    stuckSince[num] = now;
  } else if (now - stuckSince[num] >= STUCK_DROP) {
    logf("ws: клієнт %u не забирає дані, від'єдную", (unsigned)num);
    stuckSince[num] = 0;
    ws.disconnect(num);
  }
  return false;
}

// На WebSocket не діє same-origin: стороння сторінка в браузері господаря може відкрити
// ws://<адреса-модуля>:81/ напряму, без preflight, і слати команди — код охорони для «відчинити
// ворота» чи «увімкнути вентилятор» не потрібен. Модель загроз у docs/api.md, розділ 6,
// говорить лише про CORS для HTTP, і для WebSocket той аргумент не працює взагалі.
//
// Тому дивимось на Origin. Заголовок не робимо обов'язковим: браузер надсилає його завжди, а
// скрипти (esp/ws.py, esp/tap.py) — ні, і для них перевірка зайва, бо CSRF з не-браузера не
// буває. Пускаємо лише приватні адреси й власні імена модуля: сторінка з ESP, локальний
// перегляд через ?esp= і сусідній комп'ютер у тій самій мережі працюють, а сайт з інтернету —
// ні, бо Origin він підробити не може.
static bool originAllowed(String origin) {
  int p = origin.indexOf("//");
  String h = p < 0 ? origin : origin.substring(p + 2);
  int cut = h.indexOf(':');
  if (cut >= 0) h = h.substring(0, cut);
  cut = h.indexOf('/');
  if (cut >= 0) h = h.substring(0, cut);
  h.toLowerCase();
  if (h == "localhost" || h.startsWith("127.")) return true;
  if (h.startsWith("10.") || h.startsWith("192.168.")) return true;
  if (h.startsWith("172.")) {  // приватним є лише 172.16–172.31
    int dot = h.indexOf('.', 4);
    long second = h.substring(4, dot < 0 ? h.length() : dot).toInt();
    if (second >= 16 && second <= 31) return true;
  }
  String me = netHost();
  me.toLowerCase();
  return h == me || h == me + ".local";
}

bool wsHasClients() { return clients > 0; }

void wsSend(const char *line) {
  if (!clients) return;
  size_t len = strlen(line);
  for (uint8_t n = 0; n < WEBSOCKETS_SERVER_CLIENT_MAX; n++)
    if (wsReady(n, len)) ws.sendTXT(n, line);
}

void wsSendTo(uint8_t client, const char *line) {
  if (wsReady(client, strlen(line))) ws.sendTXT(client, line);
}

// ---------- привітання нової сторінки ----------
// Разом воно займає до 86 повідомлень: до 40 рядків журналу модуля, 6 кадрів знімка стану
// й до 40 подій — ≈7 КБ у буфер TCP і стрибок купи там, де вільних близько 29 КБ (саме
// через брак цієї пам'яті рвалася OTA — див. wsCloseAll нижче). Тому віддаємо порціями по
// кілька рядків за прохід webLoop(), тримаючи курсор на кожного клієнта окремо.
//
// Порядок віддачі незмінний: журнал самого ESP, потім знімок стану, потім події. Сторінка
// спершу мусить знати, що з модулем, і лише тоді розбирати будинок та його історію.
//
// Журнал віддається за НОМЕРОМ рядка, а не за індексом у кільці: поки сторінка дістає
// історію, у кільце можуть додатися рядки, і всі індекси зсунуться. Фаза журналу триває,
// поки курсор не дожене поточний logSeq() — тоді й рядки, що з'явилися за цей час, доходять
// у хронологічному порядку. Живу розсилку LOG сторінка в цій фазі не отримує (wsSendLog),
// інакше свіжий рядок прийшов би до неї поперед старіших: сторінка рахує стрибок мітки часу
// назад за перезапуск модуля й малювала б «— модуль перезавантажився —» при кожному
// відкритті, заразом скидаючи свій фільтр дублікатів.
//
// Клієнтів у WebSocketsServer щонайбільше WEBSOCKETS_SERVER_CLIENT_MAX (у цій бібліотеці
// п'ять), і номер клієнта — це індекс у її власному масиві, тож курсори тримаємо так само.
static const uint8_t GREET_PER_LOOP = 8;
static const size_t GREET_ROOM = 192;  // запас під найдовший кадр привітання (LOG, EVT)
static const uint16_t GREET_DONE = 0xFFFF;
static uint16_t greetAt[WEBSOCKETS_SERVER_CLIENT_MAX];     // крок знімка й подій
static uint32_t greetLogAt[WEBSOCKETS_SERVER_CLIENT_MAX];  // номер наступного рядка журналу
static bool greetLogging[WEBSOCKETS_SERVER_CLIENT_MAX];    // фаза журналу ще триває
static uint32_t greetEvSeq[WEBSOCKETS_SERVER_CLIENT_MAX];  // номер наступної події для клієнта

static void greetReset() {
  for (uint8_t n = 0; n < WEBSOCKETS_SERVER_CLIENT_MAX; n++) {
    greetAt[n] = GREET_DONE;
    greetLogging[n] = false;
  }
}

static void greetLoop() {
  uint8_t budget = GREET_PER_LOOP;  // бюджет спільний на всіх: пам'ять теж спільна
  for (uint8_t n = 0; n < WEBSOCKETS_SERVER_CLIENT_MAX && budget; n++) {
    while (budget && greetAt[n] != GREET_DONE) {
      // Місце питаємо до того, як курсор зрушить: logSince() і hubGreetStep() посувають його
      // самі, тож кадр, який не влізе в буфер, пропав би разом зі своїм кроком — сторінка
      // недоотримала б рядок журналу або частину знімка.
      if (!wsReady(n, GREET_ROOM)) break;  // цей клієнт дочекається наступного проходу
      if (greetLogging[n]) {
        const char *line = logSince(&greetLogAt[n]);
        if (!line) {
          greetLogging[n] = false;  // журнал віддано, далі знімок стану
          continue;                 // перехід бюджету не витрачає
        }
        char frame[LOG_LEN + 8];
        snprintf(frame, sizeof(frame), "LOG,%s", line);
        ws.sendTXT(n, frame);
      } else if (hubGreetStep(n, greetAt[n], &greetEvSeq[n])) {
        greetAt[n]++;
      } else {
        greetAt[n] = GREET_DONE;
        break;
      }
      budget--;
    }
  }
}

// Подія йде лише тим сторінкам, які вже привітались: та, що саме дістає історію, отримає її
// кадром EVT у своїй черзі. Те саме правило, що й для рядків консолі нижче.
void wsSendEv(const char *line) {
  for (uint8_t n = 0; n < WEBSOCKETS_SERVER_CLIENT_MAX; n++) {
    if (greetAt[n] != GREET_DONE) continue;
    if (wsReady(n, strlen(line))) ws.sendTXT(n, line);
  }
}

void wsSendLog(const char *line) {
  for (uint8_t n = 0; n < WEBSOCKETS_SERVER_CLIENT_MAX; n++) {
    if (greetLogging[n]) continue;  // ще дістає історію: цей рядок прийде до неї за номером
    if (wsReady(n, strlen(line))) ws.sendTXT(n, line);
  }
}

// Перед заливкою прошивки сторінки від'єднуємо. Відкрита сторінка тримає WebSocket-клієнта,
// і його буфери плюс буфер оновлення не вміщаються у вільні ~29 КБ: заливка рвалася
// посеред передачі, а модуль виходив з мережі до перезапуску живлення. Перевірено на собі.
void wsCloseAll() {
  ws.disconnect();
  clients = 0;
  greetReset();  // сторінок більше немає, вітати нікого
}

// ---------- службова сторінка ----------
// Без кирилиці в PROGMEM зайвих байтів не шкода, але charset однаково потрібен:
// сторінка інтерфейсу буде українською, тож заголовок віддаємо правильний одразу.
static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zhuk Home ESP</title>
<style>
body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}
h1{font-size:18px;margin:0 0 12px}h2{font-size:15px;margin:20px 0 8px;color:#9cf}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:0}
dt{color:#888}dd{margin:0;font-family:ui-monospace,monospace}
#log,#frames{background:#000;border:1px solid #333;border-radius:6px;padding:8px;height:30vh;
overflow:auto;font:12px/1.4 ui-monospace,monospace;white-space:pre-wrap}
form{display:grid;gap:8px;max-width:320px;margin-top:8px}
input,button{font:inherit;padding:8px;border-radius:6px;border:1px solid #444;background:#1c1c1c;color:#eee}
button{background:#2b6;border:0;color:#042;font-weight:600;cursor:pointer}
.ok{color:#6d6}.bad{color:#f66}
</style></head><body>
<h1>Zhuk Home ESP <span id="v"></span></h1>
<dl>
<dt>Мережа</dt><dd id="ssid">—</dd>
<dt>Адреса</dt><dd id="ip">—</dd>
<dt>Сигнал</dt><dd id="rssi">—</dd>
<dt>Вільна RAM</dt><dd id="ram">—</dd>
<dt>Час роботи</dt><dd id="up">—</dd>
<dt>Зв'язок</dt><dd id="wsst" class="bad">немає</dd>
<dt>Nanit</dt><dd id="nanit">—</dd>
</dl>
<h2>Кадри від Nanit</h2>
<div id="frames"></div>
<h2>Wi-Fi</h2>
<form id="f"><input name="ssid" placeholder="мережа" autocomplete="off">
<input name="pass" type="password" placeholder="пароль" autocomplete="off">
<button>Зберегти й перезапустити</button></form>
<h2>Оновлення прошивки</h2>
<form method="POST" action="/update" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin"><button>Залити .bin</button></form>
<h2>Консоль</h2>
<div id="log"></div>
<script>
const $=i=>document.getElementById(i),log=$('log'),frames=$('frames');
let ws;
function line(t){const d=document.createElement('div');d.textContent=t;log.append(d);
  while(log.children.length>200)log.firstChild.remove();log.scrollTop=log.scrollHeight}
function connect(){
  ws=new WebSocket('ws://'+location.hostname+':81/');
  ws.onopen=()=>{$('wsst').textContent='є';$('wsst').className='ok'};
  ws.onclose=()=>{$('wsst').textContent='немає';$('wsst').className='bad';setTimeout(connect,2000)};
  ws.onmessage=e=>{for(const l of e.data.split('\n')){
    const p=l.split(',');
    if(p[0]=='LOG')line(l.slice(4));
    else if(p[0]=='NANIT'){$('nanit').textContent=p[1]=='1'
      ?`на зв'язку · fw ${p[2]} · кадрів ${p[3]} · битих ${p[4]}`:'не відповідає';
      $('nanit').className=p[1]=='1'?'ok':'bad'}
    else if('SEC ENV DEV PRK SYS HI EV RSP ACK NAK VAL HW TR IO KEY'.includes(p[0])){
      const d=document.createElement('div');d.textContent=l;frames.prepend(d);
      while(frames.children.length>14)frames.lastChild.remove()}
    else if(p[0]=='ESP'){$('ssid').textContent=p[1];$('ip').textContent=p[2];
      $('rssi').textContent=p[3]=='0'?'—':p[3]+' дБм';$('ram').textContent=p[4]+' Б';
      $('up').textContent=p[5]+' с';$('v').textContent=p[6]}
  }};
}
connect();
$('f').onsubmit=e=>{e.preventDefault();
  const d=new URLSearchParams(new FormData($('f')));
  fetch('/wifi',{method:'POST',body:d}).then(r=>r.text()).then(line)};
</script></body></html>)HTML";

// За RFC 7232 тег мусить бути в лапках: без них він недійсний. Браузери таке терплять, а
// кеш або проксі по дорозі — не завжди, і сторінка тоді або не кешується, або не оновлюється.
// Саме значення приходить готовим із web_app.h (його збирає web/pack.py), тож лапки тут.
#define WEB_APP_ETAG_Q "\"" WEB_APP_ETAG "\""

// Сторінка інтерфейсу лежить у флеші стиснутою й віддається як є: браузер розпаковує сам.
// Розпаковувати на ESP означало б тримати 250 КБ у 40 КБ вільної пам'яті — неможливо.
static void handleApp() {
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.sendHeader(F("ETag"), F(WEB_APP_ETAG_Q));
  if (server.header("If-None-Match") == F(WEB_APP_ETAG_Q)) {  // сторінка не змінилась
    server.send(304);
    return;
  }
  server.send_P(200, PSTR("text/html; charset=utf-8"), (const char *)WEB_APP_GZ, WEB_APP_LEN);
}

// Службова сторінка: стан, консоль, форма Wi-Fi і заливка .bin. Це запасний вхід на
// випадок, коли основна сторінка не відкривається: вона важить 71 КБ, а ця — кілька.
static void handleSetup() {
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

// Основну сторінку віддаємо в обох режимах. У власній точці доступу Nanit так само на
// зв'язку по UART, тож будинок видно, а мережа налаштовується прямо там: «Налаштування»
// → Wi-Fi працює через HTTP (/scan, /wifi), не через кадри протоколу. Керування в цьому
// режимі лишається вимкненим — точка без пароля, і скористатися ним міг би будь-хто поруч.
// Проста службова сторінка нікуди не поділась: вона на /setup, на випадок коли основна
// не відкривається — наприклад, у вбудованому браузері captive-порталу.
static void handleRoot() { handleApp(); }

// У режимі точки доступу будь-яке ім'я веде сюди — інакше телефон вважає,
// що інтернету немає, і тихо від'єднується від нашої мережі.
static void handleNotFound() {
  if (net.mode == NM_AP) {
    server.sendHeader(F("Location"), String(F("http://")) + WiFi.softAPIP().toString() + F("/"));
    server.send(302, F("text/plain"), F(""));
    return;
  }
  server.send(404, F("text/plain; charset=utf-8"), F("немає такої сторінки"));
}

// Службові дії — зміна мережі, перезапуск, скидання, оновлення прошивки — під паролем,
// якщо він заданий. Порожній пароль означає «довіряю своїй мережі»: тоді будь-хто з неї
// може перепрошити модуль або знести налаштування Wi-Fi, і сторінка про це попереджає.
static bool allowService() {
  if (!cfg.ota[0]) return true;
  if (server.authenticate("zhuk", cfg.ota)) return true;
  server.requestAuthentication();
  logPrint("web: службову дію відхилено, невірний пароль");
  return false;
}

// Пароль задається й знімається тільки цією точкою, і лише тим, хто вже його знає
static void handlePass() {
  if (!allowService()) return;
  if (!server.hasArg("pass")) {
    server.send(400, F("text/plain; charset=utf-8"), F("потрібне поле pass"));
    return;
  }
  cfgSetStr(cfg.ota, sizeof(cfg.ota), server.arg("pass").c_str());
  bool ok = cfgSave();
  otaSetPassword();  // POST /update закривається (чи відкривається) тим самим запитом
  logPrint(cfg.ota[0] ? "web: службовий пароль встановлено" : "web: службовий пароль знято");
  server.send(ok ? 200 : 500, F("text/plain; charset=utf-8"),
              ok ? F("збережено, перезапускаюсь") : F("не вдалося зберегти"));
  if (ok) {
    // Перезапуск — через espota: ArduinoOTA дає змінити пароль лише до begin(), тож інакше
    // заливка по Wi-Fi лишалася б зі старим паролем (або без нього), хоч сторінка вже
    // показує «пароль заданий».
    evStoreFlush();
    delay(300);  // дати відповіді піти в мережу
    ESP.restart();
  }
}

static void handleWifi() {
  if (!allowService()) return;
  if (!server.hasArg("ssid")) {
    server.send(400, F("text/plain; charset=utf-8"), F("потрібне поле ssid"));
    return;
  }
  cfgSetStr(cfg.ssid, sizeof(cfg.ssid), server.arg("ssid").c_str());
  cfgSetStr(cfg.pass, sizeof(cfg.pass), server.arg("pass").c_str());
  bool ok = cfgSave();
  server.send(ok ? 200 : 500, F("text/plain; charset=utf-8"),
              ok ? F("збережено, перезапускаюсь") : F("не вдалося зберегти"));
  if (ok) {
    evStoreFlush();
    netRemember();
    logf("wifi: нова мережа %s, перезапуск", cfg.ssid);
    delay(300);  // дати відповіді піти в мережу
    ESP.restart();
  }
}

// Список мереж для сторінки. Сканування блокує цикл приблизно на дві секунди, тому це
// окремий запит на вимогу, а не щось періодичне: зате в решту часу цикл вільний. Порахувати
// ESP зниклим Nanit за цей час не встигає — у нього строк 5 с, і наступний PING укладається
// в нього. Рядки — «ssid \t рівень \t чи з паролем».
//
// Під службовим паролем: список сусідніх мереж — це вже відомості про околиці будинку,
// і віддавати їх кожному, хто дістався сторінки, ні до чого.
static void handleScan() {
  if (!allowService()) return;
  int n = WiFi.scanNetworks();
  String out;
  out.reserve(512);
  for (int i = 0; i < n && i < 20; i++) {
    out += WiFi.SSID(i);
    out += '\t';
    out += WiFi.RSSI(i);
    out += '\t';
    out += (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? '0' : '1';
    out += '\n';
  }
  WiFi.scanDelete();
  logf("scan: мереж %d", n);
  server.send(200, F("text/plain; charset=utf-8"), out);
}

// Часовий пояс. Правило й поточний зсув сторінка бере запитом, а не з кадрів протоколу:
// питання це не будинку, а самого модуля — як і Wi-Fi, і в кадри стану воно не лягає.
// GET віддає «<правило>\t<зсув у хвилинах>», POST приймає поле tz.
static void handleTz() {
  if (server.method() == HTTP_POST) {
    if (!allowService()) return;  // GET лишається відкритим: він лише читає
    String rule = server.arg("tz");
    rule.trim();
    if (rule.length() < 3 || rule.length() >= sizeof(cfg.tz)) {
      server.send(400, F("text/plain; charset=utf-8"), F("правило зони порожнє або задовге"));
      return;
    }
    for (size_t i = 0; i < rule.length(); i++) {  // у правилі лише друковані латинські знаки
      char c = rule[i];
      if (c < 0x21 || c > 0x7E) {
        server.send(400, F("text/plain; charset=utf-8"), F("у правилі зони є неприпустимі знаки"));
        return;
      }
    }
    timeSetZone(rule.c_str());
    bool ok = cfgSave();
    server.send(ok ? 200 : 500, F("text/plain; charset=utf-8"),
                ok ? F("збережено") : F("не вдалося зберегти"));
    return;
  }
  char out[64];  // третє поле — чи є вже час від NTP: без нього зсув ні про що не каже
  snprintf(out, sizeof(out), "%s\t%d\t%d\n", cfg.tz, timeOffset(), timeValid() ? 1 : 0);
  server.send(200, F("text/plain; charset=utf-8"), out);
}

static void handleReboot() {
  if (!allowService()) return;
  evStoreFlush();  // інакше загубимо саме ті події, через які перезапускають
  server.send(200, F("text/plain; charset=utf-8"), F("перезапускаюсь"));
  logPrint("web: перезапуск на вимогу");
  delay(300);  // дати відповіді піти в мережу
  ESP.restart();
}

// Скидання забирає і мережу, і копію в пам'яті SDK — інакше модуль підключився б назад
// до старої мережі, і «скинути Wi-Fi» виглядало б як дія, що нічого не зробила.
static void handleResetWifi() {
  if (!allowService()) return;
  evStoreFlush();
  cfgDefaults();
  cfgSave();
  WiFi.persistent(true);
  WiFi.disconnect(true);
  WiFi.persistent(false);
  server.send(200, F("text/plain; charset=utf-8"), F("мережу скинуто, перезапускаюсь"));
  logPrint("web: мережу скинуто");
  delay(300);
  ESP.restart();
}

void webBegin() {
  server.on("/", handleRoot);
  server.on("/setup", handleSetup);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/scan", handleScan);
  server.on("/tz", handleTz);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/reset", HTTP_POST, handleResetWifi);
  server.on("/pass", HTTP_POST, handlePass);
  server.collectHeaders("If-None-Match");  // щоб відповідати 304, коли сторінка не змінилась
  server.on("/generate_204", handleNotFound);       // Android
  server.on("/hotspot-detect.html", handleNotFound);  // Apple
  server.onNotFound(handleNotFound);
  otaAttach();  // /update реєструється в цьому ж сервері
  server.begin();

  greetReset();
  ws.begin();
  // Список обов'язкових заголовків лишаємо порожнім: бібліотека кличе перевірку для кожного
  // заголовка, який отримала, а вимагати Origin означало б відкинути скрипти без нього.
  ws.onValidateHttpHeader([](String name, String value) -> bool {
    if (!name.equalsIgnoreCase("Origin")) return true;
    if (originAllowed(value)) return true;
    logf("ws: відкинуто з'єднання з Origin %s", value.c_str());
    return false;
  }, nullptr, 0);
  ws.onEvent([](uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
    if (num >= WEBSOCKETS_SERVER_CLIENT_MAX) return;  // курсор привітання за межі не вийде
    if (type == WStype_CONNECTED) {
      // Привітання (журнал, знімок стану, події) віддає greetLoop() порціями, щоб сторінка
      // не починала з нуля, а купа в модулі не стрибала на 7 КБ у колбеку. Курсор ставимо
      // на найстаріший наявний рядок журналу — і саме до logf нижче: цей рядок сторінка має
      // побачити після історії, на своєму часі, а не поперед неї.
      greetLogAt[num] = logSeq() - logCount();
      greetLogging[num] = true;
      greetAt[num] = 0;
      greetEvSeq[num] = 0;  // hubGreetStep підтягне його до найстарішої наявної події
      stuckSince[num] = 0;
      clients++;
      logf("ws: клієнт %u підключився (%u)", num, clients);
    } else if (type == WStype_TEXT) {
      char text[96];
      size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
      memcpy(text, payload, n);
      text[n] = 0;
      hubWebText(num, text);
    } else if (type == WStype_DISCONNECTED) {
      greetAt[num] = GREET_DONE;  // недовіталися: слот піде під наступного клієнта
      greetLogging[num] = false;
      stuckSince[num] = 0;
      if (clients) clients--;
      logf("ws: клієнт %u від'єднався (%u)", num, clients);
    }
  });
  logPrint("web: сервер на :80, ws на :81");
}

void webLoop(uint32_t now) {
  server.handleClient();
  ws.loop();
  greetLoop();  // привітання нових сторінок — по кілька рядків за прохід

  static uint32_t statusAt;
  if (clients && now - statusAt >= 1000) {
    statusAt = now;
    // Не FRAME_MAX: те обмеження стосується кадрів до Nanit, а тут ім'я мережі саме
    // може зайняти 32 байти, і при довгому SSID кадр обрізало б на відпечатку сторінки.
    char frame[160];
    // Передостаннє поле — відпечаток вшитої сторінки. Сторінка порівнює його зі своїм і,
    // якщо прошивку оновили, сама просить перезавантажитись: інакше стара вкладка тихо не
    // працює, і це виглядає як несправність заліза. Саме на цьому ми вже губили час.
    // Останнє — режим мережі: у власній точці доступу сторінка мусить пояснити, чому
    // керування вимкнене, а не вдавати, що кнопки просто не спрацьовують.
    // Десяте поле — чи заданий службовий пароль. Сторінка інакше не змогла б сказати, що
    // перепрошити модуль і знести налаштування може будь-хто з цієї мережі
    snprintf(frame, sizeof(frame), "ESP,%s,%s,%d,%u,%lu,%s,%s,%s,%d", net.ssid[0] ? net.ssid : "-",
             net.ip[0] ? net.ip : "-", net.rssi, ESP.getFreeHeap(), (unsigned long)(now / 1000), ZHE_FW_VERSION,
             WEB_APP_ETAG, net.mode == NM_STA ? "sta" : (net.mode == NM_AP ? "ap" : "off"),
             cfg.ota[0] ? 1 : 0);
    ws.broadcastTXT(frame);
    snprintf(frame, sizeof(frame), "NANIT,%d,%s,%lu,%lu", nanit.online ? 1 : 0, nanit.fw,
             (unsigned long)nanit.frames, (unsigned long)nanit.bad);
    ws.broadcastTXT(frame);
  }
}
