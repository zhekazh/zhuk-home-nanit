#!/bin/sh
# Збірка й заливка прошивки Nanit.
#
#   sh nanit/flash.sh                  # зібрати й залити на /dev/ttyUSB0
#   sh nanit/flash.sh /dev/ttyUSB1
#   sh nanit/flash.sh --build          # лише зібрати
#
# Чому скрипт, а не дві команди руками: «arduino-cli upload» НЕ компілює — він заливає
# бінарник із кешу. Забути окремий «compile» означає залити попередню версію й довго
# шукати, чому зміни не діють. Тут обидва кроки завжди разом, а вивід перевіряється на
# «bytes of flash written»: без цього рядка заливка не відбулася, хоч би що писало далі.
#
# Вивід компіляції спершу йде у файл, і лише потім фільтрується. Через конвеєр
# «compile | grep» код виходу належав grep, тож провалена збірка лишалася непоміченою
# і скрипт заливав у плату попередній бінарник із кешу — саме те, від чого він мав
# захищати. Коштувало це годину пошуку «чому зміни не діють».
set -e

CLI="${ARDUINO_CLI:-$HOME/.local/share/flatpak/app/cc.arduino.IDE2/x86_64/stable/active/files/arduino-ide/resources/app/lib/backend/resources/arduino-cli}"
CFG="${ARDUINO_CFG:-$HOME/.arduinoIDE/arduino-cli.yaml}"
DIR=$(cd "$(dirname "$0")" && pwd)
SKETCH="$DIR/zhuk_home_nanit"
BOARD="arduino:avr:mega:cpu=atmega2560"

echo "== збірка =="
LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT
if ! "$CLI" --config-file "$CFG" compile -b "$BOARD" "$SKETCH" > "$LOG" 2>&1; then
  grep -iE 'error|undefined|multiple definition' "$LOG" | head -20
  echo "ЗБІРКА НЕ ВДАЛАСЯ — заливки не буде (у плату пішов би старий бінарник із кешу)." >&2
  exit 1
fi
grep -E 'Sketch uses|Global variables' "$LOG"

[ "$1" = "--build" ] && exit 0

PORT="${1:-/dev/ttyUSB0}"
echo "== заливка через $PORT =="
OUT=$("$CLI" --config-file "$CFG" upload -b "$BOARD" -p "$PORT" -v "$SKETCH" 2>&1) || true
echo "$OUT" | grep -iE 'bytes of flash written|avrdude done|error|timeout' | tail -3

if ! echo "$OUT" | grep -q 'bytes of flash written'; then
  echo "ЗАЛИВКА НЕ ВІДБУЛАСЯ: у виводі немає «bytes of flash written»." >&2
  echo "Найчастіша причина — порт тримає монітор Arduino IDE. Закрийте його." >&2
  exit 1
fi
