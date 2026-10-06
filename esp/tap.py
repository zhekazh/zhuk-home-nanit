#!/usr/bin/env python3
"""Підслухати ESP через USB-порт Nanit.

ESP не має власного монітора (Serial1 на GPIO2 нікуди не виведений), тому свій журнал
він пише в UART0 рядками з «#» — Nanit їх відкидає, бо кадр починається з «$».
Команда «$DBG,<n>,2» вмикає на Nanit дзеркало: усе, що прийшло з Serial3 і не є кадром,
іде на USB із префіксом «~». Так видно і журнал ESP, і аварійний дамп при збої.

    python3 esp/tap.py                 # увімкнути дзеркало й слухати 20 с
    python3 esp/tap.py --seconds 60
    python3 esp/tap.py --dbg 1         # лише кадри Nanit, без тексту ESP
    python3 esp/tap.py --send 'GET,hw'     # надіслати команду в Nanit (без $, номера й CRC)
    python3 esp/tap.py --send 'WATCH,1'    # номер кадру скрипт підставляє сам
    python3 esp/tap.py --no-esp        # не показувати рядки «~» від ESP
"""
import argparse
import os
import select
import subprocess
import time

BAUD = "115200"


def main():
    ap = argparse.ArgumentParser(description="монітор USB-порту Nanit із дзеркалом ESP")
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--dbg", type=int, default=2, choices=[0, 1, 2])
    ap.add_argument("--send", action="append", default=[], help="кадр без $ і CRC")
    ap.add_argument("--no-esp", action="store_true", help="сховати рядки ESP")
    ap.add_argument("--only", default="", help="показувати лише ці типи кадрів, через кому")
    args = ap.parse_args()

    only = [t.strip().upper() for t in args.only.split(",") if t.strip()]
    # -hupcl: закриття порту не смикає DTR, тож Nanit не перезавантажується
    subprocess.run(["stty", "-F", args.port, BAUD, "raw", "-echo", "-echoe", "-echok", "-hupcl"],
                   check=True)
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY)
    os.set_blocking(fd, False)

    t0 = time.monotonic()
    seq = [1]

    def say(body):
        os.write(fd, f"${body}\n".encode())
        print(f"{time.monotonic() - t0:7.2f}  →  ${body}", flush=True)

    def next_seq():
        seq[0] = seq[0] % 65000 + 1
        return seq[0]

    # Відкриття порту смикає DTR і перезапускає Nanit, а після старту дзеркало вимкнене.
    # Тому команду повторюємо весь час: вона ідемпотентна й переживає будь-який перезапуск.
    dbg_at = 0.0
    sent = False
    line = bytearray()
    try:
        while time.monotonic() - t0 < args.seconds:
            el = time.monotonic() - t0
            if el - dbg_at >= 2:
                dbg_at = el
                say(f"DBG,{next_seq()},{args.dbg}")
            if not sent and el >= 4:  # плата вже стартувала — можна давати команди
                sent = True
                for cmd in args.send:
                    say(f"{cmd.split(',')[0]},{next_seq()}," + ",".join(cmd.split(",")[1:])
                        if "," in cmd else f"{cmd},{next_seq()}")
            if not select.select([fd], [], [], 0.2)[0]:
                continue
            for b in os.read(fd, 1024):
                if b in (10, 13):
                    if line:
                        text = bytes(line).decode("utf-8", "replace")
                        text = "".join(c if c.isprintable() or c == "\t" else "." for c in text)
                        show = True
                        if args.no_esp and text.startswith("~"):
                            show = False
                        if only and text.startswith("$"):
                            show = text[1:].split(",")[0].upper() in only
                        if show:
                            print(f"{time.monotonic() - t0:7.2f}  {text}", flush=True)
                        line.clear()
                else:
                    line.append(b)
                    if len(line) > 250:
                        line.clear()
    except KeyboardInterrupt:
        pass
    finally:
        if args.dbg:
            os.write(fd, b"$DBG,99,0\n")  # не лишаємо дзеркало ввімкненим
        os.close(fd)


if __name__ == "__main__":
    main()
