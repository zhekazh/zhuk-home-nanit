// Імітація LittleFS звичайними файлами в теці: журналу подій важлива поведінка
// (постійний розмір, перезапис на місці, читання по колу), а не сам носій.
#pragma once
#include <Arduino.h>

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

void littlefsRoot(const char *dir);  // де тримати файли; викликає тест

class File {
 public:
  FILE *fp = nullptr;

  File() {}
  explicit File(FILE *f) : fp(f) {}
  File(File &&o) : fp(o.fp) { o.fp = nullptr; }
  File &operator=(File &&o) {
    if (fp) fclose(fp);
    fp = o.fp;
    o.fp = nullptr;
    return *this;
  }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  ~File() {
    if (fp) fclose(fp);
  }

  operator bool() const { return fp != nullptr; }
  uint32_t size() {
    if (!fp) return 0;
    long p = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, p, SEEK_SET);
    return (uint32_t)n;
  }
  bool seek(uint32_t off, SeekMode m = SeekSet) {
    int w = m == SeekSet ? SEEK_SET : (m == SeekCur ? SEEK_CUR : SEEK_END);
    return fp && fseek(fp, (long)off, w) == 0;
  }
  int read(uint8_t *b, size_t n) { return fp ? (int)fread(b, 1, n, fp) : -1; }
  // Потрібні cfgLoad(): він читає файл налаштувань рядок за рядком, як Arduino Stream
  int available() {
    if (!fp) return 0;
    long p = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, p, SEEK_SET);
    return (int)(n - p);
  }
  int readBytesUntil(char term, char *buf, size_t len) {
    if (!fp) return 0;
    size_t k = 0;
    while (k < len) {
      int c = fgetc(fp);
      if (c < 0) break;
      if (c == term) break;  // термінатор споживається й у буфер не потрапляє
      buf[k++] = (char)c;
    }
    return (int)k;
  }
  size_t write(const uint8_t *b, size_t n) { return fp ? fwrite(b, 1, n, fp) : 0; }
  void close() {
    if (fp) {
      fclose(fp);
      fp = nullptr;
    }
  }
};

class FSClass {
 public:
  bool begin();
  bool format();
  bool exists(const char *path);
  File open(const char *path, const char *mode);
  bool remove(const char *path);
};
extern FSClass LittleFS;
