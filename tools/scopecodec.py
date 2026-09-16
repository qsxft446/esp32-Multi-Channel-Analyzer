"""Модель сжатия осциллограммы из main/scope_codec.c - построчно.

Полубайты, старший первым: первый отсчёт - 3 полубайта (12 бит), дальше
разность с предыдущим -7..7 одним полубайтом, полубайт 8 - следом отсчёт
целиком (3 полубайта). Нечётный хвост дополняется нулём.

encode() режет выход так же, как прошивка режет сообщение WebSocket:
первый кусок меньше на заголовок, дальше куски по cap байт, и в каждом
куске кодируется, пока остаётся не меньше 2 байт (на отсчёт - до 4
полубайтов). Состояние (предыдущий отсчёт, недописанный полубайт)
переходит через границы кусков.
"""


class Encoder:
    def __init__(self):
        self.prev = 0
        self.started = False
        self.half = False
        self.acc = 0

    def _put(self, out, nib):
        if self.half:
            out.append(self.acc | nib)
            self.half = False
        else:
            self.acc = (nib << 4) & 0xFF
            self.half = True

    def encode(self, src, cap):
        """-> (сколько отсчётов взято, байты)"""
        out = bytearray()
        i = 0
        while i < len(src) and cap - len(out) >= 2:
            v = src[i] & 0x0FFF
            d = v - self.prev
            if self.started and -7 <= d <= 7:
                self._put(out, d & 0x0F)
            else:
                if self.started:
                    self._put(out, 8)
                self._put(out, v >> 8)
                self._put(out, (v >> 4) & 0x0F)
                self._put(out, v & 0x0F)
                self.started = True
            self.prev = v
            i += 1
        return i, bytes(out)

    def finish(self):
        if not self.half:
            return b''
        self.half = False
        return bytes([self.acc])


def encode(samples, first_cap=4096 - 40, cap=4096):
    """Весь кадр кусками, как ws_send_scope. -> список кусков."""
    e = Encoder()
    parts, pos, c = [], 0, first_cap
    while True:
        k, b = e.encode(samples[pos:], c)
        pos += k
        if pos >= len(samples):
            b += e.finish()
        parts.append(b)
        if pos >= len(samples):
            return parts
        c = cap


def decode(buf, n):
    """Как scodec_decode: -> список отсчётов (короче n, если поток короче)."""
    m = len(buf) * 2
    p = 0

    def nib():
        nonlocal p
        b = buf[p >> 1]
        c = (b & 15) if (p & 1) else (b >> 4)
        p += 1
        return c

    out = []
    if not n or m < 3:
        return out
    v = (nib() << 8) | (nib() << 4) | nib()
    out.append(v)
    while len(out) < n and p < m:
        c = nib()
        if c == 8:
            if p + 3 > m:
                break
            v = (nib() << 8) | (nib() << 4) | nib()
        else:
            v += c - 16 if c > 7 else c
        out.append(v)
    return out
