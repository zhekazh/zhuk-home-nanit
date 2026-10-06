#!/usr/bin/env python3
"""Перевірка прошивки Nanit на живому макеті через USB.

Говорить із платою тим самим протоколом, що й ESP (docs/protocol.md), лише через
USB-порт. Монітор порту в Arduino IDE має бути закритий: порт тримає одна програма.

Запуск:  python3 hw_test.py                  усе, крім перевірок, що потребують дій
         python3 hw_test.py --interactive    разом із датчиками й клавіатурою
         python3 hw_test.py --port /dev/ttyUSB0 --pin 1234
"""
import argparse
import glob
import random
import os
import select
import subprocess
import sys
import time

BAUD = 115200
BOOT_WAIT = 6.0  # відкриття порту перезапускає плату


def crc8(data: bytes) -> int:
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


class Nanit:
    def __init__(self, port, log=None):
        subprocess.run(["stty", "-F", port, str(BAUD), "raw", "-echo", "-echoe", "-echok", "-hupcl"],
                       check=True)
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        os.set_blocking(self.fd, False)
        self.buf = b""
        self.frames = []   # (час, тіло) правильних кадрів
        self.text = []     # рядки з «#» та інше сміття
        self.crc_errors = 0
        # Номери починаємо з випадкового: Nanit не виконує повтор тієї самої команди
        # протягом 4 с (SEEN_TTL), тож два запуски підряд не повинні брати ті самі номери
        self.seq = random.randint(1, 50000)
        self.log = open(log, "w", encoding="utf-8") if log else None

    def close(self):
        os.close(self.fd)
        if self.log:
            self.log.close()

    def _note(self, direction, line):
        if self.log:
            self.log.write(f"{time.time():.3f} {direction} {line}\n")
            self.log.flush()

    def pump(self, timeout=0.05):
        """Читає все, що прийшло, і розкладає на кадри."""
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                return
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                continue
            chunk = os.read(self.fd, 4096)
            if not chunk:
                continue
            self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.strip(b"\r").decode("utf-8", "replace").strip()
                if not line:
                    continue
                self._note("<-", line)
                if line.startswith("$") and "*" in line:
                    body, _, crc = line[1:].rpartition("*")
                    try:
                        ok = int(crc, 16) == crc8(body.encode())
                    except ValueError:
                        ok = False
                    if ok:
                        self.frames.append((time.time(), body))
                    else:
                        self.crc_errors += 1
                        self.text.append(line)
                else:
                    self.text.append(line)

    def send_raw(self, line):
        self._note("->", line)
        os.write(self.fd, (line + "\n").encode())

    def send(self, body, crc=True):
        line = f"${body}*{crc8(body.encode()):02X}" if crc else f"${body}"
        self.send_raw(line)

    def mark(self):
        return len(self.frames)

    def since(self, mark):
        return [b for _, b in self.frames[mark:]]

    def wait(self, pred, timeout=2.0, mark=None):
        """Чекає кадр, для якого pred(тіло) істинне. mark — звідки переглядати."""
        start = self.mark() if mark is None else mark
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            for _, body in self.frames[start:]:
                if pred(body):
                    return body
        return None

    def collect(self, seconds):
        start = len(self.frames)
        end = time.time() + seconds
        while time.time() < end:
            self.pump(0.1)
        return [b for _, b in self.frames[start:]]

    def cmd(self, kind, args="", timeout=2.0, crc=True):
        """Надсилає команду з номером і повертає тіло відповіді ACK/NAK."""
        self.seq = self.seq % 60000 + 1
        seq = self.seq
        self.cmd_mark = self.mark()  # звідси шукаємо кадри, що йдуть до ACK
        body = f"{kind},{seq}" + (f",{args}" if args else "")
        self.send(body, crc=crc)
        resp = self.wait(lambda b: b.startswith((f"ACK,{seq}", f"NAK,{seq}")) and
                         (b.split(",")[1] == str(seq)), timeout, self.cmd_mark)
        return resp, seq


class Report:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.notes = []

    def section(self, title):
        print(f"\n\033[1m{title}\033[0m")

    def ok(self, text, detail=""):
        self.passed += 1
        print(f"  \033[32m✓\033[0m {text}" + (f" — {detail}" if detail else ""))

    def bad(self, text, detail=""):
        self.failed += 1
        print(f"  \033[31m✗\033[0m {text}" + (f" — {detail}" if detail else ""))
        self.notes.append(f"{text}: {detail}")

    def skip(self, text, detail=""):
        self.skipped += 1
        print(f"  \033[33m•\033[0m {text}" + (f" — {detail}" if detail else ""))

    def info(self, text):
        print(f"    {text}")

    def check(self, cond, text, detail=""):
        (self.ok if cond else self.bad)(text, detail)
        return cond


def find_port():
    ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def port_busy(port):
    r = subprocess.run(["fuser", port], capture_output=True, text=True)
    return bool(r.stdout.strip())


# ---------- окремі перевірки ----------

def test_link(n, rep):
    rep.section("1. Зв'язок")
    resp, _ = n.cmd("PING")
    rep.check(resp and resp.startswith("ACK"), "PING отримує ACK", resp or "немає відповіді")

    resp, _ = n.cmd("FOO")
    rep.check(resp and ",type" in resp, "невідома команда дає NAK,type", resp or "немає відповіді")

    # кадр із поламаною контрольною сумою має бути мовчки відкинутий
    n.seq = n.seq % 60000 + 1
    seq = n.seq
    n.send_raw(f"$PING,{seq}*00")
    bad = n.wait(lambda b: b.startswith(f"ACK,{seq}"), 0.7)
    rep.check(bad is None, "кадр із неправильним CRC ігнорується", bad or "")

    resp, _ = n.cmd("GET", "hi")
    hi = next((b for b in n.since(n.cmd_mark) if b.startswith("HI,")), None)
    if rep.check(hi is not None, "GET,hi повертає привітання", hi or "немає кадру HI"):
        f = hi.split(",")
        rep.info(f"протокол {f[1]}, прошивка {f[2]}, плата {f[3]}, останній старт {f[4]}")


def test_status(n, rep):
    rep.section("2. Кадри стану")
    n.cmd("DBG", "1")
    frames = n.collect(3.0)
    n.cmd("DBG", "0")
    kinds = {}
    for b in frames:
        kinds.setdefault(b.split(",")[0], []).append(b)
    for kind in ("SEC", "ENV", "DEV", "PRK", "SYS"):
        got = kinds.get(kind, [])
        rep.check(len(got) >= 2, f"кадр {kind} надходить регулярно", f"{len(got)} за 3 с")
    rep.check(n.crc_errors == 0, "усі отримані кадри з правильним CRC", f"помилок: {n.crc_errors}")

    env = kinds.get("ENV", [None])[-1]
    if env:
        f = env.split(",")
        rep.info(f"клімат: {f[1]} °C, {f[2]} %, CO {f[3]} (АЦП {f[4]}), освітленість {f[5]} "
                 f"({'світло' if f[6] == '1' else 'темно'})")
    sys_f = kinds.get("SYS", [None])[-1]
    if sys_f:
        f = sys_f.split(",")
        rep.info(f"система: працює {f[1]} с, вільно ОЗП {f[2]} Б, цикл до {f[3]} мс, "
                 f"батарея {f[4]} % ({f[5]} мВ)")
        rep.check(int(f[2]) > 500, "вільної пам'яті достатньо", f"{f[2]} байт")
        rep.check(int(f[3]) <= 20, "цикл не довший за 20 мс", f"{f[3]} мс")
    prk = kinds.get("PRK", [None])[-1]
    if prk:
        f = prk.split(",")
        rep.info(f"паркінг: шлагбаум {f[2]}, відстань {f[3]}, світлофор {f[4]}, дисплей «{f[5]}»")


def test_settings(n, rep):
    rep.section("3. Налаштування")
    n.cmd("GET", "cfg", timeout=3.0)
    vals = [b for b in n.since(n.cmd_mark) if b.startswith("VAL,cfg,")]
    rep.check(len(vals) == 14, "GET,cfg повертає всі 14 налаштувань", f"отримано {len(vals)}")
    current = {b.split(",")[2]: b.split(",")[3] for b in vals}

    resp, _ = n.cmd("SET", "fanOff,29")
    rep.check(resp and ",arg,fanOff" in resp, "SET із суперечливим значенням відхиляється", resp or "")

    resp, _ = n.cmd("SET", "fanOff,25")
    rep.check(resp and resp.startswith("ACK"), "SET із правильним значенням приймається", resp or "")
    n.cmd("GET", "cfg", timeout=3.0)
    vals2 = {b.split(",")[2]: b.split(",")[3] for b in n.since(n.cmd_mark) if b.startswith("VAL,cfg,")}
    rep.check(vals2.get("fanOff") == "25", "нове значення справді збережено", vals2.get("fanOff"))
    n.cmd("SET", f"fanOff,{current.get('fanOff', '26')}")  # повертаємо як було

    resp, _ = n.cmd("CAL", "rgb,order,BGR")
    rep.check(resp and resp.startswith("ACK"), "калібрування приймається", resp or "")
    n.cmd("CAL", "rgb,order,RGB")


def test_hw(n, rep, strict):
    rep.section("4. Стан модулів")
    n.cmd("GET", "hw", timeout=3.0)
    hw = {b.split(",")[1]: b.split(",")[2:] for b in n.since(n.cmd_mark) if b.startswith("HW,")}
    rep.check(len(hw) == 16, "стан приходить по всіх 16 модулях", f"отримано {len(hw)}")
    auto = {"dht": "DHT11", "mq7": "MQ-7", "ldr": "датчик світла",
            "sonar": "далекомір", "tm1637": "дисплей паркінгу", "board": "плата"}
    for comp, name in auto.items():
        st = hw.get(comp, ["?"])[0]
        if st == "ok":
            rep.ok(f"{name} відповідає")
        elif strict:
            rep.bad(f"{name} не відповідає", f"стан «{st}»")
        else:
            rep.skip(f"{name}: стан «{st}»")
    others = [c for c, v in hw.items() if v[0] == "none" and c not in auto]
    if others:
        rep.info("ще не перевірялися: " + ", ".join(others))


def tr_result(n, seq, timeout=20.0, mark=None):
    """Чекає остаточний результат перевірки (пропускає проміжний wait)."""
    end = time.time() + timeout
    seen = mark if mark is not None else n.mark()
    while time.time() < end:
        body = n.wait(lambda b: b.startswith(f"TR,{seq},"), max(0.1, end - time.time()), seen)
        if body is not None:
            seen = n.mark()
        if body is None:
            return None
        parts = body.split(",")
        if len(parts) > 3 and parts[3] == "wait":
            continue
        return body
    return None


def run_test(n, rep, comp, action, title, timeout=20.0):
    resp, seq = n.cmd("TEST", f"{comp},{action}", timeout=3.0)
    mark = n.cmd_mark
    if not resp or not resp.startswith("ACK"):
        rep.bad(title, resp or "немає відповіді")
        return None
    tr = tr_result(n, seq, timeout, mark)
    if not tr:
        rep.bad(title, "немає результату TR")
        return None
    parts = tr.split(",")
    ok = parts[3] == "ok"
    (rep.ok if ok else rep.bad)(title, ", ".join(parts[4:]) if len(parts) > 4 else parts[3])
    return tr


def test_passive(n, rep):
    rep.section("5. Перевірки без руху")
    run_test(n, rep, "dht", "read", "DHT11: читання температури й вологості")
    run_test(n, rep, "mq7", "read", "MQ-7: вимірювання")
    run_test(n, rep, "sonar", "read", "далекомір: вимірювання відстані")
    run_test(n, rep, "tm1637", "all", "дисплей: усі сегменти (8888)")
    time.sleep(3)
    run_test(n, rep, "tm1637", "stop", "дисплей: напис StOP")
    run_test(n, rep, "board", "screen", "екран Nanit: кольорові смуги")
    time.sleep(3)


def test_actuators(n, rep):
    rep.section("6. Виконавчі пристрої (сервісний режим)")
    resp, _ = n.cmd("SVC", "1")
    if not rep.check(resp and resp.startswith("ACK"), "сервісний режим увімкнено", resp or ""):
        return
    try:
        run_test(n, rep, "servo", "open", "штора: відкрити", 25.0)
        run_test(n, rep, "servo", "close", "штора: зачинити", 25.0)
        run_test(n, rep, "gate", "open", "шлагбаум: відкрити", 30.0)
        run_test(n, rep, "gate", "close", "шлагбаум: зачинити", 30.0)
        run_test(n, rep, "fan", "run", "вентилятор: 3 секунди", 10.0)
        run_test(n, rep, "tlight", "cycle", "світлофор: червоний, жовтий, зелений", 10.0)
        for ch, name in (("r", "червоний"), ("g", "зелений"), ("b", "синій")):
            run_test(n, rep, "rgb", ch, f"RGB: {name} канал", 8.0)
            time.sleep(2.5)
        run_test(n, rep, "buzzer", "beep", "зумер: короткий сигнал")
        time.sleep(0.5)
        run_test(n, rep, "buzzer", "melody", "зумер: мелодія")
        time.sleep(1)
        run_test(n, rep, "buzzer", "siren", "зумер: сирена")
        time.sleep(1.5)
    finally:
        n.cmd("SVC", "0")


def ask(prompt):
    try:
        input(f"    \033[36m{prompt} (Enter — далі, s — пропустити)\033[0m ")
        return True
    except EOFError:
        return False


def ensure_disarmed(n, rep, pin):
    """Датчики, клавіатура й сервісний режим потребують знятої охорони."""
    n.cmd("DBG", "1")
    state = n.wait(lambda b: b.startswith("SEC,"), 2.0)
    n.cmd("DBG", "0")
    if state and not state.startswith("SEC,DIS"):
        resp, _ = n.cmd("DISARM", pin)
        ok = resp and resp.startswith("ACK")
        rep.check(ok, "охорону знято перед перевіркою", resp or "")
        return bool(ok)
    return True


def raw_levels(n):
    """Сирі рівні пінів датчиків порту 5 через кадр IO."""
    n.cmd("WATCH", "1", timeout=0.5)  # кадри IO йдуть на порт, що попросив WATCH
    body = n.wait(lambda b: b.startswith("IO,"), 2.0)
    n.cmd("WATCH", "0", timeout=0.5)
    if not body:
        return {}
    f = body.split(",")
    return {"pir": f[1], "sound": f[2], "wsens": f[3]}


def test_sensors(n, rep, tries=3, only=None):
    rep.section("7. Датчики (потрібна ваша дія)")
    checks = [("pir", "Пройдіть або помахайте рукою перед датчиком руху"),
              ("sound", "Плесніть у долоні 3 рази поспіль біля датчика звуку"),
              ("wsens", "Піднесіть руку до датчика біля вікна"),
              ("ldr", "Накрийте датчик освітленості долонею")]
    if only:
        checks = [c for c in checks if c[0] in only]
    for comp, prompt in checks:
        done = False
        for attempt in range(1, tries + 1):
            rep.info(f"{prompt} (спроба {attempt} з {tries}, є 15 секунд)")
            resp, seq = n.cmd("TEST", f"{comp},wait", timeout=3.0)
            mark = n.cmd_mark
            if not resp or not resp.startswith("ACK"):
                rep.bad(f"{comp}: перевірку не почато", resp or "немає відповіді")
                done = True
                break
            tr = tr_result(n, seq, 17.0, mark)
            if tr and tr.split(",")[3] == "ok":
                rep.ok(f"{comp}: датчик спрацював", ", ".join(tr.split(",")[4:]))
                done = True
                break
        if not done:
            rep.bad(f"{comp}: спрацювання не дочекались", f"{tries} спроби по 15 с")
            level = raw_levels(n).get(comp)
            if level is not None:
                meaning = {"pir": {"1": "постійно «є рух»", "0": "постійно «спокій»"},
                           "sound": {"1": "постійно «тиша»", "0": "постійно «звук»"},
                           "wsens": {"1": "постійно «вільно»", "0": "постійно «перешкода»"}}
                rep.info(f"сирий рівень піна: {level} — {meaning[comp][level]}")
                rep.info("якщо рівень не змінюється зовсім, перевірте, чи під'єднано модуль до порту 5")


def test_gesture(n, rep, seconds=30.0):
    rep.section("10. Жест шторою")
    n.cmd("GET", "cal,wsens", timeout=3.0)
    n.pump(0.7)
    gest = next((b for b in n.since(n.cmd_mark) if b.startswith("VAL,wsens,gest")), None)
    if gest and gest.split(",")[-1] == "0":
        rep.skip("жест вимкнено калібруванням", "увімкнути: $CAL,1,wsens,gest,1")
        return
    n.cmd("DBG", "1")  # кадри стану й події йдуть на порт ESP, тому вмикаємо дзеркало
    try:
        # Стан звіряємо за кадром DEV, а не за подією: черга подій на Nanit забита
        # повторами, поки ESP їх не підтверджує, і подія від жесту застрягає в хвості
        before = n.wait(lambda b: b.startswith("DEV,"), 3.0)
        if not rep.check(bool(before), "кадр стану приходить", before or "кадра DEV не було"):
            return
        was = before.split(",")
        rep.info(f"шторка зараз: режим {was[3]}, ціль {was[5]}%")
        rep.info(f"Піднесіть руку до датчика біля вікна, потримайте 2 с і приберіть "
                 f"(є {seconds:.0f} с)")
        after = n.wait(lambda b: b.startswith("DEV,") and len(b.split(",")) > 5 and
                       b.split(",")[5] != was[5], seconds)
        if not rep.check(bool(after), "жест перемикає шторку",
                         f"ціль лишилась {was[5]}%" if not after else ""):
            rep.info("жест працює лише зі знятою охороною і поза сервісним режимом")
            rep.info("час утримання руки: $CAL,1,wsens,hold,<200..3000>")
            return
        f = after.split(",")
        rep.info(f"нова ціль: {f[5]}%")
        rep.check(f[3] == "man", "шторка перейшла в ручний режим", f"winMode={f[3]}")
        ev = next((b for b in n.since(max(0, n.mark() - 40))
                   if b.startswith("EV,") and "MODE,win" in b and "gesture" in b), None)
        if ev:
            rep.info(f"подія: {ev}")
    finally:
        n.cmd("DBG", "0")


def test_keypad(n, rep):
    rep.section("8. Клавіатура")
    resp, _ = n.cmd("SVC", "1")
    if not rep.check(resp and resp.startswith("ACK"), "сервісний режим для клавіатури", resp or ""):
        rep.info("без сервісного режиму клавіша «A» поставила б будинок на охорону — пропускаємо")
        return
    n.cmd("WATCH", "1")  # кадри KEY ідуть на порт, що попросив WATCH
    try:
        want = ["1", "2", "3", "5"]
        rep.info("Натисніть по черзі клавіші: " + " ".join(want) + " (є 45 секунд)")
        got = []
        end = time.time() + 45
        while time.time() < end and len(got) < len(want):
            body = n.wait(lambda b: b.startswith("KEY,"), 1.0)
            if body:
                got.append(body.split(",")[1])
                rep.info(f"отримано «{got[-1]}»")
            n.cmd("WATCH", "1", timeout=0.3)  # поновлюємо: спостереження вимикається за 10 с
        rep.check(got == want, "клавіші приходять у правильному порядку",
                  "отримано: " + " ".join(got) if got else "жодної клавіші")
        if got and got != want:
            rep.info("якщо клавіші «не ті» — спробуйте $CAL,1,keypad,swap,1")
    finally:
        n.cmd("WATCH", "0")
        n.cmd("SVC", "0")


def test_security(n, rep, pin):
    rep.section("9. Охорона")
    n.cmd("SET", "exitDelay,3")
    n.cmd("DBG", "1")
    try:
        resp, _ = n.cmd("ARM", "ARM")
        if not rep.check(resp and resp.startswith("ACK"), "постановка на охорону", resp or ""):
            return
        sec = n.wait(lambda b: b.startswith("SEC,EXIT"), 2.0)
        rep.check(sec is not None, "почався відлік на вихід", sec or "")
        armed = n.wait(lambda b: b.startswith("SEC,ARM"), 8.0)
        rep.check(armed is not None, "будинок став під охорону", armed or "")

        resp, _ = n.cmd("DISARM", "0000")
        rep.check(resp and ",pin," in resp, "невірний код відхиляється", resp or "")

        resp, _ = n.cmd("DISARM", pin)
        ok = resp and resp.startswith("ACK")
        rep.check(ok, "правильний код знімає охорону", resp or "")
        if not ok:
            rep.info("якщо код змінювали, запустіть скрипт із --pin <ваш код>")
            n.cmd("DISARM", pin)
    finally:
        n.cmd("SET", "exitDelay,30")
        # переконуємось, що будинок не лишився під охороною після перевірки
        n.cmd("DBG", "1")
        state = n.wait(lambda b: b.startswith("SEC,"), 2.0)
        n.cmd("DBG", "0")
        if state and not state.startswith("SEC,DIS"):
            rep.bad("після перевірки охорона лишилась увімкненою", state)
            rep.info(f"зніміть вручну: $DISARM,1,{pin}")


def main():
    ap = argparse.ArgumentParser(description="Перевірка прошивки Nanit на макеті")
    ap.add_argument("--port", default=None, help="послідовний порт (типово знаходиться сам)")
    ap.add_argument("--pin", default="1234", help="код охорони (типово 1234)")
    ap.add_argument("--interactive", action="store_true", help="разом із датчиками й клавіатурою")
    ap.add_argument("--no-motion", action="store_true", help="без перевірок, що рухають механіку")
    ap.add_argument("--strict", action="store_true", help="вважати помилкою будь-який модуль не «ok»")
    ap.add_argument("--log", default="hw_test.log", help="файл із усім обміном")
    ap.add_argument("--yes", action="store_true", help="не питати підтвердження")
    ap.add_argument("--only", default=None,
                    help="лише ці секції через кому: link,status,settings,hw,passive,"
                         "actuators,sensors,gesture,keypad,security")
    ap.add_argument("--sensor", default=None,
                    help="лише ці датчики в секції sensors через кому: pir,sound,wsens,ldr")
    args = ap.parse_args()
    only = set(args.only.split(",")) if args.only else None
    sensors = set(args.sensor.split(",")) if args.sensor else None
    if sensors and not only:
        only = {"sensors"}  # назвали датчик — решту секцій не чіпаємо

    def want(name):
        return only is None or name in only

    port = args.port or find_port()
    if not port:
        print("Не знайдено жодного порту /dev/ttyUSB* або /dev/ttyACM*. Nanit під'єднано?")
        return 2
    if port_busy(port):
        print(f"Порт {port} зайнятий: закрийте монітор порту в Arduino IDE й спробуйте знову.")
        return 2
    if not args.no_motion and not args.yes and want("actuators"):
        print("Перевірка рухатиме штору, шлагбаум і вентилятор, а також вмикатиме звук.")
        try:
            input("Enter — почати, Ctrl+C — скасувати ")
        except (EOFError, KeyboardInterrupt):
            return 1

    print(f"Порт {port}, {BAUD} бод. Плата перезапуститься після підключення.")
    n = Nanit(port, args.log)
    rep = Report()
    try:
        n.wait(lambda b: False, BOOT_WAIT)  # даємо платі час завантажитись
        rep.section("0. Старт")
        if any("Zhuk Home" in t for t in n.text):
            rep.ok("плата стартувала й привіталась")
        else:
            resp, _ = n.cmd("PING", timeout=3.0)
            rep.check(resp and resp.startswith("ACK"), "плата відповідає",
                      "без перезапуску під час підключення" if resp else "мовчить")

        if want("link"):
            test_link(n, rep)
        if want("status"):
            test_status(n, rep)
        if want("settings"):
            test_settings(n, rep)
        if want("hw"):
            test_hw(n, rep, args.strict)
        if want("passive"):
            test_passive(n, rep)
        if want("actuators"):
            if not args.no_motion:
                test_actuators(n, rep)
            else:
                rep.skip("перевірки з рухом пропущено (--no-motion)")
        if args.interactive or only:
            if want("sensors"):
                ensure_disarmed(n, rep, args.pin)
                test_sensors(n, rep, only=sensors)
            if want("keypad"):
                test_keypad(n, rep)
            if want("gesture"):
                ensure_disarmed(n, rep, args.pin)
                test_gesture(n, rep)
            if want("security"):
                ensure_disarmed(n, rep, args.pin)
                test_security(n, rep, args.pin)
        else:
            rep.skip("датчики, клавіатура й охорона пропущені (додайте --interactive)")
    except KeyboardInterrupt:
        print("\nПерервано.")
    finally:
        try:
            n.cmd("SVC", "0", timeout=1.0)
            n.cmd("DBG", "0", timeout=1.0)
        except Exception:
            pass
        n.close()

    print(f"\n\033[1mПідсумок:\033[0m {rep.passed} успішно, {rep.failed} з помилкою, "
          f"{rep.skipped} пропущено. Повний обмін: {args.log}")
    if rep.notes:
        print("Не спрацювало:")
        for note in rep.notes:
            print(f"  • {note}")
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
