"""Посчитать такты горячих циклов собранной прошивки.

Это единственный честный способ понять, во что обошлась правка DSP:
исходник ничего не говорит о том, свернул ли компилятор цикл в
аппаратный zero-overhead loop или вставил обычный переход.

    idf.py build
    python tools/hotloop.py

Разбираются две функции:
  diff_vec        - векторная разность трапеции для всего чанка (идёт
                    по КАЖДОМУ отсчёту);
  mca_dsp_process - цикл ожидания порога по готовой разности.
Их такты на отсчёт складываются - это цена отсчёта, пока ждём событие,
именно она задаёт потолок частоты семплирования.

Отсчётов за итерацию: у векторного цикла - по сохранениям ee.vst.128
(8 отсчётов каждое); у скалярного - по командам max (цикл восьмёрками
сравнивает с порогом максимум восьми значений: 7 max = 8 отсчётов);
иначе 1.
Такты: инструкции, +2 за взятый переход назад (если цикл не
аппаратный) и +2 за каждый условный переход вперёд внутри тела (так
компилятор обходит дальний выход, и в обычном случае такой переход
выполняется). Векторные загрузки считаем по такту - настоящую цену
покажет «загрузка» в строке статуса на приборе.
"""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

elfs = glob.glob(os.path.join(ROOT, 'build', '*.elf'))
if not elfs:
    sys.exit('в build/ нет .elf - сначала соберите: idf.py build')
elf = elfs[0]

pat = os.path.join('C:\\', 'Espressif', 'tools', 'xtensa-esp-elf', '*',
                   'xtensa-esp-elf', 'bin', 'xtensa-esp32s3-elf-objdump.exe')
cand = sorted(glob.glob(pat))
objdump = cand[-1] if cand else 'xtensa-esp32s3-elf-objdump'

# -S подмешивает строки исходника: по ним отличаем цикл ожидания порога
# от прочих циклов в той же функции.
dis = subprocess.run([objdump, '-dS', '--no-show-raw-insn', elf],
                     capture_output=True, text=True, errors='replace').stdout


def parse(func):
    body, started, cur_src = [], False, ''
    for ln in dis.splitlines():
        if re.match(r'^[0-9a-f]{8} <%s>:' % re.escape(func), ln):
            started = True
            continue
        if started:
            if re.match(r'^[0-9a-f]{8} <', ln):
                break
            m = re.match(r'^\s*([0-9a-f]{8}):\t(\S+)\s*(.*)$', ln)
            if m:
                body.append((int(m.group(1), 16), m.group(2),
                             m.group(3).strip(), cur_src))
            elif ln.strip():
                cur_src = ln.strip()
    return body


def loops_of(body, mark):
    """(инстр, начало, конец, аппаратный?, есть ли mark в исходнике)"""
    idx = {a: i for i, (a, _, _, _) in enumerate(body)}

    def has_mark(i0, i1):
        return bool(mark) and any(mark in s for _, _, _, s in body[i0:i1])

    out = []
    for i, (a, op, args, _) in enumerate(body):
        if op.startswith('loop'):
            t = re.findall(r'\b([0-9a-f]{8})\b', args)
            if t and int(t[0], 16) in idx:
                e = idx[int(t[0], 16)]
                out.append((e - i - 1, i + 1, e, True, has_mark(i + 1, e)))
        elif re.match(r'^(b|j$)', op):
            for t in re.findall(r'\b([0-9a-f]{8})\b', args):
                ta = int(t, 16)
                if ta < a and ta in idx and i - idx[ta] < 80:
                    out.append((i - idx[ta] + 1, idx[ta], i + 1, False,
                                has_mark(idx[ta], i + 1)))
    return out


def per_iter(body, i0, i1):
    ops = [op for _, op, _, _ in body[i0:i1]]
    v = sum(1 for o in ops if o.startswith('ee.vst.128'))
    if v:
        return 8 * v
    m = sum(1 for o in ops if o == 'max')
    return m + 1 if m else 1


def fwd_taken(body, i0, i1):
    hi = body[i1 - 1][0]
    cnt = 0
    for a, op, args, _ in body[i0:i1]:
        if op.startswith('b'):
            for t in re.findall(r'\b([0-9a-f]{8})\b', args):
                if a < int(t, 16) <= hi:
                    cnt += 1
    return cnt


def cyc(body, lp):
    n, i0, i1, hw, _ = lp
    extra = (0 if hw else 2) + 2 * fwd_taken(body, i0, i1)
    return (n + extra) / per_iter(body, i0, i1)


def show(func, mark, choose):
    body = parse(func)
    if not body:
        print(f'{func}: в {os.path.basename(elf)} не найдена\n')
        return None
    lps = loops_of(body, mark)
    print(f'{func}: {len(body)} инструкций')
    for lp in sorted(lps):
        n, i0, i1, hw, mk = lp
        s = per_iter(body, i0, i1)
        kind = 'аппаратный loop' if hw else 'обычный переход'
        each = f' = {cyc(body, lp):.2f} такта/отсч' if s > 1 else ''
        tag = '  <<< сравнение с порогом' if mk else ''
        print(f'  {kind:16s} {n:3d} инстр/итер ({s} отсч{each})  '
              f'{body[i0][0]:08x}..{body[i1 - 1][0]:08x}{tag}')
    best = choose(body, lps)
    if best is None:
        print('  главный цикл не опознан\n')
        return None
    n, i0, i1, hw, _ = best
    c = cyc(body, best)
    print(f'  -> главный цикл: {n} инструкций на {per_iter(body, i0, i1)} '
          f'отсч ({"аппаратный loop" if hw else "обычный переход"}) '
          f'= {c:.2f} такта на отсчёт\n')
    for aa, oo, gg, _ in body[i0:i1]:
        print(f'     {aa:08x}  {oo:<18s} {gg}')
    print()
    return c


def widest(mark_only):
    def pick(body, lps):
        cand = [lp for lp in lps if lp[4]] if mark_only else lps
        if not cand:
            return None
        return max(cand, key=lambda lp: (per_iter(body, lp[1], lp[2]), -lp[0]))
    return pick


cv = show('diff_vec', None, widest(False))
cw = show('mca_dsp_process', '> thr', widest(True))

if cv is not None and cw is not None:
    tot = cv + cw
    print(f'ИТОГО на отсчёт в ожидании: {cv:.2f} (разность) + {cw:.2f} '
          f'(порог) = {tot:.2f} такта')
    print(f'голый потолок около {240 / tot:.1f} МГц при ядре 240 МГц; '
          'сверху ещё копирование чанка и события - смотрите «загрузку»')
