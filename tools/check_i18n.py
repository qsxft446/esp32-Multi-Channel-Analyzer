"""Проверка перевода страниц прибора: у каждого русского текста есть английский.

    python tools/check_i18n.py

Правила (так устроены страницы в main/mca_web.c):
  - в разметке русский текст лежит внутри элемента с атрибутом data-en
    (английский вариант) или внутри блока class=lr (русский блок справки,
    рядом с ним - блок class=le на английском);
  - title с русским текстом - только вместе с data-en-title;
  - в скриптах русская строка - только первым аргументом L('рус','eng'),
    второй аргумент без русских букв.
Макросы из mca_web.c (SUB_HEAD, SUB_NAV, N_*, MCA_AP_SSID) раскрываются.
Код возврата 1, если есть непереведённое.
"""
import os
import re
import sys
from html.parser import HTMLParser

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'main', 'mca_web.c')
CYR = re.compile('[А-Яа-яЁё]')
VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link',
        'meta', 'source', 'track', 'wbr'}


def strip_c_comments(src):
    out, i, n, st = [], 0, len(src), 'code'
    while i < n:
        if st == 'code':
            if src[i] == '"':
                st = 'str'; out.append(src[i]); i += 1
            elif src.startswith('/*', i):
                j = src.find('*/', i + 2); i = (j + 2) if j > 0 else n
            elif src.startswith('//', i):
                j = src.find('\n', i); i = j if j > 0 else n
            else:
                out.append(src[i]); i += 1
        else:
            if src[i] == '\\':
                out.append(src[i:i + 2]); i += 2
            elif src[i] == '"':
                st = 'code'; out.append(src[i]); i += 1
            else:
                out.append(src[i]); i += 1
    return ''.join(out)


def c_unescape(s):
    def f(m):
        e = m.group(1)
        if e[0] == 'x':
            return chr(int(e[1:], 16))
        return {'n': '\n', 't': '\t'}.get(e, e)
    return re.sub(r'\\(x[0-9a-fA-F]{2}|.)', f, s)


TOK = re.compile(r'"((?:[^"\\\n]|\\.)*)"|([A-Za-z_]\w*)|(\S)')


def c_tokens(s):
    out = []
    for m in TOK.finditer(s):
        if m.group(1) is not None:
            out.append(('s', c_unescape(m.group(1))))
        elif m.group(2):
            out.append(('id', m.group(2)))
        else:
            out.append(('p', m.group(3)))
    return out


def macros(clean):
    """#define с продолжением строк: имя -> (параметры или None, токены)."""
    defs, lines, i = {}, clean.split('\n'), 0
    while i < len(lines):
        ln = lines[i]
        if re.match(r'\s*#define\s', ln):
            while ln.endswith('\\') and i + 1 < len(lines):
                i += 1
                ln = ln[:-1] + ' ' + lines[i]
            m = re.match(r'\s*#define\s+(\w+)(\(([^)]*)\))?(.*)', ln)
            params = ([p.strip() for p in m.group(3).split(',')]
                      if m.group(2) else None)
            defs[m.group(1)] = (params, c_tokens(m.group(4)))
        i += 1
    return defs


def expand(toks, defs):
    out, i = [], 0
    while i < len(toks):
        kind, val = toks[i]
        if kind == 's':
            out.append(val); i += 1
        elif kind == 'id' and val in defs:
            params, body = defs[val]
            if params is None:
                out += expand(body, defs); i += 1
                continue
            j, depth, args = i + 2, 0, [[]]
            while True:
                t = toks[j]
                if t == ('p', '('):
                    depth += 1
                elif t == ('p', ')'):
                    if depth == 0:
                        break
                    depth -= 1
                elif t == ('p', ',') and depth == 0:
                    args.append([]); j += 1
                    continue
                args[-1].append(t); j += 1
            sub = []
            for t in body:
                if t[0] == 'id' and t[1] in params:
                    sub += args[params.index(t[1])]
                else:
                    sub.append(t)
            out += expand(sub, defs)
            i = j + 1
        else:
            i += 1
    return out


class Html(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.st, self.err = [], []

    def _attrs(self, tag, attrs):
        a = dict(attrs)
        for k, v in attrs:
            if not v:
                continue
            if k in ('data-en', 'data-en-title') and CYR.search(v):
                self.err.append(f'<{tag} {k}>: русские буквы в английском: {v[:60]}')
            elif CYR.search(v) and not (k == 'title' and 'data-en-title' in a):
                self.err.append(f'<{tag} {k}="{v[:60]}">: нет '
                                + ('data-en-title' if k == 'title' else 'перевода'))
        return a

    def handle_starttag(self, tag, attrs):
        a = self._attrs(tag, attrs)
        if tag not in VOID:
            self.st.append((tag, a))

    def handle_startendtag(self, tag, attrs):
        self._attrs(tag, attrs)

    def handle_endtag(self, tag):
        for i in range(len(self.st) - 1, -1, -1):
            if self.st[i][0] == tag:
                del self.st[i:]
                break

    def handle_data(self, d):
        if not CYR.search(d):
            return
        cls = [c for _, a in self.st for c in (a.get('class') or '').split()]
        if 'le' in cls:
            self.err.append('русский текст в английском блоке: ' + d.strip()[:70])
        elif not ('lr' in cls or any('data-en' in a for _, a in self.st)):
            self.err.append('текст без data-en: ' + d.strip()[:70])


def js_tokens(js):
    """Строки, слова и знаки; литералы регулярных выражений пропускаются."""
    toks, i, n, prev = [], 0, len(js), 'op'
    while i < n:
        c = js[i]
        if c in '"\'`':
            j = i + 1
            while j < n and js[j] != c:
                j += 2 if js[j] == '\\' else 1
            toks.append(('s', i, js[i + 1:j]))
            i, prev = j + 1, 'v'
        elif c == '/' and prev == 'op':
            j, cls = i + 1, False
            while j < n:
                if js[j] == '\\':
                    j += 2
                    continue
                if js[j] == '[':
                    cls = True
                elif js[j] == ']':
                    cls = False
                elif js[j] == '/' and not cls:
                    break
                j += 1
            j += 1
            while j < n and js[j].isalpha():
                j += 1
            i, prev = j, 'v'
        elif c.isalnum() or c in '_$':
            j = i
            while j < n and (js[j].isalnum() or js[j] in '_$'):
                j += 1
            w = js[i:j]
            toks.append(('w', i, w))
            prev = 'op' if w in ('return', 'typeof', 'case', 'in', 'of', 'else',
                                 'do', 'void', 'new', 'delete') else 'v'
            i = j
        elif c.isspace():
            i += 1
        else:
            toks.append(('p', i, c))
            prev = 'v' if c in ')]' else 'op'
            i += 1
    return toks


def check_shadow(toks, js, allow_def):
    """Имя L занято функцией перевода. Переменная или параметр L в функции
    страницы перекрывают её, и L('...') там падает с TypeError - а опрос
    прибора глотает исключения, страница молча перестаёт обновляться."""
    err = []
    for k, t in enumerate(toks):
        if t[0] != 'w':
            continue
        if t[2] == 'L' and not (k > 0 and toks[k - 1][2] == '.'):
            a = toks[k + 1] if k + 1 < len(toks) else None
            b = toks[k + 2] if k + 2 < len(toks) else None
            if (a and a[2] == '=' and not (b and b[2] == '=' and b[1] == a[1] + 1)):
                err.append('переменная L перекрывает L(): ' + js[t[1]:t[1] + 50])
        if t[2] == 'function':
            j = k + 1
            if j < len(toks) and toks[j][0] == 'w':
                if toks[j][2] == 'L' and not allow_def:
                    err.append('функция L переопределена: ' + js[t[1]:t[1] + 50])
                j += 1
            if j < len(toks) and toks[j][2] == '(':
                while j < len(toks) and toks[j][2] != ')':
                    if toks[j][0] == 'w' and toks[j][2] == 'L':
                        err.append('параметр L перекрывает L(): ' + js[t[1]:t[1] + 50])
                    j += 1
    return err


def check_js(js, allow_def=False):
    toks, inside = js_tokens(js), set()
    err = check_shadow(toks, js, allow_def)
    for k, t in enumerate(toks):
        if not (t[0] == 'w' and t[2] == 'L' and k + 1 < len(toks)
                and toks[k + 1][2] == '('):
            continue
        if k > 0 and (toks[k - 1][2] in ('.', 'function')):
            continue
        j, depth, args = k + 2, 0, [[]]
        while j < len(toks):
            v = toks[j][2]
            if toks[j][0] == 'p' and v in '([{':
                depth += 1
            elif toks[j][0] == 'p' and v in ')]}':
                if depth == 0:
                    break
                depth -= 1
            elif toks[j][0] == 'p' and v == ',' and depth == 0:
                args.append([]); j += 1
                continue
            args[-1].append(j); j += 1
        if len(args) != 2 or not args[1]:
            err.append(f'L(...) не с двумя аргументами: {js[toks[k][1]:toks[k][1] + 60]}')
            continue
        inside.update(args[0])
        for q in args[1]:
            if toks[q][0] == 's' and CYR.search(toks[q][2]):
                err.append('русские буквы во втором аргументе L: ' + toks[q][2][:60])
    for k, t in enumerate(toks):
        if t[0] == 's' and CYR.search(t[2]) and k not in inside:
            err.append('строка без L(): ' + t[2][:70])
    return err


def main():
    clean = strip_c_comments(open(SRC, encoding='utf-8').read())
    defs = macros(clean)
    total = 0
    for m in re.finditer(r'static const char (\w+)\[\]\s*=(.*?);\n', clean, re.S):
        name = m.group(1)
        html = ''.join(expand(c_tokens(m.group(2)), defs))
        err = []
        if name == 'LJS':
            err += check_js(html, allow_def=True)
        elif name != 'CSS':
            for sm in re.finditer(r'<script>(.*?)</script>', html, re.S):
                err += check_js(sm.group(1))
            p = Html()
            p.feed(re.sub(r'<script[^>]*>.*?</script>', '', html, flags=re.S))
            err += p.err
        total += len(err)
        print(f'{name:10s} {"ОК" if not err else "НЕ ПЕРЕВЕДЕНО: %d" % len(err)}')
        for e in err:
            print('   ', e)
    sys.exit(1 if total else 0)


if __name__ == '__main__':
    main()
