#!/usr/bin/env python3
"""Живий монітор мікрофона: що прошивка справді чує на піні.

Чим відрізняється від `nanit/hw_test/tune.py`: той дивиться на кадри `IO`, тобто на
рівень, зчитаний раз на цикл (≈4 мс). Імпульси компаратора бувають коротші за цей такт,
і тоді для прошивки їх не існує. Тут використовується перевірка `TEST,sound,listen`:
Nanit читає пін тісним циклом (близько 78 000 відліків за секунду) і повертає, скільки
було фронтів і яку частку часу вихід був активним.

    python3 esp/mic.py                 слухати по колу, поки не спинити (Ctrl+C)
    python3 esp/mic.py --rounds 10     рівно 10 замірів по секунді
    python3 esp/mic.py --pair          зумер грає, мікрофон слухає (TEST,buzzer,pair)
    python3 esp/mic.py --host 192.168.0.109

Як налаштовувати потенціометр: у тиші має бути «0 фронтів, 0,00%». Якщо активність не
нуль — поріг залип, крутіть у бік меншої чутливості. Далі плесніть і дивіться не лише на
фронти, а й на тривалість: на макеті плескіт давав 2–3 фронти по ~150 мкс, тобто 0,04 %
часу, і штатне опитування раз на ≈4 мс ловить такий імпульс лише в одному випадку з
двадцяти. Чим довші імпульси, тим надійніше працює охорона: прошивці потрібно
`sound peaks` сплесків за `sound win` мс (типово 3 за 3 с), і кожен із них вона мусить
застати своїм опитуванням.
"""
import argparse
import importlib.util
import pathlib
import re
import socket
import sys
import time

WS = pathlib.Path(__file__).resolve().parent / "ws.py"
spec = importlib.util.spec_from_file_location("ws", WS)
ws = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ws)


def one(sock, seq, cmd, pattern, timeout=3.0):
    """Надсилає перевірку й чекає на кадр TR саме від неї."""
    ws.send_text(sock, f"{seq},{cmd}")
    buf, deadline = "", time.time() + timeout
    while time.time() < deadline:
        try:
            buf += sock.recv(4096).decode("utf-8", "replace")
        except socket.timeout:
            continue
        m = re.search(pattern, buf)
        if m:
            return m
        if re.search(rf"RSP,{seq},NAK,(\w+)", buf):
            return None
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="zhukhome.local")
    ap.add_argument("--rounds", type=int, default=0, help="0 — доки не спините")
    ap.add_argument("--pair", action="store_true", help="зумер грає, мікрофон слухає")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, 81), timeout=5)
    ws.handshake(sock, args.host)
    sock.settimeout(0.3)

    if args.pair:
        m = one(sock, 1, "TEST,buzzer,pair", r"TR,\d+,buzzer,(ok|fail)(?:,(\d+),(\d+))?", 5.0)
        if not m:
            print("Nanit не відповів на перевірку")
            return 1
        if m.group(1) == "ok":
            print(f"зумер чути: фронтів {m.group(2)}, активно {int(m.group(3)) / 100:.2f}%")
            return 0
        print("зумер заграв, але мікрофон його не почув")
        print("це нормально, якщо поріг виставлений на плескіт: тон зумера тихіший")
        return 1

    print("плескайте, коли захочете — рядок на кожну секунду слухання (Ctrl+C спиняє)")
    seq, heard = 0, 0
    try:
        while True:
            seq += 1
            if args.rounds and seq > args.rounds:
                break
            m = one(sock, seq, "TEST,sound,listen", r"TR,\d+,sound,ok,(\d+),(\d+),(\d+),(\d+)")
            # Пауз між замірами не робимо: плескіт, що трапився в паузу, просто не порахувався б
            if not m:
                print(f"{seq:3}: немає відповіді")
                continue
            edges, bp, hundreds, isr = (int(x) for x in m.groups())
            samples = hundreds * 100
            if samples < 10000:
                continue  # обірваний замір: прошивка була зайнята іншою перевіркою
            if edges:
                heard += 1
            # Сумарна тривалість імпульсів: саме вона визначає, чи зловить їх штатне
            # опитування раз на ≈4 мс. 2–3 фронти по 150 мкс воно майже завжди пропускає
            us = bp * 1000 // 10
            bar = "#" * min(40, bp // 5)
            print(f"{seq:3}: тісний цикл {edges:>3}  переривання {isr:>3}  "
                  f"активно {bp / 100:>5.2f}% (~{us:>5} мкс)  {bar}", flush=True)
    except KeyboardInterrupt:
        print()
    print(f"секунд зі звуком: {heard} з {seq - 1 if args.rounds else seq}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
