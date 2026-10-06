#!/usr/bin/env python3
"""Клієнт WebSocket до ESP: показує кадри протоколу й надсилає команди.

    python3 esp/ws.py                          # слухати 10 с
    python3 esp/ws.py --seconds 30 --host 192.168.0.106
    python3 esp/ws.py --send "MODE,fan,on"     # надіслати команду й дочекатись відповіді
    python3 esp/ws.py --send "GET,cfg" --seconds 5
    python3 esp/ws.py --only SEC,ENV           # лише ці типи кадрів
    python3 esp/ws.py --quiet                  # без LOG і ESP/NANIT, тільки протокол

Формат обміну — той самий, що між Nanit і ESP, лише без «*CRC» (див. docs/protocol.md).
Команда від сторінки має вигляд «<id>,<ТИП>[,аргументи]», відповідь приходить як
«RSP,<id>,ACK,<seq>» або «RSP,<id>,NAK,<причина>». Бібліотек не потребує: рукостискання
й кадри WebSocket тут зроблені вручну.
"""
import argparse
import base64
import os
import select
import socket
import struct
import sys
import time

OPCODES = {0x1: "text", 0x2: "binary", 0x8: "close", 0x9: "ping", 0xA: "pong"}


def handshake(sock, host, path="/"):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(1024)
        if not chunk:
            raise SystemExit("з'єднання закрилось під час рукостискання")
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    if b"101" not in head.split(b"\r\n")[0]:
        raise SystemExit("сервер не перейшов на WebSocket:\n" + head.decode("utf-8", "replace"))
    return rest


def send_text(sock, text):
    payload = text.encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    n = len(payload)
    if n < 126:
        header = struct.pack("!BB", 0x81, 0x80 | n)
    elif n < 65536:
        header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
    else:
        header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
    sock.sendall(header + mask + masked)


class Reader:
    """Складає кадри WebSocket із потоку байтів."""

    def __init__(self, sock, rest=b""):
        self.sock = sock
        self.buf = bytearray(rest)

    def frames(self, timeout):
        """Читає доступне й повертає готові текстові повідомлення."""
        if select.select([self.sock], [], [], timeout)[0]:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("сервер закрив з'єднання")
            self.buf += chunk
        out = []
        while True:
            if len(self.buf) < 2:
                break
            b0, b1 = self.buf[0], self.buf[1]
            op = b0 & 0x0F
            n = b1 & 0x7F
            pos = 2
            if n == 126:
                if len(self.buf) < 4:
                    break
                n = struct.unpack("!H", self.buf[2:4])[0]
                pos = 4
            elif n == 127:
                if len(self.buf) < 10:
                    break
                n = struct.unpack("!Q", self.buf[2:10])[0]
                pos = 10
            if b1 & 0x80:  # сервер не маскує, але про запас
                pos += 4
            if len(self.buf) < pos + n:
                break
            payload = bytes(self.buf[pos:pos + n])
            del self.buf[:pos + n]
            if op == 0x8:
                raise ConnectionError("сервер надіслав close")
            if op == 0x9:  # ping → pong
                self.sock.sendall(struct.pack("!BB", 0x8A, 0x80) + os.urandom(4))
                continue
            if op in (0x1, 0x0):
                out.append(payload.decode("utf-8", "replace"))
            elif op != 0xA:
                out.append(f"<{OPCODES.get(op, op)} {n} байтів>")
        return out


def main():
    ap = argparse.ArgumentParser(description="WebSocket-клієнт до прошивки ESP Zhuk Home")
    ap.add_argument("--host", default="zhukhome.local")
    ap.add_argument("--port", type=int, default=81)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--send", action="append", default=[], help="команда без номера: MODE,fan,on")
    ap.add_argument("--only", default="", help="показувати лише ці типи, через кому")
    ap.add_argument("--quiet", action="store_true", help="без LOG, ESP і NANIT")
    args = ap.parse_args()

    only = [t.strip().upper() for t in args.only.split(",") if t.strip()]
    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.settimeout(None)
    reader = Reader(sock, handshake(sock, args.host))
    print(f"# ws://{args.host}:{args.port}/ — слухаю {args.seconds:g} с", flush=True)

    t0 = time.monotonic()
    pending = {}
    for i, cmd in enumerate(args.send, start=1):
        send_text(sock, f"{i},{cmd}")
        pending[str(i)] = cmd
        print(f"{0:7.2f}  →  {i},{cmd}", flush=True)

    try:
        while time.monotonic() - t0 < args.seconds:
            for msg in reader.frames(0.3):
                kind = msg.split(",", 1)[0]
                if args.quiet and kind in ("LOG", "ESP", "NANIT"):
                    continue
                if only and kind not in only and kind != "RSP":
                    continue
                print(f"{time.monotonic() - t0:7.2f}  {msg}", flush=True)
                if kind == "RSP":
                    pending.pop(msg.split(",")[1], None)
    except (ConnectionError, KeyboardInterrupt) as e:
        print(f"# {e}")
    finally:
        sock.close()
    if pending:
        print("# без відповіді: " + "; ".join(pending.values()))
        sys.exit(1)


if __name__ == "__main__":
    main()
