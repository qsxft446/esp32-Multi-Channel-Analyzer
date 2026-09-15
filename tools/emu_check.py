"""Проверка эмуляции MCA по COM-порту (протокол shproto) с ПК.

    python tools/emu_check.py COM3 600000
    python tools/emu_check.py --selftest      # только кодек, без прибора

Шлёт команды так же, как программы на ПК (текст в пакете 0x03 с нулём на
конце), и разбирает ответы по правилам эталонной реализации протокола:
  - пакет 0xFF 0xFE cmd данные CRC16(lo,hi) 0xA5, CRC16 Modbus;
    экранирование 0xFE/0xA5/0xFD через 0xFD и инверсию;
  - ответ -inf: ключи VERSION ... PileUpThr;
  - ответ -cal: 40 строк по 8 hex, CRC32 строк 0..9 в строке 10,
    серийник в строке 39 (8 hex, не FFFFFFFF);
  - набор: статус (>= 10 байт) приходит ДО конца прохода по спектру,
    проход - смещения 0, 64, ... без пропусков до 8192;
  - ответы на команды - как их ждёт BecqMoni (waitForAnswer): первый
    текстовый пакет после команды, обрезанный по \\r, совпадает целиком:
    "-ok" на -sto (в том числе посреди прохода по спектру - за 1 с),
    на -sta - "-ok" или, на 38400 и 115200, строка с предупреждением.
ВНИМАНИЕ: проверка запускает набор (-sta) и останавливает его (-sto);
спектр не сбрасывает.
"""
import struct
import sys
import time

START, ESC, FINISH = 0xFE, 0xFD, 0xA5
CMD_HIST, CMD_TEXT, CMD_STAT = 0x01, 0x03, 0x04
CHANNELS = 8192


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def frame(cmd, payload):
    body = bytes([cmd]) + bytes(payload)
    c = crc16(body)
    out = bytearray([0xFF, START])
    for b in body + bytes([c & 0xFF, c >> 8]):
        if b in (START, FINISH, ESC):
            out += bytes([ESC, (~b) & 0xFF])
        else:
            out.append(b)
    out.append(FINISH)
    return bytes(out)


class Parser:
    """Как shproto_byte_received из эталона."""

    def __init__(self):
        self.buf, self.started, self.esc = bytearray(), False, False
        self.packets, self.dropped = [], 0

    def feed(self, data):
        for b in data:
            if b == START:
                self.buf, self.started, self.esc = bytearray(), True, False
                continue
            if not self.started:
                continue
            if b == ESC:
                self.esc = True
                continue
            if b == FINISH:
                self.started = False
                if len(self.buf) >= 3 and crc16(self.buf) == 0:
                    self.packets.append((self.buf[0], bytes(self.buf[1:-2])))
                else:
                    self.dropped += 1
                continue
            if self.esc:
                b, self.esc = (~b) & 0xFF, False
            self.buf.append(b)


def crc32_std(s):
    import zlib
    return zlib.crc32(s.encode()) & 0xFFFFFFFF


def selftest():
    bad = 0
    ok = crc16(b"123456789") == 0x4B37
    print(("OK    " if ok else "ПРОВАЛ"), "CRC16 Modbus эталонное значение")
    bad += not ok
    payload = bytes([0xFE, 0xA5, 0xFD, 0xFF, 0x00, 0x41]) * 20
    p = Parser()
    p.feed(b"\x12\x34" + frame(CMD_TEXT, payload) + b"\xFF")
    ok = p.packets == [(CMD_TEXT, payload)] and p.dropped == 0
    print(("OK    " if ok else "ПРОВАЛ"), "кадр с экранируемыми байтами туда-обратно")
    bad += not ok
    f = bytearray(frame(CMD_TEXT, b"-inf\0"))
    f[4] ^= 0x01
    p = Parser()
    p.feed(bytes(f))
    ok = not p.packets and p.dropped == 1
    print(("OK    " if ok else "ПРОВАЛ"), "битый кадр отбрасывается")
    bad += not ok
    return bad


def run(port, baud):
    import serial                       # pyserial - есть в окружении IDF
    ser = serial.Serial(port, baud, timeout=0.05)
    p = Parser()

    def collect(sec):
        t_end = time.time() + sec
        while time.time() < t_end:
            p.feed(ser.read(4096))

    def cmd(text, wait=0.8):
        p.packets.clear()
        ser.write(frame(CMD_TEXT, text.encode() + b"\0"))
        collect(wait)
        return list(p.packets)

    fails = []

    def check(name, cond, detail=""):
        print(("OK    " if cond else "ПРОВАЛ"), name, ("-- " + detail) if detail else "")
        if not cond:
            fails.append(name)

    def answer(text, timeout):
        """Как waitForAnswer в BecqMoni: ПЕРВЫЙ текстовый пакет после
        команды, обрезанный по первому \\r. Возвращает (строка, секунд)."""
        p.packets.clear()
        t0 = time.time()
        ser.write(frame(CMD_TEXT, text.encode() + b"\0"))
        while time.time() - t0 < timeout:
            p.feed(ser.read(4096))
            for c, d in p.packets:
                if c == CMD_TEXT:
                    s = d.decode("ascii", errors="replace")
                    return s.split("\r")[0], time.time() - t0
        return None, timeout

    slow = baud in (38400, 115200)
    want_sta = ("Warning: silent mode forced due to low interface speed-ok"
                if slow else "-ok")

    ser.reset_input_buffer()
    a, dt = answer("-sto", 1.0)
    check("-sto: ответ -ok за 1 с (как ждёт BecqMoni)", a == "-ok", f"{a!r} за {dt:.2f} с")
    collect(1.0)                        # тишина перед проверками

    texts = [d.decode(errors="replace") for c, d in cmd("-inf") if c == CMD_TEXT]
    inf = "".join(texts)
    check("-inf: ответ с VERSION и PileUpThr", "VERSION " in inf and "PileUpThr " in inf,
          inf[:90].replace("\r\n", " | "))
    keys = {}
    toks = inf.replace("\r", " ").replace("\n", " ").split()
    for i, k in enumerate(toks[:-1]):
        if k in ("VERSION", "RISE", "FALL", "NOISE", "F", "MAX", "t"):
            keys[k] = toks[i + 1]
    check("-inf: RISE, FALL, F разбираются", all(k in keys for k in ("RISE", "FALL", "F")),
          str(keys))

    cal = "".join(d.decode(errors="replace") for c, d in cmd("-cal") if c == CMD_TEXT)
    lines = [l for l in cal.replace("\r", "").split("\n") if l]
    hexok = all(len(l) == 8 and all(ch in "0123456789ABCDEFabcdef" for ch in l)
                for l in lines)
    check("-cal: 40 строк по 8 hex", len(lines) == 40 and hexok, f"строк {len(lines)}")
    if len(lines) == 40:
        check("-cal: CRC32 строк 0..9 в строке 10",
              crc32_std("".join(lines[:10])) == int(lines[10], 16))
        check("-cal: серийник 8 hex и не FFFFFFFF", lines[39].upper() != "FFFFFFFF",
              lines[39])

    stt = "".join(d.decode(errors="replace") for c, d in cmd("-stt") if c == CMD_TEXT)
    check("-stt: stopped после -sto", stt.strip() == "stopped", repr(stt))

    a, dt = answer("-sta", 2.0 if slow else 1.0)
    check("-sta: ответ как ждёт BecqMoni", a == want_sta, f"{a!r} за {dt:.2f} с")
    # ответ на -sto посреди прохода по спектру - тоже за секунду
    collect(1.3)
    a, dt = answer("-sto", 1.0)
    check("-sto посреди прохода: -ok за 1 с", a == "-ok", f"{a!r} за {dt:.2f} с")
    collect(1.0 if not slow else 9.5)   # дать закончиться начатому проходу
    a, _ = answer("-sta", 2.0 if slow else 1.0)
    pk = [(c, d) for c, d in p.packets]
    p.packets.clear()
    collect(3.5 if not slow else 12.0)
    pk += list(p.packets)
    hist = [(struct.unpack_from("<H", d)[0], (len(d) - 2) // 4)
            for c, d in pk if c == CMD_HIST]
    stat_i = [i for i, (c, d) in enumerate(pk) if c == CMD_STAT]
    sweep_end = [i for i, (c, d) in enumerate(pk)
                 if c == CMD_HIST and struct.unpack_from("<H", d)[0] + (len(d) - 2) // 4 >= CHANNELS]
    check("набор: пакеты спектра идут", len(hist) >= CHANNELS // 64, f"пакетов {len(hist)}")
    if sweep_end and stat_i:
        check("набор: статус приходит до конца прохода", stat_i[0] < sweep_end[0])
        st = [d for c, d in pk if c == CMD_STAT][0]
        check("статус: не короче 10 байт", len(st) >= 10, f"{len(st)} байт")
        if len(st) >= 10:
            t, load, cps = struct.unpack_from("<IHI", st)
            print(f"       время {t} с, загрузка {load} %, CPS {cps}")
    first = next((i for i, (o, n) in enumerate(hist) if o == 0), None)
    if first is not None and first + CHANNELS // 64 <= len(hist):
        sw = hist[first:first + CHANNELS // 64]
        check("проход: смещения 0, 64, ... без пропусков до 8192",
              all(o == i * 64 and n == 64 for i, (o, n) in enumerate(sw)))
    else:
        check("проход по спектру целиком получен", False)
    stt = "".join(d.decode(errors="replace") for c, d in cmd("-stt") if c == CMD_TEXT)
    check("-stt: collecting во время набора", "collecting" in stt, repr(stt))

    cmd("-sto", 2.0 if not slow else 10.0)   # дать закончиться начатому проходу
    pk = cmd("-stt", 1.5)
    check("-sto: спектр больше не идёт", not any(c == CMD_HIST for c, d in pk))

    a, _ = answer("-ris 8", 1.0)
    check("заводская команда: честный отказ", a == "-err not supported", repr(a))

    pk = cmd("-sho", 3.0 if not slow else 10.0)
    n = sum(1 for c, d in pk if c == CMD_HIST)
    check("-sho: ровно один проход", n == CHANNELS // 64, f"пакетов {n}")

    print(f"\nбитых кадров: {p.dropped}")
    print("Все проверки пройдены" if not fails else f"ПРОВАЛЕНО: {len(fails)}")
    return len(fails)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(1 if selftest() else 0)
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    sys.exit(1 if run(sys.argv[1], int(sys.argv[2])) else 0)
