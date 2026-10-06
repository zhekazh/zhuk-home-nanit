#!/usr/bin/env python3
"""Визначає, який колір світиться на кожному виводі світлофора.

    python3 esp/tlprobe.py                 # запитає про кожен канал
    python3 esp/tlprobe.py --host 192.168.0.106

На порт світлофора можна поставити і три окремі світлодіоди, і RGB-модуль на кшталт
KY-016. У другому випадку виводи майже напевно не збігаються з тим, що очікує прошивка:
середній пін виявляється синім каналом, і «жовтий» світить синім. Скрипт по черзі
засвічує кожен вивід, питає, що ви бачите, і в кінці каже, яке калібрування застосувати.

Запускати з макетом перед очима: скрипт чекає на відповідь після кожного кольору.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WS = HERE / 'ws.py'

CHANNELS = [
    ('r', 'перший вивід (P3_2) — прошивка вважає його ЧЕРВОНИМ'),
    ('y', 'середній вивід (P3_3) — прошивка вважає його ЖОВТИМ'),
    ('g', 'третій вивід (P3_4) — прошивка вважає його ЗЕЛЕНИМ'),
]


def send(host, cmd, seconds=3):
    """Надсилає команду через ws.py і повертає рядок відповіді."""
    out = subprocess.run(
        [sys.executable, str(WS), '--host', host, '--seconds', str(seconds),
         '--send', cmd, '--only', 'RSP,TR'],
        capture_output=True, text=True).stdout
    return out.strip()


def main():
    ap = argparse.ArgumentParser(description='розкладка виводів світлофора')
    ap.add_argument('--host', default='zhukhome.local')
    args = ap.parse_args()

    print('Вмикаю сервісний режим, автоматика світлофора на паузі.')
    send(args.host, 'SVC,1', 2)
    send(args.host, 'CAL,tlight,rgb,0', 2)  # без пари каналів: перевіряємо кожен окремо

    seen = {}
    try:
        for key, label in CHANNELS:
            print(f'\n=== {label} ===')
            print('Дивіться на світлодіод…')
            # Перевірка тримає колір 2,5 с, тож повторюємо, поки людина не відповість
            for _ in range(3):
                send(args.host, f'TEST,tlight,{key}', 2)
            answer = input('Який колір ви побачили? (червоний / зелений / синій / нічого): ')
            seen[key] = answer.strip().lower()
    except KeyboardInterrupt:
        print('\nперервано')
    finally:
        send(args.host, 'SVC,0', 2)
        print('\nСервісний режим вимкнено.')

    if len(seen) < 3:
        return 0

    print('\nЩо на якому виводі:')
    for key, label in CHANNELS:
        print(f'  {label.split(" — ")[0]}: {seen.get(key, "?")}')

    red = [k for k, v in seen.items() if v.startswith('черв')]
    green = [k for k, v in seen.items() if v.startswith('зел')]
    if red and green:
        print('\nЖовтий на такому модулі виходить із червоного разом із зеленим.')
        if red[0] == 'r' and green[0] == 'g':
            print('Виводи збігаються з очікуваними — вмикайте режим RGB:')
            print('  python3 esp/ws.py --send "CAL,tlight,rgb,1"')
        else:
            print(f'Червоний сидить на виводі «{red[0]}», зелений — на «{green[0]}»,')
            print('а прошивка чекає їх на «r» і «g». Треба або переставити дроти,')
            print('або навчити прошивку цій розкладці — скажіть про це Клоду.')
    else:
        print('\nЧервоного або зеленого не видно — схоже, дроти стоять не на тих пінах.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
