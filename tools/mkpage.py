"""Собрать страницу веб-интерфейса из C-литералов в отдельный HTML,
подменив fetch() синтетическими данными. Нужно, чтобы проверять
отрисовку и расчёты в браузере, не прошивая плату.

    python tools/mkpage.py            -> build/page_test.html
    python tools/mkpage.py out.html

Дальше открыть файл через локальный сервер (просто file:// в некоторых
окружениях не выполняет скрипты):

    python -m http.server 8765 --directory build
    http://127.0.0.1:8765/page_test.html

Проверять синтаксис самого JS этим НЕ надо - для этого check_js.py.
"""
import json
import math
import os
import random
import re
import shutil
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SRC = os.path.join(ROOT, 'main', 'mca_web.c')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build',
                                                         'page_test.html')


def strip_c_comments(src):
    """Убрать комментарии C, не трогая содержимое строковых литералов."""
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


clean = strip_c_comments(open(SRC, encoding='utf-8').read())
m = re.search(r'static const char PAGE\[\]\s*=(.*?);\n', clean, re.S)
if not m:
    sys.exit('в mca_web.c не найден PAGE[]')
html = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
html = html.replace('\\"', '"').replace('\\n', '\n').replace('\\\\', '\\')

# Общий стиль отдаётся прошивкой отдельным файлом /s.css - в тестовую
# страницу вставляем его прямо, чтобы она не зависела от сервера.
mc = re.search(r'static const char CSS\[\]\s*=(.*?);\n', clean, re.S)
if mc:
    css = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', mc.group(1)))
    css = css.replace('\\"', '"').replace('\\\\', '\\')
    html = html.replace('<link rel=stylesheet href=/s.css>',
                        '<style>' + css + '</style>')

# --- синтетический спектр: гауссов пик на спадающем континууме ---
NCH, CEN, SIG = 2048, 600, 52
spec = []
for c in range(NCH):
    v = 400 * math.exp(-c / 260.0) + 900 * math.exp(-((c - CEN) ** 2) /
                                                    (2 * SIG ** 2))
    spec.append(max(0, int(v + random.gauss(0, math.sqrt(v + 1)))))

stub = """
<script>
window.__TRUE_FWHM__ = %.2f; window.__TRUE_CEN__ = %d;
window.__PULSE__ = (function(){var a=[];for(var i=0;i<512;i++){
  var v=2098+(Math.random()-0.5)*8,k=i-150;
  if(k>=0)v+=k<5?60*(k+1)/5:60*Math.exp(-(k-5)/43);a.push(Math.round(v))}
  return {ch:300,tg:175,age:120,at:52,ai:48,d:a};})();
window.fetch = function(u){
  var d;
  if (u.indexOf('/spectrum') === 0) d = {d: %s};
  else if (u.indexOf('/stat') === 0) d = {ev:12345,cps:210,pile:0,dead:0,
        base:2048,lost:0,over:0,samp:9000000,freq:8000000,run:1,ms:60000,
        mode:0};
  else if (u.indexOf('/cfg') === 0) d = {threshold:20,cpc:1000,nch:%d,
        algo:0,polarity:0,int_rise:20,int_fall:32,hysteresis:50,trap_L:20,trap_G:44,
        rearm:64,search:70,baseline_shift:10,
        baseline_win:150,flat_avg:0,pileup_pre_pct:0,
        pileup_post_pct:0,
        /* список частот, как у прибора с генератором LEDC (adc_clk.c) */
        fl:[1000000,2000000,4000000,5000000,6666667,8000000,8888889,
            10000000,11428571,13333333,16000000,20000000]};
  else if (u.indexOf('/scope') === 0) {
    /* двоичный ответ как у прибора: 5 x int32, затем отсчёты uint16 */
    var q = new URLSearchParams(u.split('?')[1] || ''),
        n = Math.min(+q.get('n') || 4096, 8192), pre = +q.get('pre') || 0,
        b = new ArrayBuffer(20 + 2 * n), v = new DataView(b);
    v.setInt32(0, pre, true); v.setInt32(4, 300, true); v.setInt32(8, 50, true);
    v.setInt32(12, 30, true); v.setInt32(16, 8192, true);   /* DIAG_SCOPE_LEN */
    for (var i = 0; i < n; i++) {
      var k = i - pre, x = 2048 + (Math.random() - 0.5) * 6;
      if (k >= 0) x += k < 5 ? 600 * (k + 1) / 5 : 600 * Math.exp(-(k - 5) / 140);
      v.setUint16(20 + 2 * i, Math.round(x), true);
    }
    return Promise.resolve({arrayBuffer: function(){return Promise.resolve(b)}});
  }
  else d = {d:[]};
  return Promise.resolve({json:function(){return Promise.resolve(d)}});
};
</script>
""" % (2.3548 * SIG, CEN, json.dumps(spec), NCH)

html = html.replace('<script>', stub + '<script>', 1)
# сразу выделяем участок вокруг пика, чтобы работал измеритель FWHM
html = html.replace("id=pka type=number value=0",
                    "id=pka type=number value=%d" % (CEN - 3 * SIG))
html = html.replace("id=pkb type=number value=0",
                    "id=pkb type=number value=%d" % (CEN + 3 * SIG))

os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, 'w', encoding='utf-8').write(html)
shutil.copyfile(os.path.join(ROOT, 'main', 'logo.png'),
                os.path.join(os.path.dirname(OUT), 'logo.png'))
print(f'записано: {OUT} ({len(html)} байт)')
print(f'в спектре заложен пик: центр {CEN}, FWHM {2.3548*SIG:.2f} канала')
