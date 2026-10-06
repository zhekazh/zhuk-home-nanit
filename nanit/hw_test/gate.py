#!/usr/bin/env python3
"""Ручне керування шлагбаумом: довертання на кроки й виставляння нуля.

Кроковий двигун не має зворотного зв'язку, тож пропущені кроки нікуди не
зникають: положення «зачинено» повзе, і стріла з часом не доїжджає. Цим
скриптом стрілу доводять на місце, а тоді закріплюють нуль.

Повний хід — `gate steps` кроків (типово 512). Скільки це градусів, залежить
від передачі на макеті; орієнтовно 512 кроків ≈ 90°, тобто 1° ≈ 5–6 кроків.

Запуск:  python3 gate.py --show           калібрування й стан
         python3 gate.py 40               довернути на 40 кроків уперед
         python3 gate.py -40              на 40 кроків назад
         python3 gate.py -30 --home       довернути й закріпити як «зачинено»
         python3 gate.py --home           лише закріпити нуль
         python3 gate.py --cycle 3        3 цикли відкриття-закриття
         python3 gate.py --delay 6        змінити паузу між кроками, мс
"""
import argparse
import sys
import time

from hw_test import BOOT_WAIT, Nanit, find_port, port_busy, tr_result


def read_cal(n):
    mark = n.mark()
    n.cmd("GET", "cal,gate", timeout=3.0)
    n.pump(0.7)
    cal = {}
    for b in n.since(mark):
        f = b.split(",")
        if len(f) == 4 and f[0] == "VAL" and f[1] == "gate":
            cal[f[2]] = f[3]
    return cal


def gate_state(n, timeout=2.0):
    """Стан шлагбаума з кадру PRK: closed / open / opening / closing."""
    body = n.wait(lambda b: b.startswith("PRK,"), timeout)
    f = body.split(",") if body else []
    return f[2] if len(f) > 2 else "?"


def wait_still(n, steps, delay_ms, extra=1.0):
    """Чекає, поки рух завершиться: кроки по delay мс плюс запас."""
    n.pump(abs(steps) * delay_ms / 1000.0 + extra)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("steps", nargs="?", type=int, default=None,
                    help="кроки: додатні — вперед, від'ємні — назад")
    ap.add_argument("--port", default=None)
    ap.add_argument("--home", action="store_true", help="закріпити положення як «зачинено»")
    ap.add_argument("--cycle", type=int, default=0, help="скільки циклів відкриття-закриття")
    ap.add_argument("--delay", type=int, default=None, help="пауза між кроками, 2..10 мс")
    ap.add_argument("--show", action="store_true", help="показати калібрування й вийти")
    a = ap.parse_args()

    if a.steps is not None and not -2048 <= a.steps <= 2048:
        print("кроки поза межами -2048..2048")
        return 2
    if a.delay is not None and not 2 <= a.delay <= 10:
        print("--delay поза межами 2..10")
        return 2
    if a.steps is None and not (a.home or a.cycle or a.show or a.delay is not None):
        print("нічого робити: вкажіть кроки, --home, --cycle, --delay або --show")
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
        for _ in range(3):
            resp, _ = n.cmd("PING", timeout=2.0)
            if resp and resp.startswith("ACK"):
                break
            n.pump(1.0)
        if not (resp and resp.startswith("ACK")):
            print("плата не відповідає на PING")
            return 1

        cal = read_cal(n)
        print("калібрування шлагбаума: " + ", ".join(f"{k}={v}" for k, v in cal.items()))
        delay_ms = int(cal.get("delay", 3))
        steps_full = int(cal.get("steps", 512))
        if a.show:
            n.cmd("DBG", "1", timeout=1.0)
            print("стан:", gate_state(n, 3.0))
            n.cmd("DBG", "0", timeout=1.0)
            return 0

        if a.delay is not None:
            resp, _ = n.cmd("CAL", f"gate,delay,{a.delay}", timeout=3.0)
            print(f"delay={a.delay}: {resp or 'немає відповіді'}")
            delay_ms = a.delay

        resp, _ = n.cmd("SVC", "1", timeout=3.0)
        if not (resp and resp.startswith("ACK")):
            print(f"сервісний режим не увімкнувся: {resp or 'немає відповіді'}")
            return 1
        try:
            if a.steps:
                resp, _ = n.cmd("CALDO", f"gate,step,{a.steps}", timeout=3.0)
                print(f"крок {a.steps:+d}: {resp or 'немає відповіді'}")
                wait_still(n, a.steps, delay_ms)

            for i in range(a.cycle):
                for action in ("open", "close"):
                    resp, seq = n.cmd("TEST", f"gate,{action}", timeout=3.0)
                    limit = 5.0 + steps_full * delay_ms / 1000.0 * 2
                    tr = tr_result(n, seq, limit, n.cmd_mark)
                    print(f"  цикл {i + 1}, {action:5} -> {tr or 'без відповіді'}")
                    n.pump(0.8)

            if a.home:
                resp, _ = n.cmd("CALDO", "gate,home", timeout=3.0)
                print(f"нуль закріплено: {resp or 'немає відповіді'}")
                n.pump(1.8)  # запис EEPROM відкладений на 1,5 с
        finally:
            n.cmd("SVC", "0", timeout=3.0)
    finally:
        n.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
