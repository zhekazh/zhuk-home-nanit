#!/usr/bin/env python3
"""Прогін ESP на тривалість: чи не тече пам'ять і чи тримається зв'язок.

Раз на інтервал підключається, забирає кадри стану й пише рядок у лог. Навмисно
підключається й відключається щоразу, а не тримає одне з'єднання: саме на кожному
новому з'єднанні витік помітний найкраще, та й людина працює зі сторінкою так само.

    python3 esp/soak.py                       прогін на 8 годин, замір раз на 5 хв
    python3 esp/soak.py --hours 24 --every 600
    python3 esp/soak.py --log /шлях/soak.log

Що лог показує: `heap` — вільна пам'ять ESP (падіння з годинами = витік), `up` —
скільки він працює без перезапуску (менше за попереднє = перезавантажився), `bad` —
кадри зі збитим CRC на лінії до Nanit, `rtt` — час відповіді Nanit на PING.
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

ESP = re.compile(r"ESP,([^,]*),([^,]*),(-?\d+),(\d+),(\d+),([^,]*),([^,]*),(\w+)")
NANIT = re.compile(r"NANIT,(\d),([^,]*),(\d+),(\d+)")
SYS = re.compile(r"SYS,(\d+),(\d+),(\d+),(\d+),(\d+),(\d)")
TRIPS = re.compile(r"TR,\d+,sound,ok,\d+,\d+,\d+,\d+,(\d+)")


def sample(host, seconds=5.0):
    """Одне з'єднання: кадри стану плюс виміряний час відповіді Nanit."""
    sock = socket.create_connection((host, 81), timeout=6)
    try:
        ws.handshake(sock, host)
        sock.settimeout(0.5)
        buf, deadline, t0, rtt = "", time.time() + seconds, None, None
        ws.send_text(sock, "1,PING")
        t0 = time.time()
        while time.time() < deadline:
            try:
                buf += sock.recv(4096).decode("utf-8", "replace")
            except socket.timeout:
                continue
            if rtt is None and "RSP,1,ACK" in buf:
                rtt = int((time.time() - t0) * 1000)
        # Лічильник спрацювань датчика звуку: під охороною кожне з них — тривога, тож за
        # ніч видно, скільки хибних тривог було б, і сирену для цього вмикати не треба
        trips = None
        ws.send_text(sock, "2,TEST,sound,listen")
        deadline2 = time.time() + 3.0
        while time.time() < deadline2:
            try:
                buf += sock.recv(4096).decode("utf-8", "replace")
            except socket.timeout:
                continue
            m = TRIPS.search(buf)
            if m:
                trips = int(m.group(1))
                break
        esp, nanit, sysm = ESP.search(buf), NANIT.search(buf), SYS.search(buf)
        if not esp:
            return None
        return {
            "ssid": esp.group(1), "ip": esp.group(2), "rssi": int(esp.group(3)),
            "heap": int(esp.group(4)), "up": int(esp.group(5)), "fw": esp.group(6),
            "mode": esp.group(8),
            "online": nanit.group(1) == "1" if nanit else False,
            "frames": int(nanit.group(3)) if nanit else 0,
            "bad": int(nanit.group(4)) if nanit else 0,
            "rtt": rtt, "trips": trips,
            "nup": int(sysm.group(1)) if sysm else None,
            "ram": int(sysm.group(2)) if sysm else None,
            "loop": int(sysm.group(3)) if sysm else None,
            "batt": int(sysm.group(4)) if sysm else None,
        }
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="zhukhome.local")
    ap.add_argument("--hours", type=float, default=8.0)
    ap.add_argument("--every", type=int, default=300, help="секунд між замірами")
    ap.add_argument("--log", default="soak.log")
    args = ap.parse_args()

    log = pathlib.Path(args.log)
    started = time.time()
    end = started + args.hours * 3600
    prev_up, prev_bad, heap_min, heap_first, reboots, misses = None, None, None, None, 0, 0
    prev_nup, prev_trips, nanit_reboots, ram_min = None, None, 0, None

    def say(line):
        stamp = time.strftime("%H:%M:%S")
        with log.open("a") as f:
            f.write(f"{stamp} {line}\n")
        print(f"{stamp} {line}", flush=True)

    say(f"== прогін на {args.hours} г, замір раз на {args.every} с, хост {args.host} ==")
    while time.time() < end:
        try:
            s = sample(args.host)
        except Exception as e:                       # мережа могла зникнути — це теж дані
            misses += 1
            say(f"НЕМА ЗВ'ЯЗКУ ({type(e).__name__}: {e}) — пропусків усього {misses}")
            time.sleep(args.every)
            continue
        if not s:
            misses += 1
            say(f"з'єднання є, кадрів немає — пропусків усього {misses}")
            time.sleep(args.every)
            continue
        if heap_first is None:
            heap_first = s["heap"]
        heap_min = s["heap"] if heap_min is None else min(heap_min, s["heap"])
        note = ""
        if prev_up is not None and s["up"] < prev_up:
            reboots += 1
            note += f"  ← ESP ПЕРЕЗАПУСТИВСЯ (усього {reboots})"
        if prev_bad is not None and s["bad"] > prev_bad:
            note += f"  ← збитих кадрів +{s['bad'] - prev_bad}"
        if not s["online"]:
            note += "  ← Nanit не на зв'язку"
        if prev_nup is not None and s["nup"] is not None and s["nup"] < prev_nup:
            nanit_reboots += 1
            note += f"  ← NANIT ПЕРЕЗАПУСТИВСЯ (усього {nanit_reboots})"
        if prev_trips is not None and s["trips"] is not None and s["trips"] > prev_trips:
            note += f"  ← датчик звуку спрацював {s['trips'] - prev_trips}× (під охороною = тривоги)"
        if s["ram"] is not None:
            ram_min = s["ram"] if ram_min is None else min(ram_min, s["ram"])
        drift = s["heap"] - heap_first
        say(f"esp up {s['up']:>6}с heap {s['heap']:>5} ({drift:+d})  "
            f"nanit up {s['nup']}с ОЗП {s['ram']} цикл {s['loop']}мс бат {s['batt']}%  "
            f"rssi {s['rssi']:>4}  кадрів {s['frames']:>7} bad {s['bad']}  "
            f"звук {s['trips']}  {s['mode']}{note}")
        prev_up, prev_bad = s["up"], s["bad"]
        prev_nup, prev_trips = s["nup"], s["trips"]
        # Комп'ютер може заснути посеред прогону: тоді між замірами мине набагато більше
        # часу, ніж задано, і в логу з'явиться дірка. Позначаємо її явно, щоб наступного
        # ранку не гадати, чи то модуль мовчав, чи то ноутбук спав
        before = time.time()
        time.sleep(args.every)
        gap = time.time() - before
        if gap > args.every * 2:
            say(f"— перерва {gap / 60:.0f} хв замість {args.every / 60:.0f}: "
                f"комп'ютер спав або скрипт був зупинений")

    say(f"== кінець: перезапусків ESP {reboots}, Nanit {nanit_reboots}, пропусків {misses}, "
        f"heap ESP від {heap_first} до {heap_min} (мінімум), ОЗП Nanit мінімум {ram_min} ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
