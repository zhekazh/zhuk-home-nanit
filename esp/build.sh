#!/bin/sh
# Збірка й заливка прошивки ESP8266 Witty Cloud.
#
#   sh esp/build.sh                # лише скомпілювати
#   sh esp/build.sh usb            # залити через USB (плата на нижній платі Witty)
#   sh esp/build.sh usb /dev/ttyUSB1
#   sh esp/build.sh ota 192.168.0.42   # залити по Wi-Fi, модуль лишається в порту 11
#
# arduino-cli береться з flatpak Arduino IDE; шлях можна перебити змінною ARDUINO_CLI.
set -e

CLI="${ARDUINO_CLI:-$HOME/.local/share/flatpak/app/cc.arduino.IDE2/x86_64/stable/active/files/arduino-ide/resources/app/lib/backend/resources/arduino-cli}"
CFG="${ARDUINO_CFG:-$HOME/.arduinoIDE/arduino-cli.yaml}"
DIR=$(cd "$(dirname "$0")" && pwd)
SKETCH="$DIR/zhuk_home_esp"

# Сторінка інтерфейсу вшивається у прошивку, тому web_app.h перезбирається завжди:
# інакше можна залити новий код зі старою сторінкою й довго шукати, чому нічого не змінилось.
python3 "$DIR/../web/pack.py"

# 4 МБ флеш, 1 МБ під LittleFS: решта — скетч і місце під OTA-копію.
BOARD="esp8266:esp8266:generic:eesz=4M1M,FlashMode=dio,xtal=80,baud=460800,ip=lm2f"

case "$1" in
usb)
  PORT="${2:-/dev/ttyUSB0}"
  echo "== заливка через $PORT =="
  "$CLI" --config-file "$CFG" compile -b "$BOARD" "$SKETCH"
  "$CLI" --config-file "$CFG" upload -b "$BOARD" -p "$PORT" -v "$SKETCH" 2>&1 | tail -20
  ;;
ota)
  # arduino-cli не вміє espota без «знайденого» порту, тому кличемо espota.py напряму
  [ -n "$2" ] || { echo "вкажіть адресу: sh esp/build.sh ota 192.168.0.42" >&2; exit 1; }
  ESPOTA="$HOME/.arduino15/packages/esp8266/hardware/esp8266/3.1.2/tools/espota.py"
  OUT="$DIR/build"
  echo "== заливка по Wi-Fi на $2 =="
  "$CLI" --config-file "$CFG" compile -b "$BOARD" --output-dir "$OUT" "$SKETCH"
  python3 "$ESPOTA" -i "$2" -p 8266 -f "$OUT/zhuk_home_esp.ino.bin" | tail -2
  ;;
*)
  "$CLI" --config-file "$CFG" compile -b "$BOARD" "$SKETCH"
  ;;
esac
