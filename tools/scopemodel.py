"""Модель осциллограммы с синхронизацией из main/mca_diag.c.

Повторяет scope_feed() построчно. Константы читаются из mca_diag.h и
mca_diag.c, чтобы модель не разъехалась с прошивкой.
"""
import os
import re

_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'main')


def _defines(name):
    txt = open(os.path.join(_DIR, name), encoding='utf-8').read()
    return {k: int(v)
            for k, v in re.findall(r'#define\s+(\w+)\s+(-?\d+)', txt)}


_H = _defines('mca_diag.h')
_C = _defines('mca_diag.c')
LEN = _H['DIAG_SCOPE_LEN']
PRE = _H['DIAG_SCOPE_PRE']
SPAN = _H['DIAG_TRIG_SPAN']
AUTO_WAIT_US = _C['AUTO_WAIT_US']
SNAP_PERIOD_US = _C['SNAP_PERIOD_US']
MASK = 0x0FFF
BIG = 2 ** 31 - 1

AUTO, NORMAL = 1, 2


class Scope:
    """pub - опубликованные снимки: (окно, индекс синхронизации или -1,
    перепад, начало окна в исходном сигнале - для проверки непрерывности).
    snap_us/auto_us можно уменьшить, чтобы тест шёл быстрее: логика та же."""

    def __init__(self, level=30, snap_us=SNAP_PERIOD_US, auto_us=AUTO_WAIT_US,
                 neg=False):
        self.level = level
        self.sg = -1 if neg else 1
        self.snap_us = snap_us
        self.auto_us = auto_us
        self.sc = 'IDLE'
        self.pre = []
        self.build = []
        self.snap_next = 0
        self.arm = 0
        self.cap_trig = -1
        self.cap_rise = 0
        self.abs_start = 0
        self.pub = []

    def _pre_push(self, d):
        self.pre = (self.pre + list(d))[-PRE:]

    def _take(self, d):
        self.build += list(d[:LEN - len(self.build)])
        return len(self.build) >= LEN

    def _cap_start(self, d, j, off):
        fc = min(j, PRE)
        fp = min(PRE - fc, len(self.pre))
        self.build = self.pre[len(self.pre) - fp:] + list(d[j - fc:j])
        self.cap_trig = len(self.build)
        self.abs_start = off + j - fc - fp
        return self._take(d[j:])

    def _publish(self, now):
        self.pub.append((self.build, self.cap_trig, self.cap_rise,
                         self.abs_start))
        self.build = []
        self.sc = 'IDLE'
        self.snap_next = now + self.snap_us

    def flush(self):
        self.sc = 'IDLE'
        self.pre = []

    def feed(self, d, mode, now, off):
        if self.sc == 'IDLE':
            if now < self.snap_next:
                return
            self.sc = 'ARMED'
            self.arm = now
            self.pre = []           # старая предыстория оторвана по времени
            self._pre_push(d)
            return
        if self.sc == 'POST':
            if self._take(d):
                self._publish(now)
            return

        if len(self.pre) < PRE:     # предыстория длиннее чанка - добираем
            self._pre_push(d)
            return

        K, lvl, pre, sg = SPAN, self.level, self.pre, self.sg
        rprev = (sg * ((pre[-1] & MASK) - (pre[-1 - K] & MASK))
                 if len(pre) > K else BIG)
        hit, r = None, 0
        for j in range(len(d)):
            if j >= K:
                ref = d[j - K] & MASK
            elif len(pre) >= K - j:
                ref = pre[len(pre) - (K - j)] & MASK
            else:
                rprev = BIG
                continue
            r = sg * ((d[j] & MASK) - ref)
            if r >= lvl and rprev < lvl:
                hit = j
                break
            rprev = r

        if hit is not None:
            self.cap_rise = r
            if self._cap_start(d, hit, off):
                self._publish(now)
            else:
                self.sc = 'POST'
            return

        if mode == AUTO and now - self.arm > self.auto_us:
            self.cap_rise = 0
            full = self._cap_start(d, 0, off)
            self.cap_trig = -1
            if full:
                self._publish(now)
            else:
                self.sc = 'POST'
            return

        self._pre_push(d)
