// Налаштування ESP у LittleFS: текстовий файл «ключ=значення» на рядок.
// Формат навмисно людночитабельний — файл можна прочитати й виправити з /fs.
#include <LittleFS.h>

#include "zhe.h"

Cfg cfg;
static const char CFG_PATH[] = "/cfg";
static bool mounted;

void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.host, "zhukhome", sizeof(cfg.host));
  strlcpy(cfg.ntp, "pool.ntp.org", sizeof(cfg.ntp));
  strlcpy(cfg.tz, TZ_DEFAULT, sizeof(cfg.tz));
  cfg.logUart = false;
}

bool fsBegin() {
  mounted = LittleFS.begin();
  if (!mounted) {  // перший запуск або зіпсована розмітка
    logPrint("FS: форматую LittleFS");
    mounted = LittleFS.format() && LittleFS.begin();
  }
  if (!mounted) logPrint("FS: не змонтувалась");
  return mounted;
}

// Значення з веба йдуть у файл «ключ=значення», тож перенесення рядка в них ламає формат:
// «ssid=MyNet\nota=\n» дописало б окремий рядок і **зняло службовий пароль** після
// перезапуску, а пароль Wi-Fi із випадковим \n (вставили з менеджера паролів) тихо
// обрізався б — з відповіддю «збережено». Керуючі символи відкидаємо на вході: у полях
// конфігу вони однаково не мають сенсу.
void cfgSetStr(char *dst, size_t size, const char *src) {
  size_t k = 0;
  for (; *src && k + 1 < size; src++) {
    unsigned char c = (unsigned char)*src;
    if (c >= 0x20 && c != 0x7F) dst[k++] = *src;
  }
  dst[k] = 0;
}

// ---- розбір рядка «ключ=значення» ----
static void apply(const char *key, const char *val) {
  if (!strcmp(key, "ssid")) strlcpy(cfg.ssid, val, sizeof(cfg.ssid));
  else if (!strcmp(key, "pass")) strlcpy(cfg.pass, val, sizeof(cfg.pass));
  else if (!strcmp(key, "host")) strlcpy(cfg.host, val, sizeof(cfg.host));
  else if (!strcmp(key, "ota")) strlcpy(cfg.ota, val, sizeof(cfg.ota));
  else if (!strcmp(key, "ntp")) strlcpy(cfg.ntp, val, sizeof(cfg.ntp));
  // Раніше тут лежав зсув у хвилинах («tz=180»). Для правила зони таке значення
  // безглузде, тож старий рядок пропускаємо: лишиться типове правило з переходом
  // на літній час, і годинник не поїде на годину в жовтні.
  else if (!strcmp(key, "tz")) {
    if (isalpha((unsigned char)val[0]) || val[0] == '<') strlcpy(cfg.tz, val, sizeof(cfg.tz));
  }
  else if (!strcmp(key, "logUart")) cfg.logUart = atoi(val) != 0;
}

bool cfgLoad() {
  cfgDefaults();
  if (!mounted) return false;
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) {
    logPrint("cfg: файла немає, беру типові значення");
    return false;
  }
  char line[128];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *val = eq + 1;
    // хвостовий \r від редактора у Windows. Пробіл не зрізаємо: ним може закінчуватись
    // пароль Wi-Fi, і такий пароль після перезапуску переставав працювати.
    size_t vl = strlen(val);
    while (vl && val[vl - 1] == '\r') val[--vl] = 0;
    apply(line, val);
  }
  f.close();
  logf("cfg: ssid=%s host=%s", cfg.ssid[0] ? cfg.ssid : "-", cfg.host);
  return true;
}

bool cfgSave() {
  if (!mounted) return false;
  // Спершу збираємо весь файл у буфер: так знаємо, скільки байтів мусить лягти на флеш, і
  // можемо це звірити після запису.
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "ssid=%s\npass=%s\nhost=%s\nota=%s\nntp=%s\ntz=%s\nlogUart=%d\n",
                   cfg.ssid, cfg.pass, cfg.host, cfg.ota, cfg.ntp, cfg.tz, cfg.logUart ? 1 : 0);
  if (n < 0 || (size_t)n >= sizeof(buf)) {
    logPrint("cfg: не вміщається в буфер, не зберігаю");
    return false;
  }
  File f = LittleFS.open(CFG_PATH, "w");
  if (!f) {
    logPrint("cfg: не відкривається на запис");
    return false;
  }
  size_t wrote = f.write((const uint8_t *)buf, (size_t)n);
  f.close();
  // Розмір питаємо після close() і окремим відкриттям: доти ok рахувався як f.size() > 0 ще
  // ДО закриття, тобто до моменту, коли LittleFS справді скидає дані на флеш, і результати
  // семи printf не перевіряв ніхто. На забитому розділі людина бачила «збережено,
  // перезапускаюсь», хоч старий вміст файлу вже знищено (режим «w» обнуляє його), а новий
  // не дописався — модуль піднімався з порожнім конфігом і йшов у власну точку доступу.
  File chk = LittleFS.open(CFG_PATH, "r");
  bool ok = wrote == (size_t)n && chk && chk.size() == (uint32_t)n;
  if (chk) chk.close();
  logPrint(ok ? "cfg: збережено" : "cfg: запис не вдався");
  return ok;
}
