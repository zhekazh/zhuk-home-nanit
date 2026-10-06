#!/bin/sh
# Тести логіки прошивки на комп'ютері (без Nanit): протокол, охорона, налаштування,
# сервісний режим. Потрібен лише g++. Запуск: sh host_test/run.sh
set -e
cd "$(dirname "$0")"
SK=../zhuk_home_nanit
OUT=${TMPDIR:-/tmp}/zhuk_host_test
mkdir -p "$OUT"
# freeRam() читає символи купи AVR, яких на комп'ютері немає — підміняємо заглушкою
sed '/^extern char \*__brkval;/d; /^extern char __heap_start;/d; /^int freeRam() {/,/^}/d' $SK/util.cpp > "$OUT/util_host.cpp"
g++ -std=gnu++11 -w -I. -I$SK -o "$OUT/util_test" util_test.cpp "$OUT/util_host.cpp"
"$OUT/util_test"
g++ -std=gnu++11 -w -I. -I$SK -o "$OUT/proto_test" proto_test.cpp stubs.cpp "$OUT/util_host.cpp" \
  $SK/state.cpp $SK/settings.cpp $SK/link.cpp $SK/security.cpp $SK/automation.cpp $SK/diag.cpp
"$OUT/proto_test"
