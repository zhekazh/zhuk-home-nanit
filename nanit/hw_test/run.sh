#!/bin/sh
# Перевірка прошивки на живому макеті через USB.
# Монітор порту в Arduino IDE має бути закритий: порт тримає лише одна програма.
#
#   sh run.sh                     усе, крім перевірок, що потребують ваших дій
#   sh run.sh --interactive       разом із датчиками, клавіатурою й охороною
#   sh run.sh --no-motion         без руху механіки
#   sh run.sh --port /dev/ttyUSB0 --pin 1234
cd "$(dirname "$0")"
exec python3 hw_test.py "$@"
