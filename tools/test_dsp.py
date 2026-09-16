"""Многоуровневая проверка тракта обработки MCA. Железо не нужно.

    python tools/test_dsp.py

Модель берёт константы прямо из main/mca_config.h, поэтому тесты
следуют за прошивкой при изменении размера чанка и числа каналов.
"""
import os
import random
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dspmodel import (DSP, CAP_CHUNK_SAMPLES, MCA_ADC_MAX, MCA_CHANNELS,
                      Params, make_pulse_train, run)

fails = []


def check(name, cond, detail=""):
    print(f"  [{'OK ' if cond else 'ПРОВАЛ'}] {name}" +
          (f"  -- {detail}" if detail else ""))
    if not cond:
        fails.append(name)


print(f"\nконфигурация из mca_config.h: чанк {CAP_CHUNK_SAMPLES} отсч, "
      f"каналов {MCA_CHANNELS}, шкала АЦП 0..{MCA_ADC_MAX}")

# ------------------------------------------------------------------ L1
print("\n=== УРОВЕНЬ 1: базовая работоспособность ===")
d = run(make_pulse_train(4096, [800], [1500]), Params())
check("одиночный импульс детектируется", len(d.events) == 1,
      f"каналы: {[e[0] for e in d.events]}")

lin = []
for a in (200, 400, 800, 1600):
    dd = run(make_pulse_train(4096, [a], [1500]), Params())
    if dd.events:
        lin.append((a, dd.events[0][0]))
print("  амплитуда -> канал: " +
      ", ".join(f"{a}->{c}" for a, c in lin))
ratios = [c / a for a, c in lin]
check("отклик линеен", len(ratios) == 4 and
      (max(ratios) - min(ratios)) / statistics.mean(ratios) < 0.05,
      f"разброс {100*(max(ratios)-min(ratios))/statistics.mean(ratios):.1f}%")

# ------------------------------------------------------------------ L2
print("\n=== УРОВЕНЬ 2: события на стыке чанков ===")
print("  Моноэнергетика без шума, считаем интегрированием (Способ 1):")
print("  оно берёт отсчёты вокруг запомненного положения пика, поэтому")
print("  ошибка индекса видна сразу. У самого интегрирования есть разброс")
print("  в пару процентов (плавает оценка базы), поэтому считаем только")
print("  грубые выбросы - дальше 10% от основного канала.\n")

# Интервалы СЛУЧАЙНЫЕ. При постоянном шаге фазы импульсов относительно
# границы чанка ложатся на решётку и могут целиком перепрыгнуть узкое
# окно, где проявляется ошибка: шаг 2000 при чанке 2046 попадал устаревшим
# индексом на следующий импульс, шаг 3001 давал решётку в 136 отсчётов
# при окне в 50. Зерно фиксировано - результат воспроизводим.
N, AMP = 200, 800
_rng = random.Random(3)
pos, _t = [], 3000
for _ in range(N):
    pos.append(_t)
    _t += _rng.randint(2500, 3500)
sig = make_pulse_train(pos[-1] + 4000, [AMP] * N, pos)

res = {}
for label, kw in (("без пересчёта peak_pos (старый баг)", {"rebase": False}),
                  ("как в прошивке сейчас", {"rebase": True})):
    dd = run(sig, Params(algo=1), **kw)
    ch = [c for c, _ in dd.events]
    mode = statistics.mode(ch)
    bad = [c for c in ch if abs(c - mode) > 0.1 * mode]
    res[label] = len(bad)
    print(f"  {label}: событий {len(ch)}, основной канал {mode}, "
          f"грубых выбросов {len(bad)} ({100*len(bad)/len(ch):.1f}%)")

check("баг воспроизводится на модели",
      res["без пересчёта peak_pos (старый баг)"] > 0)
check("текущая прошивка выбросов не даёт",
      res["как в прошивке сейчас"] == 0)

# ------------------------------------------------------------------ L3
print("\n=== УРОВЕНЬ 3: разрешение при реальном шуме (15 кодов RMS) ===")
sig = make_pulse_train(pos[-1] + 4000, [AMP] * N, pos, noise_rms=15.0, seed=7)
dd = run(sig, Params())
ch = [c for c, _ in dd.events]
med = statistics.median(ch)
# Ширину считаем по событиям пика: одно шумовое срабатывание далеко
# от пика раздувает СКО в разы и делает число бессмысленным.
core = [c for c in ch if abs(c - med) < 0.1 * med]
stray = len(ch) - len(core)
fwhm = 2.355 * statistics.pstdev(core)
print(f"  событий {len(ch)} (импульсов {N}): в пике {len(core)}, "
      f"вне пика {stray}")
print(f"  центр {med:.0f}, FWHM {fwhm:.1f} кан = {100*fwhm/med:.2f}%")
check("все импульсы найдены", len(core) == N, f"в пике {len(core)} из {N}")
# Порог 20 при шуме 15 кодов - это около 4 сигм выхода трапеции,
# на полумиллионе отсчётов единичные ложные срабатывания ожидаемы.
check("ложных срабатываний единицы", stray <= 3, f"вне пика {stray}")

# ------------------------------------------------------------------ L4
print("\n=== УРОВЕНЬ 4: «кодов на канал» ===")
print("  Канал = амплитуда / кодов на канал. Амплитуда делится целиком,")
print("  без округления до кода, поэтому доли кода на канал дают")
print("  настоящую подробность, а не «гребёнку» из пустых каналов.\n")
meds, off4 = {}, {}
for cpc in (2000, 1000, 500, 250):
    ch = [c for c, _ in run(sig, Params(cpc=cpc)).events]
    meds[cpc] = statistics.median(ch)
    off4[cpc] = sum(1 for c in ch if c % 4) / len(ch)
    print(f"  {cpc/1000:5.2f} кода на канал: пик в канале {meds[cpc]:6.0f}, "
          f"каналов не кратных 4: {100*off4[cpc]:3.0f}%")
check("канал обратно пропорционален «кодов на канал»",
      all(abs(meds[c] * c / (meds[1000] * 1000) - 1) < 0.01 for c in meds),
      ", ".join(f"{c/1000}: {meds[c]:.0f}" for c in meds))
check("при 0.25 кода на канал заняты все каналы, не только кратные 4",
      off4[250] > 0.5, f"не кратных 4: {100*off4[250]:.0f}%")

# ------------------------------------------------------------------ L5
print("\n=== УРОВЕНЬ 5: шкала и запас АЦП ===")
print("  База в СЕРЕДИНЕ шкалы АЦП (2048), импульсы положительные:")
print("  амплитуда не может превысить 4095-2048 = 2047 кодов.")
print("  Верх шкалы = каналов x кодов на канал.\n")

BASE = 2048
headroom = MCA_ADC_MAX - BASE


def one(amp_in, **pk):
    dd = run(make_pulse_train(6000, [amp_in], [2500], baseline=BASE),
             Params(**pk))
    return (dd.events[0][0] if dd.events else None), dd.overflow


print("  1 код на канал и 2048 каналов (по умолчанию) - шкала до 2048 кодов:")
for amp_in in (500, 1000, 2000, 4000, 8000):
    c, _ = one(amp_in)
    tag = "  <- уже в ограничении АЦП" if amp_in > headroom else ""
    print(f"    вход {amp_in:5d} кодов -> канал {c:5d} "
          f"({100*c/2048:3.0f}% из 2048){tag}")
c2000, _ = one(2000)
c4000, _ = one(4000)
c8000, _ = one(8000)
# Трапеция отдаёт чуть меньше высоты входа (спад успевает пройти за
# окно L: 2000 -> ~1886), поэтому запас «90% шкалы», а не впритык.
check("2048 каналов по 1 коду покрывают весь запас АЦП",
      0.9 * 2048 < c2000 < 2048, f"канал {c2000}")
check("после ограничения канал не растёт и остаётся в шкале",
      c4000 == c8000 and c8000 < 2048, f"{c4000} и {c8000}")
c05, _ = one(1800, cpc=500)
print(f"  0.5 кода на канал: вход 1800 кодов -> канал {c05} из 4096")
check("0.5 кода на канал разворачивает тот же запас на 4096 каналов",
      0.8 * 4096 < c05 < 4096, f"канал {c05}")

# ------------------------------------------------------------------ L6
print("\n=== УРОВЕНЬ 6: осциллограф - синхронизация по фронту ===")
from scopemodel import (Scope, LEN as SLEN, PRE as SPRE, SPAN as SSPAN,
                        FAST as SFAST, AMP_WIN as SAMP_WIN)

FS = 10e6
CHUNK_US = CAP_CHUNK_SAMPLES / FS * 1e6
LVL = 30


def scope_run(sig, mode, neg=False, **kw):
    # Период снимков и ожидание авто уменьшены в сто раз, чтобы тест
    # шёл быстро; логика движка от этого не меняется.
    s = Scope(level=LVL, snap_us=1000, auto_us=1500, neg=neg, **kw)
    now = 0.0
    step = CAP_CHUNK_SAMPLES
    for off in range(0, len(sig) - step + 1, step):
        s.feed(sig[off:off + step], mode, now, off)
        now += CHUNK_US
    return s


def run_scope(sig, mode, neg=False, **kw):
    return scope_run(sig, mode, neg, **kw).pub


def rise_at(buf, i):
    return buf[i] - buf[i - SSPAN]


# импульсы как у вас на экране: ~60 кодов, фронт 0.5 мкс, спад 4.3 мкс
rng6 = random.Random(11)
p6, t6 = [], 5000
while t6 < 1_500_000:
    p6.append(t6)
    t6 += rng6.randint(3000, 20000)
sig6 = make_pulse_train(t6 + 5000, [60] * len(p6), p6, fs_hz=FS,
                        rise_ns=500, tau_us=4.3, noise_rms=3.0, seed=5)

pub = run_scope(sig6, 2)
tpos = sorted(set(t for _, t, _, _ in pub))
print(f"  ждущий, импульсы есть: снимков {len(pub)}, "
      f"момент синхронизации в отсчёте {tpos} (предыстория {SPRE})")
check("снимки идут", len(pub) >= 10, f"{len(pub)}")
check("фронт всегда в одном месте окна", tpos == [SPRE], f"{tpos}")
check("окно непрерывно - совпадает с куском исходного сигнала",
      all(cap == sig6[a:a + SLEN] for cap, _, _, a in pub))
check("срабатывание ровно на пересечении уровня",
      all(rise_at(cap, t) >= LVL > rise_at(cap, t - 1)
          for cap, t, _, _ in pub))

pa = run_scope(sig6, 1)
print(f"  авто, импульсы есть: снимков {len(pa)}, из них "
      f"синхронизированных {sum(1 for _, t, _, _ in pa if t >= 0)}")
check("авто при импульсах синхронизируется",
      sum(1 for _, t, _, _ in pa if t == SPRE) >= 0.8 * len(pa))

quiet = make_pulse_train(600_000, [], [], fs_hz=FS, noise_rms=3.0, seed=6)
qa = run_scope(quiet, 1)
qn = run_scope(quiet, 2)
print(f"  без импульсов: авто дал {len(qa)} снимков, ждущий {len(qn)}")
check("авто без фронта пускает развёртку свободно",
      len(qa) > 0 and all(t == -1 for _, t, _, _ in qa))
check("свободные окна тоже непрерывны",
      all(cap == quiet[a:a + SLEN] for cap, _, _, a in qa))
check("ждущий без фронта ничего не публикует", len(qn) == 0)

# Окно под развёртку: на развёртке 512 страница просит n = 512 + запас
# трапеции (L+G+16) и pre = пятая часть экрана + тот же запас. Прибор
# собирает ровно столько, с фронтом на месте pre, а не всё окно, - и
# такое окно помещается во внутреннюю память (DIAG_SCOPE_FAST).
M6 = 20 + 44 + 16
W6, P6 = 512 + M6, round(512 * 0.2) + M6
ws = scope_run(sig6, 2, want=W6, pre=P6)
wlen = sorted(set(len(cap) for cap, _, _, _ in ws.pub))
wtg = sorted(set(t for _, t, _, _ in ws.pub))
print(f"  развёртка 512: снимков {len(ws.pub)}, длина окна {wlen} "
      f"(ждали {W6}), фронт на {wtg} (ждали {P6})")
check("окно собирается под развёртку",
      len(ws.pub) >= 10 and wlen == [W6] and W6 <= SFAST and wtg == [P6]
      and all(cap == sig6[a:a + len(cap)] for cap, _, _, a in ws.pub))
# Развёртка 8192 - самая длинная, что должна влезать во внутреннюю память.
M8 = 64 + 128 + 16
check("окно развёртки 8192 с наибольшим запасом трапеции влезает во "
      "внутреннюю память", 8192 + M8 <= SFAST,
      f"окно {8192 + M8}, внутренний буфер {SFAST}")
# Самая короткая развёртка: после фронта всё равно не меньше AMP_WIN -
# иначе высоту импульса не измерить.
wz = scope_run(sig6, 2, want=64 + M6, pre=13 + M6)
zlen = sorted(set(len(cap) for cap, _, _, _ in wz.pub))
check("короткое окно - предыстория + не меньше поиска высоты",
      len(wz.pub) >= 10 and zlen == [13 + M6 + SAMP_WIN], f"{zlen}")

# Синхронизация по амплитуде: импульсы двух высот вперемешку, фильтр
# пропускает только высокие.
rng6a = random.Random(12)
pa6, aa6, ta6 = [], [], 5000
while ta6 < 1_500_000:
    pa6.append(ta6)
    aa6.append(rng6a.choice((60, 200)))
    ta6 += rng6a.randint(3000, 20000)
siga = make_pulse_train(ta6 + 5000, aa6, pa6, fs_hz=FS, rise_ns=500,
                        tau_us=4.3, noise_rms=3.0, seed=13)
fa = scope_run(siga, 2, amin=140, amax=320)
fall = scope_run(siga, 2)
print(f"  фильтр 140..320: снимков {len(fa.pub)}, высоты "
      f"{min(fa.amps) if fa.amps else '-'}..{max(fa.amps) if fa.amps else '-'}, "
      f"отброшено {sum(fa.rejs)}; без фильтра высоты "
      f"{min(fall.amps)}..{max(fall.amps)}")
check("фильтр амплитуды пропускает только импульсы в диапазоне",
      len(fa.pub) >= 5 and all(140 <= a <= 320 for a in fa.amps)
      and sum(fa.rejs) > 0,
      f"снимков {len(fa.pub)}, отброшено {sum(fa.rejs)}")
check("без фильтра видны обе высоты",
      min(fall.amps) < 140 and max(fall.amps) >= 140)
check("окна с фильтром тоже непрерывны и с фронтом на месте",
      all(cap == siga[a:a + len(cap)] for cap, _, _, a in fa.pub)
      and sorted(set(t for _, t, _, _ in fa.pub)) == [SPRE])

# ------------------------------------------------------------------ L7
print("\n=== УРОВЕНЬ 7: импульсы вниз (полярность «−») ===")
print("  Тот же сигнал, перевёрнутый относительно шкалы АЦП: база наверху,")
print("  импульсы идут вниз. С полярностью «−» всё должно совпасть.\n")
sig7 = make_pulse_train(pos[-1] + 4000, [AMP] * N, pos, noise_rms=15.0, seed=7)
inv7 = [MCA_ADC_MAX - v for v in sig7]
chp = [c for c, _ in run(sig7, Params()).events]
chn = [c for c, _ in run(inv7, Params(polarity=1)).events]
ch0 = [c for c, _ in run(inv7, Params()).events]
print(f"  вверх, полярность +: событий {len(chp)}; вниз, полярность −: "
      f"{len(chn)}; вниз без переключения: {len(ch0)}")
check("полярность «−» даёт тот же спектр, что и прямой сигнал", chp == chn)
# Без переключения импульсы вниз НЕ пропадают: после каждого из них
# трапеция даёт положительный провал-«отражение», он пересекает порог.
# Событий даже больше, чем импульсов, но все они в низких каналах -
# на месте настоящего пика нет ни одного.
mp = sorted(chp)[len(chp) // 2]
near0 = sum(1 for c in ch0 if abs(c - mp) < 0.1 * mp)
check("без переключения полярности спектр ложный (нет событий у пика)",
      near0 == 0 and len(ch0) > 0,
      f"событий {len(ch0)}, у настоящего пика ({mp}) - {near0}")

inv6 = [MCA_ADC_MAX - v for v in sig6]
pn7 = run_scope(inv6, 2, neg=True)
check("осциллограф синхронизируется по фронту вниз",
      len(pn7) >= 10 and sorted(set(t for _, t, _, _ in pn7)) == [SPRE]
      and all(cap == inv6[a:a + SLEN] for cap, _, _, a in pn7),
      f"снимков {len(pn7)}")

# ------------------------------------------------------------------ L8
print("\n=== УРОВЕНЬ 8: ускорение без изменения результата ===")
print("  Прошивка считает разность трапеции для всего чанка векторными")
print("  командами (16 бит с насыщением), порог проверяет восьмёрками по")
print("  максимуму и переворачивает полярность словами по два отсчёта.")
print("  Прежний поштучный и новый варианты прогоняются на одних и тех же")
print("  сигналах: события, каналы, гистограмма и состояние фильтра обязаны")
print("  совпасть, а насыщение 16 бит - ни разу не наступить.\n")
from dspmodel import invert_words


def same(sig8, p, **new):
    a = run(sig8, p)
    b = run(sig8, p, **new)
    return a, b, (a.events == b.events and a.hist == b.hist
                  and a.trap == b.trap and a.overflow == b.overflow)


# Смесь случайных амплитуд и интервалов: срабатывания ложатся на все
# восемь позиций в восьмёрке и на остаток чанка, не кратный восьми.
rng8 = random.Random(8)
pos8, t8 = [], 1000
while t8 < 400_000:
    pos8.append(t8)
    t8 += rng8.randint(150, 3000)
amp8 = [rng8.randint(40, 1800) for _ in pos8]
mix = make_pulse_train(t8 + 4000, amp8, pos8, noise_rms=6.0, seed=9)

# Крайние отсчёты 0 и 4095 вперемешку: разность достигает ±8190 -
# проверка, что 16 бит хватает и насыщение не наступает.
rng7 = random.Random(7)
extreme = [MCA_ADC_MAX if rng7.random() < 0.5 else 0 for _ in range(60_000)]

lanes, sats = [0] * 9, 0
# L и G с разными остатками от деления на 8 - это разные сдвиги
# выравнивания у трёх невыровненных потоков векторной разности.
for name, s8, p8 in (("основной L=20 G=44", mix, Params()),
                     ("L=7 G=13", mix, Params(trap_L=7, trap_G=13)),
                     ("L=3 G=9", mix, Params(trap_L=3, trap_G=9)),
                     ("L=8 G=16", mix, Params(trap_L=8, trap_G=16)),
                     ("L=64 G=128", mix, Params(trap_L=64, trap_G=128,
                                                search=200, rearm=192)),
                     ("интегрирование", mix, Params(algo=1)),
                     ("низкий порог, шум", sig, Params(threshold=4)),
                     ("мелкие импульсы", sig6[:400_000], Params(threshold=8)),
                     ("крайние 0/4095", extreme, Params(trap_L=5, trap_G=11))):
    a, b, ok = same(s8, p8, vec=True)
    lanes = [x + y for x, y in zip(lanes, b.lanes)]
    sats += b.sat
    check(f"векторно = поштучно: {name}", ok and b.sat == 0,
          f"событий {len(a.events)} и {len(b.events)}, насыщений {b.sat}")
print(f"  где сработал порог: позиции 0-7 в восьмёрке {lanes[:8]}, "
      f"в остатке {lanes[8]}; насыщений 16 бит всего {sats}")
check("срабатывания были на всех позициях восьмёрки и в остатке",
      all(lanes), f"{lanes}")

# Переворот словами - перебором: каждая 16-битная половина при разных
# соседях, включая мусор в старших битах (маска обязана его убрать).
bad8 = 0
others = (0, 1, 0x0FFF, 0x1000, 0x7FFF, 0x8000, 0xF000, 0xFFFF, 0xABCD)
for v in range(65536):
    ref = MCA_ADC_MAX - (v & 0xFFF)
    for o in others:
        ro = MCA_ADC_MAX - (o & 0xFFF)
        if invert_words([v, o]) != [ref, ro]:
            bad8 += 1
        if invert_words([o, v]) != [ro, ref]:
            bad8 += 1
check("переворот словами = поштучно для всех 65536 значений половины",
      bad8 == 0, f"расхождений {bad8}")
check("нечётная длина: последний отсчёт переворачивается по одному",
      invert_words([5, 6, 0x1007]) ==
      [MCA_ADC_MAX - 5, MCA_ADC_MAX - 6, MCA_ADC_MAX - 7])

rng9 = random.Random(10)
dirty = [(MCA_ADC_MAX - v) | (rng9.randint(0, 15) << 12) for v in mix]
a, b, ok = same(dirty, Params(polarity=1), word_invert=True)
check("полярность «−»: словами = поштучно на всём тракте, с мусором "
      "в старших битах", ok, f"событий {len(a.events)}")
a, b, ok = same(dirty, Params(polarity=1, algo=1), word_invert=True,
                vec=True)
check("оба ускорения вместе, интегрирование", ok,
      f"событий {len(a.events)}")

# ------------------------------------------------------------------ L9
print("\n=== УРОВЕНЬ 9: сжатие осциллограммы для WebSocket ===")
print("  Прошивка (scope_codec.c) сжимает кадр кусками по 4 КБ, страница")
print("  (wdec) расшифровывает. Модель прошивки - scopecodec.py; wdec берётся")
print("  прямо из mca_web.c и гоняется в node. Всё обязано совпасть бит в бит.\n")
import json
import re
import subprocess
import tempfile
import scopecodec
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_i18n as ci

FS9 = 20e6


def frame9(n, cps, noise, seed):
    r = random.Random(seed)
    ps, t = [], 100
    while t < n - 10:
        ps.append(t)
        t += max(1, int(r.expovariate(cps / FS9)))
    am = [r.choice((80, 300, 1000, 1500)) for _ in ps]
    return make_pulse_train(n, am, ps, fs_hz=FS9, rise_ns=1300, tau_us=4.0,
                            noise_rms=noise, seed=seed)


rng9c = random.Random(99)
cases9 = {
    "развёртка 512, 7000 имп/с": frame9(592, 7000, 1.5, 1),
    "развёртка 16384, 7000 имп/с": frame9(16592, 7000, 1.5, 2),
    "развёртка 32768, 20000 имп/с": frame9(32976, 20000, 2.0, 3),
    "крайние 0/4095 вперемешку": [rng9c.choice((0, 4095)) for _ in range(3000)],
    "разности ровно ±7 и ±8": [2048 + (7 if i % 4 == 1 else -8 if i % 4 == 3
                                       else 0) for i in range(1001)],
    "один отсчёт": [1234],
    "пусто": [],
}
enc9 = {}
ok_rt = True
for name, s in cases9.items():
    parts = scopecodec.encode(s)
    blob = b''.join(parts)
    enc9[name] = blob
    back = scopecodec.decode(blob, len(s))
    ratio = (2 * len(s) / len(blob)) if blob else 0
    print(f"  {name}: {len(s)} отсчётов, {2 * len(s)} -> {len(blob)} байт "
          f"(x{ratio:.1f}), кусков {len(parts)}")
    ok_rt &= back == [v & 0xFFF for v in s]
check("модель: сжатие и расшифровка совпадают бит в бит", ok_rt)

# куски разной длины - состояние переживает границы, результат тот же
s9 = cases9["развёртка 16384, 7000 имп/с"]
same9 = all(b''.join(scopecodec.encode(s9, fc, c)) == enc9["развёртка 16384, 7000 имп/с"]
            for fc, c in ((2, 2), (5, 7), (100, 3000)))
check("сжатие не зависит от нарезки на куски", same9)
big = cases9["развёртка 32768, 20000 имп/с"]
check("длинная развёртка сжимается больше чем в 3 раза",
      2 * len(big) / len(enc9["развёртка 32768, 20000 имп/с"]) > 3.0)

# wdec из самой страницы
src9 = ci.strip_c_comments(open(ci.SRC, encoding='utf-8').read())
page9 = None
for m9 in re.finditer(r'static const char (\w+)\[\]\s*=(.*?);\n', src9, re.S):
    if m9.group(1) == 'PAGE':
        page9 = ''.join(ci.expand(ci.c_tokens(m9.group(2)), ci.macros(src9)))
i9 = page9.find('function wdec(') if page9 else -1
if i9 < 0:
    check("wdec найдена на странице", False)
else:
    depth, j9 = 0, i9
    while True:
        ch = page9[j9]
        depth += ch == '{'
        depth -= ch == '}'
        j9 += 1
        if ch == '}' and depth == 0:
            break
    js = page9[i9:j9] + """
var fs=require('fs'),inp=JSON.parse(fs.readFileSync(process.argv[2],'utf8')),res={};
for(var k in inp){var b=Buffer.from(inp[k][0],'hex');
res[k]=wdec(new Uint8Array(b.buffer,b.byteOffset,b.length),inp[k][1])}
console.log(JSON.stringify(res));"""
    with tempfile.TemporaryDirectory() as td:
        jf = os.path.join(td, 'wdec.js')
        df = os.path.join(td, 'in.json')
        open(jf, 'w', encoding='utf-8').write(js)
        # ключи - номерами: русские портятся в выводе node под Windows
        keys9 = list(cases9)
        json.dump({f"c{i}": [enc9[k].hex(), len(cases9[k])]
                   for i, k in enumerate(keys9)}, open(df, 'w'))
        try:
            r9 = subprocess.run(['node', jf, df], capture_output=True, text=True)
            out9 = json.loads(r9.stdout) if r9.returncode == 0 else None
        except FileNotFoundError:
            out9 = None
    if out9 is None:
        check("node запустил wdec", False, "нужен node")
    else:
        bad = [k for i, k in enumerate(keys9)
               if out9.get(f"c{i}") != [x & 0xFFF for x in cases9[k]]]
        check("wdec страницы расшифровывает кадры прошивки бит в бит",
              not bad, f"расхождения: {bad}")

print("\n" + "=" * 62)
if fails:
    print(f"ПРОВАЛЕНО: {len(fails)}")
    for f in fails:
        print("   -", f)
    sys.exit(1)
print("Все проверки пройдены")
