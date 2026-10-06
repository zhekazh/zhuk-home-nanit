#!/bin/sh
# Тести логіки прошивки ESP на комп'ютері (без модуля): транспорт до Nanit, кільцевий
# журнал подій у файлі, веб-консоль і вузол між Nanit та сторінками. Заліза не треба,
# потрібен лише g++. Запуск: sh esp/host_test/run.sh
set -e
cd "$(dirname "$0")"
SK=../zhuk_home_esp
OUT=${TMPDIR:-/tmp}/zhuk_esp_test
rm -rf "$OUT"
mkdir -p "$OUT/fs1" "$OUT/fs2" "$OUT/fs3" "$OUT/fs4" "$OUT/fs5" "$OUT/fs6" "$OUT/fs7"
CXX="g++ -std=gnu++11 -w -I. -I$SK"

$CXX -o "$OUT/link_test"    link_test.cpp    stubs.cpp $SK/link.cpp
$CXX -o "$OUT/logbuf_test"  logbuf_test.cpp  stubs.cpp $SK/logbuf.cpp
$CXX -o "$OUT/hub_test"     hub_test.cpp     stubs.cpp $SK/hub.cpp
$CXX -o "$OUT/evstore_test" evstore_test.cpp stubs.cpp fs_stub.cpp $SK/evstore.cpp
# store.cpp визначає cfg сам, тож stubs.cpp тут не підключаємо — лише заглушку ФС
$CXX -o "$OUT/store_test"   store_test.cpp   fs_stub.cpp $SK/store.cpp

"$OUT/link_test"
"$OUT/logbuf_test"
"$OUT/hub_test"
"$OUT/store_test" "$OUT/fs7"

# Перезапуск ESP тут — справжній перезапуск процесу: позицію запису модуль мусить
# знайти в самому файлі, а не пам'ятати з минулого разу
"$OUT/evstore_test" "$OUT/fs1" fresh
"$OUT/evstore_test" "$OUT/fs1" restart
"$OUT/evstore_test" "$OUT/fs2" wrap
"$OUT/evstore_test" "$OUT/fs2" after-wrap
"$OUT/evstore_test" "$OUT/fs3" timer

# Подія без часу (до NTP) серед звичайних: голова кільця мусить знаходитись за номером
# запису, а не за часом — інакше вона стає на неї й перезаписує свіжі події
"$OUT/evstore_test" "$OUT/fs4" zero
"$OUT/evstore_test" "$OUT/fs4" zero-restart
"$OUT/evstore_test" "$OUT/fs5" zero-wrap
"$OUT/evstore_test" "$OUT/fs5" zero-after-wrap

# Файл у старому форматі слота від попередньої прошивки
"$OUT/evstore_test" "$OUT/fs6" legacy
