#!/usr/bin/env python3
"""Локальний перегляд сторінок проєкту.

Потрібен, бо `python3 -m http.server` віддає HTML без `charset=utf-8`, і браузер
вгадує кодування — кирилиця перетворюється на кракозябри. Тут charset заданий явно.

`prototype.html` і `plan.html` — самодостатні документи: вони мають власні `<head>`,
`<meta charset>` і самі стамплюють `?theme=dark|light`. Так їх можна віддавати як
статику з GitHub Pages, відкривати з файлової системи й бачити тут — без розбіжностей
між цими трьома способами.

Обгортка лишилася для сторінок без власного `<head>` — на випадок, якщо така знову
з'явиться: для них цей сервер підставляє мінімальний каркас і сам стамплює `?theme=`.

Запуск:  python3 serve.py            http://127.0.0.1:8080/prototype.html
         python3 serve.py --port 9000
"""
import argparse
import http.server
import pathlib
import socketserver
import urllib.parse

HEAD = ('<!doctype html><html{attr}><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">'
        '<style>:root{{color-scheme:light}}body{{margin:0}}</style></head><body>')
ROOT = pathlib.Path(__file__).resolve().parent


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=str(ROOT), **kw)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        name = url.path.lstrip("/") or "prototype.html"
        page = ROOT / name
        if not (name.endswith(".html") and page.is_file()):
            return super().do_GET()

        text = page.read_text(encoding="utf-8")
        if text.lstrip()[:9].lower() == "<!doctype":
            body = text  # самодостатня сторінка: тему вона стамплює сама
        else:
            theme = urllib.parse.parse_qs(url.query).get("theme", [""])[0]
            attr = f' data-theme="{theme}"' if theme in ("dark", "light") else ""
            body = HEAD.format(attr=attr) + text + "</body></html>"
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", a.port), Handler) as srv:
        print(f"http://127.0.0.1:{a.port}/prototype.html")
        print(f"http://127.0.0.1:{a.port}/prototype.html?theme=dark   — темна тема")
        print(f"http://127.0.0.1:{a.port}/plan.html                   — схема будиночка")
        print("Ctrl+C — зупинити")
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\nзупинено")


if __name__ == "__main__":
    main()
