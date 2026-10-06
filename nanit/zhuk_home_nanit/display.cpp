// Екран Nanit (ST7735, 160×128) і вбудований світлодіод стану.
// SPI-дисплей повільний, тому за один прохід циклу перемальовуємо щонайбільше
// один блок і лише ті символи, що змінилися. Шрифт без кирилиці — написи англійською.
#include <NanitLib.h>
#include "zh.h"

static const uint16_t C_BLACK = 0x0000, C_WHITE = 0xFFFF, C_GRAY = 0x8410, C_SLATE = 0x2945;
static const uint16_t C_GREEN = 0x07E0, C_DKGREEN = 0x0320, C_RED = 0xF800, C_ORANGE = 0xFD20;
static const uint16_t C_YELLOW = 0xFFE0, C_CYAN = 0x07FF;

// Розмір буфера тексту віджета: те саме число потрібне для Widget::last, буфера в
// dispUpdate і кожного snprintf_P, інакше зв'язок між ними тримався б на повторенні літерала
static const uint8_t TEXT_MAX = 27;

struct Widget {
  uint8_t x, y, size, cols;
  char last[TEXT_MAX];
  uint16_t fg, bg;
  uint8_t progress;  // скільки символів уже перемальовано в цьому заході
};

enum : uint8_t { W_CLOCK, W_STATUS, W_SEC, W_CLIMATE, W_CO, W_GATE, W_DEV, W_BOTTOM, W_COUNT };
static Widget widgets[W_COUNT] = {
    {2, 3, 1, 5, "", 0, 0, 0},     // годинник
    {74, 3, 1, 14, "", 0, 0, 0},   // Wi-Fi і батарея
    {8, 20, 2, 12, "", 0, 0, 0},   // охорона
    {8, 46, 2, 12, "", 0, 0, 0},   // температура й вологість
    {8, 66, 2, 12, "", 0, 0, 0},   // CO
    {4, 90, 1, 25, "", 0, 0, 0},   // шлагбаум
    {4, 101, 1, 25, "", 0, 0, 0},  // вентиляція, штора, світло
    {4, 117, 1, 25, "", 0, 0, 0},  // код, сервіс, IP
};
static const uint8_t SEC_BAR_Y = 15, SEC_BAR_H = 26;
static uint8_t nextWidget, staticStep, patternStep;
static uint32_t screenUntil;
static bool fullRedraw = true;

// Екран по SPI повільний: заливка всього поля — це ≈50 мс, за які переповнився б
// буфер прийому від ESP. Тому ділимо її на смуги, по одній за прохід циклу.
static bool drawStaticStep() {
  if (staticStep < 8) {
    tft.fillRect(0, staticStep * 16, 160, 16, C_BLACK);
    staticStep++;
    return false;
  }
  staticStep = 0;
  tft.drawFastHLine(0, 13, 160, C_SLATE);
  tft.drawFastHLine(0, 85, 160, C_SLATE);
  tft.drawFastHLine(0, 112, 160, C_SLATE);
  for (Widget &w : widgets) {
    w.last[0] = 0;
    w.fg = w.bg = 1;  // невідомі кольори: наступне малювання — повне
    w.progress = 0;
  }
  return true;
}

// Поля навколо тексту смуги охорони. Самі символи малюються з фоном,
// тож заливати всю смугу (≈11 мс) не потрібно.
static void fillSecMargins(uint16_t bg) {
  Widget &w = widgets[W_SEC];
  uint8_t textW = w.cols * 6 * w.size, textH = 8 * w.size;
  tft.fillRect(0, SEC_BAR_Y, 160, w.y - SEC_BAR_Y, bg);
  tft.fillRect(0, w.y + textH, 160, SEC_BAR_Y + SEC_BAR_H - (w.y + textH), bg);
  tft.fillRect(0, w.y, w.x, textH, bg);
  tft.fillRect(w.x + textW, w.y, 160 - w.x - textW, textH, bg);
}

// Малює змінені символи, не більше кількох за прохід; повертає true, коли завершив
static bool drawWidget(uint8_t id, const char *text, uint16_t fg, uint16_t bg) {
  Widget &w = widgets[id];
  bool full = fg != w.fg || bg != w.bg;
  if (full && id == W_SEC && w.progress == 0) fillSecMargins(bg);
  uint8_t step = 6 * w.size;
  uint8_t budget = w.size == 2 ? 6 : 12;  // ≈5 мс на прохід
  uint8_t len = strlen(text);
  uint8_t i = w.progress;
  for (; i < w.cols; i++) {
    char c = i < len ? text[i] : ' ';  // добиваємо пробілами, не читаючи за кінець рядка
    char old = w.last[i] ? w.last[i] : ' ';
    if (full || c != old) {
      if (!budget--) break;
      tft.drawChar(w.x + i * step, w.y, c, fg, bg, w.size);
    }
    w.last[i] = c;
  }
  if (i < w.cols) {
    w.progress = i;  // решту домалюємо наступного проходу
    return false;
  }
  w.last[w.cols] = 0;
  w.progress = 0;
  w.fg = fg;
  w.bg = bg;
  return true;
}

static bool widgetText(uint8_t id, uint32_t now, char *out, uint16_t *fg, uint16_t *bg) {
  *fg = C_WHITE;
  *bg = C_BLACK;
  switch (id) {
    case W_CLOCK:
      if (sys.timeValid) {
        uint32_t t = sys.unixBase + (now - sys.unixAt) / 1000 + (int32_t)sys.tzMin * 60;
        snprintf_P(out, TEXT_MAX, PSTR("%02u:%02u"), (unsigned)(t / 3600 % 24), (unsigned)(t / 60 % 60));
      } else {
        strcpy_P(out, PSTR("--:--"));
      }
      *fg = C_GRAY;
      return true;
    case W_STATUS: {
      char net[9];
      if (!espOnline(now)) {
        strcpy_P(net, PSTR("no ESP"));
        *fg = C_ORANGE;
      } else if (sys.net == NET_STA) {
        snprintf_P(net, sizeof(net), PSTR("WiFi%d"), sys.rssi);
      } else if (sys.net == NET_AP) {
        strcpy_P(net, PSTR("AP"));
        *fg = C_YELLOW;
      } else {
        strcpy_P(net, PSTR("no WiFi"));
        *fg = C_ORANGE;
      }
      snprintf_P(out, TEXT_MAX, PSTR("%9s %3u%%"), net, env.batt);
      return true;
    }
    case W_SEC: {
      uint32_t left = secondsLeft(sec.until, now);
      switch (sec.state) {
        case SEC_EXIT:
          snprintf_P(out, TEXT_MAX, PSTR("EXIT %lus"), left);
          *fg = C_BLACK;
          *bg = C_ORANGE;
          break;
        case SEC_ARM:
          strcpy_P(out, PSTR("ARMED"));
          *bg = C_DKGREEN;
          break;
        case SEC_NIGHT:
          strcpy_P(out, PSTR("NIGHT MODE"));
          *bg = C_DKGREEN;
          break;
        case SEC_ENTRY:
          snprintf_P(out, TEXT_MAX, PSTR("CODE! %lus"), left);
          *fg = C_BLACK;
          *bg = C_ORANGE;
          break;
        case SEC_ALARM:
          strcpy_P(out, PSTR("ALARM!"));
          *bg = C_RED;
          break;
        default:
          strcpy_P(out, sys.service ? PSTR("SERVICE") : PSTR("DISARMED"));
          *bg = sys.service ? C_ORANGE : C_SLATE;
          if (sys.service) *fg = C_BLACK;
          break;
      }
      return true;
    }
    case W_CLIMATE:
      if (env.dhtOk) snprintf_P(out, TEXT_MAX, PSTR("%3dC  %3d%%"), env.t, env.h);
      else strcpy_P(out, PSTR("  --C   --%"));
      return true;
    case W_CO:
      if (!env.coValid) {
        strcpy_P(out, PSTR("CO warm-up"));
        *fg = C_GRAY;
      } else {
        const char *lvl = env.coLvl == 2 ? PSTR("ALARM") : env.coLvl == 1 ? PSTR("high") : PSTR("ok");
        char l[7];
        strcpy_P(l, lvl);
        snprintf_P(out, TEXT_MAX, PSTR("CO %3u %s"), env.co, l);
        *fg = env.coLvl == 2 ? C_RED : env.coLvl == 1 ? C_ORANGE : C_GREEN;
      }
      return true;
    case W_GATE: {
      char st[8];
      pgmItem(N_GATES, prk.gateState, st, sizeof(st));
      if (prk.dist >= 0) snprintf_P(out, TEXT_MAX, PSTR("Gate %-7s  car %3dcm"), st, prk.dist);
      else snprintf_P(out, TEXT_MAX, PSTR("Gate %-7s  car  --"), st);
      *fg = C_CYAN;
      return true;
    }
    case W_DEV:
      snprintf_P(out, TEXT_MAX, PSTR("Fan %-3s Win %3u%% Lt %s"), dev.fanOn ? "on" : "off", curtainPct(),
                 dev.lightOn ? "on" : "off");
      *fg = C_GRAY;
      return true;
    default: {
      uint8_t k = secKeyLen();
      if (k) {
        char stars[7] = "******";
        stars[k] = 0;
        snprintf_P(out, TEXT_MAX, PSTR("Code: %s"), stars);
        *fg = C_YELLOW;
      } else if (sys.service) {
        uint32_t s = secondsLeft(sys.svcUntil, now);
        snprintf_P(out, TEXT_MAX, PSTR("Service mode %lu:%02lu"), s / 60, s % 60);
        *fg = C_ORANGE;
      } else if (env.coAlarm) {
        strcpy_P(out, PSTR("CO ALARM! press C to mute"));
        *fg = C_RED;
      } else if (sys.net != NET_OFF && espOnline(now)) {
        snprintf_P(out, TEXT_MAX, PSTR("IP %s"), sys.ip);
        *fg = C_GRAY;
      } else {
        strcpy_P(out, PSTR("Zhuk Home " ZH_FW_VERSION));
        *fg = C_GRAY;
      }
      return true;
    }
  }
}

// ---------- світлодіод на платі ----------
static uint32_t ledColor = 0xFFFFFFFFUL;

static void ledUpdate(uint32_t now) {
  uint32_t c;
  if (sec.state == SEC_ALARM || env.coAlarm) c = 0xFF0000UL;
  else if (sys.service) c = 0xFF8000UL;
  else if (!espOnline(now)) c = 0x0000FFUL;
  else c = 0x00FF00UL;
  if (c == ledColor) return;
  ledColor = c;
  START_NANIT.BuildinRGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 30);
}

// ---------- інтерфейс модуля ----------
void dispBegin() {
  tft.setRotation(1);
  tft.setTextWrap(false);
  fullRedraw = true;
}

void dispTestPattern(uint16_t ms) {
  patternStep = 1;  // смуги малюються по одній за прохід циклу
  screenUntil = millis() + ms;
  fullRedraw = true;
}

void dispUpdate(uint32_t now) {
  ledUpdate(now);
  if (patternStep) {  // кольорові смуги перевірки екрана
    static const uint16_t COLORS[] = {C_RED, C_GREEN, 0x001F};
    tft.fillRect(0, (patternStep - 1) * 43, 160, patternStep == 3 ? 42 : 43, COLORS[patternStep - 1]);
    patternStep = patternStep == 3 ? 0 : patternStep + 1;
    return;
  }
  if (pending(screenUntil, now)) return;
  if (fullRedraw) {
    if (drawStaticStep()) fullRedraw = false;
    return;
  }
  char text[TEXT_MAX];
  uint16_t fg, bg;
  for (uint8_t n = 0; n < W_COUNT; n++) {
    uint8_t id = (nextWidget + n) % W_COUNT;
    Widget &w = widgets[id];
    if (w.progress) {  // незавершений віджет домальовуємо першим
      widgetText(id, now, text, &fg, &bg);
      drawWidget(id, text, fg, bg);
      return;
    }
    widgetText(id, now, text, &fg, &bg);
    if (strcmp(text, w.last) || fg != w.fg || bg != w.bg) {
      if (drawWidget(id, text, fg, bg)) nextWidget = (id + 1) % W_COUNT;
      return;
    }
  }
}
