#!/usr/bin/env python3
"""Проводить перевірки датчиків, які вимагають вашої дії.

    python3 esp/sensors.py                 # усі датчики по черзі
    python3 esp/sensors.py pir sound       # лише вибрані
    python3 esp/sensors.py --host 192.168.0.106

Запускати з макетом перед очима: перед кожним датчиком скрипт каже, що робити, і чекає,
поки ви натиснете Enter. Далі він просить Nanit почати перевірку й показує результат.

Навіщо окремий скрипт, а не кнопки на сторінці: команду дає той самий канал, але тут
підказки з'являються в терміналі саме тоді, коли їх треба читати, — не доводиться
вгадувати момент.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

WS = Path(__file__).resolve().parent / 'ws.py'

SENSORS = {
    'pir': ('Датчик руху (HC-SR501)',
            'Відійдіть і зачекайте, поки датчик заспокоїться, потім пройдіть перед ним.',
            'TEST,pir,wait'),
    'sound': ('Датчик звуку (мікрофон)',
              'Плесніть у долоні {peaks} рази поспіль, чітко, вклавшись у {win} с.',
              'TEST,sound,wait'),
    'window': ('Датчик вікна (TCRT5000)',
               'Піднесіть руку до датчика на вікні й приберіть.',
               'TEST,wsens,wait'),
    'ldr': ('Датчик освітленості',
            'Накрийте датчик рукою на кілька секунд.',
            'TEST,ldr,wait'),
}

OK = {
    'pir': 'рух виявлено через {0} мс — датчик працює',
    'sound': 'сплеск почуто через {0} мс — датчик працює',
    'window': 'перешкоду виявлено через {0} мс — датчик працює',
    'ldr': 'освітленість змінилася з {0} на {1} АЦП — датчик реагує',
}

FAIL = {
    'pir': 'датчик не створив нового спрацювання. Якщо він світиться постійно — на модулі\n'
           '      закручено час утримання: крутіть гвинтик TIME проти годинникової стрілки.',
    'sound': 'сплеску не почуто. Чутливість задається потенціометром на модулі;\n'
             '      налаштувати його допоможе nanit/hw_test/tune.py --guide',
    'window': 'перешкоди не виявлено. Перевірте, що сигнал іде з виводу D0, а не A0.',
    'ldr': 'освітленість не змінилася достатньо — накрийте датчик щільніше.',
}


def run_ws(host, cmd, seconds, only='TR'):
    """Виконує команду через ws.py, показуючи потік кадрів у терміналі."""
    return subprocess.run(
        [sys.executable, str(WS), '--host', host, '--seconds', str(seconds),
         '--send', cmd, '--only', only],
        capture_output=True, text=True).stdout


def sound_hint(host):
    """Скільки плесків і за який час потрібно — питаємо Nanit, а не вгадуємо.

    Прошивка рахує різкі сплески: `cal.soundPeaks` штук за `cal.soundWin` мілісекунд
    (sensors.cpp, soundUpdate). Один плесок вимогу не виконає, і перевірка провалилась
    би на рівному місці — саме тому числа беруться з калібрування.
    """
    out = run_ws(host, 'GET,cal,sound', 5, only='VAL')
    vals = dict(re.findall(r'VAL,sound,(\w+),(-?\d+)', out))
    peaks = int(vals.get('peaks', 3))
    win = int(vals.get('win', 3000))
    return {'peaks': peaks, 'win': round(win / 1000, 1) if win % 1000 else win // 1000}


def main():
    ap = argparse.ArgumentParser(description='перевірка датчиків макета')
    ap.add_argument('names', nargs='*', default=list(SENSORS), help='які датчики перевіряти')
    ap.add_argument('--host', default='zhukhome.local')
    args = ap.parse_args()

    names = [n for n in args.names if n in SENSORS] or list(SENSORS)
    print('Перевірка датчиків. Охорона має бути знята — інакше Nanit відмовить.\n')
    results = {}

    for name in names:
        title, what, cmd = SENSORS[name]
        if '{peaks}' in what:
            what = what.format(**sound_hint(args.host))
        print(f'=== {title} ===')
        print(f'  {what}')
        try:
            input('  Натисніть Enter, коли будете готові (Ctrl+C — вихід)… ')
        except (KeyboardInterrupt, EOFError):
            print('\nперервано')
            break

        print('  Перевірка почалась, маєте 15 секунд…')
        out = run_ws(args.host, cmd, 19)
        line = next((l for l in out.splitlines() if ',ok' in l or ',fail' in l), '')
        values = re.findall(r'(-?\d+)', line.split(',ok')[-1]) if ',ok' in line else []

        if ',ok' in line:
            print('  ✔ ' + OK[name].format(*values, *[''] * 3))
            results[name] = True
        elif ',fail' in line:
            print('  ✘ ' + FAIL[name])
            results[name] = False
        else:
            print('  ? відповіді не було — перевірте зв\'язок із модулем')
            results[name] = None
        print()

    good = sum(1 for v in results.values() if v)
    print(f'Підсумок: працюють {good} із {len(results)}.')
    bad = [n for n, v in results.items() if v is False]
    if bad:
        print('Потребують уваги: ' + ', '.join(bad))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
