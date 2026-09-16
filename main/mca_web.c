#include "mca_web.h"
#include "mca_dsp.h"
#include "mca_prof.h"
#include "mca_settings.h"
#include "adc_cap.h"
#include "adc_clk.h"
#include "mca_diag.h"
#include "mca_eth.h"
#include "mca_emu.h"
#include "scope_codec.h"
#include "mca_hist.h"
#include "mca_time.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "mca_web";

volatile mca_mode_t mca_mode        = MCA_MODE_SPECTRUM;
volatile bool       mca_spec_run    = false; /* до «Старт» набора нет  */
volatile bool       mca_scope_run   = true;  /* открыли - сразу видно  */
volatile bool       mca_cmd_clear   = false;
volatile int        mca_cmd_freq_idx= -1;

/* ---------------- страница ---------------- */
/* Общий стиль всех страниц:
 * тёмная зелёная гамма, моноширинный шрифт, панели, цветные кнопки
 * действий, сегментные переключатели. Отдаётся одним файлом /s.css,
 * а не копией в каждой странице. */
static const char CSS[] =
":root{--accent:#34d3c0;--accent-dim:rgba(52,211,192,.12);--bg:#0a0c0a;"
"--panel:#11150f;--bd:#20281e;--dim:#8a9686;--txt:#cfd6cd}"
"*{box-sizing:border-box}"
"html,body{margin:0;padding:0;background:var(--bg)}"
"body{color:var(--txt);font-size:13px;padding-bottom:28px;"
"font-family:ui-monospace,'SF Mono','Cascadia Mono','DejaVu Sans Mono',Consolas,monospace}"
"a{color:var(--accent);text-decoration:none}"
"header{display:flex;align-items:center;justify-content:space-between;gap:16px;"
"padding:8px 24px;border-bottom:1px solid #1c241c}"
".brand{display:flex;align-items:center;gap:13px;font-weight:700;font-size:16px;color:#e8efe2}"
".brand img{height:56px;width:auto;display:block}"
".chips{display:flex;align-items:center;gap:8px;flex-wrap:wrap;justify-content:flex-end}"
".chip{background:#10140f;border:1px solid #243024;border-radius:999px;padding:5px 11px;"
"font-size:12px;color:var(--dim);display:inline-flex;align-items:center;gap:7px}"
".chip.warn{color:#f0c45a;border-color:rgba(240,180,60,.35)}"
".dot{width:7px;height:7px;border-radius:50%;background:var(--accent);box-shadow:0 0 7px var(--accent)}"
".dot.off{background:#5b655a;box-shadow:none}"
"nav{display:flex;align-items:center;gap:6px;padding:0 20px;border-bottom:1px solid #1c241c;"
"overflow-x:auto;scrollbar-width:none}"
/* на узком экране вкладки листаются пальцем, а системная полоса
   прокрутки белым пятном ломала тёмную шапку */
"nav::-webkit-scrollbar{display:none}"
"nav a,nav span{padding:12px 16px;font-size:13px;color:var(--dim);flex-shrink:0;white-space:nowrap;cursor:pointer}"
"nav .on{font-weight:600;color:#e8efe2;border-bottom:2px solid var(--accent)}"
".wrap{padding:18px 22px 0;display:flex;flex-direction:column;gap:14px}"
".panel{background:var(--panel);border:1px solid var(--bd);border-radius:10px}"
".pad{padding:13px 16px}"
".row{display:flex;align-items:center;gap:16px;padding:12px 16px;flex-wrap:wrap}"
".row+.row{border-top:1px solid #1b211a}"
".grp{display:flex;align-items:center;gap:8px;flex-wrap:wrap}"
".status{display:flex;align-items:center;gap:14px;flex-wrap:wrap;font-size:12.5px;color:var(--dim);padding:2px}"
".sep{color:#3a443a}.ac{color:var(--accent);font-weight:600}.w{color:var(--txt)}.mut{color:#6b756a}"
"button{border-radius:8px;padding:9px 16px;font-size:13px;cursor:pointer;background:#161b14;"
"border:1px solid #2a322a;color:var(--txt);font-family:inherit}"
".btn.sm,button.sm{padding:6px 12px;font-size:12px}"
".btn.green{background:var(--accent-dim);border-color:rgba(52,211,192,.5);color:var(--accent);font-weight:600}"
".btn.amber{background:rgba(240,180,60,.08);border-color:rgba(240,180,60,.45);color:#f0c45a}"
".btn.red{background:rgba(240,90,80,.07);border-color:rgba(240,90,80,.45);color:#ff8a82}"
"button:hover:not(:disabled){filter:brightness(1.2)}"
"button:disabled{opacity:.4;cursor:default}"
".seg{display:flex;border:1px solid #2a322a;border-radius:7px;overflow:hidden}"
".seg button{padding:7px 14px;font-size:12px;border:none;border-radius:0;background:transparent;color:#9aa496}"
".seg button.on{background:var(--accent);color:#0c1206;font-weight:700}"
".lbl,label{font-size:11.5px;color:#6b756a}"
"input,select{background:#0c0f0b;border:1px solid #283024;color:#e8efe2;padding:6px 9px;"
"border-radius:7px;font-size:12px;font-family:inherit;outline:none}"
"input[type=number]{width:78px}"
"input:focus,select:focus{border-color:rgba(52,211,192,.5)}"
"input[type=checkbox]{accent-color:var(--accent);vertical-align:middle}"
".big{font-size:26px;font-weight:600;color:#e8efe2;letter-spacing:1px}"
".big.ac{color:var(--accent);font-size:22px}.big.w{font-size:22px}"
".ml{margin-left:auto}"
"canvas{width:100%;display:block;background:#0c0f0b;border-radius:6px}"
".mono{font-size:12px;line-height:1.7;color:#aeb6a8}"
"table{border-collapse:collapse;width:100%;font-size:12px}"
"th,td{border-bottom:1px solid #1b211a;padding:7px 10px;text-align:left;color:var(--txt);vertical-align:top}"
"th{color:var(--accent);font-weight:600;font-size:11.5px}"
"h3{margin:0 0 8px;font-size:15px;color:#e8efe2}"
"h4{margin:14px 0 4px;color:var(--accent);font-size:13px}"
"code{background:#0c0f0b;border:1px solid #20281e;padding:1px 5px;border-radius:4px;font-size:11.5px;color:#aeb6a8}"
".ok{color:var(--accent)}.bad{color:#ff8a82}.warn{color:#f0c45a}"
".n{color:#6b756a;font-size:12px;line-height:1.6}"
/* таблица настроек на вкладке осциллографа */
/* separate, а не collapse: половинки схлопнутых границ дают лишний
   пиксель ширины и полосу прокрутки под таблицей */
".ptab{width:100%;border-collapse:separate;border-spacing:0;font-size:12.5px}"
".ptab td{padding:5px 8px;border-bottom:1px solid #1f261f;vertical-align:middle}"
".ptab tr.gh td{padding-top:14px;color:#e8efe2;font-weight:600;border-bottom:1px solid #2c362c}"
".ptab td:first-child{white-space:nowrap}"
".ptab td:nth-child(2),.ptab td:nth-child(3){width:1%;white-space:nowrap}"
".ptab td:last-child{color:#8a948a;min-width:220px}"
".ptab label{border-bottom:1px dotted #3a443a;cursor:help}"
".ptab input,.ptab select{width:104px}"
".ptab tr.off{opacity:.35}"
".bdg{display:inline-block;padding:1px 8px;border-radius:9px;font-size:11px;"
"border:1px solid #3a443a;color:#9aa59a;white-space:nowrap}"
".bdg.t{color:#f0c45a;border-color:#6b5a2a}.bdg.i{color:#34d3c0;border-color:#1f5f57}"
/* переключатель языка в шапке и блоки справки на двух языках (см. LJS) */
".lng button{padding:4px 9px;font-size:11px}"
"html[lang=en] .lr,html[lang=ru] .le{display:none}"
/* мониторинг CPS на вкладке «Спектр» */
".mcards{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}"
".mcard{border:1px solid var(--bd);border-radius:8px;padding:10px 12px}"
".mcard .big{margin:4px 0}.mcard .mut{font-size:11.5px}"
".mtab{max-height:260px;overflow-y:auto;margin-top:10px}"
"@media (max-width:620px){.big{font-size:20px}.big.ac,.big.w{font-size:18px}"
"header{padding:8px 12px}.wrap{padding:12px 10px 0}.brand img{height:44px}}";

/* ЯЗЫК СТРАНИЦ: русский и английский. Один файл /l.js на все страницы,
 * как стиль, подключается в <head> до разметки.
 * Язык - выбор в этом браузере (кнопки RU/EN в шапке), а если выбора не
 * было - язык браузера. Смена языка перезагружает страницу.
 * Тексты - парами прямо на месте:
 *   - в разметке у элемента атрибут data-en: при английском его
 *     содержимое заменяется на английское; title - атрибут data-en-title;
 *   - в скриптах L('русский','english');
 *   - длинная справка - два блока, class=lr и class=le (прячет стиль).
 * Новые надписи - сразу парой; проверка: python tools/check_i18n.py. */
static const char LJS[] =
"var LANG=(function(){try{var s=localStorage.getItem('mca_lang');if(s=='ru'||s=='en')return s}catch(e){}"
"return /^ru/i.test(navigator.language||'')?'ru':'en'})();"
"document.documentElement.lang=LANG;"
"function L(r,e){return LANG=='en'?e:r}"
"function setLang(l){if(l==LANG)return;try{localStorage.setItem('mca_lang',l)}catch(e){}location.reload()}"
/* Страницы зовут i18n() первой строкой своего скрипта, чтобы их код уже
   видел переведённую разметку; повторный вызов ничего не делает. */
"function i18n(){var a,i;"
"if(LANG=='en'){a=document.querySelectorAll('[data-en]');"
"for(i=0;i<a.length;i++){a[i].innerHTML=a[i].getAttribute('data-en');a[i].removeAttribute('data-en')}"
"a=document.querySelectorAll('[data-en-title]');"
"for(i=0;i<a.length;i++){a[i].title=a[i].getAttribute('data-en-title');a[i].removeAttribute('data-en-title')}}"
"a=document.querySelectorAll('.lng button');"
"for(i=0;i<a.length;i++)a[i].className=a[i].textContent.toLowerCase()==LANG?'on':''}"
"document.addEventListener('DOMContentLoaded',i18n);"
/* ЧАСЫ ПРИБОРА (mca_time.c): своих часов у него нет. Если время не от
   SNTP и не задано или ушло больше чем на 2 с, любая открытая страница
   отдаёт прибору время браузера; повтор раз в час. */
"function tsync(){try{fetch('/time').then(function(r){return r.json()}).then(function(j){"
"if(!j||j.src==1)return;var d=Date.now();if(!j.now||Math.abs(j.now-d)>2000)fetch('/time?set='+d)})"
".catch(function(){})}catch(e){}}"
"document.addEventListener('DOMContentLoaded',function(){tsync();setInterval(tsync,3600000)});";

static const char PAGE[] =
"<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>MCA · AD9226</title><link rel=stylesheet href=/s.css><script src=/l.js></script></head><body>"
"<header><div class=brand><img src=/logo.png?v=2 alt=''>MCA "
"<span style=color:var(--accent)>AD9226</span></div>"
"<div class=chips><span class=chip id=runchip><span class='dot off' id=rundot></span>"
"<span id=runtxt data-en='no connection'>нет связи</span></span><span class=chip id=fchip>&mdash;</span>"
/* язык страниц: RU / EN (см. LJS) - рядом с частотой АЦП */
"<span class='seg lng'><button onclick=\"setLang('ru')\">RU</button>"
"<button onclick=\"setLang('en')\">EN</button></span></div></header>"
/* Режимы - вкладками. Сам режим живёт в скрытом
   списке md: на него завязаны опрос и синхронизация с прибором. */
"<nav id=tabs><span data-md=0 onclick=tab(0) data-en='Spectrum'>Спектр</span>"
"<span data-md=1 onclick=tab(SCM) data-en='Oscilloscope'>Осциллограф</span>"
"<a href=/wifi data-en='Network'>Сеть</a><a href=/help data-en='Help'>Справка</a></nav>"
"<select id=md hidden onchange=setmode()><option value=0><option value=1>"
"<option value=2><option value=3></select>"
"<div class=wrap>"
"<div class=status id=st data-en='connecting...'>подключение...</div>"
"<section class=panel>"
"<div class=row><div class=grp>"
"<button class='btn green' onclick=run(1) data-en='&#9654; Start'>&#9654; Старт</button>"
"<button class='btn amber' onclick=run(0) data-en='&#9632; Stop'>&#9632; Стоп</button>"
"<button class='btn red' id=bclr onclick=cmd('clear') data-en='&#8634; Reset'>&#8634; Сброс</button></div>"
"<div class=ml id=g_big style='display:flex;align-items:center;gap:16px;flex-wrap:wrap'>"
"<span class=big id=bt>00:00:00</span><span class=mut>|</span>"
"<span class='big ac'><span id=bc>0</span><span class=mut style=font-size:12px> CPS</span></span>"
"<span class=mut>|</span>"
"<span class='big w'><span id=be>0</span><span class=mut style=font-size:12px data-en=' events'> событий</span></span>"
"</div></div>"
"<div class=row>"
"<span class=grp id=g_sp><span class=lbl data-en='Y scale'>Шкала Y</span>"
"<span class=seg><button id=blin onclick=setLog(0)>Lin</button>"
"<button id=blog class=on onclick=setLog(1)>Log</button></span>"
"<input type=checkbox id=lg checked hidden>"
"<span class=lbl style=margin-left:8px data-en='peak from'>пик от</span><input id=pka type=number value=0 onchange=poll()>"
"<span class=lbl data-en='to'>до</span><input id=pkb type=number value=0 onchange=poll()>"
"<span class=lbl style=margin-left:8px title='Сохранить спектр файлом: "
"XML - ResultDataFile (BecqMoni), CSV - канал и счёт, N42 - ANSI N42.42, SPE - SpectraLine. "
"Выгружаются каналы, видимые на экране; время замера берётся из часов компьютера.' "
"data-en-title='Save the spectrum to a file: XML - ResultDataFile (BecqMoni), CSV - channel and count, "
"N42 - ANSI N42.42, SPE - SpectraLine. The channels visible on screen are exported; the measurement "
"time is taken from the computer clock.' data-en='export'>выгрузить</span>"
"<button class='btn sm' onclick=expo('xml')>XML</button>"
"<button class='btn sm' onclick=expo('csv')>CSV</button>"
"<button class='btn sm' onclick=expo('n42')>N42</button>"
"<button class='btn sm' onclick=expo('spe')>SPE</button></span>"
"<span class=grp id=g_sc>"
"<span class=seg><button id=bauto onclick=scm(1) title='синхронизация по фронту, а без фронта - свободный пуск' "
"data-en-title='edge trigger; without an edge - free run' data-en='Auto'>Авто</button>"
"<button id=bwait onclick=scm(3) title='только по фронту, держит последний пойманный импульс' "
"data-en-title='edge only; holds the last captured pulse' data-en='Normal'>Ждущий</button></span>"
"<span class=lbl title='Перепад сигнала в сторону импульса (с учётом полярности) за 8 отсчётов, коды АЦП. Выше шума, но ниже амплитуды нужных импульсов.' "
"data-en-title='Signal step toward the pulse (polarity-aware) over 8 samples, ADC codes. Above the noise but below the height of the wanted pulses.' "
"data-en='trigger on edge &ge;'>синхр. по фронту &ge;</span><input id=tl type=number value=30>"
"<span class=lbl title='Синхронизация по амплитуде: показывать только импульсы, у которых высота над базой (коды АЦП, с учётом полярности) в этом диапазоне. 0 и 0 - без фильтра. Высота показанного импульса и сколько отброшено - под графиком.' "
"data-en-title='Amplitude trigger: show only pulses whose height above the baseline (ADC codes, polarity-aware) is within this range. 0 and 0 - no filter. The height of the shown pulse and how many were rejected are shown under the plot.' "
"data-en='amplitude from'>амплитуда от</span><input id=amin type=number value=0 min=0>"
"<span class=lbl data-en='to'>до</span><input id=amax type=number value=0 min=0>"
"<span class=lbl title='От какой линии отсчитывать ось Y' data-en-title='Which line the Y axis is measured from' data-en='Y axis from'>ось Y от</span>"
"<span class=seg><button id=rf0 onclick=setRef(0) title='от измеренной базовой линии' data-en-title='from the measured baseline' data-en='baseline'>базы</button>"
"<button id=rf1 onclick=setRef(1) title='от середины шкалы АЦП, код 2048; на плате со входом ±5 В это 0 В' "
"data-en-title='from the middle of the ADC scale, code 2048; on a board with a ±5 V input this is 0 V'>2048</button>"
"<button id=rf2 onclick=setRef(2) title='настоящие коды АЦП 0..4095' data-en-title='true ADC codes 0..4095' data-en='codes'>кодов</button></span>"
"<label title='вся шкала АЦП: видно, сколько осталось до потолка и до пола' "
"data-en-title='full ADC scale: shows how much room is left to the ceiling and the floor'>"
"<input type=checkbox id=fx onchange=vis()> <span data-en='full scale'>вся шкала</span></label></span>"
"<span class=grp id=g_zm>"
"<span class=lbl title='Сколько отсчётов на экране. Время на деление подписано на графике.' "
"data-en-title='How many samples are on screen. Time per division is shown on the plot.' data-en='timebase'>развёртка</span>"
"<select id=zm><option value=0 data-en='auto'>авто</option><option value=64>64</option>"
"<option value=128>128</option><option value=256>256</option>"
"<option value=512>512</option><option value=1024>1024</option>"
"<option value=2048>2048</option><option value=4096>4096</option>"
"<option value=8192>8192</option><option value=16384>16384</option>"
"<option value=32768 data-en='32768 smp'>32768 отсч</option></select>"
"<span class=lbl title='Подписи сетки по горизонтали: время или номера отсчётов. Отсчёты считаются от момента синхронизации (синяя метка = 0), до неё - отрицательные: так прямо с экрана читаются числа для L, G, «до/после вершины», перезапуска и поиска пика.' "
"data-en-title='Horizontal grid labels: time or sample numbers. Samples are counted from the trigger point (blue mark = 0), negative before it: this way the numbers for L, G, before/after peak, re-arm and peak search are read straight from the screen.' "
"data-en='X axis'>ось X</span>"
"<span class=seg><button id=bx0 onclick=setX(0) data-en='µs'>мкс</button>"
"<button id=bx1 onclick=setX(1) data-en='smp'>отсч</button></span>"
"<span class=lbl title='0 = автоматически' data-en-title='0 = automatic'>Y max</span><input id=ymax type=number value=0></span>"
"<label id=g_shs><input type=checkbox id=shset onchange=vis()> <span data-en='settings'>настройки</span></label>"
"<label id=g_shd title='Тракт АЦП: поток, профиль, журнал потерь, проверка линий данных' "
"data-en-title='ADC path: stream, profile, loss log, data line check'>"
"<input type=checkbox id=shdiag onchange=vis()> <span data-en='diagnostics'>диагностика</span></label>"
"<span class='ml mut' id=g_leg style=font-size:11.5px data-en='ruler: a click on the plot places "
"<b style=color:#7ee081>A</b> start, <b style=color:#f0c45a>B</b> peak, <b style=color:#ff8a82>C</b> end; marks can be dragged'>"
"линейка: щелчок по графику ставит <b style=color:#7ee081>A</b> начало, "
"<b style=color:#f0c45a>B</b> вершину, <b style=color:#ff8a82>C</b> конец; метки можно тащить</span>"
"</div></section>"
"<section class='panel pad'><canvas id=cv height=320></canvas>"
"<div id=g_rul class=grp style='margin-top:8px'>"
"<span id=rult class=mono style=font-size:12px></span><span class=ml></span>"
"<button id=rset class='btn sm' onclick=rulSet() title='До вершины = B−A, После вершины = C−B. "
"Поля заполнятся в настройках, в прибор уйдут кнопкой «Применить». Действуют при способе «интегрирование».' "
"data-en-title='Before peak = B−A, After peak = C−B. The fields are filled in the settings and go to the device "
"with the Apply button. Used with the integration method.' data-en='set before / after peak'>"
"в «до / после вершины»</button>"
"<button class='btn sm' onclick=rulClr() data-en='clear'>сбросить</button></div></section>"
"<section class='panel pad'><div id=sc class=mono style=min-height:110px></div></section>"
/* МОНИТОРИНГ CPS - только на вкладке «Спектр» */
"<section class='panel pad' id=g_mon>"
"<div class=grp style=margin-bottom:10px>"
"<b style=color:#e8efe2 data-en='CPS monitor'>Мониторинг CPS</b>"
"<span class=mut id=mtime style=font-size:11.5px></span><span class=ml></span>"
"<span class=lbl title='По скольку секунд складывать отсчёты в одну точку графика' "
"data-en-title='How many seconds of samples go into one point of the plot' data-en='averaging, s'>усреднение, с</span>"
"<input id=mint type=number min=1 max=3600 value=1 onchange=mdraw()>"
"<span class=lbl title='Сколько времени видно на графике. Колесо мыши меняет масштаб, двойной щелчок возвращает к этому окну' "
"data-en-title='How much time the plot shows. The mouse wheel changes the zoom, double click returns to this window' data-en='window'>окно</span>"
"<select id=mwin onchange=mwsel()><option value=30000 data-en='30 s'>30 с</option><option value=120000 data-en='2 min'>2 мин</option>"
"<option value=600000 data-en='10 min'>10 мин</option><option value=1800000 data-en='30 min'>30 мин</option>"
"<option value=3600000 data-en='1 h'>1 ч</option><option value=21600000 data-en='6 h'>6 ч</option></select>"
"<span class=lbl title='Скользящее среднее (оранжевая линия): по скольким точкам' "
"data-en-title='Moving average (orange line): over how many points' data-en='SMA window'>окно SMA</span>"
"<input id=msma type=number min=2 max=200 value=10 onchange=mdraw()>"
"<button class='btn sm' onclick=mcsv() title='Точки графика файлом: время, длительность, импульсы, CPS, погрешность' "
"data-en-title='Plot points as a file: time, duration, counts, CPS, error'>CSV</button>"
"<button class='btn sm' onclick=mclr() data-en='clear history'>очистить историю</button></div>"
"<div class=mcards>"
"<div class=mcard><div class=lbl data-en='Current CPS'>Текущий CPS</div><div class='big ac' id=mc1>&mdash;</div>"
"<div class=mut id=mc1s></div></div>"
"<div class=mcard><div class=lbl data-en='Mean CPS (history)'>Средний CPS (история)</div><div class='big ac' id=mc2>&mdash;</div>"
"<div class=mut id=mc2s></div></div>"
"<div class=mcard><div class=lbl data-en='Last interval'>Последний интервал</div><div class='big ac' id=mc3>&mdash;</div>"
"<div class=mut id=mc3s></div></div></div>"
"<canvas id=mcv height=260 style=margin-top:10px></canvas>"
"<div class=mut style=font-size:11px;margin-top:4px>"
"<span style=color:#34d3c0>&#9644;</span> <span data-en='intervals'>интервалы</span> &nbsp;"
"<span style=color:#ff8c1a>&#9644;</span> SMA &nbsp; "
"<span data-en='wheel - zoom, drag - move into the past, double click - back to the window and now'>"
"колесо - масштаб, перетаскивание - сдвиг в прошлое, двойной щелчок - к окну и текущему моменту</span></div>"
"<div class=mtab><table><thead><tr><th data-en='Time'>Время</th><th>&Delta;t, <span data-en='s'>с</span></th>"
"<th data-en='Counts'>Импульсы</th><th>CPS</th><th>&delta;, %</th></tr></thead><tbody id=mtb></tbody></table></div>"
"</section>"
"<section class='panel pad' id=g_set>"
"<div style=overflow-x:auto><table class=ptab id=pbox></table></div>"
"<div class=grp style=margin-top:10px><button class='btn green' onclick=apply() data-en='Apply'>Применить</button>"
"<span class=mut style=font-size:11.5px data-en='values go to the device only with this button and are saved in its memory'>"
"в прибор значения уходят только по этой кнопке и "
"сохраняются в его памяти</span></div></section>"
/* ДИАГНОСТИКА ТРАКТА АЦП - под галочкой «диагностика», на обеих вкладках
   (раньше - отдельная страница /diag) */
"<section class='panel pad' id=g_diag>"
"<h3 data-en='ADC path diagnostics'>Диагностика тракта АЦП</h3>"
"<label><input type=checkbox id=den onchange=dtog()> "
"<span data-en='per-bit statistics (analyses 1 chunk/s)'>побитовая статистика (разбор 1 чанка/с)</span></label>"
"<div id=dout></div>"
"<button class='btn sm' onclick=dprobe() style=margin-top:10px data-en='Check data lines'>Проверить линии данных</button>"
"<div id=dpr style=margin-top:8px;font-size:12.5px></div></section>"
"</div>"
"<script>i18n();"
/* Список частот присылает прибор (/cfg, поле fl): он зависит от
   генератора CLK в прошивке. Выбор уходит номером в этом списке.
   Пометки к верхним частотам: по замеру на 16 МГц обработка спектра
   занимает ~85 % ядра, дальше растёт пропорционально частоте. */
"var FHZ=[];"
"function fqName(h){return +(h/1e6).toFixed(2)+L(' МГц',' MHz')+"
"(h>17e6&&h<18e6?L(' (предел)',' (limit)'):h>=19e6?L(' (осциллограф)',' (scope)'):'')}"
/* ТАБЛИЦА НАСТРОЕК. Третий столбец - на что параметр влияет:
   b - при любом способе, t - только трапеция, i - только интегрирование.
   Сверено с mca_dsp.c: событие в обоих способах ищется по трапеции
   (порог, L, G, гистерезис, перезапуск, поиск пика, наложения; по L и G
   при интегрировании ещё и находится вершина), а амплитуда считается
   по-разному. Параметры другого способа не прячем, а приглушаем:
   видно, что они есть и что сейчас не действуют. */
"var GR=[[L('Обнаружение импульса','Pulse detection'),["
"['threshold',L('Порог','Threshold'),'b',L('Порог по выходу трапеции, коды АЦП. Ставить выше шума: см. «шум фильтра» под графиком',"
"'Threshold on the trapezoid output, ADC codes. Set above the noise: see “filter noise” under the plot')],"
"['fq',L('Частота','Sample rate'),'b',L('Частота АЦП. Выше - подробнее форма импульса, но больше нагрузка',"
"'ADC sample rate. Higher - more detailed pulse shape, but more load')],"
"['polarity',L('Полярность','Polarity'),'b',L('0 - импульсы вверх, 1 - вниз','0 - pulses up, 1 - down')],"
"['trap_L',L('Окно L','Window L'),'b',L('Окно трапеции, отсч. При трапеции задаёт ещё и амплитуду',"
"'Trapezoid window, samples. With the trapezoid method it also sets the amplitude')],"
"['trap_G',L('Зазор G','Gap G'),'b',L('Отступ вычитаемого окна, отсч. G−L - длина плоской вершины',"
"'Offset of the subtracted window, samples. G−L is the flat-top length')],"
"['hysteresis',L('Гистерезис %','Hysteresis %'),'b',L('Ниже какой доли порога должна упасть трапеция, чтобы ловить следующий',"
"'Fraction of the threshold the trapezoid must drop below before the next pulse is caught')],"
"['rearm',L('Перезапуск','Re-arm'),'b',L('Отсч. после пика, когда новые импульсы не ловятся',"
"'Samples after the peak during which new pulses are not caught')],"
"['search',L('Поиск пика','Peak search'),'b',L('Сколько отсч. после порога искать вершину',"
"'How many samples after the threshold to search for the peak')]]],"
"[L('Амплитуда','Amplitude'),["
"['algo',L('Способ измерения','Method'),'',L('Трапеция или интегрирование. Смена очищает спектр',"
"'Trapezoid or integration. Changing it clears the spectrum')],"
"['flat_avg',L('Средн. по плато','Flat-top avg'),'t',L('1 - среднее по плоской вершине вместо максимума. Работает только при G−L ≥ 16',"
"'1 - mean over the flat top instead of the maximum. Works only when G−L ≥ 16')],"
"['int_rise',L('До вершины','Before peak'),'i',L('Отсч. до вершины в среднем. Линейка: B−A',"
"'Samples before the peak, on average. Ruler: B−A')],"
"['int_fall',L('После вершины','After peak'),'i',L('Отсч. после вершины. Линейка: C−B. Длиннее окно - пик левее',"
"'Samples after the peak. Ruler: C−B. Longer window - the peak moves left')],"
"['baseline_shift',L('База 2^N','Baseline 2^N'),'i',L('Скорость слежения за базовой линией (трапеции база не нужна)',"
"'Baseline tracking speed (the trapezoid does not need a baseline)')],"
"['baseline_win',L('Окно базы','Baseline window'),'i',L('Отсчёты дальше этого от базы в неё не берутся, коды',"
"'Samples farther than this from the baseline are not used for it, codes')]]],"
"[L('Шкала спектра','Spectrum scale'),["
"['cpc',L('Кодов на канал','Codes per channel'),'b',L('Канал = амплитуда / это число. Смена очищает спектр.',"
"'Channel = amplitude / this number. Changing it clears the spectrum.')+' <b id=cpctop></b>'],"
"['nch',L('Каналов','Channels'),'b',L('Длина шкалы вправо. Спектр не сбрасывает',"
"'Scale length to the right. Does not reset the spectrum')]]],"
"[L('Отбраковка наложений','Pile-up rejection'),["
"['pileup_pre_pct',L('Наложение до %','Pile-up before %'),'b',L('Брак, если перед пиком трапеция выше этого % амплитуды. 0 - выключено',"
"'Reject if before the peak the trapezoid is above this % of the amplitude. 0 - off')],"
"['pileup_post_pct',L('Наложение после %','Pile-up after %'),'b',L('Брак, если после пика не упала ниже этого %. Работают, только если оба больше 0',"
"'Reject if after the peak it has not dropped below this %. Active only if both are above 0')]]],"
"[L('Связь с программами на ПК','PC software link'),["
"['emu',L('Эмуляция MCA на COM','MCA emulation on COM'),'b',L('Порт UART0 отвечает по протоколу shproto (BecqMoni и др.). Консоль при этом молчит',"
"'UART0 answers using the shproto protocol (BecqMoni etc.). The console goes silent')]]]];"
/* fq и emu - не параметры обработки: уходят в прибор сразу, своими запросами */
"var P=[];GR.forEach(function(g){g[1].forEach(function(r){if(r[0]!='fq'&&r[0]!='emu')P.push(r[0])})});"
/* пометка только у параметров одного способа; без пометки - действует всегда */
"var BDG={t:[L('трапеция','trapezoid'),'t'],i:[L('интегрирование','integration'),'i']};"
/* поля выпадающими меню: значение и подпись */
"var SEL={algo:[['0',L('трапеция','trapezoid')],['1',L('интегрирование','integration')]],"
"nch:[['2048','2048'],['4096','4096'],['8192','8192']],"
"emu:[['0',L('выключена','off')],['38400','38400'],['115200','115200'],['460800','460800'],"
"['600000','600000'],['921600','921600']]};"
"var H={"
"emu:L('Эмуляция анализатора на последовательном порту UART0 - разъём UART/COM на плате (не встроенный USB). Прибор отвечает по бинарному протоколу shproto, и программы на ПК, умеющие работать с такими анализаторами (например, BecqMoni), видят его как MCA: набор, остановка, сброс, выгрузка спектра и статуса, калибровка. Скорость порта выбирается здесь, действует сразу и сохраняется. Пока эмуляция включена, консоль молчит: сообщения прошивки на порт не идут. Программе отдаётся столько каналов, сколько выбрано в «Каналов»: в BecqMoni выставьте то же число каналов, тогда они совпадут с прибором один в один. Заводские команды настройки из программы не принимаются - параметры задаются на этой странице.',"
"'Analyzer emulation on the serial port UART0 - the UART/COM connector on the board (not the native USB). The device answers using the binary shproto protocol, and PC programs that work with such analyzers (for example, BecqMoni) see it as an MCA: start, stop, reset, spectrum and status upload, calibration. The port speed is selected here, takes effect immediately and is saved. While emulation is on, the console is silent: firmware messages are not sent to the port. The program receives as many channels as selected in “Channels”: set the same number of channels in BecqMoni, then they match the device one to one. Factory tuning commands from the program are not accepted - parameters are set on this page.'),"
"fq:L('Частота семплирования АЦП. Выше - подробнее форма импульса, но обработка может не успевать: смотрите потерянные чанки под галочкой «диагностика». На 16 МГц обработка спектра занимает около 85 % ядра, на 17.14 - около 92 % (предел), на 20 МГц спектр не успевает - она для осциллографа. Гармоники CLK могут мешать WiFi: если на какой-то частоте страница начинает замирать, а пинг до прибора растёт, смените частоту или канал роутера. Параметры фильтра заданы в отсчётах, поэтому на другой частоте то же L или G - другое время.',"
"'ADC sample rate. Higher - more detailed pulse shape, but processing may not keep up: watch lost chunks under the “diagnostics” checkbox. At 16 MHz spectrum processing takes about 85 % of the core, at 17.14 about 92 % (limit), at 20 MHz the spectrum does not keep up - it is for the oscilloscope. CLK harmonics can interfere with WiFi: if at some rate the page starts to freeze and ping to the device grows, change the rate or the router channel. Filter parameters are set in samples, so at another rate the same L or G is a different time.'),"
"polarity:L('0 - импульсы вверх от базовой линии, 1 - вниз (например, анод ФЭУ напрямую). При 1 сигнал переворачивается ещё до обработки, и порог и спектр работают как с импульсами вверх; осциллограф синхронизируется по фронту вниз. Постоянная составляющая сигнала на обработку не влияет - её вычитает трапеция.',"
"'0 - pulses go up from the baseline, 1 - down (for example, PMT anode directly). With 1 the signal is inverted before processing, so the threshold and spectrum work as with upward pulses; the oscilloscope triggers on a falling edge. A DC offset of the signal does not affect processing - the trapezoid subtracts it.'),"
"algo:L('Трапеция: амплитуда по вершине трапеции (окно L, зазор G, по желанию среднее по плато). Интегрирование: среднее отсчётов импульса вокруг вершины за вычетом базовой линии (до и после вершины, база 2^N, окно базы). Поля, которые при выбранном способе ни на что не влияют, в таблице приглушены. Обнаружение события в обоих способах одинаковое - по выходу трапеции, поэтому L, G, порог, гистерезис, перезапуск и поиск пика нужны всегда. Масштабы способов различаются - после переключения подберите «Кодов на канал». Смена способа очищает спектр.',"
"'Trapezoid: amplitude from the trapezoid top (window L, gap G, optionally flat-top average). Integration: mean of the pulse samples around the peak minus the baseline (before and after peak, baseline 2^N, baseline window). Fields that have no effect with the selected method are dimmed in the table. Event detection is the same for both methods - on the trapezoid output, so L, G, threshold, hysteresis, re-arm and peak search are always needed. The methods have different scales - after switching, adjust “Codes per channel”. Changing the method clears the spectrum.'),"
"int_rise:L('Интегрирование: сколько отсчётов ДО вершины включать в сумму. Смотрите на картинку импульса и считайте по сетке.',"
"'Integration: how many samples BEFORE the peak to include in the sum. Look at the pulse picture and count by the grid.'),"
"int_fall:L('Интегрирование: сколько отсчётов ПОСЛЕ вершины включать в сумму. Обычно заметно больше, чем до вершины, потому что спад длиннее фронта.',"
"'Integration: how many samples AFTER the peak to include in the sum. Usually noticeably more than before the peak, because the decay is longer than the rise.'),"
"cpc:L('Сколько кодов амплитуды приходится на один канал спектра: 1 - канал равен коду АЦП, 0.5 - вдвое подробнее, 2 - вдвое грубее. Верх шкалы = каналов x кодов на канал, он подписан рядом. При базе в середине АЦП запас вверх около 2047 кодов, поэтому 2048 каналов по 1 коду как раз покрывают всю шкалу. Амплитуда в обоих способах в кодах: трапеция - высота импульса, интегрирование - средняя высота в окне. Изменение этого числа очищает спектр: старые события разложены по другой шкале.',"
"'How many amplitude codes per spectrum channel: 1 - a channel equals an ADC code, 0.5 - twice as fine, 2 - twice as coarse. Top of scale = channels x codes per channel, shown next to it. With the baseline in the middle of the ADC there are about 2047 codes of headroom, so 2048 channels of 1 code cover the whole scale. The amplitude is in codes for both methods: trapezoid - pulse height, integration - mean height in the window. Changing this number clears the spectrum: old events are laid out on a different scale.'),"
"nch:L('Сколько каналов показывать: 2048, 4096 или 8192. Это длина шкалы: верх = каналов x кодов на канал. Раскладка событий по каналам от этого не зависит - спектр не сжимается и не сбрасывается, меняется только, докуда видно вправо. Прибор копит все 8192 канала, так что переключать можно в любой момент.',"
"'How many channels to show: 2048, 4096 or 8192. This is the scale length: top = channels x codes per channel. How events are laid out in channels does not depend on it - the spectrum is not compressed or reset, only how far to the right is visible changes. The device accumulates all 8192 channels, so you can switch at any time.'),"
"threshold:L('Порог по выходу трапеции, В КОДАХ АЦП (нормирован на длину окна, поэтому не зависит от L). Ставится выше шума трапеции - см. строку «шум фильтра» на вкладке «Осциллограф».',"
"'Threshold on the trapezoid output, IN ADC CODES (normalized to the window length, so it does not depend on L). Set above the trapezoid noise - see the “filter noise” line on the “Oscilloscope” tab.'),"
"hysteresis:L('Доля порога В ПРОЦЕНТАХ, ниже которой должна опуститься трапеция, чтобы детектор снова взвёлся. 50 означает половину порога. Прежний вариант вычитал единицу, что означало почти полное отсутствие гистерезиса.',"
"'Fraction of the threshold IN PERCENT that the trapezoid must drop below before the detector re-arms. 50 means half the threshold. The previous version subtracted one, which meant almost no hysteresis.'),"
"trap_L:L('Длина окна интегрирования: сколько отсчётов импульса суммируется. Именно это окно определяет, какая часть импульса пойдёт в амплитуду, и насколько усреднится шум. ВНИМАНИЕ: в приборах, где окно задаётся парой RISE и FALL, окно интегрирования - это их сумма: RISE=6 FALL=15 соответствует нашему L=21. При интегрировании L и G тоже работают: по трапеции находится импульс и место его вершины.',"
"'Integration window length: how many pulse samples are summed. This window determines which part of the pulse goes into the amplitude and how much the noise is averaged. NOTE: in devices where the window is set by a RISE and FALL pair, the integration window is their sum: RISE=6 FALL=15 corresponds to our L=21. With integration L and G also work: the trapezoid finds the pulse and its peak position.'),"
"trap_G:L('На сколько отсчётов назад отстоит второе окно, которое вычитается. Оно должно целиком лежать на базовой линии ДО импульса, поэтому G обязан быть больше L плюс длительность фронта. Разность G минус L даёт длину плоской вершины.',"
"'How many samples back the second, subtracted window is. It must lie entirely on the baseline BEFORE the pulse, so G must be greater than L plus the rise time. G minus L gives the flat-top length.'),"
"rearm:L('Сколько отсчётов жёстко пропустить после пика, независимо от гистерезиса. Защита от дребезга на спадающем хвосте. Это НЕ метрика потерь, а параметр перезапуска детектора.',"
"'How many samples to skip unconditionally after the peak, regardless of hysteresis. Protection against chatter on the falling tail. This is NOT a loss metric but a detector re-arm parameter.'),"
"search:L('Сколько отсчётов после срабатывания порога перебирать в поисках максимума трапеции. Должно накрывать весь импульс.',"
"'How many samples after the threshold crossing to search for the trapezoid maximum. Must cover the whole pulse.'),"
"baseline_shift:L('Только для интегрирования: трапеция вычитает базу сама. Постоянная времени фильтра, отслеживающего нулевой уровень. Больше значение - медленнее и плавнее подстройка. Обычно 8-12.',"
"'Integration only: the trapezoid subtracts the baseline itself. Time constant of the filter that tracks the zero level. Larger value - slower and smoother tracking. Usually 8-12.'),"
"flat_avg:L('Только для трапеции. 1 = амплитуда как среднее по плоской вершине трапеции, 0 = по максимуму. Проверено симуляцией: усреднение выигрывает ТОЛЬКО при длинном плато (G минус L не меньше 19). При коротком плато максимум даёт даже меньший разброс, поэтому при (G-L) меньше 16 происходит автоматический откат на максимум. Смещение максимума растёт с шумом, но это постоянный сдвиг для всех амплитуд - уходит в калибровку, ширину пиков не портит.',"
"'Trapezoid only. 1 = amplitude as the mean over the flat top of the trapezoid, 0 = maximum. Verified by simulation: averaging wins ONLY with a long flat top (G minus L at least 19). With a short flat top the maximum gives even less spread, so with (G-L) below 16 it automatically falls back to the maximum. The bias of the maximum grows with noise, but it is a constant shift for all amplitudes - it goes into calibration and does not widen the peaks.'),"
"baseline_win:L('Только для интегрирования. Окно приёма отсчёта в базовую линию, коды АЦП. В среднее берутся только отсчёты, лежащие ближе этого значения к текущей оценке нуля - иначе длинные хвосты импульсов утянут базовую линию вверх. Ставить примерно вдвое-втрое больше размаха шума.',"
"'Integration only. Acceptance window for a sample into the baseline, ADC codes. Only samples closer than this to the current zero estimate are averaged - otherwise long pulse tails would pull the baseline up. Set about two to three times the noise swing.'),"
"pileup_pre_pct:L('Если ПЕРЕД пиком трапеция уже выше этого процента от амплитуды - импульс сидит на хвосте предыдущего, бракуем. Меньше процент - строже отбраковка.',"
"'If BEFORE the peak the trapezoid is already above this percentage of the amplitude - the pulse sits on the tail of the previous one, reject. Lower percentage - stricter rejection.'),"
"pileup_post_pct:L('Если ПОСЛЕ пика трапеция не упала ниже этого процента - на импульс наложился следующий, бракуем. Меньше процент - строже отбраковка.',"
"'If AFTER the peak the trapezoid has not dropped below this percentage - the next pulse piled up on this one, reject. Lower percentage - stricter rejection.'),"
"};"
"var fs=document.createElement('select');fs.id='fq';fs.onchange=setfreq;"
"function fillFq(){fs.innerHTML='';FHZ.forEach(function(h,i){"
"var o=document.createElement('option');o.value=i;o.text=fqName(h);fs.add(o)})}"
/* строки таблицы: подпись (подробная подсказка - при наведении), поле,
   на какой способ влияет, коротко что делает */
"var pt='';"
"GR.forEach(function(g){pt+='<tr class=gh><td colspan=4>'+g[0]+'</td></tr>';"
"g[1].forEach(function(r){var k=r[0],b=BDG[r[2]],c;"
"if(k=='fq')c='<span id=fqslot></span>';"
"else if(SEL[k])c='<select id=p_'+k+'>'+SEL[k].map(function(o){"
"return '<option value='+o[0]+'>'+o[1]+'</option>'}).join('')+'</select>';"
"else c='<input id=p_'+k+' type=number'+(k=='cpc'?' step=0.05':'')+'>';"
"pt+='<tr id=w_'+k+'><td><label title=\"'+(H[k]||'').replace(/\"/g,'&quot;')+'\">'+r[1]+'</label></td>'+"
"'<td>'+c+'</td><td>'+(b?'<span class=\"bdg '+b[1]+'\">'+b[0]+'</span>':'')+'</td>'+"
"'<td>'+r[3]+'</td></tr>'})});"
"var pb=document.getElementById('pbox');pb.innerHTML=pt;"
"document.getElementById('fqslot').appendChild(fs);"
/* способ измерения: строки другого способа приглушаются */
"function showFields(){var al=+document.getElementById('p_algo').value;"
"GR.forEach(function(g){g[1].forEach(function(r){var w=document.getElementById('w_'+r[0]);"
"if(w)w.className=(r[2]=='t'&&al!=0)||(r[2]=='i'&&al!=1)?'off':''})})}"
"showFields();"
/* «Кодов на канал»: в описании сразу виден верх шкалы - каналов x кодов */
"function cpcTop(){var c=+document.getElementById('p_cpc').value,n=+document.getElementById('p_nch').value;"
"document.getElementById('cpctop').textContent=c>0&&n?L('Сейчас шкала до ','Scale now up to ')+(+(c*n).toFixed(1))+L(' кодов.',' codes.'):''}"
"document.getElementById('p_algo').onchange=showFields;"
"document.getElementById('p_emu').onchange=setemu;"
"document.getElementById('p_cpc').oninput=cpcTop;document.getElementById('p_nch').onchange=cpcTop;"
"function cmd(c){fetch('/cmd?do='+c)}"
/* Старт/Стоп относятся к открытой вкладке: на «Спектре» - набор
   спектра, на «Осциллографе» - только осциллограф. На паузе осциллограф
   не опрашивается и держит последний кадр (линейка работает). */
/* RLOCK - как MDLOCK для режима: ответ /stat, ушедший с прибора ДО
   команды, иначе вернул бы прежнее состояние, и опрос осциллографа и
   подпись в шапке на секунду дёрнулись бы обратно */
"function run(on){window.RLOCK=Date.now()+1500;"
"if(isScope()){window.SRUN=on?1:0;cmd(on?'scope_start':'scope_stop');"
"runChip(on,1)}else{cmd(on?'start':'stop');runChip(on,0)}}"
/* подпись в шапке: состояние той работы, что на открытой вкладке */
"function runChip(on,sc){"
"document.getElementById('rundot').className=on?'dot':'dot off';"
"document.getElementById('runtxt').textContent=sc?(on?L('осциллограф идёт','scope running'):L('осциллограф на паузе','scope paused')):"
"(on?L('идёт набор','acquiring'):L('остановлен','stopped'));"
"document.getElementById('runchip').className=on?'chip':'chip warn'}"
/* Выгрузка спектра файлом. Часов у прибора нет - время для файла
   берётся отсюда: Unix-время и смещение пояса в минутах. Ответ идёт
   как вложение, поэтому страница никуда не уходит. */
"function expo(f){var d=new Date();"
"location.href='/export.'+f+'?t='+Math.floor(d/1000)+'&tz='+(-d.getTimezoneOffset())}"
/* MDLOCK - окно, в течение которого ответы /stat не трогают выбор
   режима. Без него ответ, ушедший с прибора ДО применения новой
   команды, возвращал бы список обратно, и пункт дёргался бы. */
"function setmode(){window.MDLOCK=Date.now()+1500;"
"fetch('/cfg?mode='+document.getElementById('md').value)}"
"function setfreq(){fetch('/cfg?freq='+fs.value)}"
/* эмуляция MCA на COM: включается сразу, с подтверждением - консоль замолчит */
"function setemu(){var e=document.getElementById('p_emu'),v=e.value;"
"if(v!='0'&&!confirm(L('Включить эмуляцию MCA на порту UART0, ','Enable MCA emulation on UART0 at ')+v+"
"L(' бод? Сообщения консоли на этот порт идти перестанут.',' baud? Console messages will stop going to this port.')))"
"{e.value=window.EMU||'0';return}window.EMU=v;fetch('/cfg?emu='+v)}"
/* «Кодов на канал» прибор хранит в тысячных: 0.5 уходит как 500 */
"function apply(){var q=P.map(function(k){var v=document.getElementById('p_'+k).value;"
"return k=='cpc'?'cpc='+Math.round(v*1000):k+'='+v}).join('&');fetch('/cfg?'+q)}"
"var cv=document.getElementById('cv'),cx=cv.getContext('2d');"
/* Поле графика: сверху TOPM, снизу BOTM под шкалу каналов.
   Обе функции обязаны считать высоту одинаково, иначе кривая
   разъедется с сеткой. */
"var TOPM=10,BOTM=22;"
"function plotH(H){return H-TOPM-BOTM}"
/* Шаг подписей: округляем до 1/2/5 x 10^n, чтобы числа были
   круглыми при любом количестве каналов. */
"function nicestep(span,want){var raw=span/want;"
"var p=Math.pow(10,Math.floor(Math.log(raw)/Math.LN10)),n=raw/p;"
"return (n<=1?1:n<=2?2:n<=5?5:10)*p}"
/* Спектр: лёгкая заливка под кривой и чёткая линия
   сверху. Сплошные столбцы давали тяжёлую «плиту» и топили форму. */
"function line(a,col,mn,mx,W,H,lw){if(!a||!a.length)return;"
"var sp=(mx-mn)||1,Hp=plotH(H),pts=[];"
"for(var i=0;i<a.length;i++){var v=(a[i]-mn)/sp;pts.push([i*W/a.length,H-BOTM-v*Hp])}"
"cx.beginPath();cx.moveTo(pts[0][0],H-BOTM);"
"for(var i=0;i<pts.length;i++)cx.lineTo(pts[i][0],pts[i][1]);"
"cx.lineTo(pts[pts.length-1][0],H-BOTM);cx.closePath();cx.fillStyle=col+'1e';cx.fill();"
"cx.beginPath();for(var i=0;i<pts.length;i++)i?cx.lineTo(pts[i][0],pts[i][1]):cx.moveTo(pts[i][0],pts[i][1]);"
"cx.strokeStyle=col;cx.lineWidth=lw||1;cx.stroke()}"
"function grid(W,H,mn,mx,lbl,lg,nch){cx.lineWidth=1;"
"cx.font='11px ui-monospace,monospace';var Hp=plotH(H);"
/* горизонтальные линии с подписями счёта */
"for(var k=0;k<=4;k++){var y=TOPM+k*Hp/4;"
"cx.strokeStyle='rgba(255,255,255,.05)';cx.beginPath();"
"cx.moveTo(0,y);cx.lineTo(W,y);cx.stroke();"
"var v=mx-(mx-mn)*k/4;"
"if(lg)v=Math.round(Math.exp(v)-1);else v=Math.round(v);"
"cx.fillStyle='#6b756a';cx.fillText(v,3,y-3)}"
/* ШКАЛА КАНАЛОВ СНИЗУ */
"if(nch>0){var st=nicestep(nch,8);cx.textAlign='center';"
"for(var c=0;c<=nch;c+=st){var x=c*W/nch;"
"cx.strokeStyle='rgba(255,255,255,.035)';cx.beginPath();"
"cx.moveTo(x,TOPM);cx.lineTo(x,H-BOTM);cx.stroke();"
"cx.strokeStyle='#283024';cx.beginPath();"
"cx.moveTo(x,H-BOTM);cx.lineTo(x,H-BOTM+4);cx.stroke();"
"cx.fillStyle='#6b756a';"
"cx.fillText(c,Math.min(Math.max(x,14),W-14),H-8)}"
/* Последняя засечка - полное число каналов. Рисуем, только если
   до неё осталось место: иначе она налезает на предыдущую подпись
   (при 1024 каналах шаг 200 давал "1000" и "1024" друг на друге). */
"if(nch%st && (nch%st)*W/nch>34){cx.strokeStyle='#283024';cx.beginPath();"
"cx.moveTo(W-1,H-BOTM);cx.lineTo(W-1,H-BOTM+4);cx.stroke();"
"cx.fillStyle='#6b756a';cx.fillText(nch,W-14,H-8)}"
"cx.textAlign='left'}"
"cx.strokeStyle='#283024';cx.lineWidth=1;cx.strokeRect(0.5,TOPM+0.5,W-1,plotH(H));"
"if(lbl){cx.fillStyle='#6b756a';cx.fillText(lbl,W-150,TOPM+11)}}"
"function gridS(W,H,ymn,ymx,ns,lbl,us,x0){"
"cx.font='11px ui-monospace,monospace';"
/* горизонтальные линии с подписями амплитуды */
"for(var k=0;k<=5;k++){var y=10+k*(H-30)/5;"
"cx.strokeStyle='rgba(255,255,255,.05)';cx.beginPath();cx.moveTo(28,y);cx.lineTo(W,y);"
"cx.stroke();cx.fillStyle='#6b756a';"
"cx.fillText(Math.round(ymx-(ymx-ymn)*k/5),2,y+4)}"
/* вертикальные линии: по времени, если известен шаг отсчёта
   (us - микросекунд на отсчёт), иначе по номерам отсчётов. Номера
   считаются от x0 - индекса момента синхронизации (x0 < 0 - без
   синхронизации, тогда от левого края); шаг круглый, и 0 всегда
   попадает на линию сетки. */
"if(us){var span=ns*us,ts=nicestep(span,10);"
"for(var k=0;k*ts<=span*1.0001;k++){var t=k*ts,x=28+(t/us)*(W-30)/ns;"
"cx.strokeStyle='rgba(255,255,255,.035)';cx.beginPath();cx.moveTo(x,10);"
"cx.lineTo(x,H-20);cx.stroke();"
"cx.fillStyle='#6b756a';cx.fillText(+t.toFixed(2),x-6,H-6)}"
"cx.fillStyle='#6b756a';cx.fillText(L('мкс · ','µs · ')+(+ts.toFixed(3))+L(' мкс/дел',' µs/div'),W-170,22)}"
"else{var o0=x0>0?x0:0,stp=Math.max(1,Math.round(nicestep(ns,10)));"
"for(var k=Math.ceil(-o0/stp);o0+k*stp<=ns;k++){var x=28+(o0+k*stp)*(W-30)/ns;"
"cx.strokeStyle='rgba(255,255,255,.035)';cx.beginPath();cx.moveTo(x,10);"
"cx.lineTo(x,H-20);cx.stroke();"
"cx.fillStyle='#6b756a';cx.fillText(k*stp,x-6,H-6)}"
"cx.fillStyle='#6b756a';cx.fillText(L('отсч · ','smp · ')+stp+L(' отсч/дел',' smp/div')+(x0>=0?L(' · 0 = синхр.',' · 0 = trigger'):''),W-230,22)}"
"if(lbl){cx.fillStyle='#6b756a';cx.fillText(lbl,W-170,H-6)}}"
"function lineS(a,col,ymn,ymx,W,H,lw){if(!a||!a.length)return;"
"cx.strokeStyle=col;cx.lineWidth=lw||1.4;cx.beginPath();"
"var sp=(ymx-ymn)||1,ns=a.length-1;"
"for(var i=0;i<a.length;i++){var x=28+i*(W-30)/ns,"
"y=(H-20)-((a[i]-ymn)/sp)*(H-30);i?cx.lineTo(x,y):cx.moveTo(x,y)}"
"cx.stroke()}"
"function hline(val,mn,mx,W,H,col,txt){var sp=(mx-mn)||1;"
"var y=H-((val-mn)/sp)*(H-20)-10;if(y<0||y>H)return;"
"cx.strokeStyle=col;cx.lineWidth=1;cx.setLineDash([5,4]);cx.beginPath();"
"cx.moveTo(0,y);cx.lineTo(W,y);cx.stroke();cx.setLineDash([]);"
"cx.fillStyle=col;cx.font='11px ui-monospace,monospace';"
"cx.fillText(txt,W-90,y-4)}"
"function draw(a,log){var W=cv.width=cv.clientWidth,H=cv.height;"
"cx.clearRect(0,0,W,H);if(!a||!a.length)return;"
"var m=1;for(var i=0;i<a.length;i++)if(a[i]>m)m=a[i];"
"if(log){var b=[],lm=Math.log(1+m);"
"for(var i=0;i<a.length;i++)b[i]=Math.log(1+a[i]);"
"grid(W,H,0,lm,L('лог. шкала','log scale'),1,a.length);"
"line(b,'#34d3c0',0,lm,W,H)}else{"
"grid(W,H,0,m,L('линейная шкала','linear scale'),0,a.length);"
"line(a,'#34d3c0',0,m,W,H)}}"
"function draw2(a,fx,o){o=o||{};var W=cv.width=cv.clientWidth,H=cv.height;"
"cx.clearRect(0,0,W,H);if(!a||!a.length)return;"
/* база: если передали - по участку перед фронтом; иначе среднее по окну */
"var bl=o.bl;if(bl===undefined){bl=0;for(var i=0;i<a.length;i++)bl+=a[i];bl/=a.length}"
"var sig=[],smx=0,smn=0;"
"for(var i=0;i<a.length;i++){sig[i]=a[i]-bl;"
"if(sig[i]>smx)smx=sig[i];if(sig[i]<smn)smn=sig[i]}"
/* «вся шкала» - настоящий диапазон АЦП 0..4095 в выбранном отсчёте;
   иначе - по данным. При отсчёте не от базы ноль оси далеко от сигнала,
   и тянуть к нему масштаб нельзя. */
"var ym=+document.getElementById('ymax').value,ymn;"
"if(fx){ym=4095-bl;ymn=-bl}"
"else if(o.abs){var sp=Math.max(8,smx-smn);if(!ym||ym<=0)ym=Math.ceil(smx+sp*0.15);"
"ymn=Math.floor(smn-sp*0.15)}"
"else{if(!ym||ym<=0)ym=Math.max(8,Math.ceil(smx*1.2/5)*5);ymn=Math.min(-4,Math.round(smn*1.2))}"
"gridS(W,H,ymn,ym,a.length-1,o.us?'':L('отсчёты от базовой линии','samples from baseline'),o.xs?0:o.us,o.tg);"
"lineS(sig,'#34d3c0',ymn,ym,W,H,1.2);"
/* метка момента синхронизации */
"if(o.tg>=0){var xt=28+o.tg*(W-30)/Math.max(1,a.length-1);"
"cx.strokeStyle='#6ea8ff';cx.setLineDash([3,3]);cx.beginPath();"
"cx.moveTo(xt,10);cx.lineTo(xt,H-20);cx.stroke();cx.setLineDash([]);"
"cx.fillStyle='#6ea8ff';cx.fillText('▼',xt-4,10)}"
/* Трапеция на экран не выводится: она считается в приборе, а на
   картинке мало что добавляла. Масштаб отдаём линейке. */
"return {ymn:ymn,ym:ym}}"
/* Нормируем на длину окна, как это делает прошивка: иначе картинка
   и числа в браузере не совпадали бы с каналами спектра. */
/* Окно и зазор здесь - wL/wG, а не L/G: имя L занято функцией перевода
   L() (см. LJS), локальное L перекрыло бы её (проверяет check_i18n.py). */
"function trapOf(a,wL,wG){var t=[],acc=0;"
"for(var n=0;n<a.length;n++){var v=a[n],"
"vL=(n-wL>=0)?a[n-wL]:a[0],vG=(n-wG>=0)?a[n-wG]:a[0],"
"vGL=(n-wG-wL>=0)?a[n-wG-wL]:a[0];var d=v-vL-vG+vGL;"
"acc+=d;t[n]=acc/wL}return t}"
/* ИЗМЕРИТЕЛЬ ПИКА (поля "пик от"/"пик до").
   Считает центр тяжести, ширину на полувысоте и разрешение в
   процентах - это объективная замена разглядыванию формы импульса.
   Полувысота отсчитывается от ЛИНИИ ФОНА, проведённой по краям
   выделенного участка: без вычитания подложки комптоновский
   пьедестал завышал бы FWHM. */
"function peakInfo(a){window.PKMARK=null;"
"var A=+document.getElementById('pka').value|0,"
"B=+document.getElementById('pkb').value|0;"
"if(A<0)A=0;if(B>a.length-1)B=a.length-1;"
"if(!(B>A+4))return '';"
/* фон - прямая по краям участка, вычитаем её из всех каналов */
"var bg0=a[A],bg1=a[B],v=[];"
"for(var i=A;i<=B;i++)v[i-A]=a[i]-(bg0+(bg1-bg0)*(i-A)/(B-A));"
/* Сглаживание по трём каналам ТОЛЬКО для поиска высоты и полувысоты:
   одиночный шумовой выброс завышал максимум, и полувысота уезжала
   вверх - FWHM выходила заниженной (на модели 57 вместо 61). */
"var s=[];for(var i=0;i<v.length;i++){"
"var l=i>0?v[i-1]:v[i],r=i<v.length-1?v[i+1]:v[i];s[i]=(l+v[i]+r)/3}"
"var mx=-1,pk=0;for(var i=0;i<s.length;i++)if(s[i]>mx){mx=s[i];pk=i}"
"if(mx<=0)return ' &nbsp; <span style=color:#ff8a82>'+L('в этом диапазоне пика нет','no peak in this range')+'</span>';"
/* центр тяжести и площадь считаем по НЕсглаженным данным */
"var sw=0,swi=0;"
"for(var i=0;i<v.length;i++)if(v[i]>0){sw+=v[i];swi+=v[i]*(A+i)}"
"var cen=sw>0?swi/sw:A+pk;"
"var half=mx/2;"
"function cross(from,dir){var pv=s[from];"
"for(var i=from+dir;i>=0&&i<s.length;i+=dir){"
"if(s[i]<half)return A+i-dir*(half-s[i])/((pv-s[i])||1);"
"pv=s[i]}return A+(dir<0?0:s.length-1)}"
"var xl=cross(pk,-1),xr=cross(pk,1);"
"var fw=xr-xl;"
"if(!(fw>0))return ' &nbsp; <span style=color:#ff8a82>'+L('полувысота не найдена - расширьте диапазон','half maximum not found - widen the range')+'</span>';"
"var res=cen>0?100*fw/cen:0;"
"window.PKMARK=[A,B,xl,xr];"
"return '<br><b>'+L('пик:</b> центр ','peak:</b> centroid ')+cen.toFixed(1)+L(' кан',' ch')+' &nbsp; '+"
"'FWHM '+fw.toFixed(1)+L(' кан',' ch')+' &nbsp; <b>'+L('разрешение ','resolution ')+res.toFixed(2)+' %</b>'+"
"' <span style=opacity:.6>('+L('площадь за вычетом фона ','net area ')+Math.round(sw)+')</span>'}"
/* Границы участка (оранжевым) и точки полувысоты (зелёным). */
"function drawPk(nch,W,H){var m=window.PKMARK;if(!m)return;"
"cx.save();cx.setLineDash([4,3]);cx.lineWidth=1;"
"cx.strokeStyle='#f0c45a';"
"[m[0],m[1]].forEach(function(c){var x=c*W/nch;"
"cx.beginPath();cx.moveTo(x,TOPM);cx.lineTo(x,H-BOTM);cx.stroke()});"
"cx.strokeStyle='#34d3c0';"
"[m[2],m[3]].forEach(function(c){var x=c*W/nch;"
"cx.beginPath();cx.moveTo(x,TOPM);cx.lineTo(x,H-BOTM);cx.stroke()});"
"cx.restore()}"
/* ОСЦИЛЛОГРАФ. Прибор присылает окно с моментом синхронизации tg
   (-1 = свободный пуск). На экран идёт кусок длиной «развёртка»,
   момент синхронизации - на пятой части ширины, как у осциллографа. */
/* диапазон синхронизации по амплитуде строкой, '' - фильтр выключен */
"function AF(){var lo=+document.getElementById('amin').value||0,hi=+document.getElementById('amax').value||0;"
"return hi>0?Math.min(lo,hi)+'&ndash;'+Math.max(lo,hi):''}"
"function drawScope(j,wait){var a=j.d,sc=document.getElementById('sc');"
"if(!a||!a.length){cv.width=cv.clientWidth;cx.clearRect(0,0,cv.width,cv.height);"
"sc.innerHTML=wait?(AF()?L('ждущий режим: импульсов с амплитудой ','normal mode: no pulses with amplitude ')+AF()+"
"L(' кодов ещё не было',' codes yet')+(j.rej>0?L(' (отброшено ',' (rejected ')+j.rej+')':''):"
"L('ждущий режим: фронта &ge; ','normal mode: no edge &ge; ')+j.lvl+"
"L(' кодов ещё не было &mdash; снизьте «синхр. по фронту»',' codes yet &mdash; lower “trigger on edge”')):"
"L('ждём данные...','waiting for data...');return}"
"var fh=window.REALHZ||8e6,tg=j.tg;window.LASTJ=j;window.LASTW=wait;"
/* wL/wG, а не L/G: локальное L перекрыло бы функцию перевода L(), и весь
   текст под графиком пропадал (исключение глотал опрос прибора) */
"var wL=+document.getElementById('p_trap_L').value,wG=+document.getElementById('p_trap_G').value;"
"var Wn=Math.min(+document.getElementById('zm').value||512,a.length);"
/* без синхронизации пропускаем запас слева: там трапеция ещё не встала */
"var st=tg>=0?tg-Math.round(Wn*0.2):(wL|0)+(wG|0)+16;"
"if(st<0)st=0;if(st+Wn>a.length)st=a.length-Wn;"
"var tr=trapOf(a,wL,wG);"
/* база и шум - по участку ДО фронта: в окне с импульсом среднее завышено */
"var e=tg>0?Math.max(1,tg-8):a.length,bl=0,nv=0;"
"for(var i=0;i<e;i++)bl+=a[i];bl/=e;"
"for(var i=0;i<e;i++)nv+=(a[i]-bl)*(a[i]-bl);var sd=Math.sqrt(nv/e);"
/* отсчёт оси: от базы, от середины шкалы или от нуля кодов */
"var R=window.YREF|0,ref=R==0?bl:(R==1?2048:0),da=a.slice(st,st+Wn);"
"var rr=draw2(da,document.getElementById('fx').checked,{bl:ref,abs:R!=0,"
"us:1e6/fh,xs:window.XAX==1,tg:tg>=0?tg-st:-1});"
/* кадр для линейки: z - индекс момента синхронизации на экране */
"window.SCV={n:Wn,z:tg>=0?tg-st:0,W:cv.width,a:da,sg:NEG()?-1:1,us:1e6/fh,bl:bl};"
"if(rr)rulDraw(rr,ref);rulText();"
"var mn=a[0],mx=a[0];for(var i=0;i<a.length;i++){if(a[i]<mn)mn=a[i];if(a[i]>mx)mx=a[i]}"
"var sg=NEG()?-1:1,nmx=-1e9;for(var i=wL+wG;i<e;i++)if(sg*tr[i]>nmx)nmx=sg*tr[i];"
"var thr=+document.getElementById('p_threshold').value;"
"var sy=tg>=0?L('<b style=color:#34d3c0>синхронизирован</b> по фронту ','<b style=color:#34d3c0>triggered</b> on an edge of ')+j.rise+"
"L(' кодов (уровень ',' codes (level ')+j.lvl+')':"
"'<span style=color:#f0c45a>'+(AF()?L('импульсов с амплитудой ','no pulses with amplitude ')+AF()+L(' кодов',' codes'):"
"L('фронта &ge; ','no edge &ge; ')+j.lvl+L(' кодов',' codes'))+"
"L(' нет &mdash; свободный пуск',' &mdash; free run')+'</span>';"
/* возраст снимка - с поправкой на время с его прихода (по WebSocket
   старый кадр не повторяется, страница перерисовывает его сама) */
"var ag=j.age<0?-1:j.age+(j.rcv?Date.now()-j.rcv:0);"
"if(wait&&ag>1500)sy+=' &nbsp;<span style=opacity:.7>'+L('снимок ','captured ')+(ag/1000).toFixed(0)+L(' с назад',' s ago')+'</span>';"
/* высота показанного импульса и работа фильтра амплитуды */
"if(j.amp>=0)sy+=' &nbsp;'+L('высота импульса','pulse height')+' <b>'+j.amp+'</b> '+L('кодов','codes');"
"if(AF())sy+=' &nbsp;<span class=mut>'+L('фильтр ','filter ')+AF()+L(', отброшено с прошлого кадра ',', rejected since last frame ')+(j.rej||0)+'</span>';"
"sc.innerHTML=sy+"
"L('<br>на экране ','<br>on screen ')+Wn+L(' отсч = ',' smp = ')+(Wn/fh*1e6).toFixed(1)+L(' мкс из ',' µs of ')+(j.len||a.length)+L(' отсч = ',' smp = ')+"
"((j.len||a.length)/fh*1e6).toFixed(0)+L(' мкс записи',' µs recorded')+"
"(window.SFPS?L(' &nbsp; обновление ',' &nbsp; refresh ')+window.SFPS+L(' раз/с','/s'):'')+"
/* поток осциллографа и чем он идёт */
"(window.SKBPS?' &nbsp; '+L('поток ','stream ')+window.SKBPS+L(' кбит/с',' kbit/s')+' ('+(WSOK?'WebSocket':'HTTP')+')':'')+"
"L('<br>сигнал: мин ','<br>signal: min ')+mn+L('  макс ','  max ')+mx+L('  база ','  baseline ')+bl.toFixed(0)+L('  шум(СКО) ','  noise(RMS) ')+sd.toFixed(1)+"
"((mx>=4095||mn<=0)?'  <b style=color:#ff8a82>'+L('упор в шкалу АЦП','ADC scale clipped')+'</b>':'')+"
/* сколько места осталось от базы до потолка и пола шкалы */
"L('<br>запас по шкале от базы: вверх <b>','<br>headroom from baseline: up <b>')+(4095-Math.round(bl))+L('</b>, вниз <b>','</b>, down <b>')+"
"Math.round(bl)+L('</b> кодов <span class=mut>(импульсы ','</b> codes <span class=mut>(pulses ')+(NEG()?L('вниз','down'):L('вверх','up'))+')</span>'+"
"(nmx>-1e9?L('<br>шум фильтра до импульса (трапеция считается в фоне): макс ','<br>filter noise before the pulse (trapezoid computed in the background): max ')+"
"nmx.toFixed(1)+L('  &nbsp; порог ','  &nbsp; threshold ')+thr+"
"(nmx>thr?'  <b style=color:#ff8a82>'+L('шум перебивает порог','noise exceeds threshold')+'</b>':"
"'  <b style=color:#34d3c0>'+L('порог выше шума','threshold above noise')+'</b>'):'')}"
/* ЛИНЕЙКА. Метки A - начало, B - вершина, C - конец импульса. Хранятся
   в отсчётах от момента синхронизации, поэтому стоят на месте от кадра
   к кадру (без синхронизации - от левого края экрана). Щелчок ставит
   следующую метку, после третьей - начинает заново; метку можно тащить.
   B при установке прилипает к вершине рядом со щелчком: на глаз её
   легко поставить на отсчёт мимо. */
"var RUL=[null,null,null],RCOL=['#7ee081','#f0c45a','#ff8a82'],HOV=null,DRAG=-1,RAF=0;"
/* Холст общий со спектром: осциллограф рисуем, только если он открыт -
   и при вызове, и в момент кадра (вкладку могли сменить за это время).
   Без этого уход курсора с холста на «Спектре» на мгновение рисовал
   поверх спектра последний кадр осциллографа. */
"function scRedraw(){if(RAF||!window.LASTJ||!isScope())return;"
"RAF=requestAnimationFrame(function(){RAF=0;if(isScope())drawScope(window.LASTJ,window.LASTW)})}"
"function rPx(i){var g=window.SCV;return 28+i*(g.W-30)/Math.max(1,g.n-1)}"
"function rIdx(x){var g=window.SCV;"
"return Math.max(0,Math.min(g.n-1,Math.round((x-28)*(g.n-1)/(g.W-30))))}"
"function snapPk(i){var g=window.SCV,w=Math.max(3,Math.round(g.n*0.015)),b=i;"
"for(var k=Math.max(0,i-w);k<=Math.min(g.n-1,i+w);k++)if(g.sg*g.a[k]>g.sg*g.a[b])b=k;return b}"
"function isScope(){var m=document.getElementById('md').value;return m=='1'||m=='3'}"
"function rulDraw(rr,ref){var g=window.SCV,H=cv.height,sp=(rr.ym-rr.ymn)||1;"
"function Y(v){return (H-20)-((v-ref-rr.ymn)/sp)*(H-30)}"
"cx.save();cx.font='11px ui-monospace,monospace';cx.lineWidth=1;"
"for(var k=0;k<3;k++){if(RUL[k]==null)continue;var i=RUL[k]+g.z;"
"if(i<0||i>=g.n)continue;var x=rPx(i);"
"cx.strokeStyle=RCOL[k];cx.setLineDash([4,3]);cx.beginPath();cx.moveTo(x,10);"
"cx.lineTo(x,H-20);cx.stroke();cx.setLineDash([]);cx.fillStyle=RCOL[k];"
"cx.fillText('ABC'[k],x+3,22);cx.beginPath();cx.arc(x,Y(g.a[i]),3.5,0,7);cx.fill()}"
/* перекрестие под курсором с подписью */
"if(HOV!=null&&HOV<g.n){var x=rPx(HOV),r=HOV-g.z,d=Math.round(g.a[HOV]-g.bl);"
"cx.strokeStyle='rgba(230,236,226,.35)';cx.beginPath();cx.moveTo(x,10);cx.lineTo(x,H-20);cx.stroke();"
"var t=(r>0?'+':'')+r+L(' отсч · ',' smp · ')+(r*g.us).toFixed(2)+L(' мкс · ',' µs · ')+g.a[HOV]+L(' кодов (',' codes (')+"
"(d>=0?'+':'')+d+L(' от базы)',' from baseline)');"
"var tw=cx.measureText(t).width+8,tx=Math.min(x+8,g.W-tw-2);"
"cx.fillStyle='rgba(12,16,12,.88)';cx.fillRect(tx,30,tw,17);"
"cx.fillStyle='#e6ece2';cx.fillText(t,tx+4,42)}"
"cx.restore()}"
"function rulText(){var g=window.SCV,t='';"
"function ds(a,b){return (b-a)+L(' отсч (',' smp (')+((b-a)*g.us).toFixed(2)+L(' мкс)',' µs)')}"
"for(var k=0;k<3;k++)if(RUL[k]!=null)t+='ABC'[k]+' '+(RUL[k]>0?'+':'')+RUL[k]+'   ';"
"if(RUL[0]!=null&&RUL[1]!=null)t+=L('| фронт A→B ','| rise A→B ')+ds(RUL[0],RUL[1])+'   ';"
"if(RUL[1]!=null&&RUL[2]!=null)t+=L('| спад B→C ','| fall B→C ')+ds(RUL[1],RUL[2])+'   ';"
"if(RUL[0]!=null&&RUL[2]!=null)t+=L('| всего A→C ','| total A→C ')+ds(RUL[0],RUL[2])+'   ';"
"var ib=RUL[1]!=null?RUL[1]+g.z:-1;"
"if(ib>=0&&ib<g.n)t+=L('| высота B ','| height B ')+Math.round(g.sg*(g.a[ib]-g.bl))+L(' кодов от базы',' codes from baseline');"
"document.getElementById('rult').textContent=t||L('меток нет: щёлкните по графику','no marks: click on the plot');"
"document.getElementById('rset').disabled=!(RUL[0]!=null&&RUL[1]!=null&&RUL[2]!=null)}"
"function rulClr(){RUL=[null,null,null];scRedraw()}"
/* перенести измеренное в параметры интегрирования; в прибор уйдут
   кнопкой «Применить» */
"function rulSet(){if(RUL[0]==null||RUL[1]==null||RUL[2]==null)return;"
"document.getElementById('p_int_rise').value=Math.max(0,RUL[1]-RUL[0]);"
"document.getElementById('p_int_fall').value=Math.max(1,RUL[2]-RUL[1]);"
"document.getElementById('shset').checked=true;vis()}"
"cv.addEventListener('pointerdown',function(e){if(!isScope()||!window.SCV)return;"
"var g=window.SCV,x=e.offsetX,i=rIdx(x),best=-1,bd=8;"
"for(var k=0;k<3;k++)if(RUL[k]!=null){var dd=Math.abs(rPx(RUL[k]+g.z)-x);if(dd<bd){bd=dd;best=k}}"
"if(best>=0){DRAG=best;cv.setPointerCapture(e.pointerId);return}"
"var k=RUL[0]==null?0:RUL[1]==null?1:RUL[2]==null?2:0;"
"if(k==0)RUL=[null,null,null];"
"RUL[k]=(k==1?snapPk(i):i)-g.z;scRedraw()});"
"cv.addEventListener('pointermove',function(e){if(!isScope()||!window.SCV)return;"
"var i=rIdx(e.offsetX);HOV=i;if(DRAG>=0)RUL[DRAG]=i-window.SCV.z;scRedraw()});"
"cv.addEventListener('pointerup',function(){DRAG=-1});"
"cv.addEventListener('pointerleave',function(){if(DRAG<0){HOV=null;if(isScope())scRedraw()}});"
/* Вкладки режимов и видимость управления: каждому режиму - свои поля. */
/* Авто и ждущий - один осциллограф: вкладка одна, режим развёртки
   переключается сегментом. SCM помнит последний выбранный. Режим 2
   (прежние «Импульсы») для прошивки тот же набор спектра - показываем
   его как вкладку «Спектр». */
/* Галочки «настройки» и «вся шкала»: выбор помнится в браузере. */
"function vis(){try{localStorage.setItem('mca_shset',document.getElementById('shset').checked?1:0);"
"localStorage.setItem('mca_fx',document.getElementById('fx').checked?1:0);"
"localStorage.setItem('mca_shdiag',document.getElementById('shdiag').checked?1:0)}catch(e){}"
"window.LASTSIG='';showCtl();if(document.getElementById('shdiag').checked)dupd()}"
/* ось Y осциллографа: 0 - от базы, 1 - от середины шкалы, 2 - коды АЦП */
"function setRef(v){window.YREF=v;"
"for(var i=0;i<3;i++)document.getElementById('rf'+i).className=i==v?'on':'';"
"try{localStorage.setItem('mca_ref',v)}catch(e){}poll()}"
/* ось X осциллографа: 0 - микросекунды, 1 - отсчёты от синхронизации.
   Перерисует следующий кадр осциллографа - он приходит каждые 100 мс. */
"function setX(v){window.XAX=v;"
"for(var i=0;i<2;i++)document.getElementById('bx'+i).className=i==v?'on':'';"
"try{localStorage.setItem('mca_xax',v)}catch(e){}}"
"function NEG(){var e=document.getElementById('p_polarity');return !!(e&&+e.value==1)}"
"try{document.getElementById('shset').checked=localStorage.getItem('mca_shset')==='1';"
"document.getElementById('fx').checked=localStorage.getItem('mca_fx')==='1';"
"document.getElementById('shdiag').checked=localStorage.getItem('mca_shdiag')==='1';"
"window.YREF=+(localStorage.getItem('mca_ref')||0);"
"window.XAX=+(localStorage.getItem('mca_xax')||0)}catch(e){window.YREF=0;window.XAX=0}"
"for(var i=0;i<3;i++)document.getElementById('rf'+i).className=i==window.YREF?'on':'';"
"for(var i=0;i<2;i++)document.getElementById('bx'+i).className=i==window.XAX?'on':'';"
"var SCM=1;"
"function showCtl(){var m=document.getElementById('md').value,sp=(m=='0'||m=='2'),sc=!sp;"
"cv.style.cursor=sc?'crosshair':'';cv.style.touchAction=sc?'none':'';"
"var v={g_sp:sp,g_sc:sc,g_zm:sc,g_leg:sc,g_rul:sc,g_big:sp,bclr:sp,st:sp,g_mon:sp,"
/* Настройки - на обеих вкладках: на осциллографе их подбирают по
   импульсам, на «Спектре» - меняют прямо во время набора и смотрят, как
   меняется спектр (вкладки переключают режим всего прибора, поэтому
   открыть обе сразу нельзя). Галочка общая. */
"g_shs:1,g_set:document.getElementById('shset').checked,"
"g_shd:1,g_diag:document.getElementById('shdiag').checked};"
"for(var k in v){var e=document.getElementById(k);if(e)e.style.display=v[k]?'':'none'}"
"if(sp)setTimeout(mdraw,0)}"
"function hl(){var m=document.getElementById('md').value,g=(m=='3')?'1':(m=='2'?'0':m);"
"if(m=='1'||m=='3')SCM=+m;"
"var t=document.querySelectorAll('#tabs [data-md]');"
"for(var i=0;i<t.length;i++)t[i].className=t[i].getAttribute('data-md')==g?'on':'';"
"document.getElementById('bauto').className=m=='1'?'on':'';"
"document.getElementById('bwait').className=m=='3'?'on':'';"
"showCtl()}"
"function tab(v){document.getElementById('md').value=v;setmode();hl();window.LASTSIG='';poll()}"
"function scm(v){SCM=v;tab(v)}"
"function setLog(v){document.getElementById('lg').checked=!!v;"
"document.getElementById('blin').className=v?'':'on';"
"document.getElementById('blog').className=v?'on':'';poll()}"
"function hms(s){s=Math.floor(s);var h=Math.floor(s/3600),m=Math.floor(s%3600/60),c=s%60;"
"return (h<10?'0':'')+h+':'+(m<10?'0':'')+m+':'+(c<10?'0':'')+c}"
"hl();"
"function poll(){if(document.getElementById('shdiag').checked)dupd();"
"var md=document.getElementById('md').value;"
"if(md=='0'||md=='2'){mload();fetch('/spectrum').then(r=>r.json()).then(function(j){"
"draw(j.d,document.getElementById('lg').checked);"
"var a=j.d,mx=0,tot=0,pk=0;"
"for(var i=0;i<a.length;i++){tot+=a[i];if(a[i]>mx){mx=a[i];pk=i}}"
"var s=L('всего в спектре: ','total in spectrum: ')+tot+L('   максимум ','   max ')+mx+L(' в канале ',' in channel ')+pk+"
"L('   каналов: ','   channels: ')+a.length;"
"s+=peakInfo(a);"
"drawPk(a.length,cv.width,cv.height);"
"document.getElementById('sc').innerHTML=s})}"
"fetch('/stat').then(r=>r.json()).then(function(j){"
/* Строка статуса: значения через разделители,
   подписи приглушены. Время, CPS и события - ещё и крупно в панели,
   состояние набора и частота - в шапке. */
"var dt=j.samp>0?100*j.dead/j.samp:0;"
"document.getElementById('st').innerHTML="
"'<span class=ac>CPS '+j.cps+'</span><span class=sep>|</span>'+"
"'<span class=w>'+j.ev+' <span class=mut>'+L('событий','events')+'</span></span><span class=sep>|</span>'+"
"'<span title=\"'+L('зависит от Перезапуск и Поиск пика','depends on Re-arm and Peak search')+'\"><span class=mut>'+"
"L('мёртвое','dead')+'</span> '+dt.toFixed(1)+'%</span>'+"
"'<span class=sep>|</span><span><span class=mut>'+L('истинно','true')+'</span> '+"
"(j.cps/Math.max(0.01,1-dt/100)).toFixed(0)+L(' имп/с',' cps')+'</span>'+"
/* загрузка обработки: около 100% - не успевает, пойдут потерянные чанки.
   На вкладке «Осциллограф» спектр не набирается, и нагрузки нет. */
"'<span class=sep>|</span><span title=\"'+L('Какую долю ядра занимает обработка потока. '+"
"'Около 100% - не успевает, пойдут потерянные чанки: снизьте частоту.','Share of the core taken by stream processing. '+"
"'Near 100% - it cannot keep up, chunks will be lost: lower the rate.')+'\"><span class=mut>'+L('загрузка','load')+'</span> '+"
"(md=='1'||md=='3'?'—':((j.load||0)/10).toFixed(0)+'%')+'</span>'+"
/* векторная обработка не прошла самопроверку при старте - предупредить */
"(j.vec===0?'<span class=sep>|</span><span style=color:#f0c45a title=\"'+L('Векторные команды не прошли '+"
"'самопроверку при старте, обработка идёт обычным кодом - медленнее.','Vector instructions failed the self-test at start, '+"
"'processing runs plain code - slower.')+'\">'+L('векторы выкл.','vectors off')+'</span>':'');"
"document.getElementById('bt').textContent=hms(j.ms/1000);"
"document.getElementById('bc').textContent=j.cps;"
"document.getElementById('be').textContent=j.ev;"
/* часы прибора - в заголовке мониторинга */
"var mt=document.getElementById('mtime');if(mt&&j.tsrc!==undefined){var dd=new Date(j.tnow);"
"mt.textContent=L('время прибора ','device time ')+(j.tnow?p2(dd.getHours())+':'+p2(dd.getMinutes())+':'+"
"p2(dd.getSeconds()):L('не задано','not set'))+(j.tsrc==1?' (SNTP)':j.tsrc==2?L(' (от браузера)',' (from browser)'):'')}"
"if(!(window.RLOCK>Date.now())){if(j.srun!==undefined)window.SRUN=j.srun;"
"if(isScope())runChip(j.srun,1);else runChip(j.run,0)}"
"document.getElementById('fchip').textContent=(j.freq/1e6).toFixed(2)+L(' МГц',' MHz');"
/* Режим ОБЩИЙ для всего прибора, а не для вкладки: если его сменили
   из другого окна, список должен это показать, иначе страница рисует
   чужие устаревшие данные и молча не набирает спектр. */
"if(j.mode!==undefined&&!(window.MDLOCK>Date.now())){"
"var mdel=document.getElementById('md');"
"if(+mdel.value!==+j.mode){mdel.value=j.mode;hl()}}"
"window.REALHZ=j.freq;"
"if(!window.FQSYNC&&FHZ.length){window.FQSYNC=1;var bi=0,bd=1e9;"
"for(var k=0;k<FHZ.length;k++){var d=Math.abs(FHZ[k]-j.freq);"
"if(d<bd){bd=d;bi=k}}fs.value=bi}"
"if(!window.pl){window.pl=1;fetch('/cfg').then(r=>r.json()).then(function(p){"
"if(p.fl){FHZ=p.fl;fillFq()}"
"if(p.emu!==undefined){window.EMU=String(p.emu);document.getElementById('p_emu').value=window.EMU}"
"P.forEach(function(k){document.getElementById('p_'+k).value=k=='cpc'?p[k]/1000:p[k]});"
"showFields();cpcTop()})}})}"
/* ОСЦИЛЛОГРАФ опрашивается отдельно от спектра и статуса: следующий
   запрос уходит, как только отрисован предыдущий, но не чаще раза в
   SPER мс. Частота сама подстраивается под длину развёртки и скорость
   WiFi, и запросы не копятся в очередь. Просим только кусок под
   развёртку плюс запас L+G+16 слева, чтобы трапеция у края экрана
   успела установиться. Ответ двоичный: семь int32 и отсчёты по 2 байта. */
/* 50 мс - до 20 кадров в секунду: окна коротких развёрток прибор держит
   во внутренней памяти и отдаёт без копии, ответ занимает единицы мс */
"var SPER=50;"
"function sparse(b){var v=new DataView(b),n=(b.byteLength-28)>>1,d=new Array(n);"
"for(var i=0;i<n;i++)d[i]=v.getUint16(28+2*i,true);"
"return {tg:v.getInt32(0,true),rise:v.getInt32(4,true),age:v.getInt32(8,true),"
"lvl:v.getInt32(12,true),len:v.getInt32(16,true),amp:v.getInt32(20,true),"
"rej:v.getInt32(24,true),d:d}}"
/* настройки осциллографа строкой - одни и те же для /scope и WebSocket */
"function scq(){var Wn=+document.getElementById('zm').value||512,"
"M=(+document.getElementById('p_trap_L').value|0)+(+document.getElementById('p_trap_G').value|0)+16;"
"return 'lvl='+(+document.getElementById('tl').value||30)+'&n='+(Wn+M)+'&pre='+(Math.round(Wn*0.2)+M)+"
"'&amin='+(+document.getElementById('amin').value||0)+'&amax='+(+document.getElementById('amax').value||0)}"
/* Кадр WebSocket: 10 x int32 (как у /scope, плюс номер снимка, число
   отсчётов и способ записи: 0 - uint16, 1 - сжатие), затем отсчёты.
   Сжатие (scope_codec.c в прошивке): полубайты, старший первым; первый
   отсчёт - 3 полубайта целиком, дальше разность -7..7 одним полубайтом,
   полубайт 8 - следом отсчёт целиком (3 полубайта). */
"function wdec(u,n){var d=new Array(n),p=0,m=u.length*2,i=0,x=0;"
"function nb(){var b=u[p>>1],c=(p&1)?b&15:b>>4;p++;return c}"
"if(n>0&&m>=3){x=(nb()<<8)|(nb()<<4)|nb();d[i++]=x;"
"while(i<n&&p<m){var c=nb();if(c==8){if(p+3>m)break;x=(nb()<<8)|(nb()<<4)|nb()}else x+=c>7?c-16:c;d[i++]=x}}"
"d.length=i;return d}"
"function wparse(b){if(b.byteLength<40)return null;"
"var v=new DataView(b),n=v.getInt32(32,true),e=v.getInt32(36,true),d;"
"if(e==1)d=wdec(new Uint8Array(b,40),n);"
"else{n=Math.min(n,(b.byteLength-40)>>1);d=new Array(n);for(var i=0;i<n;i++)d[i]=v.getUint16(40+2*i,true)}"
"return {tg:v.getInt32(0,true),rise:v.getInt32(4,true),age:v.getInt32(8,true),lvl:v.getInt32(12,true),"
"len:v.getInt32(16,true),amp:v.getInt32(20,true),rej:v.getInt32(24,true),seq:v.getInt32(28,true),d:d}}"
/* Пришёл кадр (любым путём). Рисуем НЕ ЧАЩЕ РАЗА В SPER мс, на любой
   развёртке: большие кадры приходят по TCP пачками, и если рисовать каждый
   сразу, браузер не успевал (32768 точек на кадр), кадры копились, и
   обновление то разгонялось, то падало. Пришедший раньше срока кадр ждёт,
   а следующий его заменяет - рисуется всегда самый свежий. Ответ мог прийти
   уже на «Спектре» - тогда не рисуем. */
"var SBYTES=0,SFR=0,SPEND=null,SLASTD=0,STMR=0;"
"function sframe(j,md,nb){SBYTES+=nb;if(!isScope())return;j.rcv=Date.now();SPEND=[j,md];"
"var w=SLASTD+SPER-Date.now();if(w<=0)sdraw();else if(!STMR)STMR=setTimeout(sdraw,w)}"
"function sdraw(){STMR=0;var p=SPEND;SPEND=null;if(!p||!isScope())return;"
"SLASTD=Date.now();SFR++;drawScope(p[0],p[1]=='3')}"
/* Раз в секунду: частота обновления - сколько кадров НАРИСОВАНО за секунду
   (прежнее сглаживание мгновенных скоростей в пачках завышало цифру), и
   поток в кбит/с. В ждущем режиме по WebSocket старые кадры не
   повторяются, поэтому возраст снимка обновляем перерисовкой. */
"setInterval(function(){window.SFPS=SFR;SFR=0;window.SKBPS=Math.round(SBYTES*8/1000);SBYTES=0;"
"if(WSOK&&isScope()&&window.LASTJ&&window.LASTW)scRedraw()},1000);"
/* WEBSOCKET: прибор сам шлёт новые кадры. Настройки уходят при изменении
   и раз в 2 с (чтобы сервер не счёл соединение простаивающим). Не
   открылся или оборвался - опрос /scope, новая попытка через 3 с. */
"var WS=null,WSOK=0,WSQ='',WST=0;"
"function wsOpen(){if(!window.WebSocket)return;"
"try{WS=new WebSocket((location.protocol=='https:'?'wss://':'ws://')+location.host+'/ws')}catch(e){WS=null;return}"
"WS.binaryType='arraybuffer';"
"WS.onopen=function(){WSOK=1;WSQ=''};"
"WS.onclose=function(){WSOK=0;WS=null;setTimeout(wsOpen,3000)};"
"WS.onmessage=function(ev){if(typeof ev.data=='string')return;"
"if(window.SRUN===0&&window.LASTJ)return;"
"var j=wparse(ev.data);if(j)sframe(j,document.getElementById('md').value,ev.data.byteLength)}}"
"function sloop(){var md=document.getElementById('md').value,sc=(md=='1'||md=='3'),"
/* пауза: кадры не берём, на экране последний */
"on=sc&&!(window.SRUN===0&&window.LASTJ);"
"if(!on)SPEND=null;"
"if(WSOK){var q=scq()+'&run='+(on?1:0),t=Date.now();"
"if(q!=WSQ||t-WST>2000){try{WS.send(q);WSQ=q;WST=t}catch(e){}}"
"setTimeout(sloop,100);return}"
"if(!on){setTimeout(sloop,250);return}"
/* без WebSocket - опрос: следующий запрос, как только отрисован
   предыдущий, но не чаще раза в SPER мс */
"var t0=Date.now();"
"fetch('/scope?'+scq()).then(function(r){return r.arrayBuffer()}).then(function(b){"
"if(b.byteLength>=28)sframe(sparse(b),md,b.byteLength)})"
".catch(function(){}).then(function(){setTimeout(sloop,Math.max(10,SPER-(Date.now()-t0)))})}"
/* МОНИТОРИНГ CPS. Прибор копит односекундные отсчёты набора (кольцо 6 ч
   в PSRAM, mca_hist.c), страница раз в секунду дочитывает новые
   (/hist?from=номер) и сама складывает их в интервалы. Время отсчёта -
   по часам браузера: сейчас минус его возраст по часам прибора, так что
   для графика часы прибора не обязательны. CPS интервала - импульсы,
   делённые на время, пока реально шёл набор. */
"function p2(x){return (x<10?'0':'')+x}"
"var MH={ep:-1,nx:0,t:[],c:[],d:[],busy:0,bn:0,B:null},MV={span:0,end:0,live:0,tA:0,pw:1,x0:0,hx:null};"
"function mload(){if(MH.busy)return;MH.busy=1;"
"fetch('/hist?from='+MH.nx).then(function(r){return r.arrayBuffer()}).then(function(b){MH.busy=0;"
"if(b.byteLength<28)return;var v=new DataView(b),ep=v.getUint32(0,true),fs=v.getUint32(4,true),"
"nx=v.getUint32(8,true),nds=v.getUint32(12,true),n=v.getUint32(16,true),now=Date.now();"
"if(ep!==MH.ep){MH.ep=ep;MH.t=[];MH.c=[];MH.d=[];MV.end=0}"
"for(var i=0;i<n&&38+10*i<=b.byteLength;i++){var o=28+10*i;"
"MH.t.push(now-(nds-v.getUint32(o,true))*100);MH.c.push(v.getUint32(o+4,true));MH.d.push(v.getUint16(o+8,true))}"
"var ex=MH.t.length-21600;if(ex>0){MH.t.splice(0,ex);MH.c.splice(0,ex);MH.d.splice(0,ex)}"
"MH.nx=fs+n;if(MH.nx<nx)mload();else mdraw()}).catch(function(){MH.busy=0})}"
/* Интервалы по step мс, выровненные по часам; отдаются только законченные
   (конец не позже последнего отсчёта). T - время конца отсчёта. */
"function mbuck(T,C,D,step){var o={t:[],c:[],d:[]},k=-1,cur=NaN,n=T.length;if(!n)return o;"
"var last=T[n-1];for(var i=0;i<n;i++){var b=Math.floor(T[i]/step)*step;if(b+step>last+500)break;"
"if(b!==cur){cur=b;o.t.push(b);o.c.push(0);o.d.push(0);k++}o.c[k]+=C[i];o.d[k]+=D[i]}return o}"
"function mrel(c){return c>0?(100/Math.sqrt(c)).toFixed(2):'—'}"
"function mcard(id,v,s){document.getElementById(id).textContent=v==null?'—':v.toFixed(1);"
"document.getElementById(id+'s').textContent=s}"
"function mdraw(){var gm=document.getElementById('g_mon');if(!gm||gm.style.display=='none')return;"
"var st=Math.max(1,+document.getElementById('mint').value|0)*1000,B=mbuck(MH.t,MH.c,MH.d,st),"
"n=MH.t.length,N=B.t.length;MH.bn=N;MH.B=B;"
"var ct=0,dt=0;for(var i=0;i<n;i++){ct+=MH.c[i];dt+=MH.d[i]}"
"mcard('mc1',n&&MH.d[n-1]?MH.c[n-1]*1000/MH.d[n-1]:null,L('за последнюю секунду набора','over the last second of acquisition'));"
"mcard('mc2',dt?ct*1000/dt:null,dt?'δ '+mrel(ct)+' % · '+ct+L(' имп. за ',' counts in ')+hms(dt/1000):"
"L('история копится, пока идёт набор спектра','history accumulates while the spectrum is acquired'));"
"var lb=N-1;mcard('mc3',N&&B.d[lb]?B.c[lb]*1000/B.d[lb]:null,"
"N?'δ '+mrel(B.c[lb])+' % · '+B.c[lb]+L(' имп. за ',' counts in ')+(B.d[lb]/1000).toFixed(1)+L(' с',' s'):'');"
"mchart();"
"var h='';for(var i=N-1;i>=Math.max(0,N-100);i--){var d=new Date(B.t[i]);"
"h+='<tr><td>'+p2(d.getHours())+':'+p2(d.getMinutes())+':'+p2(d.getSeconds())+'</td><td>'+(B.d[i]/1000).toFixed(1)+"
"'</td><td>'+B.c[i]+'</td><td>'+(B.d[i]?(B.c[i]*1000/B.d[i]).toFixed(1):'—')+'</td><td>'+mrel(B.c[i])+'</td></tr>'}"
"document.getElementById('mtb').innerHTML=h}"
/* График: интервалы (с разрывом, где набора не было) и скользящее
   среднее в окне по времени (см. ниже); ось X - настоящее время. */
"function mchart(){var B=MH.B,c2=document.getElementById('mcv');if(!B||!c2)return;"
"var g=c2.getContext('2d'),W=c2.width=c2.clientWidth,H=c2.height,N=B.t.length,x0=58,x1=W-10,y0=10,y1=H-22;"
"g.clearRect(0,0,W,H);g.strokeStyle='#283024';g.lineWidth=1;g.strokeRect(x0+.5,y0+.5,x1-x0,y1-y0);"
"g.font='11px ui-monospace,monospace';g.textBaseline='middle';MV.pw=x1-x0;MV.x0=x0;"
"if(N<1){g.fillStyle='#6b756a';g.fillText(L('нет данных: история копится, пока идёт набор спектра',"
"'no data: history accumulates while the spectrum is acquired'),x0+10,y0+24);return}"
"var stp=Math.max(1,+document.getElementById('mint').value|0)*1000,"
"w=Math.min(200,Math.max(2,+document.getElementById('msma').value|0)),Y=[],S=[],a=0;"
"for(var i=0;i<N;i++){Y.push(B.d[i]>0?B.c[i]*1000/B.d[i]:0);a+=Y[i];if(i>=w)a-=Y[i-w];S.push(a/Math.min(i+1,w))}"
/* Окно ПО ВРЕМЕНИ: MV.span мс, кончая MV.end (0 - текущий момент, окно
   едет за новыми данными). Раньше по умолчанию показывалась вся история, и
   график сжимался по мере накопления. Масштаб оси X от числа точек не
   зависит, остановки набора видны пустыми местами. */
"if(!MV.span)MV.span=+document.getElementById('mwin').value||30000;"
"var live=B.t[N-1]+stp,tB=MV.end||live,tA=tB-MV.span;MV.live=live;MV.tA=tA;"
"var s=0;while(s<N&&B.t[s]<tA)s++;var e=s;while(e<N&&B.t[e]<tB)e++;"
"function X(t){return x0+(x1-x0)*(t-tA)/MV.span}"
"var mn=1e30,mx=-1e30;for(var i=s;i<e;i++){mn=Math.min(mn,Y[i],S[i]);mx=Math.max(mx,Y[i],S[i])}"
"if(e<=s){mn=0;mx=1}if(mx-mn<1e-9)mx=mn+1;var pd=(mx-mn)*0.08;mn-=pd;mx+=pd;"
"function Yp(v){return y1-(y1-y0)*(v-mn)/(mx-mn)}"
"g.textAlign='right';"
"for(var k=0;k<=4;k++){var vv=mn+(mx-mn)*k/4,yy=Yp(vv);g.strokeStyle='rgba(255,255,255,.05)';g.beginPath();"
"g.moveTo(x0,yy);g.lineTo(x1,yy);g.stroke();g.fillStyle='#6b756a';g.fillText(vv>=100?vv.toFixed(0):vv.toFixed(1),x0-6,yy)}"
/* подписи времени: круглый шаг, по местному времени браузера */
"var TS=[1,2,5,10,15,30,60,120,300,600,900,1800,3600,7200,10800,21600],ts=TS[TS.length-1]*1000;"
"for(var k=0;k<TS.length;k++)if(TS[k]*1000>=MV.span/6){ts=TS[k]*1000;break}"
"var tz=new Date().getTimezoneOffset()*60000;g.textAlign='center';g.textBaseline='top';"
"for(var t=Math.ceil((tA-tz)/ts)*ts+tz;t<=tB;t+=ts){var xx=X(t),dd=new Date(t);"
"g.strokeStyle='rgba(255,255,255,.04)';g.beginPath();g.moveTo(xx,y0);g.lineTo(xx,y1);g.stroke();g.fillStyle='#6b756a';"
"g.fillText(p2(dd.getHours())+':'+p2(dd.getMinutes())+(ts<60000?':'+p2(dd.getSeconds()):''),xx,y1+5)}"
"g.save();g.beginPath();g.rect(x0,y0,x1-x0,y1-y0);g.clip();"
"g.strokeStyle='#34d3c0';g.lineWidth=1.4;g.beginPath();"
"for(var i=s;i<e;i++){var xx=X(B.t[i]),yy=Yp(Y[i]);"
"if(i>s&&B.t[i]-B.t[i-1]<=1.5*stp)g.lineTo(xx,yy);else g.moveTo(xx,yy)}g.stroke();"
"if(e-s<=300){g.fillStyle='#34d3c0';for(var i=s;i<e;i++){g.beginPath();g.arc(X(B.t[i]),Yp(Y[i]),2,0,6.3);g.fill()}}"
"g.strokeStyle='#ff8c1a';g.lineWidth=2;g.beginPath();"
"for(var i=s;i<e;i++){var xx=X(B.t[i]),yy=Yp(S[i]);"
"if(i>s&&B.t[i]-B.t[i-1]<=1.5*stp)g.lineTo(xx,yy);else g.moveTo(xx,yy)}g.stroke();g.restore();"
/* какое окно видно и не в прошлом ли мы */
"var sw=MV.span/1000,wt=sw<120?sw.toFixed(0)+L(' с',' s'):sw<7200?(sw/60).toFixed(1)+L(' мин',' min'):(sw/3600).toFixed(1)+L(' ч',' h');"
"g.textAlign='left';g.textBaseline='middle';g.fillStyle='#6b756a';"
"g.fillText(L('окно ','window ')+wt+(MV.end?L(' · прошлое, двойной щелчок - к текущему моменту',"
"' · past, double click - back to now'):''),x0+6,y1-10);"
/* подсказка под курсором: ближайший видимый интервал */
"if(e>s&&MV.hx!=null&&MV.hx>=x0&&MV.hx<=x1){var tt=tA+(MV.hx-x0)/(x1-x0)*MV.span,bi=s;"
"for(var i=s;i<e;i++)if(Math.abs(B.t[i]-tt)<Math.abs(B.t[bi]-tt))bi=i;"
"var xx=X(B.t[bi]),dd=new Date(B.t[bi]);g.strokeStyle='rgba(230,236,226,.35)';g.lineWidth=1;g.beginPath();"
"g.moveTo(xx,y0);g.lineTo(xx,y1);g.stroke();"
"var tx=p2(dd.getHours())+':'+p2(dd.getMinutes())+':'+p2(dd.getSeconds())+'  '+Y[bi].toFixed(1)+' cps  SMA '+"
"S[bi].toFixed(1)+'  δ '+mrel(B.c[bi])+' %';"
"var tw=g.measureText(tx).width+10,bx=Math.min(xx+8,x1-tw);g.fillStyle='rgba(12,16,12,.9)';g.fillRect(bx,y0+4,tw,18);"
"g.fillStyle='#e6ece2';g.fillText(tx,bx+5,y0+13)}}"
"function mwsel(){MV.span=+document.getElementById('mwin').value||30000;MV.end=0;mchart()}"
/* колесо - масштаб по времени вокруг курсора, перетаскивание - сдвиг в
   прошлое и обратно, двойной щелчок - к окну из списка и текущему моменту */
"(function(){var c2=document.getElementById('mcv'),dr=0,dx=0,dB=0;"
"c2.addEventListener('wheel',function(ev){if(!MH.bn)return;ev.preventDefault();"
"var fr=Math.min(1,Math.max(0,(ev.offsetX-MV.x0)/MV.pw)),tc=MV.tA+fr*MV.span,"
"stp=Math.max(1,+document.getElementById('mint').value|0)*1000,"
"ns=Math.min(21600000,Math.max(Math.max(5000,3*stp),MV.span*(ev.deltaY<0?0.8:1.25))),nB=tc+(1-fr)*ns;"
"MV.span=ns;MV.end=nB>=MV.live?0:nB;mchart()},{passive:false});"
"c2.addEventListener('mousedown',function(ev){if(!MH.bn)return;dr=1;dx=ev.clientX;dB=MV.end||MV.live;"
"c2.style.cursor='grabbing'});"
"window.addEventListener('mousemove',function(ev){if(!dr)return;var B=MH.B,"
"nB=dB-(ev.clientX-dx)/MV.pw*MV.span,lo=(B&&B.t.length?B.t[0]:0)+MV.span*0.2;if(nB<lo)nB=lo;"
"MV.end=nB>=MV.live?0:nB;mchart()});"
"window.addEventListener('mouseup',function(){if(dr){dr=0;c2.style.cursor='grab'}});"
"c2.addEventListener('mousemove',function(ev){MV.hx=ev.offsetX;if(!dr)mchart()});"
"c2.addEventListener('mouseleave',function(){MV.hx=null;mchart()});"
"c2.addEventListener('dblclick',function(){mwsel()});c2.style.cursor='grab'})();"
/* выгрузка точек графика файлом */
"function mcsv(){var st=Math.max(1,+document.getElementById('mint').value|0)*1000,B=mbuck(MH.t,MH.c,MH.d,st);"
"if(!B.t.length){alert(L('Нет данных','No data'));return}"
"var s='time,dt_s,counts,cps,rel_err_pct\\r\\n';"
"for(var i=0;i<B.t.length;i++){var d=new Date(B.t[i]),z=-d.getTimezoneOffset();"
"s+=d.getFullYear()+'-'+p2(d.getMonth()+1)+'-'+p2(d.getDate())+'T'+p2(d.getHours())+':'+p2(d.getMinutes())+':'+"
"p2(d.getSeconds())+(z>=0?'+':'-')+p2(Math.floor(Math.abs(z)/60))+':'+p2(Math.abs(z)%60)+','+(B.d[i]/1000).toFixed(3)+','+"
"B.c[i]+','+(B.d[i]?(B.c[i]*1000/B.d[i]).toFixed(3):'')+','+(B.c[i]?(100/Math.sqrt(B.c[i])).toFixed(3):'')+'\\r\\n'}"
"var a=document.createElement('a'),n=new Date();a.href=URL.createObjectURL(new Blob([s],{type:'text/csv'}));"
"a.download='mca_cps_'+n.getFullYear()+p2(n.getMonth()+1)+p2(n.getDate())+'_'+p2(n.getHours())+p2(n.getMinutes())+"
"p2(n.getSeconds())+'.csv';document.body.appendChild(a);a.click();"
"setTimeout(function(){URL.revokeObjectURL(a.href);a.remove()},1000)}"
"function mclr(){if(!confirm(L('Очистить историю CPS на приборе?','Clear the CPS history on the device?')))return;"
"fetch('/hist?clear=1').then(function(){mload()})}"
/* диагностика тракта АЦП (была страницей /diag): опрос раз в секунду,
   только пока галочка включена (см. poll) */
"function dtog(){fetch('/diag/set?en='+(document.getElementById('den').checked?1:0))}"
"function df(v,c){return '<span class='+c+'>'+v+'</span>'}"
/* строка таблицы: подпись и значение */
"function dR(a,b){return '<tr><td>'+a+'</td><td>'+b+'</td></tr>'}"
"function dupd(){fetch('/diag/data').then(r=>r.json()).then(function(j){"
"document.getElementById('den').checked=j.enabled;"
"var h='<table><tr><th>'+L('Параметр','Parameter')+'</th><th>'+L('Значение','Value')+'</th></tr>';"
"h+=dR(L('Поток данных','Data flow'),j.flow?df(L('идёт','flowing'),'ok'):df(L('НЕТ ДАННЫХ','NO DATA'),'bad'));"
"h+=dR(L('Чанков всего','Chunks total'),j.chunks);"
"h+=dR(L('Отсчётов всего','Samples total'),j.samples);"
"var ec=Math.abs(j.err)<=2?'ok':(Math.abs(j.err)<=10?'warn':'bad');"
"h+=dR(L('Скорость измеренная','Measured rate'),(j.rate/1e6).toFixed(3)+L(' МГц',' MHz'));"
"h+=dR(L('Скорость ожидаемая','Expected rate'),(j.exp/1e6).toFixed(3)+L(' МГц',' MHz'));"
"h+=dR(L('Отклонение','Deviation'),df(j.err+' %',ec));"
"h+=dR(L('Потеряно чанков','Chunks lost'),j.lost>0?df(j.lost,'warn'):j.lost);"
"var tot=j.chunks+j.lost;var lp=tot?100*j.lost/tot:0;"
"h+=dR(L('Доля потерь','Loss share'),lp<0.1?df(lp.toFixed(2)+L(' % — чисто',' % — clean'),'ok'):"
"df(lp.toFixed(1)+L(' % — обработка не успевает',' % — processing cannot keep up'),'bad'));"
"if(lp>0.1){var can=j.exp*(1-lp/100);"
"h+=dR(L('Потолок обработки','Processing ceiling'),df(L('около ','about ')+(can/1e6).toFixed(1)+"
"L(' МГц — поставьте эту частоту или ниже',' MHz — set this rate or lower'),'warn'))}"
"var cs=(j.ctrl>>>29)&3;var st=(j.ctrl1>>>29)&1;var bl=j.ctrl1&0xFFFF;"
"var tb=(j.ctrl1>>>24)&1;"
"h+=dR(L('Такт модуля CAM','CAM module clock'),cs?df(L('вкл','on')+' (clk_sel='+cs+')','ok'):df(L('ВЫКЛЮЧЕН','OFF'),'bad'));"
"h+=dR('cam_start',st?df(L('запущен','started'),'ok'):df(L('НЕ УСТАНОВЛЕН','NOT SET'),'bad'));"
"h+=dR(L('16-битный режим','16-bit mode'),tb?df(L('да','yes'),'ok'):df(L('нет','no'),'bad'));"
"h+=dR(L('Длина чанка, байт','Chunk length, bytes'),bl+1);"
"h+=dR(L('Уровень на PCLK','PCLK level'),j.pclk);"
"h+='<tr><td>cam_ctrl / ctrl1</td><td>0x'+(j.ctrl>>>0).toString(16)+' / 0x'+(j.ctrl1>>>0).toString(16)+'</td></tr>';"
"h+='</table>';"
/* ПРОФИЛЬ: что было за прошлую секунду. Очередь 16 из 16 - следующий
   чанк потерян; долгие запросы - кандидаты в виновники. */
"var p=j.pf;if(p){"
"var qc=p.q>=14?'bad':(p.q>=8?'warn':'ok');"
"h+='<h4>'+L('Профиль за последнюю секунду','Profile for the last second')+'</h4><table><tr><th>'+"
"L('Что','What')+'</th><th>'+L('Значение','Value')+'</th></tr>';"
"h+=dR(L('Чанков обработано','Chunks processed'),p.ch);"
/* занятость задачи обработки: около 100% - очередь растёт, пойдут потери */
"h+=dR(L('Занятость обработки','Processing busy'),"
"df((p.busy/10).toFixed(1)+' %',p.busy>900?'bad':(p.busy>700?'warn':'ok')));"
"h+=dR(L('Пик очереди чанков','Chunk queue peak'),df(p.q+L(' из 16',' of 16'),qc)+"
"' <span style=opacity:.6>'+L('(16 - следующий теряется)','(16 - the next one is lost)')+'</span>');"
"h+=dR(L('Самая долгая обработка чанка','Longest chunk processing'),(p.work/1000).toFixed(2)+L(' мс',' ms'));"
"h+=dR(L('Самое долгое ожидание чанка','Longest chunk wait'),(p.wait/1000).toFixed(2)+L(' мс',' ms'));"
"h+=dR(L('Потеряно за секунду','Lost in the last second'),p.lost?df(p.lost,'bad'):'0');"
"var wn=['/spectrum','/scope','/stat'];"
"for(var k=0;k<3;k++)h+=dR(L('Запрос ','Request ')+wn[k],p.wcnt[k]+L(' за с, самый долгий ','/s, longest ')+"
"(p.wmax[k]/1000).toFixed(1)+L(' мс',' ms'));"
"h+=dR(L('Свободно внутр. памяти','Free internal memory'),(p.heap/1024).toFixed(0)+L(' КБ (минимум ',' KB (minimum ')+"
"(p.hmin/1024).toFixed(0)+L(' КБ)',' KB)'));"
"h+=dR(L('Сигнал WiFi','WiFi signal'),p.rssi?p.rssi+L(' дБм',' dBm'):L('нет связи с роутером','no connection to the router'));"
/* такты процессора на отсчёт по этапам против бюджета 240 МГц / частота:
   видно, хватает ли ядра на эту частоту и что в обработке дороже всего */
"if(j.cyc&&j.fhz){var bud=240e6/j.fhz,cs=(j.cyc[0]+j.cyc[1]+j.cyc[2])/100;"
"h+=dR(L('Такты на отсчёт (спектр)','CPU cycles per sample (spectrum)'),cs>0?"
"L('копия ','copy ')+(j.cyc[0]/100).toFixed(2)+L(' + разность ',' + difference ')+(j.cyc[1]/100).toFixed(2)+"
"L(' + порог и события ',' + threshold and events ')+(j.cyc[2]/100).toFixed(2)+' = '+"
"df(cs.toFixed(2),cs>bud*0.9?'bad':(cs>bud*0.75?'warn':'ok'))+L(' из ',' of ')+bud.toFixed(1):"
"L('спектр сейчас не обрабатывается','the spectrum is not being processed now'))}"
"h+='</table>';"
/* Журнал потерь: что делал веб в момент потери чанка. Если потери
   совпадают с запросами - виноват веб, если нет - обработка. */
"var wn2=[L('нет','none'),'/spectrum','/scope','/stat'];"
"if(j.ev&&j.ev.length){"
"h+='<h4>'+L('Журнал потерь чанков','Chunk loss log')+'</h4><table><tr><th>'+L('Время','Time')+'</th><th>'+"
"L('Потеряно','Lost')+'</th><th>'+L('Очередь','Queue')+'</th><th>'+L('Шёл запрос','Request in progress')+'</th><th>'+"
"L('Длился','Took')+'</th><th>'+L('Обработка','Processing')+'</th><th>'+L('Режим','Mode')+'</th></tr>';"
"j.ev.forEach(function(e){"
"h+='<tr><td>'+(e[0]/1000).toFixed(1)+L(' с',' s')+'</td><td>'+e[1]+'</td><td>'+e[2]+'</td>'+"
"'<td>'+(wn2[e[3]]||'?')+'</td><td>'+(e[4]/1000).toFixed(1)+L(' мс',' ms')+'</td><td>'+"
"(e[5]/1000).toFixed(2)+L(' мс',' ms')+'</td><td>'+"
"(e[6]==1||e[6]==3?L('осциллограф','scope'):L('спектр','spectrum'))+'</td></tr>'});"
"h+='</table>'}"
"else h+='<p style=opacity:.6>'+L('Потерь чанков не было.','No chunk losses.')+'</p>';"
"}"
"if(j.stats){"
"h+='<table><tr><th>'+L('Бит','Bit')+'</th><th>'+L('Переключений','Toggles')+'</th><th>'+L('Состояние','State')+'</th></tr>';"
"var nm=['D0','D1','D2','D3','D4','D5','D6','D7','D8','D9','D10','D11','OTR'];"
"for(var b=0;b<13;b++){var st='',c='ok';"
"if(j.a1&(1<<b)){st=L('всегда 1','always 1');c='bad'}"
"else if(j.a0&(1<<b)){st=L('всегда 0','always 0');c='bad'}"
"else st=L('меняется','toggling');"
"h+='<tr><td>'+nm[b]+'</td><td>'+j.tg[b]+'</td><td>'+df(st,c)+'</td></tr>'}"
"h+='</table>';"
"h+='<table><tr><th>'+L('Значения в чанке','Values in the chunk')+'</th><th></th></tr>';"
"h+=dR('min / max',j.vmin+' / '+j.vmax);"
"h+=dR(L('среднее','mean'),j.vmean);"
"h+=dR(L('размах','range'),j.vmax-j.vmin);"
"h+=dR(L('OTR (переполнение)','OTR (overflow)'),j.otr>0?df(j.otr,'warn'):j.otr);"
"h+=dR(L('проанализировано','analysed'),j.an+L(' отсчётов',' samples'));"
"h+='</table>'}"
"else h+='<p style=opacity:.6>'+L('Побитовая статистика выключена.','Per-bit statistics are off.')+'</p>';"
"document.getElementById('dout').innerHTML=h})}"
"function dprobe(){document.getElementById('dpr').textContent=L('измеряю 4 с...','measuring 4 s...');"
"fetch('/diag/probe').then(r=>r.json()).then(function(j){"
"var nm=['D0','D1','D2','D3','D4','D5','D6','D7','D8','D9','D10','D11','OTR'];"
"var h='<table><tr><th>'+L('Линия','Line')+'</th><th>'+L('Состояние','State')+'</th></tr>';var live=0;"
"for(var b=0;b<13;b++){var st,c;"
"if(j.ch&(1<<b)){st=L('меняется','toggling');c='ok';live++}"
"else if(j.hi&(1<<b)){st=L('залипла в 1','stuck at 1');c='bad'}"
"else st=L('залипла в 0','stuck at 0'),c='bad';"
"h+='<tr><td>'+nm[b]+'</td><td>'+df(st,c)+'</td></tr>'}h+='</table>';"
"h+=live?df(L('Активных линий: ','Active lines: ')+live+L(' — АЦП преобразует',' — the ADC is converting'),'ok'):"
"df(L('НИ ОДНА линия не шевелится — АЦП не преобразует. Смотрите питание платы и приходит ли на неё такт.',"
"'NOT A SINGLE line is changing — the ADC is not converting. Check the board power and whether the clock reaches it.'),'bad');"
"document.getElementById('dpr').innerHTML=h})}"
"setInterval(poll,1000);poll();wsOpen();sloop();"
"</script></body></html>";

/* ---------------- обработчики ---------------- */
static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_spectrum(httpd_req_t *r)
{
    mca_prof_web_begin(PROF_EP_SPEC);
    /* Числа собираются в буфер и отправляются крупными кусками.
     * Раньше на каждый канал приходился отдельный вызов отправки -
     * четыре тысячи записей в сокет на один запрос, и это заметно
     * отъедало процессор, мешая обработке потока. */
    static uint32_t val[512];
    static char     out[2048];
    size_t pos = 0;

    /* Показываем первые nch каналов - это длина шкалы. Прибор копит все
     * MCA_CHANNELS, поэтому переключение числа каналов ничего не теряет
     * и не сбрасывает: меняется только, докуда видно вправо. */
    mca_params_t p;
    mca_dsp_get_params(&p);
    size_t nch = (size_t)p.nch;
    if (nch > MCA_CHANNELS) nch = MCA_CHANNELS;

    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr_chunk(r, "{\"d\":[");

    bool first = true;
    for (size_t off = 0; off < nch; off += 512) {
        size_t want = nch - off < 512 ? nch - off : 512;
        size_t got  = mca_dsp_get_spectrum(val, off, want);
        for (size_t i = 0; i < got; i++) {
            if (pos > sizeof(out) - 16) {
                httpd_resp_send_chunk(r, out, pos);
                pos = 0;
            }
            pos += snprintf(out + pos, sizeof(out) - pos, "%s%lu",
                            first ? "" : ",", (unsigned long)val[i]);
            first = false;
        }
    }
    if (pos) httpd_resp_send_chunk(r, out, pos);

    httpd_resp_sendstr_chunk(r, "]}");
    esp_err_t e = httpd_resp_send_chunk(r, NULL, 0);
    mca_prof_web_end(PROF_EP_SPEC);
    return e;
}

/* Настройки осциллографа из строки запроса - одни и те же для /scope и
 * для WebSocket: lvl - уровень синхронизации, n - сколько отсчётов нужно
 * странице, pre - сколько из них до момента синхронизации, amin/amax -
 * синхронизация по амплитуде (amax=0 - выкл). */
static void scope_apply(const char *q, size_t *want, size_t *pre)
{
    char v[12];
    if (httpd_query_key_value(q, "lvl", v, sizeof(v)) == ESP_OK)
        mca_diag_set_trig_level(atoi(v));
    if (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
        int x = atoi(v);
        if (x > 0 && x <= DIAG_SCOPE_LEN) *want = (size_t)x;
    }
    if (httpd_query_key_value(q, "pre", v, sizeof(v)) == ESP_OK) {
        int x = atoi(v);
        if (x >= 0) *pre = (size_t)x;
    }
    int32_t lo = 0, hi = 0;
    if (httpd_query_key_value(q, "amin", v, sizeof(v)) == ESP_OK)
        lo = atoi(v);
    if (httpd_query_key_value(q, "amax", v, sizeof(v)) == ESP_OK)
        hi = atoi(v);
    mca_diag_set_amp_window(lo, hi);
}

static esp_err_t h_scope(httpd_req_t *r)
{
    /* Осциллограмма с синхронизацией по фронту. lvl в запросе -
     * уровень синхронизации, коды АЦП. Режим (авто / ждущий) задаётся
     * выбором режима прибора, а не этим запросом. */
    mca_prof_web_begin(PROF_EP_SCOPE);
    /* n - сколько отсчётов нужно странице, pre - сколько из них до
     * момента синхронизации. Отдаём только кусок под развёртку: время
     * ответа растёт с его длиной (см. LWIP_TCP_SND_BUF_DEFAULT). */
    char q[128];
    size_t want = DIAG_SCOPE_LEN, pre = DIAG_SCOPE_PRE;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK)
        scope_apply(q, &want, &pre);
    /* прибор соберёт окно ровно под эту развёртку */
    mca_diag_set_scope_want(want, pre);

    /* Ответ двоичный, little-endian: семь int32 (tg, rise, age, lvl,
     * длина всего окна, высота импульса, отброшено фильтром амплитуды),
     * затем отсчёты по uint16. Втрое короче JSON, и прибору не нужно
     * форматировать десятки тысяч чисел через snprintf - от этого и
     * зависит, сколько раз в секунду обновляется картинка.
     *
     * Отсчёты уходят в сеть ПРЯМО ИЗ БУФЕРА СНИМКА, без копии. Раньше они
     * копировались в отдельный буфер 64 КБ в PSRAM, а снимок тоже лежал в
     * PSRAM: веб гонял кэш PSRAM, пока задача обработки писала туда
     * следующее окно, и на 20 МГц терялись чанки. Маска не нужна: в
     * камерный интерфейс заведены только 12 бит данных (см. adc_cap.c).
     * Пока ответ уходит, снимок занят - новые кадры не публикуются. */
    int32_t tg = -1, rise = 0, age = -1, amp = -1;
    uint32_t rej = 0;
    size_t full = 0;
    const uint16_t *d = NULL;
    size_t n = mca_diag_scope_acquire(want, pre, &d, &tg, &rise, &age, &full,
                                      &amp, &rej, NULL);

    const int32_t h[7] = { tg, rise, age, mca_diag_get_trig_level(),
                           (int32_t)full, amp, (int32_t)rej };
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send_chunk(r, (const char *)h, sizeof(h));
    if (e == ESP_OK && n)
        e = httpd_resp_send_chunk(r, (const char *)d, n * sizeof(uint16_t));
    mca_diag_scope_release();
    if (e == ESP_OK) e = httpd_resp_send_chunk(r, NULL, 0);
    mca_prof_web_end(PROF_EP_SCOPE);
    return e;
}

static esp_err_t h_stat(httpd_req_t *r)
{
    mca_prof_web_begin(PROF_EP_STAT);
    mca_stats_t s;
    mca_dsp_get_stats(&s);
    char buf[600];
    int n = snprintf(buf, sizeof(buf),
        "{\"ev\":%llu,\"cps\":%lu,\"pile\":%llu,\"dead\":%llu,"
        "\"base\":%ld,\"lost\":%llu,\"over\":%llu,"
        "\"samp\":%llu,\"freq\":%lu,"
        "\"run\":%d,\"ms\":%lu,\"mode\":%d,\"load\":%lu,\"vec\":%d,"
        "\"srun\":%d,\"cap\":%d,\"tnow\":%lld,\"tsrc\":%d}",
        (unsigned long long)s.total_events, (unsigned long)s.cps,
        (unsigned long long)s.skipped_pileup,
        (unsigned long long)s.skipped_deadtime,
        (long)s.baseline, (unsigned long long)s.chunks_lost,
        (unsigned long long)s.overflow,
        (unsigned long long)s.samples_processed,
        (unsigned long)adc_clk_get_freq(),
        /* run - набор спектра, srun - осциллограф, cap - сам захват АЦП */
        mca_spec_run ? 1 : 0, (unsigned long)s.run_ms,
        (int)mca_mode, (unsigned long)s.load_pm, mca_dsp_vec_ok() ? 1 : 0,
        mca_scope_run ? 1 : 0, adc_cap_is_running() ? 1 : 0,
        /* часы прибора: мс Unix-времени (0 - не заданы) и откуда время */
        (long long)mca_time_now_ms(), (int)mca_time_source());
    httpd_resp_set_type(r, "application/json");
    /* snprintf возвращает длину, которая ПОТРЕБОВАЛАСЬ БЫ. При нехватке
     * места это больше размера буфера, и отправка читала бы за его
     * границей. Ограничиваем. */
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    esp_err_t e = httpd_resp_send(r, buf, n);
    mca_prof_web_end(PROF_EP_STAT);
    return e;
}

static bool q_int(const char *q, const char *key, int32_t *out)
{
    char v[16];
    if (httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK) {
        *out = atoi(v);
        return true;
    }
    return false;
}

static esp_err_t h_cfg(httpd_req_t *r)
{
    char q[1024];
    mca_params_t p, before;
    mca_dsp_get_params(&p);
    before = p;

    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        int32_t v;
        if (q_int(q, "mode", &v) && v >= 0 && v <= MCA_MODE_MAX)
            mca_mode = (mca_mode_t)v;
        if (q_int(q, "freq", &v)) mca_cmd_freq_idx = v;
        /* эмуляция MCA на COM: скорость порта или 0 - выключить */
        if (q_int(q, "emu", &v)) mca_emu_set((uint32_t)v);

        q_int(q, "threshold",       &p.threshold);
        q_int(q, "cpc",             &p.cpc_milli);
        q_int(q, "nch",             &p.nch);
        q_int(q, "algo",            &p.algo);
        q_int(q, "polarity",        &p.polarity);
        q_int(q, "int_rise",        &p.int_rise);
        q_int(q, "int_fall",        &p.int_fall);
        q_int(q, "hysteresis",      &p.hysteresis);
        q_int(q, "trap_L",          &p.trap_L);
        q_int(q, "trap_G",          &p.trap_G);
        q_int(q, "rearm",           &p.rearm);
        q_int(q, "search",          &p.search);
        q_int(q, "baseline_shift",  &p.baseline_shift);
        q_int(q, "baseline_win",    &p.baseline_win);
        q_int(q, "flat_avg",        &p.flat_avg);
        q_int(q, "pileup_pre_pct",  &p.pileup_pre_pct);
        q_int(q, "pileup_post_pct", &p.pileup_post_pct);
        mca_dsp_set_params(&p);
        mca_dsp_get_params(&p);

        /* В память прибора - только если настройки реально поменялись,
         * чтобы запросы режима и частоты не изнашивали флеш. */
        if (memcmp(&before, &p, sizeof(p)) != 0) mca_settings_save_params(&p);
    }

    char buf[900];
    int n = snprintf(buf, sizeof(buf),
        "{\"threshold\":%ld,\"cpc\":%ld,\"nch\":%ld,"
        "\"algo\":%ld,\"polarity\":%ld,\"int_rise\":%ld,\"int_fall\":%ld,"
        "\"hysteresis\":%ld,\"trap_L\":%ld,"
        "\"trap_G\":%ld,\"rearm\":%ld,\"search\":%ld,"
        "\"baseline_shift\":%ld,\"baseline_win\":%ld,"
        "\"flat_avg\":%ld,"
        "\"pileup_pre_pct\":%ld,"
        "\"pileup_post_pct\":%ld,\"emu\":%lu,\"fl\":[",
        (long)p.threshold, (long)p.cpc_milli, (long)p.nch,
        (long)p.algo, (long)p.polarity, (long)p.int_rise, (long)p.int_fall,
        (long)p.hysteresis, (long)p.trap_L,
        (long)p.trap_G, (long)p.rearm, (long)p.search,
        (long)p.baseline_shift, (long)p.baseline_win,
        (long)p.flat_avg,
        (long)p.pileup_pre_pct,
        (long)p.pileup_post_pct, (unsigned long)mca_emu_baud());
    /* snprintf возвращает длину, которая ПОТРЕБОВАЛАСЬ БЫ. При нехватке
     * места это больше размера буфера, и отправка читала бы за его
     * границей. Ограничиваем после каждого шага. */
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    /* Список частот: он свой у каждого генератора CLK (см. adc_clk.c),
     * и страница строит выбор по нему, а не по своей копии. */
    for (int i = 0; i < MCA_FREQ_COUNT; i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%s%lu", i ? "," : "",
                      (unsigned long)mca_freq_table[i]);
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}");
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_cmd(httpd_req_t *r)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "do", v, sizeof(v)) == ESP_OK) {
        if      (!strcmp(v, "start"))       mca_spec_run  = true;
        else if (!strcmp(v, "stop"))        mca_spec_run  = false;
        else if (!strcmp(v, "scope_start")) mca_scope_run = true;
        else if (!strcmp(v, "scope_stop"))  mca_scope_run = false;
        else if (!strcmp(v, "clear"))       mca_cmd_clear = true;
    }
    return httpd_resp_sendstr(r, "ok");
}

/* /time - часы прибора: {"now": мс Unix-времени (0 - не заданы),
 * "src": 0 - нет, 1 - SNTP, 2 - от браузера}; /time?set=<мс> - время от
 * браузера (принимается, если оно не от SNTP, см. mca_time.h). */
static esp_err_t h_time(httpd_req_t *r)
{
    char q[48], v[24];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "set", v, sizeof(v)) == ESP_OK)
        mca_time_set_ms(strtoll(v, NULL, 10));
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"now\":%lld,\"src\":%d}",
                     (long long)mca_time_now_ms(), (int)mca_time_source());
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_send(r, buf, n);
}

/* /hist?from=N - история CPS (mca_hist.h), двоичная, little-endian:
 * 7 x uint32 (эпоха, номер первого отданного отсчёта, номер следующего,
 * время прибора с включения в 0.1 с, сколько отдано, длина кольца,
 * резерв), затем отсчёты по 10 байт. Не больше 1024 за раз - страница
 * дочитывает, пока не догонит. /hist?clear=1 - очистить историю. */
static esp_err_t h_hist(httpd_req_t *r)
{
    char q[48], v[16];
    uint32_t from = 0;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "clear", v, sizeof(v)) == ESP_OK)
            mca_hist_clear();
        if (httpd_query_key_value(q, "from", v, sizeof(v)) == ESP_OK)
            from = (uint32_t)strtoul(v, NULL, 10);
    }
    /* веб-сервер обрабатывает запросы по одному - общий буфер безопасен */
    static mca_hist_sample_t buf[1024];
    uint32_t first = 0, next = 0, epoch = 0;
    const size_t n = mca_hist_copy(from, buf, 1024, &first, &next, &epoch);
    const uint32_t h[7] = { epoch, first, next,
                            (uint32_t)(esp_timer_get_time() / 100000),
                            (uint32_t)n, MCA_HIST_LEN, 0 };
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send_chunk(r, (const char *)h, sizeof(h));
    if (e == ESP_OK && n)
        e = httpd_resp_send_chunk(r, (const char *)buf,
                                  n * sizeof(mca_hist_sample_t));
    if (e == ESP_OK) e = httpd_resp_send_chunk(r, NULL, 0);
    return e;
}

/* ---------------- ОСЦИЛЛОГРАФ ПО WEBSOCKET ----------------
 * Страница открывает /ws и присылает туда текстом те же настройки, что
 * и в /scope, плюс run=0|1 (открыта ли вкладка осциллографа и не на паузе
 * ли он). Прибор сам шлёт кадр, как только снимок НОВЫЙ и предыдущий
 * кадр ушёл, не чаще 20 раз в секунду. Раньше страница опрашивала /scope
 * и получала снимок, даже если он не менялся: в ждущем режиме по сети
 * гонялся один и тот же кадр до 66 КБ.
 *
 * Кадр - одно двоичное сообщение: 10 x int32 (tg, rise, age, lvl, длина
 * окна, высота импульса, отброшено фильтром, номер снимка, число
 * отсчётов, способ записи: 0 - uint16, 1 - сжатие scope_codec), затем
 * отсчёты. Сжатие без потерь уменьшает кадр в 3.5-4 раза. Сообщение
 * режется на куски по 4 КБ (фрагменты WebSocket) - буфер во внутренней
 * памяти небольшой, а сжатие идёт на лету.
 *
 * Отправка - в задаче веб-сервера (httpd_queue_work): так она не
 * пересекается с его собственными служебными кадрами на том же сокете.
 * Если WebSocket не открылся, страница опрашивает /scope, как раньше. */
#define WS_HDR      40          /* 10 x int32                          */
#define WS_MIN_US   50000       /* не чаще 20 кадров в секунду          */
#define WS_ENC_RAW  0
#define WS_ENC_NIB  1

static httpd_handle_t    s_srv;
static volatile int      s_ws_fd = -1;      /* клиент осциллографа        */
static volatile bool     s_ws_run;          /* ему нужны кадры            */
static volatile bool     s_ws_busy;         /* кадр в очереди или уходит  */
static volatile bool     s_ws_force;        /* отдать снимок, даже старый */
static volatile size_t   s_ws_want = DIAG_SCOPE_LEN;
static volatile size_t   s_ws_pre  = DIAG_SCOPE_PRE;
static uint32_t          s_ws_seq;          /* номер отправленного снимка */
static bool              s_codec_ok;
static uint8_t           s_ws_out[4096];    /* кусок сообщения            */

static void ws_send_scope(void *arg)
{
    const int fd = s_ws_fd;
    if (fd < 0 ||
        httpd_ws_get_fd_info(s_srv, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        if (fd == s_ws_fd) s_ws_fd = -1;
        s_ws_busy = false;
        return;
    }
    mca_prof_web_begin(PROF_EP_SCOPE);

    int32_t tg = -1, rise = 0, age = -1, amp = -1;
    uint32_t rej = 0, seq = 0;
    size_t full = 0;
    const uint16_t *d = NULL;
    const size_t n = mca_diag_scope_acquire(s_ws_want, s_ws_pre, &d, &tg,
                                            &rise, &age, &full, &amp, &rej,
                                            &seq);
    const bool force = s_ws_force;
    s_ws_force = false;

    esp_err_t e = ESP_OK;
    if (seq != s_ws_seq || force) {
        const int32_t enc = s_codec_ok ? WS_ENC_NIB : WS_ENC_RAW;
        const int32_t h[10] = { tg, rise, age, mca_diag_get_trig_level(),
                                (int32_t)full, amp, (int32_t)rej,
                                (int32_t)seq, (int32_t)n, enc };
        memcpy(s_ws_out, h, WS_HDR);

        scodec_t cs = { 0 };
        size_t off = WS_HDR, pos = 0;
        bool first = true;
        for (;;) {
            size_t used = 0;
            if (n && enc == WS_ENC_NIB) {
                pos += scodec_encode(&cs, d + pos, n - pos, s_ws_out + off,
                                     sizeof(s_ws_out) - off, &used);
                if (pos >= n) used += scodec_finish(&cs, s_ws_out + off + used);
            } else if (n) {
                size_t k = (sizeof(s_ws_out) - off) / sizeof(uint16_t);
                if (k > n - pos) k = n - pos;
                memcpy(s_ws_out + off, d + pos, k * sizeof(uint16_t));
                pos  += k;
                used  = k * sizeof(uint16_t);
            }
            const bool last = pos >= n;
            httpd_ws_frame_t fr = {
                .final      = last,
                .fragmented = true,
                .type       = first ? HTTPD_WS_TYPE_BINARY
                                    : HTTPD_WS_TYPE_CONTINUE,
                .payload    = s_ws_out,
                .len        = off + used,
            };
            e = httpd_ws_send_frame_async(s_srv, fd, &fr);
            first = false;
            off   = 0;
            if (e != ESP_OK || last) break;
        }
        if (e == ESP_OK) s_ws_seq = seq;
    }
    mca_diag_scope_release();

    if (e != ESP_OK && fd == s_ws_fd) s_ws_fd = -1;   /* клиент ушёл */
    mca_prof_web_end(PROF_EP_SCOPE);
    s_ws_busy = false;
}

/* Раз в 10 мс: есть ли кому и что отправить. Сам кадр собирает и шлёт
 * задача веб-сервера. */
static void ws_task(void *arg)
{
    int64_t last = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (s_ws_fd < 0 || !s_ws_run || s_ws_busy) continue;
        if (!s_ws_force && mca_diag_scope_seq() == s_ws_seq) continue;
        const int64_t now = esp_timer_get_time();
        if (now - last < WS_MIN_US) continue;
        last = now;
        s_ws_busy = true;
        if (httpd_queue_work(s_srv, ws_send_scope, NULL) != ESP_OK)
            s_ws_busy = false;
    }
}

static esp_err_t h_ws(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {            /* рукопожатие */
        s_ws_fd  = httpd_req_to_sockfd(r);
        s_ws_run = false;                   /* до первых настроек */
        return ESP_OK;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t e = httpd_ws_recv_frame(r, &f, 0);
    if (e != ESP_OK) return e;
    char q[160];
    if (f.len >= sizeof(q)) return ESP_FAIL;    /* не наше - закрыть */
    f.payload = (uint8_t *)q;
    e = httpd_ws_recv_frame(r, &f, sizeof(q) - 1);
    if (e != ESP_OK) return e;
    q[f.len] = 0;
    if (f.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    size_t want = DIAG_SCOPE_LEN, pre = DIAG_SCOPE_PRE;
    scope_apply(q, &want, &pre);
    s_ws_want = want;
    s_ws_pre  = pre;
    mca_diag_set_scope_want(want, pre);
    int32_t run = 0;
    if (q_int(q, "run", &run)) s_ws_run = run != 0;
    /* кадры получает последний заговоривший клиент; новые настройки -
     * сразу кадр, не дожидаясь следующего снимка */
    s_ws_fd    = httpd_req_to_sockfd(r);
    s_ws_force = true;
    return ESP_OK;
}

/* ---------------- WiFi: постоянный хотспот для настройки ----------------
 * ESP всегда поднимает свою точку доступа (AP) и одновременно пытается
 * подключиться клиентом (STA) к сохранённым в NVS учётным данным.
 * Так можно в любой момент подключиться к прибору напрямую и сменить
 * сеть, не перепрошивая и не трогая Kconfig.
 */
#define MCA_AP_SSID "MCA-Setup"
#define MCA_NVS_NS  "mca_wifi"

static char s_sta_ssid[33];
static char s_sta_pass[65];

static void wifi_creds_load(void)
{
    nvs_handle_t h;
    size_t len;
    bool has = false;

    if (nvs_open(MCA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        len = sizeof(s_sta_ssid);
        if (nvs_get_str(h, "ssid", s_sta_ssid, &len) == ESP_OK && len > 1)
            has = true;
        len = sizeof(s_sta_pass);
        nvs_get_str(h, "pass", s_sta_pass, &len);
        nvs_close(h);
    }
    if (!has) {
        /* первая прошивка - берём дефолт из Kconfig */
        strncpy(s_sta_ssid, CONFIG_MCA_WIFI_SSID, sizeof(s_sta_ssid) - 1);
        strncpy(s_sta_pass, CONFIG_MCA_WIFI_PASS, sizeof(s_sta_pass) - 1);
    }
}

static esp_err_t wifi_creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MCA_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ВЫКЛЮЧЕНИЕ WIFI (ключ "off" в той же области NVS).
 *
 * Прибор управляется только по сети, поэтому WiFi нельзя выключить так,
 * чтобы к нему стало не подключиться:
 *  - со страницы - только когда у Ethernet уже есть адрес;
 *  - при старте с сохранённым «выключено» и нет модуля W5500 - WiFi
 *    включается сразу;
 *  - пока WiFi выключен, сторож следит за адресом Ethernet: 30 с без
 *    адреса (при старте или позже) - WiFi включается (wifi_guard_task).
 *    Сама настройка при этом остаётся: на следующем старте прибор снова
 *    попробует работать только по Ethernet.
 * Без WiFi пропадает и помеха от гармоник CLK АЦП в эфире (см. README). */
static bool          s_wifi_off_cfg;     /* сохранено «выключено»      */
static volatile bool s_wifi_on;          /* радио сейчас работает      */
static bool          s_wifi_inited;

static void wifi_off_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(MCA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "off", &v);
        nvs_close(h);
    }
    s_wifi_off_cfg = v != 0;
}

static esp_err_t wifi_off_save(bool off)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MCA_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "off", off ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_wifi_off_cfg = off;
    return err;
}

/* ПОВТОРНЫЕ ПОПЫТКИ ПОДКЛЮЧЕНИЯ К РОУТЕРУ - ОТЛОЖЕННО И РЕДКО.
 *
 * Было: vTaskDelay(2000) прямо в обработчике событий и новая попытка
 * каждые две секунды. Первое блокировало задачу событий WiFi на две
 * секунды - через неё идут все события WiFi и IP. Второе хуже: точка
 * доступа и клиент делят ОДНО радио, и каждая попытка подключения
 * это сканирование каналов, на время которого радио уходит с канала
 * точки доступа. У клиента, подключённого к хотспоту прибора,
 * страница замирала волнами примерно по две секунды - ровно то, что
 * наблюдалось на приборе.
 *
 * Теперь попытка ставится одноразовым таймером (обработчик событий не
 * блокируется), первые попытки раз в 10 секунд, дальше раз в минуту,
 * а если сеть не задана - клиентская часть в эфир не выходит. */
static esp_timer_handle_t s_sta_timer;
static int  s_sta_tries;

static void sta_try_cb(void *arg)
{
    if (s_wifi_on) esp_wifi_connect();
}

static void sta_retry_later(void)
{
    /* сети нет или WiFi выключен - не лезем в эфир */
    if (!s_wifi_on || !s_sta_ssid[0] || !s_sta_timer) return;
    const int sec = s_sta_tries < 3 ? 10 : 60;
    if (s_sta_tries < 1000) s_sta_tries++;
    if (s_sta_tries <= 4)
        ESP_LOGW(TAG, "STA не подключилась, следующая попытка через %d с",
                 sec);
    esp_timer_stop(s_sta_timer);
    esp_timer_start_once(s_sta_timer, (uint64_t)sec * 1000000ULL);
}

static void wifi_ev(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        sta_retry_later();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_sta_tries = 0;
        /* Канал роутера нужен, чтобы проверять, не попадает ли гармоника
         * CLK АЦП в полосу WiFi (канал N: 2407+5N МГц, ширина 20 МГц). */
        wifi_ap_record_t ap;
        const int ch = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                     ? ap.primary : -1;
        ESP_LOGI(TAG, "STA подключена: http://" IPSTR "/ канал %d (%d МГц)",
                 IP2STR(&e->ip_info.ip), ch, ch > 0 ? 2407 + 5 * ch : 0);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "к хотспоту '%s' подключился клиент - "
                      "настройка на http://192.168.4.1/wifi", MCA_AP_SSID);
    }
}

static void wifi_apply_sta_config(void)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, s_sta_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_sta_pass, sizeof(wc.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

/* Создать WiFi (один раз) и запустить радио. */
static void wifi_bring_up(void)
{
    if (s_wifi_on) return;
    if (!s_wifi_inited) {
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&cfg);
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_ev, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_ev, NULL, NULL);

        const esp_timer_create_args_t ta = {
            .callback = sta_try_cb,
            .name     = "sta_try",
        };
        esp_timer_create(&ta, &s_sta_timer);
        s_wifi_inited = true;
    }
    s_wifi_on   = true;
    s_sta_tries = 0;

    /* AP: открытый (без пароля) - прибор в лаборатории, не в поле.
     * Если нужен пароль - раскомментировать .authmode и .password ниже. */
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, MCA_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(MCA_AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.channel        = 1;

    /* Сеть роутера не задана - поднимаем ТОЛЬКО точку доступа. В режиме
     * «точка доступа + клиент» клиентская часть сканировала бы каналы,
     * а радио одно: связь с хотспотом прерывалась бы волнами. */
    const bool sta_on = s_sta_ssid[0] != 0;
    esp_wifi_set_mode(sta_on ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (sta_on) wifi_apply_sta_config();
    esp_wifi_set_ps(WIFI_PS_NONE);      /* без сна: питание от сети */
    esp_wifi_start();

    if (sta_on)
        ESP_LOGI(TAG, "хотспот '%s' поднят (192.168.4.1), "
                      "STA пытается подключиться к '%s'",
                 MCA_AP_SSID, s_sta_ssid);
    else
        ESP_LOGI(TAG, "хотспот '%s' поднят (192.168.4.1); сеть роутера "
                      "не задана - клиент выключен, эфир не сканируется",
                 MCA_AP_SSID);
}

static void wifi_shut_down(void)
{
    if (!s_wifi_on) return;
    s_wifi_on = false;
    if (s_sta_timer) esp_timer_stop(s_sta_timer);
    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi выключен - прибор доступен только по Ethernet");
}

/* СТОРОЖ, пока WiFi выключен: раз в секунду смотрит, есть ли адрес у
 * Ethernet, и после 30 с без адреса включает WiFi - иначе к прибору не
 * подключиться. Работает и при старте, и позже (выдернули кабель,
 * перезагрузился роутер). Сама настройка «выключено» не меняется.
 * Своя задача, а не таймер esp_timer: запуск WiFi требует заметного
 * стека. Завершается, как только WiFi включён. */
#define WIFI_GUARD_S 30

static void wifi_guard_task(void *arg)
{
    int no_ip_s = 0;
    while (!s_wifi_on) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        no_ip_s = mca_eth_ip() ? 0 : no_ip_s + 1;
        if (no_ip_s >= WIFI_GUARD_S && !s_wifi_on) {
            ESP_LOGW(TAG, "у Ethernet нет адреса %d с - включаю WiFi, чтобы "
                          "прибор был доступен", WIFI_GUARD_S);
            wifi_bring_up();
        }
    }
    vTaskDelete(NULL);
}

/* Выключение со страницы - через полсекунды, отдельной задачей: ответ
 * должен успеть уйти, если запрос пришёл как раз по WiFi. */
static void wifi_off_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_shut_down();
    if (!s_wifi_on)
        xTaskCreate(wifi_guard_task, "wifi_guard", 4096, NULL, 5, NULL);
    vTaskDelete(NULL);
}

/* Сеть целиком: NVS, стек TCP/IP, Ethernet, затем решение про WiFi. */
static void net_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    wifi_creds_load();
    wifi_off_load();

    esp_netif_init();
    esp_event_loop_create_default();

    const bool eth = mca_eth_start() == ESP_OK;
    if (s_wifi_off_cfg && eth) {
        ESP_LOGI(TAG, "WiFi выключен в настройках - работаем по Ethernet");
        xTaskCreate(wifi_guard_task, "wifi_guard", 4096, NULL, 5, NULL);
    } else {
        if (s_wifi_off_cfg)
            ESP_LOGW(TAG, "WiFi выключен в настройках, но Ethernet нет - "
                          "включаю WiFi");
        wifi_bring_up();
    }
    /* часы прибора: SNTP, когда появится выход в интернет */
    mca_time_init();
}

/* ---- шапка и навигация вспомогательных страниц ---- */
/* Та же шапка, что на главной, но без индикаторов состояния - только
 * переключатель языка. Заголовок вкладки - на двух языках. */
#define SUB_HEAD(TITLE, TITLE_EN) \
"<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>" \
"<meta name=viewport content='width=device-width,initial-scale=1'>" \
"<title data-en='" TITLE_EN " · MCA'>" TITLE " · MCA</title>" \
"<link rel=stylesheet href=/s.css><script src=/l.js></script></head><body>" \
"<header><div class=brand><img src=/logo.png?v=2 alt=''>MCA " \
"<span style=color:var(--accent)>AD9226</span></div>" \
"<div class=chips><span class='seg lng'><button onclick=\"setLang('ru')\">RU</button>" \
"<button onclick=\"setLang('en')\">EN</button></span></div></header>"
#define SUB_NAV(W, H) "<nav><a href=/ data-en='Spectrum'>Спектр</a>" W H "</nav>"
#define N_WIFI    "<a href=/wifi data-en='Network'>Сеть</a>"
#define N_WIFI_ON "<span class=on data-en='Network'>Сеть</span>"
#define N_HELP    "<a href=/help data-en='Help'>Справка</a>"
#define N_HELP_ON "<span class=on data-en='Help'>Справка</span>"

/* ---- страница/обработчики настройки сети: Ethernet и WiFi ---- */
static const char WIFI_PAGE[] =
SUB_HEAD("Сеть", "Network") SUB_NAV(N_WIFI_ON, N_HELP)
"<div class=wrap><section class='panel pad' style=max-width:560px>"
"<h3>Ethernet (W5500)</h3>"
"<div id=eth class=n style=margin-bottom:6px>...</div>"
"<h3 style=margin-top:18px>WiFi</h3>"
"<div id=wst class=n style=margin-bottom:10px>...</div>"
"<div id=woff style=margin-bottom:16px></div>"
"<div class=lbl data-en='Network (SSID)'>Сеть (SSID)</div><input id=s style='width:100%;margin:4px 0 10px'>"
"<div class=lbl data-en='Password'>Пароль</div><input id=p type=password style='width:100%;margin:4px 0 12px'>"
"<button class='btn green' onclick=go() data-en='Save and connect'>Сохранить и подключиться</button>"
"<p class=n data-en='The device saves the credentials and tries to connect without turning off "
"the " MCA_AP_SSID " access point.'>Прибор сохранит данные и попробует "
"подключиться, не отключая точку доступа " MCA_AP_SSID ".</p>"
"</section></div>"
"<script>i18n();"
"function ld(){fetch('/wifi/status').then(r=>r.json()).then(function(j){"
"var ei=j.eth_ip!='0.0.0.0';"
"document.getElementById('eth').innerHTML=!j.eth_present?L('модуль W5500 не найден','W5500 module not found'):"
"ei?L('подключён: ','connected: ')+'<b><a href=http://'+j.eth_ip+'/>http://'+j.eth_ip+'/</a></b>':"
"(j.eth_link?L('кабель подключён, жду адрес от роутера','cable connected, waiting for an address from the router'):"
"L('модуль найден, кабель не подключён','module found, cable not connected'));"
"document.getElementById('wst').innerHTML=j.wifi_on?"
"(L('включён. Сеть <b>','on. Network <b>')+j.ssid+'</b> &mdash; '+"
"(j.connected?L('подключено, IP ','connected, IP ')+j.ip:L('нет связи','no connection'))):"
"L('<b>выключен</b>','<b>off</b>')+(j.wifi_off_cfg?'':L(' (временно)',' (temporarily)'));"
"var b=document.getElementById('woff');"
"if(!j.wifi_on)b.innerHTML='<button class=\"btn green\" onclick=wset(1)>'+L('Включить WiFi','Turn WiFi on')+'</button>';"
"else if(ei)b.innerHTML='<button class=\"btn amber\" onclick=wset(0)>'+L('Выключить WiFi','Turn WiFi off')+'</button> "
"<span class=n>'+L('прибор останется доступен по Ethernet: ','the device stays reachable over Ethernet: ')+"
"'http://'+j.eth_ip+'/</span>';"
"else b.innerHTML='<span class=n>'+L('Выключить WiFi можно, когда Ethernet получит адрес: '+"
"'иначе к прибору будет не подключиться.','WiFi can be turned off once Ethernet has an address: '+"
"'otherwise the device would be unreachable.')+'</span>';"
"var s=document.getElementById('s');if(!s.value&&document.activeElement!==s)s.value=j.ssid})}"
"function wset(on){if(!on&&!confirm(L('Выключить WiFi? Прибор останется доступен только по Ethernet.',"
"'Turn WiFi off? The device will be reachable only over Ethernet.')))return;"
/* прибор присылает сообщение на двух языках: msg - русский, en - английский */
"fetch('/net/set?wifi='+on).then(r=>r.json()).then(function(j){alert(L(j.msg,j.en||j.msg));ld()})}"
"function go(){var s=document.getElementById('s').value,"
"p=document.getElementById('p').value;"
"fetch('/wifi/set?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))"
".then(()=>alert(L('Сохранено, подключаюсь...','Saved, connecting...')))}"
"ld();setInterval(ld,3000);"
"</script></body></html>";

static esp_err_t h_wifi_page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, WIFI_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_wifi_status(httpd_req_t *r)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = { 0 };
    bool connected = s_wifi_on && sta &&
                     esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
                     ip.ip.addr != 0;
    const esp_ip4_addr_t eip = { .addr = mca_eth_ip() };
    char buf[320];
    int n = snprintf(buf, sizeof(buf),
        "{\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"" IPSTR "\","
        "\"wifi_on\":%s,\"wifi_off_cfg\":%s,"
        "\"eth_present\":%s,\"eth_link\":%s,\"eth_ip\":\"" IPSTR "\"}",
        s_sta_ssid, connected ? "true" : "false", IP2STR(&ip.ip),
        s_wifi_on ? "true" : "false", s_wifi_off_cfg ? "true" : "false",
        mca_eth_present() ? "true" : "false",
        mca_eth_link_up() ? "true" : "false", IP2STR(&eip));
    httpd_resp_set_type(r, "application/json");
    /* snprintf возвращает длину, которая ПОТРЕБОВАЛАСЬ БЫ. При нехватке
     * места это больше размера буфера, и отправка читала бы за его
     * границей. Ограничиваем. */
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    return httpd_resp_send(r, buf, n);
}

/* Раскодировать %XX (и + как пробел, на всякий случай) на месте.
 * encodeURIComponent() в браузере кодирует пробел как %20, а не '+',
 * но некоторые клиенты/формы могут прислать и так - обрабатываем оба. */
static void url_decode(char *s)
{
    char *w = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *w++ = ' ';
            s++;
        } else {
            *w++ = *s++;
        }
    }
    *w = 0;
}

static esp_err_t h_wifi_set(httpd_req_t *r)
{
    char q[160], ssid[33] = {0}, pass[65] = {0};
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "ssid", ssid, sizeof(ssid));
        httpd_query_key_value(q, "pass", pass, sizeof(pass));
    }
    url_decode(ssid);
    url_decode(pass);
    if (strlen(ssid) == 0) return httpd_resp_send(r, "empty ssid", -1);

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    strncpy(s_sta_pass, pass, sizeof(s_sta_pass) - 1);
    wifi_creds_save(s_sta_ssid, s_sta_pass);
    if (!s_wifi_on) {
        /* WiFi выключен - только запоминаем, радио не трогаем */
        ESP_LOGI(TAG, "STA-данные сохранены (WiFi выключен): SSID='%s'",
                 s_sta_ssid);
        return httpd_resp_sendstr(r, "ok");
    }
    /* сеть задана - поднимаем клиентскую часть и начинаем попытки заново */
    s_sta_tries = 0;
    if (s_sta_timer) esp_timer_stop(s_sta_timer);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_apply_sta_config();
    esp_wifi_disconnect();
    esp_wifi_connect();

    ESP_LOGI(TAG, "новые STA-данные сохранены: SSID='%s'", s_sta_ssid);
    return httpd_resp_sendstr(r, "ok");
}

/* Включить / выключить WiFi: /net/set?wifi=0|1. Выключить можно только
 * при адресе у Ethernet - иначе к прибору станет не подключиться. */
static esp_err_t h_net_set(httpd_req_t *r)
{
    /* Сообщение - на двух языках: msg по-русски, en по-английски, какое
     * показать, решает страница. */
    char q[32], v[4] = { 0 }, buf[400];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "wifi", v, sizeof(v)) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "wifi=0|1");

    int n;
    if (v[0] == '0') {
        const esp_ip4_addr_t eip = { .addr = mca_eth_ip() };
        if (!eip.addr) {
            n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"У Ethernet "
                         "нет адреса - WiFi не выключаю, иначе к прибору не "
                         "подключиться.\",\"en\":\"Ethernet has no address - "
                         "not turning WiFi off, otherwise the device would be "
                         "unreachable.\"}");
        } else if (wifi_off_save(true) != ESP_OK) {
            n = snprintf(buf, sizeof(buf), "{\"ok\":false,\"msg\":\"Не удалось "
                         "сохранить настройку.\",\"en\":\"Could not save the "
                         "setting.\"}");
        } else {
            xTaskCreate(wifi_off_task, "wifi_off", 3072, NULL, 5, NULL);
            n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"msg\":\"WiFi "
                         "выключится через секунду. Прибор: http://" IPSTR
                         "/\",\"en\":\"WiFi turns off in a second. Device: "
                         "http://" IPSTR "/\"}", IP2STR(&eip), IP2STR(&eip));
        }
    } else {
        wifi_off_save(false);
        wifi_bring_up();
        n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"msg\":\"WiFi включён.\","
                     "\"en\":\"WiFi is on.\"}");
    }
    httpd_resp_set_type(r, "application/json");
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    return httpd_resp_send(r, buf, n);
}



static esp_err_t h_diag_data(httpd_req_t *r)
{
    mca_diag_t d;
    mca_diag_get(&d);

    uint32_t reg_ctrl = 0, reg_ctrl1 = 0;
    int pclk_lvl = -1;
    adc_cap_dump_regs(&reg_ctrl, &reg_ctrl1, &pclk_lvl);

    char buf[2600];
    int n = snprintf(buf, sizeof(buf),
        "{\"enabled\":%s,\"flow\":%s,\"chunks\":%llu,\"samples\":%llu,"
        "\"rate\":%lu,\"exp\":%lu,\"err\":%ld,\"lost\":%llu,"
        "\"ctrl\":%lu,\"ctrl1\":%lu,\"pclk\":%d,\"stats\":%s",
        mca_diag_get_enabled() ? "true" : "false",
        d.data_flowing ? "true" : "false",
        (unsigned long long)d.chunks_total,
        (unsigned long long)d.samples_total,
        (unsigned long)d.rate_measured, (unsigned long)d.rate_expected,
        (long)d.rate_error_pct,
        (unsigned long long)adc_cap_chunks_lost(),
        (unsigned long)reg_ctrl, (unsigned long)reg_ctrl1, pclk_lvl,
        d.stats_valid ? "true" : "false");

    if (d.stats_valid) {
        n += snprintf(buf + n, sizeof(buf) - n,
            ",\"a1\":%u,\"a0\":%u,\"vmin\":%u,\"vmax\":%u,\"vmean\":%lu,"
            "\"otr\":%lu,\"an\":%lu,\"tg\":[",
            d.bit_always_1, d.bit_always_0, d.vmin, d.vmax,
            (unsigned long)d.vmean, (unsigned long)d.otr_count,
            (unsigned long)d.analyzed_samples);
        for (int b = 0; b < DIAG_NBITS; b++)
            n += snprintf(buf + n, sizeof(buf) - n, "%s%lu",
                          b ? "," : "", (unsigned long)d.bit_toggles[b]);
        n += snprintf(buf + n, sizeof(buf) - n, "]");
    }

    /* ПРОФИЛЬ за прошлую секунду и журнал последних потерь (mca_prof) */
    mca_prof_t pf;
    mca_prof_get(&pf);
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"pf\":{\"ch\":%lu,\"q\":%lu,\"wait\":%lu,\"work\":%lu,\"busy\":%lu,"
        "\"lost\":%lu,\"heap\":%lu,\"hmin\":%lu,\"rssi\":%ld,"
        "\"wmax\":[%lu,%lu,%lu],\"wcnt\":[%lu,%lu,%lu]}",
        (unsigned long)pf.chunks, (unsigned long)pf.q_max,
        (unsigned long)pf.wait_max_us, (unsigned long)pf.work_max_us,
        (unsigned long)pf.busy_pm,
        (unsigned long)pf.lost_1s, (unsigned long)pf.heap_now,
        (unsigned long)pf.heap_min, (long)pf.rssi,
        (unsigned long)pf.web_max_us[PROF_EP_SPEC],
        (unsigned long)pf.web_max_us[PROF_EP_SCOPE],
        (unsigned long)pf.web_max_us[PROF_EP_STAT],
        (unsigned long)pf.web_cnt[PROF_EP_SPEC],
        (unsigned long)pf.web_cnt[PROF_EP_SCOPE],
        (unsigned long)pf.web_cnt[PROF_EP_STAT]);

    /* Такты на отсчёт по этапам обработки спектра (x100) и частота АЦП:
     * страница сравнивает их с бюджетом 240 МГц / частота. */
    mca_stats_t st;
    mca_dsp_get_stats(&st);
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\"cyc\":[%lu,%lu,%lu],\"fhz\":%lu",
        (unsigned long)st.cyc_x100[0], (unsigned long)st.cyc_x100[1],
        (unsigned long)st.cyc_x100[2], (unsigned long)adc_clk_get_freq());

    mca_prof_ev_t ev[12];
    size_t ne = mca_prof_events(ev, 12);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"ev\":[");
    for (size_t i = 0; i < ne && n < (int)sizeof(buf) - 96; i++)
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s[%lu,%lu,%lu,%u,%lu,%lu,%u]", i ? "," : "",
                      (unsigned long)ev[i].t_ms, (unsigned long)ev[i].lost,
                      (unsigned long)ev[i].q_depth, (unsigned)ev[i].web_ep,
                      (unsigned long)ev[i].web_us,
                      (unsigned long)ev[i].work_us, (unsigned)ev[i].mode);
    n += snprintf(buf + n, sizeof(buf) - n, "]}");

    httpd_resp_set_type(r, "application/json");
    /* snprintf возвращает длину, которая ПОТРЕБОВАЛАСЬ БЫ. При нехватке
     * места это больше размера буфера, и отправка читала бы за его
     * границей. Ограничиваем. */
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    return httpd_resp_send(r, buf, n);
}

static esp_err_t h_diag_probe(httpd_req_t *r)
{
    uint32_t hi = 0, lo = 0;
    uint32_t ch = adc_cap_probe_data_lines(&hi, &lo);
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ch\":%lu,\"hi\":%lu,\"lo\":%lu}",
                     (unsigned long)ch, (unsigned long)hi,
                     (unsigned long)lo);
    httpd_resp_set_type(r, "application/json");
    /* snprintf возвращает длину, которая ПОТРЕБОВАЛАСЬ БЫ. При нехватке
     * места это больше размера буфера, и отправка читала бы за его
     * границей. Ограничиваем. */
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    return httpd_resp_send(r, buf, n);
}


static esp_err_t h_diag_set(httpd_req_t *r)
{
    char q[32], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "en", v, sizeof(v)) == ESP_OK) {
        mca_diag_set_enabled(atoi(v) != 0);
    }
    return httpd_resp_sendstr(r, "ok");
}


/* ---- справка по параметрам ---- */
static const char HELP_PAGE[] =
SUB_HEAD("Справка", "Help") SUB_NAV(N_WIFI, N_HELP_ON)
"<style>td:first-child{white-space:nowrap;font-weight:600;width:1%}"
"table{margin:4px 0 14px}</style>"
"<div class=wrap><section class='panel pad' style=max-width:920px>"
/* два блока: lr - русский, le - английский; лишний прячет стиль по языку */
"<div class=lr>"
"<h3>Справка по параметрам</h3>"

"<h4>Обнаружение события</h4><table>"
"<tr><th>Параметр</th><th>Что делает</th></tr>"
"<tr><td>Порог<br><code>threshold</code></td><td>На сколько кодов АЦП "
"отсчёт должен превысить базовую линию, чтобы считаться событием. "
"Ставится чуть выше шума &mdash; посмотрите шум на вкладке «Осциллограф».</td></tr>"
"<tr><td>Полярность<br><code>polarity</code></td><td>0 &mdash; импульсы "
"вверх от базовой линии, 1 &mdash; вниз (например, анод ФЭУ напрямую). "
"При 1 сигнал переворачивается ещё до обработки: порог и спектр "
"работают как с импульсами вверх, осциллограф "
"синхронизируется по фронту вниз. Постоянная составляющая сигнала "
"(например, база на &minus;4 В) на обработку не влияет &mdash; её "
"вычитает трапеция.</td></tr>"
"<tr><td>Гистерезис<br><code>hysteresis</code></td><td>Чтобы засчитать "
"следующее событие, сигнал должен сначала упасть ниже "
"доли порога. Задаётся В ПРОЦЕНТАХ: 50 значит, что трапеция должна "
"опуститься ниже половины порога. Прежний вариант вычитал единицу, "
"то есть гистерезиса практически не было.</td></tr>"
"<tr><td>Перезапуск<br><code>rearm</code></td><td>Сколько отсчётов жёстко "
"пропустить после пика, независимо от гистерезиса. Защита от дребезга "
"на спадающем хвосте. Это <b>не</b> метрика потерь, а параметр "
"перезапуска детектора.</td></tr>"
"</table>"

"<h4>Трапецеидальный фильтр</h4>"
"<p style=opacity:.7;font-size:13px>Алгоритм Джорданова-Нолла: разность "
"двух скользящих сумм, разнесённых на зазор. Даёт плоскую вершину &mdash; "
"амплитуда почти не зависит от того, в какую точку вершины попали. "
"Постоянная составляющая (базовая линия) вычитается автоматически: "
"обе суммы содержат её поровну.</p><table>"
"<tr><th>Параметр</th><th>Что делает</th></tr>"
"<tr><td>Окно L<br><code>trap_L</code></td><td>Длина окна "
"интегрирования: сколько отсчётов импульса суммируется. Определяет, "
"какая часть импульса идёт в амплитуду и насколько усредняется шум. "
"<b>Если окно задаётся парой RISE и FALL</b> (так бывает в других "
"приборах), окно интегрирования &mdash; их сумма: RISE=6, FALL=15 "
"соответствует нашему L&asymp;21.</td></tr>"
"<tr><td>Зазор G<br><code>trap_G</code></td><td>На сколько отсчётов "
"назад отстоит второе окно, которое вычитается. Оно должно целиком "
"лежать на базовой линии ДО импульса, поэтому G обязан быть больше "
"L плюс длительность фронта. Разность G&minus;L даёт длину плоской "
"вершины: чем она больше, тем устойчивее измеряется амплитуда.</td></tr>"
"<tr><td>Поиск пика<br><code>search</code></td><td>Сколько отсчётов после "
"срабатывания порога перебирать в поисках максимума трапеции. "
"Должно накрывать весь импульс.</td></tr>"
"</table>"

"<h4>Способ измерения амплитуды</h4>"
"<p style=opacity:.7;font-size:13px>Выбирается в меню «Способ измерения». "
"В таблице настроек параметры одного способа помечены <b>трапеция</b> "
"или <b>интегрирование</b>, без пометки &mdash; действуют всегда; "
"параметры, которые при выбранном способе не действуют, приглушены. Событие в обоих "
"способах обнаруживается одинаково &mdash; по трапеции, поэтому порог, "
"гистерезис, перезапуск, поиск пика, наложения, L и G нужны всегда (при "
"интегрировании по L и G ещё и находится вершина).</p><table>"
"<tr><th>Способ</th><th>Свои параметры</th></tr>"
"<tr><td>Трапеция</td><td>Амплитуда &mdash; вершина трапеции. Окно L и "
"зазор G задают и обнаружение, и измерение; дополнительно «Средн. по "
"плато». Базовая линия не нужна: трапеция вычитает её сама.</td></tr>"
"<tr><td>Интегрирование</td><td>Амплитуда &mdash; среднее отсчётов "
"импульса вокруг вершины за вычетом базовой линии. Свои параметры: "
"«До вершины», «После вершины» (сколько "
"отсчётов брать, считаются прямо по осциллографу), «База 2^N» и «Окно "
"базы». Масштаб у способов разный &mdash; после переключения подберите "
"«Кодов на канал». Смена способа очищает спектр.</td></tr>"
"</table>"

"<h4>Амплитуда и базовая линия</h4><table>"
"<tr><th>Параметр</th><th>Что делает</th></tr>"
"<tr><td>Кодов на канал<br><code>cpc</code></td><td>Сколько кодов "
"амплитуды приходится на один канал: 1 &mdash; канал равен коду АЦП, "
"0.5 &mdash; вдвое подробнее, 2 &mdash; вдвое грубее. <b>Верх шкалы = "
"каналов &times; кодов на канал</b>, он подписан рядом с полем. При "
"базе в середине АЦП запас вверх около 2047 кодов, поэтому 2048 "
"каналов по 1 коду покрывают его целиком. Амплитуда делится без "
"округления до кода, так что доли кода на канал дают настоящую "
"подробность, а не «гребёнку» пустых каналов. Изменение очищает "
"спектр.</td></tr>"
"<tr><td>Каналов<br><code>nch</code></td><td>Сколько каналов "
"показывать: 2048, 4096 или 8192. Это длина шкалы вправо; раскладка "
"событий от неё не зависит &mdash; спектр не сжимается и не "
"сбрасывается. Прибор копит все 8192 канала.</td></tr>"
"<tr><td>База 2^N<br><code>baseline_shift</code></td><td>Постоянная "
"времени фильтра, отслеживающего нулевой уровень между импульсами. "
"Больше значение &mdash; медленнее и плавнее подстройка. Обычно "
"8&ndash;12.</td></tr>"
"<tr><td>Окно базы<br><code>baseline_win</code></td><td>В среднее "
"берутся только отсчёты, отстоящие от текущей оценки нуля не дальше "
"этого значения. Нужно потому, что между импульсами сигнал редко "
"возвращается к настоящему нулю &mdash; мешают недоспавшие хвосты "
"(у нас tau&asymp;18 мкс). Без окна базовая линия уползала бы вверх "
"вслед за наложениями: при 10 000 имп/с ошибка была 190 кодов, "
"с окном стала 24. Ставить примерно вдвое-втрое больше размаха шума. "
"Если оценка долго не находит похожих отсчётов, ноль перезахватывается "
"автоматически.</td></tr>"
"<tr><td>Средн. по плато<br><code>flat_avg</code></td><td>Брать "
"амплитуду как среднее по плоской вершине трапеции вместо максимума. "
"<b>По умолчанию выключено.</b> Проверено симуляцией: усреднение "
"выигрывает только при длинном плато (G&minus;L от 19 отсчётов). "
"При коротком плато максимум даёт даже меньший разброс &mdash; "
"соседние отсчёты трапеции сильно скоррелированы, усреднять почти "
"нечего. При (G&minus;L) меньше 16 происходит автоматический откат "
"на максимум. Смещение максимума растёт с шумом, но это постоянный "
"сдвиг для всех амплитуд &mdash; уходит в калибровку шкалы и ширину "
"пиков не портит.</td></tr>"
"</table>"
"<h4>Отбраковка наложений</h4>"
"<p style=opacity:.7;font-size:13px>Меньше проценты &mdash; строже "
"отбраковка, больше событий уходит в брак. Следите за счётчиком "
"«наложений» в статистике.</p><table>"
"<tr><th>Параметр</th><th>Что делает</th></tr>"
"<tr><td>Наложение до %<br><code>pileup_pre_pct</code></td><td>Если "
"<b>перед</b> пиком трапеция уже выше этого процента от амплитуды &mdash; "
"импульс сидит на хвосте предыдущего, бракуем.</td></tr>"
"<tr><td>Наложение после %<br><code>pileup_post_pct</code></td><td>Если "
"<b>после</b> пика трапеция не упала ниже этого процента &mdash; "
"на импульс наложился следующий, бракуем.</td></tr>"
"</table>"

"<h4>Режимы</h4><table>"
"<tr><th>Режим</th><th>Назначение</th></tr>"
"<tr><td>Спектр</td><td>Набор гистограммы. Поля «пик от / до» выделяют "
"участок спектра: для него считаются центр, ширина на полувысоте и "
"разрешение. Кнопки «выгрузить» сохраняют спектр файлом: "
"XML (ResultDataFile, открывается в BecqMoni), CSV (канал и счёт), "
"N42 (ANSI N42.42) и SPE (SpectraLine). "
"Выгружаются каналы, видимые на экране; время замера берётся из часов "
"компьютера &mdash; своих часов у прибора нет. Под галочкой «настройки» "
"&mdash; та же таблица параметров обработки, что на «Осциллографе»: их можно "
"менять прямо во время набора и смотреть, как меняется спектр. Смена "
"«Кодов на канал» и способа измерения очищает спектр, смена L, G и "
"полярности перезапускает фильтр. Ниже &mdash; мониторинг CPS: история за "
"6 часов по секунде хранится в приборе и копится, пока идёт набор, даже при "
"закрытой странице; график складывает её в интервалы «усреднение, с» и рисует "
"скользящее среднее. Видно окно по времени (по умолчанию последние 30 с, выбор "
"«окно»), оно едет за новыми данными; колесо &mdash; масштаб, перетаскивание "
"&mdash; сдвиг в прошлое (там окно стоит на месте), двойной щелчок &mdash; обратно "
"к окну и текущему моменту. CPS интервала &mdash; импульсы, делённые "
"на время, пока реально шёл набор; &delta; &mdash; статистическая погрешность "
"1/&radic;N. Время прибора берётся по SNTP или от браузера.</td></tr>"
"<tr><td>Осциллограф</td><td>Осциллограф и настройки обработки. Сырые отсчёты с синхронизацией "
"по фронту: как только сигнал вырос за 8 отсчётов не меньше чем на "
"«синхр. по фронту», момент срабатывания ставится на пятую часть "
"экрана (синяя метка), и импульс стоит на месте. Переключатель "
"<b>Авто</b> / <b>Ждущий</b>: в авто без фронта 0.15 с развёртка идёт "
"свободно, чтобы были видны база и шум; ждущий обновляется только по "
"фронту и держит последний пойманный импульс. Ширину окна задаёт "
"«развёртка» &mdash; от 64 до 32768 отсчётов (при 8 МГц это 4 мс), "
"цена деления подписана на графике. «Амплитуда от&ndash;до»: "
"синхронизация по амплитуде &mdash; кадр показывается, только если высота "
"импульса над базой в этом диапазоне кодов АЦП (0 и 0 &mdash; без фильтра); "
"высота показанного импульса и сколько импульсов отброшено &mdash; под "
"графиком. «Ось X»: подписи сетки в "
"микросекундах или в отсчётах. Отсчёты считаются от момента "
"синхронизации (синяя метка = 0, до неё &mdash; отрицательные), так "
"что числа для L, G, «до/после вершины», перезапуска и поиска пика "
"читаются прямо с экрана. <b>Линейка</b>: щелчок по графику ставит "
"метки A (начало), B (вершина &mdash; прилипает к пику рядом со "
"щелчком) и C (конец), метки можно перетаскивать. Под графиком "
"&mdash; расстояния A&rarr;B, B&rarr;C, A&rarr;C в отсчётах и "
"микросекундах и высота вершины; кнопка «в до / после вершины» "
"переносит B&minus;A и C&minus;B в параметры интегрирования. Трапеция "
"на экран не выводится &mdash; она считается в приборе, а строка "
"«шум фильтра до импульса» показывает, выше ли порог её шума. Здесь же, "
"под галочкой «настройки», &mdash; таблица всех параметров обработки: "
"их удобнее подбирать, глядя на импульсы. Спектр в этом "
"режиме не набирается. «Ось Y от»: от базовой линии, от середины шкалы "
"(2048) или в настоящих кодах АЦП. «Вся шкала» показывает весь диапазон "
"АЦП, а строка «запас по шкале» &mdash; сколько кодов осталось от базы "
"до потолка и до пола.</td></tr>"
"<tr><td>Диагностика</td><td>Галочка «диагностика» на вкладках «Спектр» и "
"«Осциллограф» показывает под графиком тракт АЦП: поток отсчётов и его "
"реальную скорость, регистры камерного интерфейса, профиль за последнюю "
"секунду (занятость обработки, такты на отсчёт, запросы, память, сигнал "
"WiFi), журнал потерь чанков, побитовую статистику линий данных и их "
"проверку. Отдельной страницы больше нет.</td></tr>"
"</table></div>"

"<div class=le>"
"<h3>Parameter reference</h3>"
"<h4>Event detection</h4><table>"
"<tr><th>Parameter</th><th>What it does</th></tr>"
"<tr><td>Threshold<br><code>threshold</code></td><td>How many ADC codes a sample "
"must exceed the baseline by to count as an event. Set just above the noise &mdash; "
"look at the noise on the “Oscilloscope” tab.</td></tr>"
"<tr><td>Polarity<br><code>polarity</code></td><td>0 &mdash; pulses go up from the "
"baseline, 1 &mdash; down (for example, PMT anode directly). With 1 the signal is "
"inverted before processing: the threshold and spectrum work as with upward pulses, "
"the oscilloscope triggers on a falling edge. A DC offset of the signal (for example, "
"a baseline at &minus;4 V) does not affect processing &mdash; the trapezoid subtracts "
"it.</td></tr>"
"<tr><td>Hysteresis<br><code>hysteresis</code></td><td>To count the next event, the "
"signal must first drop below a fraction of the threshold. Set IN PERCENT: 50 means "
"the trapezoid must drop below half the threshold. The previous version subtracted "
"one, so there was practically no hysteresis.</td></tr>"
"<tr><td>Re-arm<br><code>rearm</code></td><td>How many samples to skip "
"unconditionally after the peak, regardless of hysteresis. Protection against "
"chatter on the falling tail. This is <b>not</b> a loss metric but a detector "
"re-arm parameter.</td></tr>"
"</table>"

"<h4>Trapezoidal filter</h4>"
"<p style=opacity:.7;font-size:13px>Jordanov&ndash;Knoll algorithm: the difference "
"of two moving sums separated by a gap. It gives a flat top &mdash; the amplitude "
"hardly depends on which point of the top is hit. The DC component (baseline) is "
"subtracted automatically: both sums contain it equally.</p><table>"
"<tr><th>Parameter</th><th>What it does</th></tr>"
"<tr><td>Window L<br><code>trap_L</code></td><td>Integration window length: how "
"many pulse samples are summed. It determines which part of the pulse goes into the "
"amplitude and how much the noise is averaged. <b>If the window is set by a RISE and "
"FALL pair</b> (as in some other devices), the integration window is their sum: "
"RISE=6, FALL=15 corresponds to our L&asymp;21.</td></tr>"
"<tr><td>Gap G<br><code>trap_G</code></td><td>How many samples back the second, "
"subtracted window is. It must lie entirely on the baseline BEFORE the pulse, so G "
"must be greater than L plus the rise time. The difference G&minus;L is the flat-top "
"length: the longer it is, the more stable the amplitude measurement.</td></tr>"
"<tr><td>Peak search<br><code>search</code></td><td>How many samples after the "
"threshold crossing to search for the trapezoid maximum. Must cover the whole "
"pulse.</td></tr>"
"</table>"

"<h4>Amplitude method</h4>"
"<p style=opacity:.7;font-size:13px>Selected in the “Method” menu. In the settings "
"table the parameters of one method are marked <b>trapezoid</b> or "
"<b>integration</b>, unmarked ones always apply; parameters that have no effect with "
"the selected method are dimmed. Events are detected the same way with both methods "
"&mdash; by the trapezoid, so threshold, hysteresis, re-arm, peak search, pile-up, L "
"and G are always needed (with integration L and G also locate the peak).</p><table>"
"<tr><th>Method</th><th>Its own parameters</th></tr>"
"<tr><td>Trapezoid</td><td>Amplitude &mdash; the top of the trapezoid. Window L and "
"gap G set both detection and measurement; in addition “Flat-top avg”. No baseline "
"is needed: the trapezoid subtracts it itself.</td></tr>"
"<tr><td>Integration</td><td>Amplitude &mdash; the mean of the pulse samples around "
"the peak minus the baseline. Its own parameters: “Before peak”, “After peak” (how "
"many samples to take, counted straight from the oscilloscope), “Baseline 2^N” and "
"“Baseline window”. The methods have different scales &mdash; after switching, "
"adjust “Codes per channel”. Changing the method clears the spectrum.</td></tr>"
"</table>"

"<h4>Amplitude and baseline</h4><table>"
"<tr><th>Parameter</th><th>What it does</th></tr>"
"<tr><td>Codes per channel<br><code>cpc</code></td><td>How many amplitude codes per "
"channel: 1 &mdash; a channel equals an ADC code, 0.5 &mdash; twice as fine, 2 "
"&mdash; twice as coarse. <b>Top of scale = channels &times; codes per channel</b>, "
"shown next to the field. With the baseline in the middle of the ADC the headroom is "
"about 2047 codes, so 2048 channels of 1 code cover it entirely. The amplitude is "
"divided without rounding to a code, so fractions of a code per channel give real "
"detail rather than a “comb” of empty channels. Changing it clears the "
"spectrum.</td></tr>"
"<tr><td>Channels<br><code>nch</code></td><td>How many channels to show: 2048, 4096 "
"or 8192. This is the scale length to the right; how events are laid out does not "
"depend on it &mdash; the spectrum is neither compressed nor reset. The device "
"accumulates all 8192 channels.</td></tr>"
"<tr><td>Baseline 2^N<br><code>baseline_shift</code></td><td>Time constant of the "
"filter that tracks the zero level between pulses. Larger value &mdash; slower and "
"smoother tracking. Usually 8&ndash;12.</td></tr>"
"<tr><td>Baseline window<br><code>baseline_win</code></td><td>Only samples within "
"this distance from the current zero estimate are averaged. This is needed because "
"between pulses the signal rarely returns to the true zero &mdash; unfinished tails "
"get in the way (ours have tau&asymp;18 µs). Without the window the baseline would "
"creep up following pile-ups: at 10,000 cps the error was 190 codes, with the window "
"it became 24. Set about two to three times the noise swing. If the estimate finds "
"no similar samples for a long time, the zero is re-captured automatically.</td></tr>"
"<tr><td>Flat-top avg<br><code>flat_avg</code></td><td>Take the amplitude as the "
"mean over the flat top of the trapezoid instead of the maximum. <b>Off by "
"default.</b> Verified by simulation: averaging wins only with a long flat top "
"(G&minus;L from 19 samples). With a short flat top the maximum gives even less "
"spread &mdash; neighbouring trapezoid samples are strongly correlated, there is "
"little to average. With (G&minus;L) below 16 it automatically falls back to the "
"maximum. The bias of the maximum grows with noise, but it is a constant shift for "
"all amplitudes &mdash; it goes into the scale calibration and does not widen the "
"peaks.</td></tr>"
"</table>"
"<h4>Pile-up rejection</h4>"
"<p style=opacity:.7;font-size:13px>Lower percentages &mdash; stricter rejection, "
"more events are rejected. Watch the pile-up counter in the statistics.</p><table>"
"<tr><th>Parameter</th><th>What it does</th></tr>"
"<tr><td>Pile-up before %<br><code>pileup_pre_pct</code></td><td>If <b>before</b> "
"the peak the trapezoid is already above this percentage of the amplitude &mdash; "
"the pulse sits on the tail of the previous one, reject.</td></tr>"
"<tr><td>Pile-up after %<br><code>pileup_post_pct</code></td><td>If <b>after</b> "
"the peak the trapezoid has not dropped below this percentage &mdash; the next pulse "
"piled up on this one, reject.</td></tr>"
"</table>"

"<h4>Modes</h4><table>"
"<tr><th>Mode</th><th>Purpose</th></tr>"
"<tr><td>Spectrum</td><td>Histogram acquisition. The “peak from / to” fields select "
"a part of the spectrum: the centroid, FWHM and resolution are computed for it. The "
"“export” buttons save the spectrum to a file: XML (ResultDataFile, opens in "
"BecqMoni), CSV (channel and count), N42 (ANSI N42.42) and SPE (SpectraLine). The "
"channels visible on screen are exported; the measurement time is taken from the "
"computer clock &mdash; the device has no clock of its own. Under the "
"“settings” checkbox is the same processing parameter table as on the "
"“Oscilloscope” tab: the parameters can be changed right during acquisition to see how "
"the spectrum changes. Changing “Codes per channel” or the method clears the "
"spectrum, changing L, G or polarity restarts the filter. Below is the CPS "
"monitor: a 6-hour history at one sample per second is kept in the device and "
"accumulates while the spectrum is acquired, even with the page closed; the "
"plot sums it into “averaging, s” intervals and draws a moving average. It "
"shows a time window (the last 30 s by default, the “window” selector) that "
"follows new data; wheel &mdash; zoom, drag &mdash; move into the past (the "
"window then stays put), double click &mdash; back to the window and now. The CPS "
"of an interval is the counts divided by the time acquisition actually ran; "
"&delta; is the statistical error 1/&radic;N. The device time comes from SNTP "
"or from the browser.</td></tr>"
"<tr><td>Oscilloscope</td><td>Oscilloscope and processing settings. Raw samples with "
"edge triggering: as soon as the signal rises by at least “trigger on edge” within "
"8 samples, the trigger point is placed at one fifth of the screen (blue mark), and "
"the pulse stays in place. The <b>Auto</b> / <b>Normal</b> switch: in auto, with no "
"edge for 0.15 s the sweep runs freely so the baseline and noise are visible; normal "
"refreshes only on an edge and holds the last captured pulse. The window width is "
"set by “timebase” &mdash; from 64 to 32768 samples (at 8 MHz that is 4 ms), the "
"time per division is shown on the plot. “Amplitude from&ndash;to”: amplitude "
"triggering &mdash; a frame is shown only if the pulse height above the baseline is "
"within this range of ADC codes (0 and 0 &mdash; no filter); the height of the shown "
"pulse and how many pulses were rejected are shown under the plot. “X axis”: grid "
"labels in microseconds or in samples. Samples are counted from the trigger point "
"(blue mark = 0, negative before it), so the numbers for L, G, “before/after peak”, "
"re-arm and peak search are read straight from the screen. <b>Ruler</b>: a click on "
"the plot places marks A (start), B (peak &mdash; snaps to the maximum near the "
"click) and C (end); marks can be dragged. Under the plot &mdash; the distances "
"A&rarr;B, B&rarr;C, A&rarr;C in samples and microseconds and the height of the "
"peak; the “set before / after peak” button copies B&minus;A and C&minus;B into the "
"integration parameters. The trapezoid is not drawn &mdash; it is computed in the "
"device, and the “filter noise before the pulse” line shows whether the threshold is "
"above its noise. Here too, under the “settings” checkbox, is the table of all "
"processing parameters: they are easier to tune while looking at the pulses. The "
"spectrum is not acquired in this mode. “Y axis from”: from the baseline, from the "
"middle of the scale (2048) or in true ADC codes. “Full scale” shows the whole ADC "
"range, and the “headroom from baseline” line shows how many codes are left from the "
"baseline to the ceiling and the floor.</td></tr>"
"<tr><td>Diagnostics</td><td>The “diagnostics” checkbox on the “Spectrum” and "
"“Oscilloscope” tabs shows the ADC path under the plot: the sample stream and "
"its real rate, camera interface registers, the last-second profile "
"(processing busy time, cycles per sample, requests, memory, WiFi signal), "
"the chunk loss log, per-bit statistics of the data lines and their check. "
"There is no separate page any more.</td></tr>"
"</table></div>"

"</section></div></body></html>";

static esp_err_t h_help(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, HELP_PAGE, HTTPD_RESP_USE_STRLEN);
}


/* ---- ВЫГРУЗКА СПЕКТРА ----
 * Форматы и разметка - общепринятые у программ для гамма-спектров,
 * чтобы файлы открывались ими без переделки: XML - ResultDataFile
 * (BecqMoni), CSV - заголовок и строки «канал,счёт», N42 - ANSI
 * N42.42-2011, SPE - ЛСРМ
 * SpectraLine (текстовый заголовок и счёты uint32 little-endian).
 * Калибровки в кэВ у нас нет - её блоки в файлах просто пропускаются.
 * Выгружается то, что видно на экране:
 * первые nch каналов.
 * Часов реального времени у прибора нет, поэтому время замера
 * присылает страница: t - Unix-время в секундах, tz - смещение пояса
 * в минутах. Без них время помечается как неизвестное. */
typedef struct {
    uint32_t *d;          /* снимок каналов 0..n-1                     */
    size_t    n;
    uint64_t  sum;        /* событий в выгружаемых каналах             */
    uint64_t  total;      /* всего найдено импульсов, с отбракованными */
    double    treal, tlive;
    struct tm ts, te;     /* начало и конец замера, местное время      */
    bool      tvalid;
    /* httpd_resp_set_hdr запоминает указатель, а не копию: строка
     * обязана жить до конца отправки, поэтому она здесь, а не на стеке
     * вспомогательной функции */
    char      disp[96];
} exp_snap_t;

#define TMA(t) (t)->tm_year + 1900, (t)->tm_mon + 1, (t)->tm_mday, \
               (t)->tm_hour, (t)->tm_min, (t)->tm_sec

static bool exp_snap(httpd_req_t *r, exp_snap_t *s)
{
    mca_params_t p;
    mca_stats_t  st;
    mca_dsp_get_params(&p);
    mca_dsp_get_stats(&st);

    /* снимок целиком, иначе сумма в заголовке разошлась бы с каналами */
    s->n = (size_t)p.nch > MCA_CHANNELS ? MCA_CHANNELS : (size_t)p.nch;
    s->d = heap_caps_malloc(s->n * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s->d) s->d = malloc(s->n * sizeof(uint32_t));
    if (!s->d) return false;
    for (size_t off = 0; off < s->n; off += 512)
        mca_dsp_get_spectrum(s->d + off, off,
                             s->n - off < 512 ? s->n - off : 512);
    s->sum = 0;
    for (size_t i = 0; i < s->n; i++) s->sum += s->d[i];
    s->total = st.total_events + st.skipped_pileup;

    /* живое время - за вычетом мёртвого, та же доля, что в статусе */
    s->treal = st.run_ms / 1000.0;
    double dead = st.samples_processed
                ? (double)st.skipped_deadtime / (double)st.samples_processed
                : 0.0;
    s->tlive = s->treal * (1.0 - dead);

    char q[64], v[24];
    long long t = 0;
    int tz = 0;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "t", v, sizeof(v)) == ESP_OK)
            t = atoll(v);
        if (httpd_query_key_value(q, "tz", v, sizeof(v)) == ESP_OK)
            tz = atoi(v);
    }
    /* страница не прислала время - берём часы прибора (UTC), если заданы */
    if (t <= 1500000000LL && mca_time_now_ms()) {
        t  = mca_time_now_ms() / 1000;
        tz = 0;
    }
    s->tvalid = t > 1500000000LL;
    /* местное время считаем как UTC со сдвигом пояса - так не нужна
     * настройка часового пояса на самом приборе */
    time_t te = (time_t)(t + (long long)tz * 60);
    time_t ts = te - (time_t)(s->treal + 0.5);
    gmtime_r(&te, &s->te);
    gmtime_r(&ts, &s->ts);
    return true;
}

static void exp_head(httpd_req_t *r, exp_snap_t *s, const char *ext,
                     const char *type)
{
    if (s->tvalid)
        snprintf(s->disp, sizeof(s->disp),
                 "attachment; filename=\"mca_%04d%02d%02d_%02d%02d%02d.%s\"",
                 TMA(&s->te), ext);
    else
        snprintf(s->disp, sizeof(s->disp),
                 "attachment; filename=\"mca_spectrum.%s\"", ext);
    httpd_resp_set_type(r, type);
    httpd_resp_set_hdr(r, "Content-Disposition", s->disp);
}

static void exp_xml(httpd_req_t *r, exp_snap_t *s, char *b)
{
    exp_head(r, s, "xml", "application/xml");
    httpd_resp_sendstr_chunk(r,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<ResultDataFile xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\r\n"
        "  <FormatVersion>120920</FormatVersion>\r\n"
        "  <ResultDataList>\r\n"
        "    <ResultData>\r\n");
    if (!s->tvalid)
        httpd_resp_sendstr_chunk(r,
            "      <!-- TIME UNKNOWN: the device has no clock -->\r\n");
    int n = snprintf(b, 4096,
        "      <SampleInfo>\r\n"
        "        <Name>MCA AD9226</Name>\r\n"
        "        <Location />\r\n"
        "        <Time>%04d-%02d-%02dT%02d:%02d:%02d</Time>\r\n"
        "        <Weight>1</Weight>\r\n"
        "        <Volume>1</Volume>\r\n"
        "        <Note />\r\n"
        "      </SampleInfo>\r\n"
        "      <DeviceConfigReference>\r\n"
        "        <Name>MCA AD9226</Name>\r\n"
        "        <Guid>00000000-0000-0000-0000-000000000000</Guid>\r\n"
        "      </DeviceConfigReference>\r\n"
        "      <StartTime>%04d-%02d-%02dT%02d:%02d:%02d</StartTime>\r\n"
        "      <EndTime>%04d-%02d-%02dT%02d:%02d:%02d</EndTime>\r\n"
        "      <PresetTime>0</PresetTime>\r\n"
        "      <EnergySpectrum>\r\n"
        "        <NumberOfChannels>%u</NumberOfChannels>\r\n"
        "        <ChannelPitch>1</ChannelPitch>\r\n"
        "        <ValidPulseCount>%" PRIu64 "</ValidPulseCount>\r\n"
        "        <TotalPulseCount>%" PRIu64 "</TotalPulseCount>\r\n"
        "        <MeasurementTime>%lu</MeasurementTime>\r\n"
        "        <LiveTime>%.1f</LiveTime>\r\n"
        "        <NumberOfSamples>1</NumberOfSamples>\r\n"
        "        <Spectrum>\r\n",
        TMA(&s->ts), TMA(&s->ts), TMA(&s->te), (unsigned)s->n,
        s->sum, s->total, (unsigned long)(s->treal + 0.5), s->tlive);
    httpd_resp_send_chunk(r, b, n);
    for (size_t i = 0; i < s->n; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < s->n; j++, i++)
            pos += snprintf(b + pos, 4096 - pos,
                            "          <DataPoint>%" PRIu32 "</DataPoint>\r\n",
                            s->d[i]);
        httpd_resp_send_chunk(r, b, pos);
    }
    httpd_resp_sendstr_chunk(r,
        "        </Spectrum>\r\n"
        "      </EnergySpectrum>\r\n"
        "      <PulseCollection>\r\n"
        "        <Format>Base64 encoded binary</Format>\r\n"
        "        <Pulses />\r\n"
        "      </PulseCollection>\r\n"
        "    </ResultData>\r\n"
        "  </ResultDataList>\r\n"
        "</ResultDataFile>\r\n");
}

static void exp_csv(httpd_req_t *r, exp_snap_t *s, char *b)
{
    /* простой CSV: одна строка-заголовок, дальше
     * «канал,счёт» с канала 0, без пробелов, окончания CRLF */
    exp_head(r, s, "csv", "text/csv");
    int n = snprintf(b, 4096, "Channel,Counts (TotalTime=%.1fs%s)\r\n",
                     s->treal, s->tvalid ? "" : "; TIME UNKNOWN");
    httpd_resp_send_chunk(r, b, n);
    for (size_t i = 0; i < s->n; ) {
        int pos = 0;
        for (int j = 0; j < 200 && i < s->n; j++, i++)
            pos += snprintf(b + pos, 4096 - pos, "%u,%" PRIu32 "\r\n",
                            (unsigned)i, s->d[i]);
        httpd_resp_send_chunk(r, b, pos);
    }
}

static void exp_n42(httpd_req_t *r, exp_snap_t *s, char *b)
{
    /* UTF-8 BOM, CRLF, без перевода строки в конце,
     * значения ChannelData через пробел с замыкающим пробелом */
    exp_head(r, s, "n42", "application/octet-stream");
    uint32_t r0 = esp_random(), r1 = esp_random();
    uint32_t r2 = esp_random(), r3 = esp_random();
    int n = snprintf(b, 4096,
        "\xEF\xBB\xBF"
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        "<RadInstrumentData xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\""
        " xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""
        " n42DocUUID=\"%08lx-%04lx-%04lx-%04lx-%04lx%08lx\""
        " xmlns=\"http://physics.nist.gov/N42/2011/N42\">\r\n",
        (unsigned long)r0,
        (unsigned long)(r1 >> 16), (unsigned long)(r1 & 0xffff),
        (unsigned long)(r2 >> 16), (unsigned long)(r2 & 0xffff),
        (unsigned long)r3);
    httpd_resp_send_chunk(r, b, n);
    if (!s->tvalid)
        httpd_resp_sendstr_chunk(r,
            "  <Remark>TIME UNKNOWN: the device has no clock</Remark>\r\n");
    httpd_resp_sendstr_chunk(r,
        "  <RadInstrumentInformation id=\"RadInstrument\">\r\n"
        "    <RadInstrumentManufacturerName>DIY</RadInstrumentManufacturerName>\r\n"
        "    <RadInstrumentModelName>MCA AD9226</RadInstrumentModelName>\r\n"
        "    <RadInstrumentClassCode>Radionuclide Identifier</RadInstrumentClassCode>\r\n"
        "    <RadInstrumentVersion>\r\n"
        "      <RadInstrumentComponentName>Hardware</RadInstrumentComponentName>\r\n"
        "      <RadInstrumentComponentVersion>ESP32-S3 + AD9226</RadInstrumentComponentVersion>\r\n"
        "    </RadInstrumentVersion>\r\n"
        "  </RadInstrumentInformation>\r\n");
    n = snprintf(b, 4096,
        "  <RadMeasurement id=\"SpectrumMeasurement-0\">\r\n"
        "    <MeasurementClassCode>Foreground</MeasurementClassCode>\r\n"
        "    <StartDateTime>%02d.%02d.%04d %02d:%02d:%02d</StartDateTime>\r\n"
        "    <RealTimeDuration>PT%luS</RealTimeDuration>\r\n"
        "    <Spectrum id=\"SpectrumData\" radDetectorInformationReference=\"Detector\">\r\n"
        "      <LiveTimeDuration>PT%.1fS</LiveTimeDuration>\r\n"
        "      <ChannelData compressionCode=\"None\">",
        s->ts.tm_mday, s->ts.tm_mon + 1, s->ts.tm_year + 1900,
        s->ts.tm_hour, s->ts.tm_min, s->ts.tm_sec,
        (unsigned long)(s->treal + 0.5), s->tlive);
    httpd_resp_send_chunk(r, b, n);
    for (size_t i = 0; i < s->n; ) {
        int pos = 0;
        for (int j = 0; j < 80 && i < s->n; j++, i++)
            pos += snprintf(b + pos, 4096 - pos, "%" PRIu32 " ", s->d[i]);
        httpd_resp_send_chunk(r, b, pos);
    }
    n = snprintf(b, 4096,
        "</ChannelData>\r\n"
        "    </Spectrum>\r\n"
        "    <GrossCounts id=\"GrossForeground\" radDetectorInformationReference=\"Detector\">\r\n"
        "      <TotalCounts>%" PRIu64 "</TotalCounts>\r\n"
        "    </GrossCounts>\r\n"
        "  </RadMeasurement>\r\n"
        "</RadInstrumentData>",
        s->total);
    httpd_resp_send_chunk(r, b, n);
}

static void exp_spe(httpd_req_t *r, exp_snap_t *s, char *b)
{
    /* заголовок KEY=VALUE, маркер SPECTR=, затем счёты uint32
     * little-endian подряд - ESP32 сам little-endian, шлём как есть */
    exp_head(r, s, "spe", "application/octet-stream");
    int pos = snprintf(b, 4096,
        "SHIFR=MCA AD9226\r\n"
        "CONFIGNAME=MCA AD9226\r\n"
        "MEASBEGIN=%02d-%02d-%02d %02d:%02d:%02d.00\r\n"
        "TLIVE=%.2f\r\n"
        "TREAL=%.2f\r\n"
        "DETECTOR=MCA AD9226\r\n"
        "%s"
        "SPECTRSIZE=%u\r\n"
        "SPECTR=",
        s->ts.tm_mday, s->ts.tm_mon + 1, (s->ts.tm_year + 1900) % 100,
        s->ts.tm_hour, s->ts.tm_min, s->ts.tm_sec,
        s->tlive, s->treal,
        s->tvalid ? "" : "COMMENT=TIME UNKNOWN (the device has no clock)\r\n",
        (unsigned)s->n);
    httpd_resp_send_chunk(r, b, pos);
    for (size_t i = 0; i < s->n; ) {
        size_t cnt = s->n - i < 1024 ? s->n - i : 1024;
        httpd_resp_send_chunk(r, (const char *)(s->d + i), cnt * 4);
        i += cnt;
    }
}

/* /export.xml, .csv, .n42, .spe - формат передаётся через user_ctx */
static esp_err_t h_export(httpd_req_t *r)
{
    const char *f = (const char *)r->user_ctx;
    exp_snap_t s;
    if (!exp_snap(r, &s)) return httpd_resp_send_500(r);
    char *b = malloc(4096);
    if (!b) {
        free(s.d);
        return httpd_resp_send_500(r);
    }
    switch (f[0]) {
    case 'x': exp_xml(r, &s, b); break;
    case 'c': exp_csv(r, &s, b); break;
    case 'n': exp_n42(r, &s, b); break;
    default:  exp_spe(r, &s, b); break;
    }
    esp_err_t e = httpd_resp_send_chunk(r, NULL, 0);
    free(b);
    free(s.d);
    return e;
}

static esp_err_t h_css(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/css; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    return httpd_resp_send(r, CSS, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_ljs(httpd_req_t *r)
{
    httpd_resp_set_type(r, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
    return httpd_resp_send(r, LJS, HTTPD_RESP_USE_STRLEN);
}

/* Логотип вшит в прошивку (EMBED_FILES в CMakeLists.txt). */
extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[]   asm("_binary_logo_png_end");

static esp_err_t h_logo(httpd_req_t *r)
{
    httpd_resp_set_type(r, "image/png");
    /* между прошивками не меняется - пусть браузер держит его в кэше */
    httpd_resp_set_hdr(r, "Cache-Control", "max-age=86400");
    return httpd_resp_send(r, (const char *)logo_png_start,
                           logo_png_end - logo_png_start);
}

esp_err_t mca_web_start(void)
{
    net_init();

    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 28;
    cfg.stack_size       = 8192;
    cfg.core_id          = 0;           /* веб на ядро 0, DSP на ядро 1 */
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) return err;

    /* именованные поля: при включённом WebSocket у httpd_uri_t есть ещё
     * is_websocket и др., позиционная запись оставляла их без значения */
#define URI(p, h, c) { .uri = p, .method = HTTP_GET, .handler = h, .user_ctx = c }
    httpd_uri_t u[] = {
        URI("/", h_root, NULL),
        URI("/s.css", h_css, NULL),
        URI("/l.js", h_ljs, NULL),
        URI("/logo.png", h_logo, NULL),
        URI("/spectrum", h_spectrum, NULL),
        URI("/scope", h_scope, NULL),
        URI("/stat", h_stat, NULL),
        URI("/cfg", h_cfg, NULL),
        URI("/cmd", h_cmd, NULL),
        URI("/wifi", h_wifi_page, NULL),
        URI("/wifi/status", h_wifi_status, NULL),
        URI("/wifi/set", h_wifi_set, NULL),
        URI("/net/set", h_net_set, NULL),
        URI("/diag/data", h_diag_data, NULL),
        URI("/diag/set", h_diag_set, NULL),
        URI("/diag/probe", h_diag_probe, NULL),
        URI("/help", h_help, NULL),
        URI("/time", h_time, NULL),
        URI("/hist", h_hist, NULL),
        URI("/export.xml", h_export, (void *)"xml"),
        URI("/export.csv", h_export, (void *)"csv"),
        URI("/export.n42", h_export, (void *)"n42"),
        URI("/export.spe", h_export, (void *)"spe"),
    };
#undef URI
    for (int i = 0; i < sizeof(u) / sizeof(u[0]); i++)
        httpd_register_uri_handler(srv, &u[i]);

    /* осциллограф по WebSocket */
    s_srv = srv;
    static const httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = h_ws,
        .user_ctx = NULL, .is_websocket = true,
    };
    httpd_register_uri_handler(srv, &ws);
    s_codec_ok = scodec_selftest();
    ESP_LOGI(TAG, "сжатие осциллограммы: %s", s_codec_ok
             ? "самопроверка пройдена"
             : "самопроверка НЕ пройдена - кадры идут без сжатия");
    xTaskCreatePinnedToCore(ws_task, "ws_scope", 3072, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "веб-сервер запущен");
    return ESP_OK;
}
