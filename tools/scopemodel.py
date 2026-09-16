"""Модель осциллограммы с синхронизацией из main/mca_diag.c.

Повторяет scope_step() / scope_feed() построчно: кольцо из DIAG_PRV_N
предыдущих чанков, фронт ищется только при полной предыстории, поиск шагом
TRIG_STEP с точным уточнением, окно под развёртку (want), высота импульса
и фильтр амплитуды. Константы читаются из mca_diag.h и mca_diag.c, чтобы
модель не разъехалась с прошивкой. Защита от перезаписи буферов DMA
(prv_safe) в модели не нужна: очереди захвата нет.
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
FAST = _C['DIAG_SCOPE_FAST']
SPAN = _H['DIAG_TRIG_SPAN']
AUTO_WAIT_US = _C['AUTO_WAIT_US']
SNAP_PERIOD_US = _C['SNAP_PERIOD_US']
PRV_N = _C['DIAG_PRV_N']
TRIG_STEP = _C['TRIG_STEP']
AMP_WIN = _C['AMP_WIN']
AMP_BASE = _C['AMP_BASE']
MASK = 0x0FFF
BIG = 2 ** 31 - 1

AUTO, NORMAL = 1, 2


class Scope:
    """pub - опубликованные снимки: (окно, индекс синхронизации или -1,
    перепад, начало окна в исходном сигнале - для проверки непрерывности).
    amps / rejs - высота импульса и число отброшенных фильтром для каждого
    снимка. snap_us/auto_us можно уменьшить, чтобы тест шёл быстрее: логика
    та же. want - сколько отсчётов просит страница, pre - сколько из них до
    фронта (как mca_diag_set_scope_want); amin/amax - фильтр амплитуды
    (amax <= 0 - выключен)."""

    def __init__(self, level=30, snap_us=SNAP_PERIOD_US, auto_us=AUTO_WAIT_US,
                 neg=False, want=LEN, pre=PRE, amin=0, amax=0):
        self.level = level
        self.sg = -1 if neg else 1
        self.snap_us = snap_us
        self.auto_us = auto_us
        self.want = min(want, LEN)
        self.pre = min(pre, self.want, PRE)
        if amin < 0:
            amin = 0
        if amax > 0 and amin > amax:
            amin, amax = amax, amin
        self.amin, self.amax = amin, amax
        self.sc = 'IDLE'
        self.prv = [None] * PRV_N           # (начало в сигнале, отсчёты)
        self.prv_i = 0
        self.build = []
        self.cap_len = LEN
        self.snap_next = 0
        self.arm = 0
        self.cap_trig = -1
        self.cap_rise = 0
        self.abs_start = 0
        self.amp_done = False
        self.cap_amp = -1
        self.rej = 0
        self.pub, self.amps, self.rejs = [], [], []

    # ---- кольцо предыдущих чанков ----
    def _PRV(self, k):
        return (self.prv_i + PRV_N - 1 - k) % PRV_N

    def _push(self, d, off):
        self.prv[self.prv_i] = (off, list(d))
        self.prv_i = (self.prv_i + 1) % PRV_N

    def _avail(self):
        s = 0
        for k in range(PRV_N):
            c = self.prv[self._PRV(k)]
            if c is None:
                break
            s += len(c[1])
        return s

    def flush(self):
        self.sc = 'IDLE'
        self.prv = [None] * PRV_N

    # ---- сборка окна ----
    def _take(self, d):
        take = min(self.cap_len - len(self.build), len(d))
        self.build += list(d[:take])
        return len(self.build) >= self.cap_len

    def _cap_start(self, d, j, off):
        pre = self.pre
        after = max(self.want - pre, AMP_WIN)
        self.cap_len = min(pre + after, LEN)
        need = pre
        cur = min(j, need)
        need -= cur
        takes = []
        for k in range(PRV_N):
            if not need:
                break
            c = self.prv[self._PRV(k)]
            if c is None:
                break
            t = min(len(c[1]), need)
            takes.append((k, t))
            need -= t
        build = []
        for k, t in reversed(takes):
            c = self.prv[self._PRV(k)][1]
            build += c[len(c) - t:]
        build += list(d[j - cur:j])
        self.build = build
        self.cap_trig = len(build)
        self.abs_start = off + j - len(build)
        self.amp_done = False
        self.cap_amp = -1
        return self._take(d[j:])

    def _amp_measure(self):
        t = self.cap_trig
        end = t + AMP_WIN
        fill = len(self.build)
        if fill < end and fill < self.cap_len:
            return -1
        e = min(end, fill)
        b1 = t - 8 if t > 8 else 0
        b0 = b1 - AMP_BASE if b1 > AMP_BASE else 0
        if b1 > b0:
            base = sum(x & MASK for x in self.build[b0:b1]) // (b1 - b0)
        else:
            base = self.build[t] & MASK
        pk = 0
        for x in self.build[t:e]:
            pk = max(pk, self.sg * ((x & MASK) - base))
        return pk

    def _gate(self):
        if self.amp_done or self.cap_trig < 0:
            return True
        a = self._amp_measure()
        if a < 0:
            return True
        self.amp_done = True
        self.cap_amp = a
        if self.amax > 0 and (a < self.amin or a > self.amax):
            self.rej += 1
            self.sc = 'ARMED'
            return False
        return True

    def _publish(self, now):
        self.pub.append((self.build, self.cap_trig, self.cap_rise,
                         self.abs_start))
        self.amps.append(self.cap_amp)
        self.rejs.append(self.rej)
        self.rej = 0
        self.build = []
        self.sc = 'IDLE'
        self.snap_next = now + self.snap_us

    # ---- один чанк ----
    def _diff(self, d, j, pv):
        K = SPAN
        if j >= K:
            ref = d[j - K] & MASK
        elif len(pv) >= K - j:
            ref = pv[len(pv) - (K - j)] & MASK
        else:
            return None
        return self.sg * ((d[j] & MASK) - ref)

    def _step(self, d, mode, now, off):
        if self.sc == 'IDLE':
            if now < self.snap_next:
                return
            self.sc = 'ARMED'
            self.arm = now
            return
        if self.sc == 'POST':
            full = self._take(d)
            if not self._gate():
                return
            if full:
                self._publish(now)
            return

        if self._avail() < self.pre:
            return

        K, lvl, sg = SPAN, self.level, self.sg
        pv = self.prv[self._PRV(0)][1]
        n = len(d)
        hit, r = None, 0
        c = 0
        while c < n and hit is None:
            rc = self._diff(d, c, pv)
            if rc is None or rc < lvl:
                c += TRIG_STEP
                continue
            lo = c - TRIG_STEP + 1 if c >= TRIG_STEP else 0
            if lo > 0:
                rp = self._diff(d, lo - 1, pv)
                if rp is None:
                    rp = BIG
            elif len(pv) > K:
                rp = sg * ((pv[-1] & MASK) - (pv[-1 - K] & MASK))
            else:
                rp = BIG
            for x in range(lo, c + 1):
                rx = self._diff(d, x, pv)
                if rx is None:
                    rp = BIG
                    continue
                if rx >= lvl and rp < lvl:
                    hit, r = x, rx
                    break
                rp = rx
            c += TRIG_STEP

        if hit is not None:
            self.cap_rise = r
            full = self._cap_start(d, hit, off)
            if not self._gate():
                return
            if full:
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

    def feed(self, d, mode, now, off):
        self._step(d, mode, now, off)
        self._push(d, off)
