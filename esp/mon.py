#!/usr/bin/env python3
"""Монітор послідовного порту: показує, що говорить ESP8266 (або Nanit).

    python3 esp/mon.py                       # /dev/ttyUSB1, 38400 бод
    python3 esp/mon.py /dev/ttyUSB0 115200   # порт і швидкість
    python3 esp/mon.py /dev/ttyUSB1 74880    # швидкість бутлоадера ESP

Рядки друкуються з міткою часу від запуску. Недруковані байти показуються як «.»:
бутлоадер ESP шле службовий текст на 74880 бод, і на 38400 він виглядає як сміття.
Як і в hw_test.py, порт відкривається через stty й os.open — pyserial не потрібен.
"""
import os
import select
import subprocess
import sys
import time

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
baud = sys.argv[2] if len(sys.argv) > 2 else "38400"

# -hupcl: без цього закриття порту смикає DTR і перезавантажує плату
subprocess.run(["stty", "-F", port, baud, "raw", "-echo", "-echoe", "-echok", "-hupcl"], check=True)
fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
os.set_blocking(fd, False)

print(f"# {port} {baud} бод, Ctrl+C щоб вийти", flush=True)
t0 = time.monotonic()
line = bytearray()
try:
    while True:
        if not select.select([fd], [], [], 0.2)[0]:
            continue
        chunk = os.read(fd, 512)
        for b in chunk:
            if b in (10, 13):
                if line:
                    text = bytes(line).decode("utf-8", "replace")
                    text = "".join(c if c.isprintable() or c == "\t" else "." for c in text)
                    print(f"{time.monotonic() - t0:7.2f}  {text}", flush=True)
                    line.clear()
            else:
                line.append(b)
                if len(line) > 250:  # потік без переводів рядка не повинен з'їсти пам'ять
                    line.clear()
except KeyboardInterrupt:
    pass
finally:
    os.close(fd)
