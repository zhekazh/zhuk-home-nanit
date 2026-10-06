// Тести налаштувань ESP у файлі: запис, читання назад і фільтрація керуючих символів.
// Формат «ключ=значення» на рядок, тож перенесення рядка у значенні ламало б увесь файл.
#include <string>

#include "check.h"
#include "LittleFS.h"
#include "zhe.h"

int checks = 0, fails = 0;

void logPrint(const char *) {}
void logf(const char *, ...) {}

static std::string fileText(const std::string &dir) {
  std::string out;
  FILE *f = fopen((dir + "/cfg").c_str(), "rb");
  if (!f) return out;
  char b[256];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
  fclose(f);
  return out;
}

int main(int argc, char **argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  littlefsRoot(dir.c_str());
  CHECK(fsBegin());

  // ---------- звичайний обіг: записали, прочитали назад ----------
  cfgDefaults();
  strlcpy(cfg.ssid, "MyNet", sizeof(cfg.ssid));
  strlcpy(cfg.pass, "secret ", sizeof(cfg.pass));  // пробіл у кінці пароля лишається
  strlcpy(cfg.ota, "service1", sizeof(cfg.ota));
  CHECK(cfgSave());
  std::string raw = fileText(dir);
  CHECK(raw.find("ssid=MyNet\n") != std::string::npos);
  CHECK(raw.find("pass=secret \n") != std::string::npos);

  cfgDefaults();
  CHECK(cfg.ssid[0] == 0);
  CHECK(cfgLoad());
  CHECK_EQ(std::string(cfg.ssid), "MyNet");
  CHECK_EQ(std::string(cfg.pass), "secret ");
  CHECK_EQ(std::string(cfg.ota), "service1");

  // ---------- значення з перенесенням рядка не дописує свій ключ ----------
  // «MyNet\nota=» зняло б службовий пароль після перезапуску: файл читається рядок за рядком
  cfgSetStr(cfg.ssid, sizeof(cfg.ssid), "MyNet\nota=\n");
  CHECK_EQ(std::string(cfg.ssid), "MyNetota=");
  CHECK(cfgSave());
  raw = fileText(dir);
  CHECK(raw.find("\nota=\n") == std::string::npos);  // окремого рядка «ota=» у файлі немає
  cfgDefaults();
  CHECK(cfgLoad());
  CHECK_EQ(std::string(cfg.ota), "service1");  // пароль на місці

  // ---------- \r і \t у паролі теж не доходять до файлу ----------
  cfgSetStr(cfg.pass, sizeof(cfg.pass), "a\rb\tc\nd");
  CHECK_EQ(std::string(cfg.pass), "abcd");

  // ---------- задовге значення обрізається за розміром поля, а не за буфером ----------
  std::string big(200, 'x');
  cfgSetStr(cfg.ssid, sizeof(cfg.ssid), big.c_str());
  CHECK(strlen(cfg.ssid) == sizeof(cfg.ssid) - 1);
  CHECK(cfgSave());
  cfgDefaults();
  CHECK(cfgLoad());
  CHECK(strlen(cfg.ssid) == sizeof(cfg.ssid) - 1);

  printf("store: %d перевірок, %d не пройшли\n", checks, fails);
  return fails ? 1 : 0;
}
