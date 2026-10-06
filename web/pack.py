#!/usr/bin/env python3
"""Вшиває сторінку інтерфейсу у прошивку ESP: app.html → gzip → web_app.h у PROGMEM.

    python3 web/pack.py           # зібрати web_app.h
    python3 web/pack.py --check   # лише показати розміри, нічого не писати

Сторінка лежить у флеші стиснутою й віддається як є, із заголовком
«Content-Encoding: gzip» — браузер розпаковує сам. Так вона займає близько чверті
свого розміру, а версія інтерфейсу завжди відповідає версії прошивки: одне оновлення
не може залити нову логіку зі старою сторінкою.

Файлова система для цього не потрібна: LittleFS тримає тільки налаштування, які
пишуться з веба. Так роблять Tasmota, WLED і ESPHome — з тих самих причин.
"""
import gzip
import hashlib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / 'app.html'
OUT = HERE.parent / 'esp' / 'zhuk_home_esp' / 'web_app.h'
# Скільки лишається скетчу при розкладці 4M1M: якщо сторінка розростеться до цього,
# прошивка перестане вміщатися разом із копією для OTA.
FLASH_BUDGET = 1019 * 1024


def main() -> int:
    raw = SRC.read_bytes()
    # mtime=0 і фіксований рівень — щоб один і той самий вхід давав однаковий вихід
    # і збірка не «змінювалася» сама собою.
    packed = gzip.compress(raw, compresslevel=9, mtime=0)
    digest = hashlib.sha256(raw).hexdigest()[:16]

    print(f'{SRC.name}: {len(raw)} Б → gzip {len(packed)} Б '
          f'({len(packed) * 100 // len(raw)} %), etag {digest}')
    if len(packed) > FLASH_BUDGET // 2:
        print('УВАГА: сторінка займає понад половину місця під скетч', file=sys.stderr)

    if '--check' in sys.argv:
        return 0

    rows = []
    for i in range(0, len(packed), 16):
        rows.append('  ' + ' '.join(f'0x{b:02X},' for b in packed[i:i + 16]))

    OUT.write_text(
        '// Створено web/pack.py з web/app.html — руками не правити.\n'
        '// Сторінка інтерфейсу, стиснута gzip, у флеші. Віддається як є:\n'
        '// браузер розпаковує сам за заголовком Content-Encoding.\n'
        '#pragma once\n'
        '#include <Arduino.h>\n\n'
        f'#define WEB_APP_LEN {len(packed)}\n'
        f'#define WEB_APP_RAW {len(raw)}\n'
        f'#define WEB_APP_ETAG "{digest}"\n\n'
        'static const uint8_t WEB_APP_GZ[] PROGMEM = {\n'
        + '\n'.join(rows) + '\n};\n',
        encoding='utf-8')
    print(f'{OUT.relative_to(HERE.parent)}: {OUT.stat().st_size} Б')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
