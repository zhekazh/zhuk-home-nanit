// Заглушка LittleFS: файли в теці тесту. Окремо від stubs.cpp, бо потрібна й тим тестам,
// які збирають модулі, що визначають cfg самі (store_test).
#include <string>

#include "LittleFS.h"

// ---------- файлова система ----------
static std::string fsRoot = ".";
FSClass LittleFS;

void littlefsRoot(const char *dir) { fsRoot = dir; }

static std::string fsPath(const char *path) {
  // «/events» у корені імітації — це просто файл у теці тесту
  return fsRoot + "/" + (path[0] == '/' ? path + 1 : path);
}

bool FSClass::begin() { return true; }
bool FSClass::format() { return true; }

bool FSClass::exists(const char *path) {
  FILE *f = fopen(fsPath(path).c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

File FSClass::open(const char *path, const char *mode) {
  // Режими прошивки: «w» створює, «r» читає, «r+» перезаписує на місці
  const char *m = !strcmp(mode, "w") ? "wb+" : (!strcmp(mode, "r+") ? "rb+" : "rb");
  return File(fopen(fsPath(path).c_str(), m));
}

bool FSClass::remove(const char *path) { return ::remove(fsPath(path).c_str()) == 0; }
