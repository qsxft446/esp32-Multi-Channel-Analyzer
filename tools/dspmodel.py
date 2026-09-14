"""Модель обработки из main/mca_dsp.c для проверок без железа.

Повторяет раскладку s_work, сдвиг хвоста через memmove, перенос
состояния автомата между чанками, amp_integrate и
пересчёт амплитуды в номер канала.

Константы НЕ дублируются руками, а читаются из main/mca_config.h -
иначе модель незаметно расходится с прошивкой.

Запуск проверок:  python tools/test_dsp.py
"""
import math
import os
import random
import re

_HDR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    '..', 'main', 'mca_config.h')


def _defines(path):
    txt = open(path, encoding='utf-8').read()
    out = {}
    for name, val in re.findall(r'#define\s+(\w+)\s+(-?\d+)', txt):
        out[name] = int(val)
    return out


_D = _defines(_HDR)
TRAP_L_MAX = _D['TRAP_L_MAX']
TRAP_G_MAX = _D['TRAP_G_MAX']
CAP_CHUNK_SAMPLES = _D['CAP_CHUNK_SAMPLES']
MCA_CHANNELS = _D['MCA_CHANNELS']
MCA_ADC_MAX = _D['MCA_ADC_MAX']

DSP_TAIL = TRAP_L_MAX + TRAP_G_MAX + 8
WORK = DSP_TAIL + CAP_CHUNK_SAMPLES

ST_IDLE, ST_PEAK, ST_REARM = 0, 1, 2


class Params:
    """Значения по умолчанию из mca_dsp_init(). При правке дефолтов
    в прошивке поправьте и здесь."""

    def __init__(self, **kw):
        self.threshold = 20
        self.hysteresis = 50
        self.trap_L = 20
        self.trap_G = 44
        self.rearm = 64
        self.search = 70
        self.algo = 0
        self.polarity = 0
        self.int_rise = 20
        self.int_fall = 32
        self.cpc = 1000          # кодов на канал x1000
        self.nch = 2048
        self.baseline_shift = 10
        self.baseline_win = 150
        self.flat_avg = 0
        self.__dict__.update(kw)


class DSP:
    """rebase=True   - пересчёт s_peak_pos после сдвига буфера
                       (исправление, которое стоит в прошивке).
       guard_end=True - запрет чтения за концом свежих данных."""

    def __init__(self, p, rebase=True, guard_end=False, vec=False,
                 word_invert=False):
        """vec         - как в прошивке: разность трапеции d для всего
                         чанка заранее, в 16-битной арифметике с насыщением
                         (векторные команды), цикл ожидания восьмёрками с
                         проверкой порога по максимуму;
           word_invert - переворот полярности словами по два отсчёта.
        Без флагов - прежний поштучный вариант: тест сравнивает оба."""
        self.p = p
        self.rebase = rebase
        self.guard_end = guard_end
        self.vec = vec
        self.word_invert = word_invert
        self.lanes = [0] * 9     # где сработал порог: 0..7 в восьмёрке, 8 - остаток
        self.sat = 0             # сколько раз сработало бы насыщение 16 бит
        self.w = [0] * WORK
        self.trap = 0
        self.state = ST_IDLE
        self.peak = 0
        self.peak_pos = 0
        self.since = 0
        self.pre_trap = 0
        self.need_prime = True
        self.prime_left = 0
        self.base_fp = 0
        self.base_init = False
        self.base_lost = 0
        self.hist = [0] * MCA_CHANNELS
        self.overflow = 0
        self.events = []
        self.valid_end = WORK

    # ---- расчёт амплитуды (порт функций из mca_dsp.c) ----
    def amp_integrate(self, peak_pos, rise, fall, L, G, base):
        total = self.valid_end if self.guard_end else WORK
        lo = max(peak_pos - L - G, 1)
        hi = min(peak_pos + 4, total - 1)
        w = self.w
        mx, pk = -32768, lo
        for i in range(lo, hi + 1):
            if w[i] > mx:
                mx, pk = w[i], i
        a = max(pk - rise, 0)
        b = min(pk + fall, total - 1)
        ssum = sum(w[i] - base for i in range(a, b + 1))
        n = b - a + 1
        return (ssum, n) if n > 0 else (0, 1)

    def record_event(self, num, den):
        """Канал = (num/den) / кодов на канал, дробь не округляется -
        как в прошивке (деление целиком в 64 битах)."""
        d = den * self.p.cpc
        ch = (num * 1000) // d if (d > 0 and num > 0) else 0
        if ch >= MCA_CHANNELS:
            self.overflow += 1
            return
        self.hist[ch] += 1
        self.events.append((ch, num / den))

    # ---- порт mca_dsp_process ----
    def process(self, data):
        p = self.p
        n = len(data)
        L, G = p.trap_L, p.trap_G
        thr = p.threshold * L
        hp = min(max(p.hysteresis, 1), 99)
        thr_lo = thr * hp // 100

        w = self.w
        if p.polarity and self.word_invert:
            w[DSP_TAIL:DSP_TAIL + n] = invert_words(data)
        elif p.polarity:
            w[DSP_TAIL:DSP_TAIL + n] = [MCA_ADC_MAX - (v & 0xFFF) for v in data]
        else:
            w[DSP_TAIL:DSP_TAIL + n] = data
        self.valid_end = DSP_TAIL + n

        if self.need_prime:
            v0 = w[DSP_TAIL]
            for k in range(DSP_TAIL):
                w[k] = v0
            self.trap = 0
            self.state = ST_IDLE
            self.prime_left = L + G + p.search + 4
            self.need_prime = False

        trap = self.trap
        base_fp = self.base_fp
        end = DSP_TAIL + n
        i = DSP_TAIL

        if self.vec:
            # Как diff_vec в прошивке: три операции с насыщением 16 бит
            # в том же порядке (w - w[-L], затем - w[-G], затем + w[-G-L]).
            def sat(v):
                if v > 32767 or v < -32768:
                    self.sat += 1
                    return max(-32768, min(32767, v))
                return v
            dd = [sat(sat(sat(w[a] - w[a - L]) - w[a - G]) + w[a - G - L])
                  for a in range(DSP_TAIL, end)]

            def D(a):
                return dd[a - DSP_TAIL]
        else:
            def D(a):
                return w[a] - w[a - L] - w[a - G] + w[a - G - L]

        while self.prime_left > 0 and i < end:
            trap += D(i)
            self.prime_left -= 1
            i += 1

        for k in range(DSP_TAIL + 8, end, 64):
            v = w[k]
            dev = v - (base_fp >> 8)
            if not self.base_init:
                base_fp = v << 8
                self.base_init = True
            elif -p.baseline_win <= dev <= p.baseline_win:
                base_fp += ((v << 8) - base_fp) >> p.baseline_shift
                self.base_lost = 0
            else:
                self.base_lost += 1
                if self.base_lost > 64:
                    base_fp = v << 8
                    self.base_lost = 0

        while i < end:
            if self.state == ST_IDLE:
                if self.vec:
                    # Как в прошивке: восьмёрками, порог проверяется один
                    # раз по максимуму восьми значений; если он выше
                    # порога - восьмёрка разбирается поштучно.
                    cnt = end - i
                    q = cnt >> 3
                    k = 0
                    while q > 0:
                        t, ts = trap, []
                        for o in range(8):
                            t += D(i + k + o)
                            ts.append(t)
                        if max(ts) > thr:
                            break
                        trap = t
                        k += 8
                        q -= 1
                    if q > 0:
                        for o in range(8):
                            trap += D(i + k)
                            if trap > thr:
                                self.lanes[o] += 1
                                break
                            k += 1
                    else:
                        while k < cnt:
                            trap += D(i + k)
                            if trap > thr:
                                self.lanes[8] += 1
                                break
                            k += 1
                    i += k
                else:
                    while i < end:
                        trap += D(i)
                        if trap > thr:
                            break
                        i += 1
                if i >= end:
                    break
                self.pre_trap = trap
                self.peak = trap
                self.peak_pos = i
                self.since = 0
                self.state = ST_PEAK
                i += 1

            if self.state == ST_PEAK:
                fired = False
                while i < end:
                    trap += D(i)
                    if trap > self.peak:
                        self.peak, self.peak_pos = trap, i
                    self.since += 1
                    if self.since >= p.search:
                        if p.algo == 1:
                            s, cnt = self.amp_integrate(
                                self.peak_pos, p.int_rise, p.int_fall,
                                L, G, base_fp >> 8)
                            self.record_event(s, cnt)
                        else:
                            self.record_event(self.peak, L)
                        self.since = 0
                        self.state = ST_REARM
                        i += 1
                        fired = True
                        break
                    i += 1
                if not fired and i >= end:
                    break

            if self.state == ST_REARM:
                while i < end:
                    trap += D(i)
                    self.since += 1
                    if self.since >= p.rearm and trap < thr_lo:
                        self.state = ST_IDLE
                        i += 1
                        break
                    i += 1

        shift = end - DSP_TAIL
        w[0:DSP_TAIL] = w[shift:shift + DSP_TAIL]
        if self.rebase and self.state == ST_PEAK:
            self.peak_pos -= shift
            if self.peak_pos < L + G + 1:
                self.state = ST_IDLE
                self.since = 0
        self.trap = trap
        self.base_fp = base_fp


def invert_words(data):
    """Переворот полярности как в прошивке: по два отсчёта в 32-битном
    слове, 0x0FFF0FFF - (слово & 0x0FFF0FFF). Нечётный последний - по
    одному."""
    out = []
    n2 = len(data) // 2
    for k in range(n2):
        word = (data[2 * k] & 0xFFFF) | ((data[2 * k + 1] & 0xFFFF) << 16)
        r = (0x0FFF0FFF - (word & 0x0FFF0FFF)) & 0xFFFFFFFF
        out += [r & 0xFFFF, r >> 16]
    if len(data) & 1:
        out.append(MCA_ADC_MAX - (data[-1] & 0xFFF))
    return out


def run(sig, p, **kw):
    """Прогнать сигнал чанками, как это делает задача dsp_task."""
    d = DSP(p, **kw)
    step = CAP_CHUNK_SAMPLES
    for off in range(0, len(sig) - step + 1, step):
        d.process(sig[off:off + step])
    return d


def make_pulse_train(n_samples, amps, positions, fs_hz=8e6,
                     rise_ns=350, tau_us=18.0, baseline=2048,
                     noise_rms=0.0, seed=1, clip=True):
    """Реальная форма измеренного сигнала: быстрый фронт плюс
    экспоненциальный спад tau=18 мкс, положительная полярность,
    база в середине шкалы АЦП."""
    rnd = random.Random(seed)
    rise = max(1, int(rise_ns * 1e-9 * fs_hz))
    tau = tau_us * 1e-6 * fs_hz
    sig = [float(baseline)] * n_samples
    for amp, pos in zip(amps, positions):
        for k in range(int(tau * 6)):
            idx = pos + k
            if idx >= n_samples:
                break
            if k < rise:
                sig[idx] += amp * (k + 1) / rise
            else:
                sig[idx] += amp * math.exp(-(k - rise) / tau)
    out = []
    for v in sig:
        if noise_rms > 0:
            v += rnd.gauss(0, noise_rms)
        iv = int(round(v))
        if clip:                      # за шкалу АЦП сигнал выйти не может
            iv = max(0, min(MCA_ADC_MAX, iv))
        out.append(iv)
    return out
