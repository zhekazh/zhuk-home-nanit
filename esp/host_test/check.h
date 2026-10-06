// Спільний лічильник перевірок: як у тестах Nanit, щоб вивід читався однаково.
#pragma once
#include <stdio.h>

extern int checks, fails;
#define CHECK(c)                                          \
  do {                                                    \
    checks++;                                             \
    if (!(c)) {                                           \
      fails++;                                            \
      printf("FAIL line %d: %s\n", __LINE__, #c);         \
    }                                                     \
  } while (0)

// Те саме, але з поясненням: у кільцевих буферах «не те значення» саме по собі
// нічого не каже, і без фактичного вмісту тест доводиться дебажити наосліп.
#define CHECK_EQ(got, want)                                                  \
  do {                                                                       \
    checks++;                                                                \
    std::string g_ = (got), w_ = (want);                                     \
    if (g_ != w_) {                                                          \
      fails++;                                                               \
      printf("FAIL line %d: «%s» замість «%s»\n", __LINE__, g_.c_str(), w_.c_str()); \
    }                                                                        \
  } while (0)

inline int report(const char *name) {
  printf("%s: %d перевірок, %d не пройшли\n", name, checks, fails);
  return fails ? 1 : 0;
}
