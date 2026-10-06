#!/usr/bin/env python3
"""Ручне керування шторкою: ставить серво в указані кути.

Потрібно, коли налаштовуєш зчеплення шестерні серво з колесом шторки. Кут
задається через калібрування `servo open` і `TEST servo,open`: у протоколі
немає команди «стати в такий-то кут», а перевірка вміє лише close/half/open.
Початкове значення `open` скрипт повертає наприкінці, якщо не сказано --keep.

Запуск:  python3 servo.py 90                кут 90°
         python3 servo.py 0 90 165 0        послідовність кутів
         python3 servo.py 90 --pause 5      затримка між кутами, секунд
         python3 servo.py 90 --keep         лишити цей кут як «відкрито»
         python3 servo.py --set-open 150    лише змінити калібрування, без руху
         python3 servo.py --show            показати поточне калібрування
"""
import argparse
import sys
import time

from hw_test import BOOT_WAIT, Nanit, find_port, port_busy, tr_result

SVC_REFRESH = 20.0  # сервісний режим має свій тайм-аут, тож поновлюємо його


def read_cal(n):
    """Поточне калібрування серво як словник."""
    mark = n.mark()
    n.cmd("GET", "cal,servo", timeout=3.0)
    n.pump(0.7)
    cal = {}
    for b in n.since(mark):
        f = b.split(",")
        if len(f) == 4 and f[0] == "VAL" and f[1] == "servo":
            cal[f[2]] = f[3]
    return cal


def goto(n, angle, closed=0, timeout=25.0):
    """Ставить серво в кут: задаємо його як «відкрито» і виконуємо перевірку."""
    if angle == closed:
        # «відкрито» не може дорівнювати «зачинено» (інакше хід шторки нульовий),
        # тож у сам нуль їдемо штатним закриттям
        resp, seq = n.cmd("TEST", "servo,close", timeout=3.0)
        if not (resp and resp.startswith("ACK")):
            return f"перевірку не почато: {resp or 'немає відповіді'}"
        return tr_result(n, seq, timeout, n.cmd_mark) or "серво не відповіло"
    resp, _ = n.cmd("CAL", f"servo,open,{angle}", timeout=3.0)
    if not (resp and resp.startswith("ACK")):
        return f"калібрування не прийнято: {resp or 'немає відповіді'}"
    resp, seq = n.cmd("TEST", "servo,open", timeout=3.0)
    if not (resp and resp.startswith("ACK")):
        return f"перевірку не почато: {resp or 'немає відповіді'}"
    tr = tr_result(n, seq, timeout, n.cmd_mark)
    return tr or "серво не відповіло"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("angles", nargs="*", type=int, help="кути 0..180 по порядку")
    ap.add_argument("--port", default=None)
    ap.add_argument("--pause", type=float, default=2.0, help="пауза між кутами, секунд")
    ap.add_argument("--keep", action="store_true", help="лишити останній кут як «відкрито»")
    ap.add_argument("--set-open", type=int, default=None, help="змінити калібрування без руху")
    ap.add_argument("--show", action="store_true", help="показати калібрування й вийти")
    a = ap.parse_args()

    for ang in a.angles:
        if not 0 <= ang <= 180:
            print(f"кут {ang} поза межами 0..180")
            return 2
    if a.set_open is not None and not 0 <= a.set_open <= 180:
        print("--set-open поза межами 0..180")
        return 2
    if not a.angles and a.set_open is None and not a.show:
        print("нічого робити: вкажіть кути, --set-open або --show")
        return 2

    port = a.port or find_port()
    if not port:
        print("Плату не знайдено: немає /dev/ttyUSB* чи /dev/ttyACM*")
        return 2
    if port_busy(port):
        print(f"Порт {port} зайнятий — закрийте монітор порту в Arduino IDE")
        return 2

    n = Nanit(port)
    try:
        print(f"{port}: чекаю на плату…")
        time.sleep(BOOT_WAIT)
        n.pump(0.2)
        resp = None
        for attempt in range(3):
            resp, _ = n.cmd("PING", timeout=2.0)
            if resp and resp.startswith("ACK"):
                break
            n.pump(1.0)
        if not (resp and resp.startswith("ACK")):
            print("плата не відповідає на PING")
            return 1

        cal = read_cal(n)
        print("калібрування серво: " + ", ".join(f"{k}={v}" for k, v in cal.items()))
        if a.show:
            return 0
        was_open = cal.get("open")

        if a.set_open is not None:
            resp, _ = n.cmd("CAL", f"servo,open,{a.set_open}", timeout=3.0)
            print(f"open={a.set_open}: {resp or 'немає відповіді'}")
            if not a.angles:
                return 0

        resp, _ = n.cmd("SVC", "1", timeout=3.0)
        if not (resp and resp.startswith("ACK")):
            print(f"сервісний режим не увімкнувся: {resp or 'немає відповіді'}")
            return 1
        svc_at = time.time()
        try:
            for i, ang in enumerate(a.angles):
                if time.time() - svc_at >= SVC_REFRESH:
                    n.cmd("SVC", "1", timeout=1.0)
                    svc_at = time.time()
                print(f"  {ang:3d}° -> {goto(n, ang, int(cal.get('closed', 0)))}")
                if i + 1 < len(a.angles):
                    n.pump(a.pause)
        finally:
            if was_open is not None and not a.keep:
                n.cmd("CAL", f"servo,open,{was_open}", timeout=3.0)
                print(f"калібрування «відкрито» повернуто: {was_open}")
            elif a.keep:
                print(f"кут {a.angles[-1]} лишено як «відкрито»")
            n.cmd("SVC", "0", timeout=3.0)
            n.pump(1.8)  # даємо записати EEPROM: запис відкладений на 1,5 с
    finally:
        n.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
