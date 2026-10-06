#!/usr/bin/env python3
"""Живий монітор сирих рівнів датчиків порту 5 — для калібрування потенціометрів.

Показує стан пінів PIR, мікрофона й датчика вікна так, як їх бачить прошивка
(кадр IO, розділ 8 docs/protocol.md), і рахує, скільки часу кожен пін проводить
у якому стані. Зручно крутити потенціометр і бачити, коли модуль замовкає.

Запуск:  python3 tune.py                  міряти 10 с і показати підсумок
         python3 tune.py --seconds 30
         python3 tune.py --follow         писати рядок стану раз на пів секунди
         python3 tune.py --guide --seconds 300
                                         живий індикатор: крутіть потенціометр
                                         мікрофона й дивіться, куди падає шум
"""
import argparse
import sys
import time

from hw_test import BOOT_WAIT, Nanit, find_port, port_busy

WATCH_REFRESH = 5.0  # вікно WATCH на Nanit — 10 с, поновлюємо з запасом

NAMES = {"pir": "рух", "sound": "мікрофон", "wsens": "вікно"}
MEANING = {"pir": {1: "є рух", 0: "спокій"},
           "sound": {1: "тиша", 0: "звук"},
           "wsens": {1: "вільно", 0: "перешкода"}}


class Levels:
    """Накопичує, скільки часу кожен пін провів у стані 1 і скільки разів змінився."""

    def __init__(self):
        self.value = {}
        self.since = {}
        self.high = dict.fromkeys(NAMES, 0.0)
        self.total = dict.fromkeys(NAMES, 0.0)
        self.edges = dict.fromkeys(NAMES, 0)

    def feed(self, body, now):
        f = body.split(",")
        if len(f) < 4:
            return
        for name, raw in zip(("pir", "sound", "wsens"), f[1:4]):
            try:
                v = int(raw)
            except ValueError:
                continue
            prev = self.value.get(name)
            if prev is not None:
                dt = now - self.since[name]
                self.total[name] += dt
                if prev:
                    self.high[name] += dt
                if v != prev:
                    self.edges[name] += 1
            self.value[name] = v
            self.since[name] = now

    def close(self, now):
        """Зараховує час від останнього кадру до кінця вимірювання."""
        for name, v in self.value.items():
            dt = now - self.since[name]
            self.total[name] += dt
            if v:
                self.high[name] += dt
            self.since[name] = now

    def share(self, name):
        """Частка часу в стані 1, у відсотках."""
        return 100.0 * self.high[name] / self.total[name] if self.total[name] else 0.0

    def line(self):
        parts = []
        for name in ("pir", "sound", "wsens"):
            v = self.value.get(name)
            if v is None:
                parts.append(f"{NAMES[name]}: —")
                continue
            parts.append(f"{NAMES[name]}: {v} {MEANING[name][v]:<9} {self.share(name):5.1f}% "
                         f"змін {self.edges[name]:3d}")
        return " | ".join(parts)


WINDOW = 3.0     # вікно, за яким рахуємо шум
PEAK_WIN = 2.0   # вікно піків: прошивці треба cal.soundPeaks піків за cal.soundWin


def guide_line(rate, npeaks, value, elapsed):
    """Рядок живого індикатора для налаштування потенціометра мікрофона."""
    if value is None:
        return f"{elapsed:5.1f} с  НЕМАЄ КАДРІВ ВІД ПЛАТИ — перевірте зв'язок і прошивку"
    # шкала логарифмічна: шум падає з тисяч до нуля, і на лінійній
    # шкалі прогрес крутіння був би невидимий до самого кінця
    filled = min(14, rate.bit_length())
    bar = "█" * filled + "░" * (14 - filled)
    if rate:
        hint = "ШУМИТЬ — крутіть так, щоб число падало"
    elif value == 0:
        hint = "ЗАЛИПЛО В «ЗВУК» — крутіть далі в той самий бік"
    else:
        hint = "ТИХО — плесніть 3 рази, має бути «піків 3»"
    state = "тиша" if value else "звук"
    return (f"{elapsed:5.1f} с  шум {bar} {rate:3d} | стан: {state:<4} | "
            f"піків: {npeaks} | {hint}")


def monitor(n, seconds, follow, guide=False):
    lv = Levels()
    mark = n.mark()
    watched = 0.0
    printed = 0.0
    edges, peaks = [], []
    prev = None
    last_shown = None
    end = time.time() + seconds
    while time.time() < end:
        now = time.time()
        if now - watched >= WATCH_REFRESH:
            n.cmd("WATCH", "1", timeout=0.2)  # номер щоразу новий, інакше Nanit вважатиме це повтором
            watched = now
        n.pump(0.05)
        for t, body in n.frames[mark:]:
            if not body.startswith("IO,"):
                continue
            lv.feed(body, t)
            v = lv.value.get("sound")
            if prev is not None and v != prev:
                edges.append(t)
                if v == 0:
                    peaks.append(t)  # фронт «стало гучно» — саме його рахує прошивка
            prev = v
        mark = n.mark()
        edges = [t for t in edges if now - t <= WINDOW]
        peaks = [t for t in peaks if now - t <= PEAK_WIN]
        if guide:
            rate = len(edges)
            npeaks = len(peaks)
            value = lv.value.get("sound")
            # зона важливіша за точне число: рядок лише коли картина змінилась,
            # інакше при активному шумі екран заллє однаковими рядками
            zone = 0 if value is None else 1 if rate else 2 if value == 0 else 3
            shown = (zone, npeaks >= 3, rate // 3)
            if (shown != last_shown and now - printed >= 1.0) or now - printed >= 4.0:
                printed = now
                last_shown = shown
                sys.stdout.write(guide_line(rate, npeaks, value, now - (end - seconds)) + "\n")
                sys.stdout.flush()
        elif follow and now - printed >= 0.5:
            printed = now
            sys.stdout.write(f"{now - (end - seconds):5.1f} с  {lv.line()}\n")
            sys.stdout.flush()
    lv.close(time.time())
    return lv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="послідовний порт (типово знаходиться сам)")
    ap.add_argument("--seconds", type=float, default=10.0, help="скільки міряти")
    ap.add_argument("--follow", action="store_true", help="друкувати стан раз на пів секунди")
    ap.add_argument("--guide", action="store_true",
                    help="живий індикатор для налаштування потенціометра мікрофона")
    ap.add_argument("--log", default=None, help="файл із усім обміном")
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        print("Плату не знайдено: немає /dev/ttyUSB* чи /dev/ttyACM*")
        return 2
    if port_busy(port):
        print(f"Порт {port} зайнятий — закрийте монітор порту в Arduino IDE")
        return 2

    n = Nanit(port, log=a.log)
    try:
        print(f"{port}: чекаю на плату…")
        time.sleep(BOOT_WAIT)
        n.pump(0.2)
        resp = None
        for attempt in range(3):  # разовий збій буває, коли порт щойно відкрили двічі
            resp, _ = n.cmd("PING", timeout=2.0)
            if resp and resp.startswith("ACK"):
                break
            if attempt < 2:
                print(f"плата не відповіла на PING, спроба {attempt + 2} з 3…")
                n.pump(1.0)
        if not (resp and resp.startswith("ACK")):
            print("плата не відповідає на PING. Перевірте, чи закритий монітор порту "
                  "в Arduino IDE і чи не запущено інший скрипт.")
            return 1
        print(f"міряю {a.seconds:.0f} с…")
        lv = monitor(n, a.seconds, a.follow, a.guide)
    finally:
        n.close()

    print()
    for name in ("pir", "sound", "wsens"):
        if not lv.total[name]:
            print(f"{NAMES[name]:<9} даних немає")
            continue
        v = lv.value[name]
        print(f"{NAMES[name]:<9} зараз {v} ({MEANING[name][v]}), "
              f"у стані 1 ({MEANING[name][1]}) {lv.share(name):.1f}% часу, "
              f"змін {lv.edges[name]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
