#include "mca_web.h"
#include "mca_dsp.h"
#include "mca_prof.h"
#include "mca_settings.h"
#include "adc_cap.h"
#include "adc_clk.h"
#include "mca_diag.h"

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
"@media (max-width:620px){.big{font-size:20px}.big.ac,.big.w{font-size:18px}"
"header{padding:8px 12px}.wrap{padding:12px 10px 0}.brand img{height:44px}}";

static const char PAGE[] =
"<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>MCA · AD9226</title><link rel=stylesheet href=/s.css></head><body>"
"<header><div class=brand><img src=/logo.png?v=2 alt=''>MCA "
"<span style=color:var(--accent)>AD9226</span></div>"
"<div class=chips><span class=chip id=runchip><span class='dot off' id=rundot></span>"
"<span id=runtxt>нет связи</span></span><span class=chip id=fchip>&mdash;</span></div></header>"
/* Режимы - вкладками. Сам режим живёт в скрытом
   списке md: на него завязаны опрос и синхронизация с прибором. */
"<nav id=tabs><span data-md=0 onclick=tab(0)>Спектр</span>"
"<span data-md=1 onclick=tab(SCM)>Конфиг MCA</span>"
"<a href=/diag>Диагностика</a>"
"<a href=/wifi>WiFi</a><a href=/help>Справка</a></nav>"
"<select id=md hidden onchange=setmode()><option value=0><option value=1>"
"<option value=2><option value=3></select>"
"<div class=wrap>"
"<div class=status id=st>подключение...</div>"
"<section class=panel>"
"<div class=row><div class=grp>"
"<button class='btn green' onclick=run(1)>&#9654; Старт</button>"
"<button class='btn amber' onclick=run(0)>&#9632; Стоп</button>"
"<button class='btn red' id=bclr onclick=cmd('clear')>&#8634; Сброс</button></div>"
"<div class=ml id=g_big style='display:flex;align-items:center;gap:16px;flex-wrap:wrap'>"
"<span class=big id=bt>00:00:00</span><span class=mut>|</span>"
"<span class='big ac'><span id=bc>0</span><span class=mut style=font-size:12px> CPS</span></span>"
"<span class=mut>|</span>"
"<span class='big w'><span id=be>0</span><span class=mut style=font-size:12px> событий</span></span>"
"</div></div>"
"<div class=row>"
"<span class=grp id=g_sp><span class=lbl>Шкала Y</span>"
"<span class=seg><button id=blin onclick=setLog(0)>Lin</button>"
"<button id=blog class=on onclick=setLog(1)>Log</button></span>"
"<input type=checkbox id=lg checked hidden>"
"<span class=lbl style=margin-left:8px>пик от</span><input id=pka type=number value=0 onchange=poll()>"
"<span class=lbl>до</span><input id=pkb type=number value=0 onchange=poll()>"
"<span class=lbl style=margin-left:8px title='Сохранить спектр файлом: "
"XML - ResultDataFile (BecqMoni), CSV - канал и счёт, N42 - ANSI N42.42, SPE - SpectraLine. "
"Выгружаются каналы, видимые на экране; время замера берётся из часов компьютера.'>выгрузить</span>"
"<button class='btn sm' onclick=expo('xml')>XML</button>"
"<button class='btn sm' onclick=expo('csv')>CSV</button>"
"<button class='btn sm' onclick=expo('n42')>N42</button>"
"<button class='btn sm' onclick=expo('spe')>SPE</button></span>"
"<span class=grp id=g_sc>"
"<span class=seg><button id=bauto onclick=scm(1) title='синхронизация по фронту, а без фронта - свободный пуск'>Авто</button>"
"<button id=bwait onclick=scm(3) title='только по фронту, держит последний пойманный импульс'>Ждущий</button></span>"
"<span class=lbl title='Перепад сигнала в сторону импульса (с учётом полярности) за 8 отсчётов, коды АЦП. Выше шума, но ниже амплитуды нужных импульсов.'>"
"синхр. по фронту &ge;</span><input id=tl type=number value=30>"
"<span class=lbl title='Синхронизация по амплитуде: показывать только импульсы, у которых высота над базой (коды АЦП, с учётом полярности) в этом диапазоне. 0 и 0 - без фильтра. Высота показанного импульса и сколько отброшено - под графиком.'>"
"амплитуда от</span><input id=amin type=number value=0 min=0>"
"<span class=lbl>до</span><input id=amax type=number value=0 min=0>"
"<span class=lbl title='От какой линии отсчитывать ось Y'>ось Y от</span>"
"<span class=seg><button id=rf0 onclick=setRef(0) title='от измеренной базовой линии'>базы</button>"
"<button id=rf1 onclick=setRef(1) title='от середины шкалы АЦП, код 2048; на плате со входом ±5 В это 0 В'>2048</button>"
"<button id=rf2 onclick=setRef(2) title='настоящие коды АЦП 0..4095'>кодов</button></span>"
"<label title='вся шкала АЦП: видно, сколько осталось до потолка и до пола'>"
"<input type=checkbox id=fx onchange=vis()> вся шкала</label></span>"
"<span class=grp id=g_zm>"
"<span class=lbl title='Сколько отсчётов на экране. Время на деление подписано на графике.'>развёртка</span>"
"<select id=zm><option value=0>авто</option><option value=64>64</option>"
"<option value=128>128</option><option value=256>256</option>"
"<option value=512>512</option><option value=1024>1024</option>"
"<option value=2048>2048</option><option value=4096>4096</option>"
"<option value=8192>8192</option><option value=16384>16384</option>"
"<option value=32768>32768 отсч</option></select>"
"<span class=lbl title='Подписи сетки по горизонтали: время или номера отсчётов. Отсчёты считаются от момента синхронизации (синяя метка = 0), до неё - отрицательные: так прямо с экрана читаются числа для L, G, «до/после вершины», перезапуска и поиска пика.'>ось X</span>"
"<span class=seg><button id=bx0 onclick=setX(0)>мкс</button>"
"<button id=bx1 onclick=setX(1)>отсч</button></span>"
"<span class=lbl title='0 = автоматически'>Y max</span><input id=ymax type=number value=0></span>"
"<label id=g_shs><input type=checkbox id=shset onchange=vis()> настройки</label>"
"<span class='ml mut' id=g_leg style=font-size:11.5px>"
"линейка: щелчок по графику ставит <b style=color:#7ee081>A</b> начало, "
"<b style=color:#f0c45a>B</b> вершину, <b style=color:#ff8a82>C</b> конец; метки можно тащить</span>"
"</div></section>"
"<section class='panel pad'><canvas id=cv height=320></canvas>"
"<div id=g_rul class=grp style='margin-top:8px'>"
"<span id=rult class=mono style=font-size:12px></span><span class=ml></span>"
"<button id=rset class='btn sm' onclick=rulSet() title='До вершины = B−A, После вершины = C−B. "
"Поля заполнятся в настройках, в прибор уйдут кнопкой «Применить». Действуют при способе «интегрирование».'>"
"в «до / после вершины»</button>"
"<button class='btn sm' onclick=rulClr()>сбросить</button></div></section>"
"<section class='panel pad'><div id=sc class=mono style=min-height:110px></div></section>"
"<section class='panel pad' id=g_set>"
"<div style=overflow-x:auto><table class=ptab id=pbox></table></div>"
"<div class=grp style=margin-top:10px><button class='btn green' onclick=apply()>Применить</button>"
"<span class=mut style=font-size:11.5px>в прибор значения уходят только по этой кнопке и "
"сохраняются в его памяти</span></div></section>"
"</div>"
"<script>"
/* Список частот присылает прибор (/cfg, поле fl): он зависит от
   генератора CLK в прошивке. Выбор уходит номером в этом списке.
   Пометки к верхним частотам: по замеру на 16 МГц обработка спектра
   занимает ~85 % ядра, дальше растёт пропорционально частоте. */
"var FHZ=[];"
"function fqName(h){return +(h/1e6).toFixed(2)+' МГц'+"
"(h>17e6&&h<18e6?' (предел)':h>=19e6?' (осциллограф)':'')}"
/* ТАБЛИЦА НАСТРОЕК. Третий столбец - на что параметр влияет:
   b - при любом способе, t - только трапеция, i - только интегрирование.
   Сверено с mca_dsp.c: событие в обоих способах ищется по трапеции
   (порог, L, G, гистерезис, перезапуск, поиск пика, наложения; по L и G
   при интегрировании ещё и находится вершина), а амплитуда считается
   по-разному. Параметры другого способа не прячем, а приглушаем:
   видно, что они есть и что сейчас не действуют. */
"var GR=[['Обнаружение импульса',["
"['threshold','Порог','b','Порог по выходу трапеции, коды АЦП. Ставить выше шума: см. «шум фильтра» под графиком'],"
"['fq','Частота','b','Частота АЦП. Выше - подробнее форма импульса, но больше нагрузка'],"
"['polarity','Полярность','b','0 - импульсы вверх, 1 - вниз'],"
"['trap_L','Окно L','b','Окно трапеции, отсч. При трапеции задаёт ещё и амплитуду'],"
"['trap_G','Зазор G','b','Отступ вычитаемого окна, отсч. G−L - длина плоской вершины'],"
"['hysteresis','Гистерезис %','b','Ниже какой доли порога должна упасть трапеция, чтобы ловить следующий'],"
"['rearm','Перезапуск','b','Отсч. после пика, когда новые импульсы не ловятся'],"
"['search','Поиск пика','b','Сколько отсч. после порога искать вершину']]],"
"['Амплитуда',["
"['algo','Способ измерения','','Трапеция или интегрирование. Смена очищает спектр'],"
"['flat_avg','Средн. по плато','t','1 - среднее по плоской вершине вместо максимума. Работает только при G−L ≥ 16'],"
"['int_rise','До вершины','i','Отсч. до вершины в среднем. Линейка: B−A'],"
"['int_fall','После вершины','i','Отсч. после вершины. Линейка: C−B. Длиннее окно - пик левее'],"
"['baseline_shift','База 2^N','i','Скорость слежения за базовой линией (трапеции база не нужна)'],"
"['baseline_win','Окно базы','i','Отсчёты дальше этого от базы в неё не берутся, коды']]],"
"['Шкала спектра',["
"['cpc','Кодов на канал','b','Канал = амплитуда / это число. Смена очищает спектр. <b id=cpctop></b>'],"
"['nch','Каналов','b','Длина шкалы вправо. Спектр не сбрасывает']]],"
"['Отбраковка наложений',["
"['pileup_pre_pct','Наложение до %','b','Брак, если перед пиком трапеция выше этого % амплитуды. 0 - выключено'],"
"['pileup_post_pct','Наложение после %','b','Брак, если после пика не упала ниже этого %. Работают, только если оба больше 0']]]];"
"var P=[];GR.forEach(function(g){g[1].forEach(function(r){if(r[0]!='fq')P.push(r[0])})});"
/* пометка только у параметров одного способа; без пометки - действует всегда */
"var BDG={t:['трапеция','t'],i:['интегрирование','i']};"
/* поля выпадающими меню: значение и подпись */
"var SEL={algo:[['0','трапеция'],['1','интегрирование']],"
"nch:[['2048','2048'],['4096','4096'],['8192','8192']]};"
"var H={"
"fq:'Частота семплирования АЦП. Выше - подробнее форма импульса, но обработка может не успевать: смотрите потерянные чанки на странице Диагностика. На 16 МГц обработка спектра занимает около 85 % ядра, на 17.14 - около 92 % (предел), на 20 МГц спектр не успевает - она для осциллографа. Гармоники CLK могут мешать WiFi: если на какой-то частоте страница начинает замирать, а пинг до прибора растёт, смените частоту или канал роутера. Параметры фильтра заданы в отсчётах, поэтому на другой частоте то же L или G - другое время.',"
"polarity:'0 - импульсы вверх от базовой линии, 1 - вниз (например, анод ФЭУ напрямую). При 1 сигнал переворачивается ещё до обработки, и порог и спектр работают как с импульсами вверх; осциллограф синхронизируется по фронту вниз. Постоянная составляющая сигнала на обработку не влияет - её вычитает трапеция.',"
"algo:'Трапеция: амплитуда по вершине трапеции (окно L, зазор G, по желанию среднее по плато). Интегрирование: среднее отсчётов импульса вокруг вершины за вычетом базовой линии (до и после вершины, база 2^N, окно базы). Поля, которые при выбранном способе ни на что не влияют, в таблице приглушены. Обнаружение события в обоих способах одинаковое - по выходу трапеции, поэтому L, G, порог, гистерезис, перезапуск и поиск пика нужны всегда. Масштабы способов различаются - после переключения подберите «Кодов на канал». Смена способа очищает спектр.',"
"int_rise:'Интегрирование: сколько отсчётов ДО вершины включать в сумму. Смотрите на картинку импульса и считайте по сетке.',"
"int_fall:'Интегрирование: сколько отсчётов ПОСЛЕ вершины включать в сумму. Обычно заметно больше, чем до вершины, потому что спад длиннее фронта.',"
"cpc:'Сколько кодов амплитуды приходится на один канал спектра: 1 - канал равен коду АЦП, 0.5 - вдвое подробнее, 2 - вдвое грубее. Верх шкалы = каналов x кодов на канал, он подписан рядом. При базе в середине АЦП запас вверх около 2047 кодов, поэтому 2048 каналов по 1 коду как раз покрывают всю шкалу. Амплитуда в обоих способах в кодах: трапеция - высота импульса, интегрирование - средняя высота в окне. Изменение этого числа очищает спектр: старые события разложены по другой шкале.',"
"nch:'Сколько каналов показывать: 2048, 4096 или 8192. Это длина шкалы: верх = каналов x кодов на канал. Раскладка событий по каналам от этого не зависит - спектр не сжимается и не сбрасывается, меняется только, докуда видно вправо. Прибор копит все 8192 канала, так что переключать можно в любой момент.',"
"threshold:'Порог по выходу трапеции, В КОДАХ АЦП (нормирован на длину окна, поэтому не зависит от L). Ставится выше шума трапеции - см. строку «шум фильтра» на вкладке «Конфиг MCA».',"
"hysteresis:'Доля порога В ПРОЦЕНТАХ, ниже которой должна опуститься трапеция, чтобы детектор снова взвёлся. 50 означает половину порога. Прежний вариант вычитал единицу, что означало почти полное отсутствие гистерезиса.',"
"trap_L:'Длина окна интегрирования: сколько отсчётов импульса суммируется. Именно это окно определяет, какая часть импульса пойдёт в амплитуду, и насколько усреднится шум. ВНИМАНИЕ: в приборах, где окно задаётся парой RISE и FALL, окно интегрирования - это их сумма: RISE=6 FALL=15 соответствует нашему L=21. При интегрировании L и G тоже работают: по трапеции находится импульс и место его вершины.',"
"trap_G:'На сколько отсчётов назад отстоит второе окно, которое вычитается. Оно должно целиком лежать на базовой линии ДО импульса, поэтому G обязан быть больше L плюс длительность фронта. Разность G минус L даёт длину плоской вершины.',"
"rearm:'Сколько отсчётов жёстко пропустить после пика, независимо от гистерезиса. Защита от дребезга на спадающем хвосте. Это НЕ метрика потерь, а параметр перезапуска детектора.',"
"search:'Сколько отсчётов после срабатывания порога перебирать в поисках максимума трапеции. Должно накрывать весь импульс.',"
"baseline_shift:'Только для интегрирования: трапеция вычитает базу сама. Постоянная времени фильтра, отслеживающего нулевой уровень. Больше значение - медленнее и плавнее подстройка. Обычно 8-12.',"
"flat_avg:'Только для трапеции. 1 = амплитуда как среднее по плоской вершине трапеции, 0 = по максимуму. Проверено симуляцией: усреднение выигрывает ТОЛЬКО при длинном плато (G минус L не меньше 19). При коротком плато максимум даёт даже меньший разброс, поэтому при (G-L) меньше 16 происходит автоматический откат на максимум. Смещение максимума растёт с шумом, но это постоянный сдвиг для всех амплитуд - уходит в калибровку, ширину пиков не портит.',"
"baseline_win:'Только для интегрирования. Окно приёма отсчёта в базовую линию, коды АЦП. В среднее берутся только отсчёты, лежащие ближе этого значения к текущей оценке нуля - иначе длинные хвосты импульсов утянут базовую линию вверх. Ставить примерно вдвое-втрое больше размаха шума.',"
"pileup_pre_pct:'Если ПЕРЕД пиком трапеция уже выше этого процента от амплитуды - импульс сидит на хвосте предыдущего, бракуем. Меньше процент - строже отбраковка.',"
"pileup_post_pct:'Если ПОСЛЕ пика трапеция не упала ниже этого процента - на импульс наложился следующий, бракуем. Меньше процент - строже отбраковка.',"
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
"document.getElementById('cpctop').textContent=c>0&&n?'Сейчас шкала до '+(+(c*n).toFixed(1))+' кодов.':''}"
"document.getElementById('p_algo').onchange=showFields;"
"document.getElementById('p_cpc').oninput=cpcTop;document.getElementById('p_nch').onchange=cpcTop;"
"function cmd(c){fetch('/cmd?do='+c)}"
/* Старт/Стоп относятся к открытой вкладке: на «Спектре» - набор
   спектра, на «Конфиг MCA» - только осциллограф. На паузе осциллограф
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
"document.getElementById('runtxt').textContent=sc?(on?'осциллограф идёт':'осциллограф на паузе'):"
"(on?'идёт набор':'остановлен');"
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
"cx.fillStyle='#6b756a';cx.fillText('мкс · '+(+ts.toFixed(3))+' мкс/дел',W-170,22)}"
"else{var o0=x0>0?x0:0,stp=Math.max(1,Math.round(nicestep(ns,10)));"
"for(var k=Math.ceil(-o0/stp);o0+k*stp<=ns;k++){var x=28+(o0+k*stp)*(W-30)/ns;"
"cx.strokeStyle='rgba(255,255,255,.035)';cx.beginPath();cx.moveTo(x,10);"
"cx.lineTo(x,H-20);cx.stroke();"
"cx.fillStyle='#6b756a';cx.fillText(k*stp,x-6,H-6)}"
"cx.fillStyle='#6b756a';cx.fillText('отсч · '+stp+' отсч/дел'+(x0>=0?' · 0 = синхр.':''),W-230,22)}"
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
"grid(W,H,0,lm,'лог. шкала',1,a.length);"
"line(b,'#34d3c0',0,lm,W,H)}else{"
"grid(W,H,0,m,'линейная шкала',0,a.length);"
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
"gridS(W,H,ymn,ym,a.length-1,o.us?'':'отсчёты от базовой линии',o.xs?0:o.us,o.tg);"
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
"function trapOf(a,L,G){var t=[],acc=0;"
"for(var n=0;n<a.length;n++){var v=a[n],"
"vL=(n-L>=0)?a[n-L]:a[0],vG=(n-G>=0)?a[n-G]:a[0],"
"vGL=(n-G-L>=0)?a[n-G-L]:a[0];var d=v-vL-vG+vGL;"
"acc+=d;t[n]=acc/L}return t}"
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
"if(mx<=0)return ' &nbsp; <span style=color:#ff8a82>в этом диапазоне пика нет</span>';"
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
"if(!(fw>0))return ' &nbsp; <span style=color:#ff8a82>полувысота не найдена - расширьте диапазон</span>';"
"var res=cen>0?100*fw/cen:0;"
"window.PKMARK=[A,B,xl,xr];"
"return '<br><b>пик:</b> центр '+cen.toFixed(1)+' кан &nbsp; '+"
"'FWHM '+fw.toFixed(1)+' кан &nbsp; <b>разрешение '+res.toFixed(2)+' %</b>'+"
"' <span style=opacity:.6>(площадь за вычетом фона '+Math.round(sw)+')</span>'}"
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
"sc.innerHTML=wait?(AF()?'ждущий режим: импульсов с амплитудой '+AF()+' кодов ещё не было'+"
"(j.rej>0?' (отброшено '+j.rej+')':''):"
"'ждущий режим: фронта &ge; '+j.lvl+' кодов ещё не было &mdash; снизьте «синхр. по фронту»'):"
"'ждём данные...';return}"
"var fh=window.REALHZ||8e6,tg=j.tg;window.LASTJ=j;window.LASTW=wait;"
"var L=+document.getElementById('p_trap_L').value,G=+document.getElementById('p_trap_G').value;"
"var Wn=Math.min(+document.getElementById('zm').value||512,a.length);"
/* без синхронизации пропускаем запас слева: там трапеция ещё не встала */
"var st=tg>=0?tg-Math.round(Wn*0.2):(L|0)+(G|0)+16;"
"if(st<0)st=0;if(st+Wn>a.length)st=a.length-Wn;"
"var tr=trapOf(a,L,G);"
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
"var sg=NEG()?-1:1,nmx=-1e9;for(var i=L+G;i<e;i++)if(sg*tr[i]>nmx)nmx=sg*tr[i];"
"var thr=+document.getElementById('p_threshold').value;"
"var sy=tg>=0?'<b style=color:#34d3c0>синхронизирован</b> по фронту '+j.rise+"
"' кодов (уровень '+j.lvl+')':"
"'<span style=color:#f0c45a>'+(AF()?'импульсов с амплитудой '+AF()+' кодов':'фронта &ge; '+j.lvl+' кодов')+"
"' нет &mdash; свободный пуск</span>';"
"if(wait&&j.age>1500)sy+=' &nbsp;<span style=opacity:.7>снимок '+(j.age/1000).toFixed(0)+' с назад</span>';"
/* высота показанного импульса и работа фильтра амплитуды */
"if(j.amp>=0)sy+=' &nbsp;высота импульса <b>'+j.amp+'</b> кодов';"
"if(AF())sy+=' &nbsp;<span class=mut>фильтр '+AF()+', отброшено с прошлого кадра '+(j.rej||0)+'</span>';"
"sc.innerHTML=sy+"
"'<br>на экране '+Wn+' отсч = '+(Wn/fh*1e6).toFixed(1)+' мкс из '+(j.len||a.length)+' отсч = '+"
"((j.len||a.length)/fh*1e6).toFixed(0)+' мкс записи'+"
"(window.SFPS?' &nbsp; обновление '+window.SFPS.toFixed(1)+' раз/с':'')+"
"'<br>сигнал: мин '+mn+'  макс '+mx+'  база '+bl.toFixed(0)+'  шум(СКО) '+sd.toFixed(1)+"
"((mx>=4095||mn<=0)?'  <b style=color:#ff8a82>упор в шкалу АЦП</b>':'')+"
/* сколько места осталось от базы до потолка и пола шкалы */
"'<br>запас по шкале от базы: вверх <b>'+(4095-Math.round(bl))+'</b>, вниз <b>'+"
"Math.round(bl)+'</b> кодов <span class=mut>(импульсы '+(NEG()?'вниз':'вверх')+')</span>'+"
"(nmx>-1e9?'<br>шум фильтра до импульса (трапеция считается в фоне): макс '+nmx.toFixed(1)+'  &nbsp; порог '+thr+"
"(nmx>thr?'  <b style=color:#ff8a82>шум перебивает порог</b>':"
"'  <b style=color:#34d3c0>порог выше шума</b>'):'')}"
/* ЛИНЕЙКА. Метки A - начало, B - вершина, C - конец импульса. Хранятся
   в отсчётах от момента синхронизации, поэтому стоят на месте от кадра
   к кадру (без синхронизации - от левого края экрана). Щелчок ставит
   следующую метку, после третьей - начинает заново; метку можно тащить.
   B при установке прилипает к вершине рядом со щелчком: на глаз её
   легко поставить на отсчёт мимо. */
"var RUL=[null,null,null],RCOL=['#7ee081','#f0c45a','#ff8a82'],HOV=null,DRAG=-1,RAF=0;"
"function scRedraw(){if(RAF||!window.LASTJ)return;"
"RAF=requestAnimationFrame(function(){RAF=0;drawScope(window.LASTJ,window.LASTW)})}"
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
"var t=(r>0?'+':'')+r+' отсч · '+(r*g.us).toFixed(2)+' мкс · '+g.a[HOV]+' кодов ('+(d>=0?'+':'')+d+' от базы)';"
"var tw=cx.measureText(t).width+8,tx=Math.min(x+8,g.W-tw-2);"
"cx.fillStyle='rgba(12,16,12,.88)';cx.fillRect(tx,30,tw,17);"
"cx.fillStyle='#e6ece2';cx.fillText(t,tx+4,42)}"
"cx.restore()}"
"function rulText(){var g=window.SCV,t='';"
"function ds(a,b){return (b-a)+' отсч ('+((b-a)*g.us).toFixed(2)+' мкс)'}"
"for(var k=0;k<3;k++)if(RUL[k]!=null)t+='ABC'[k]+' '+(RUL[k]>0?'+':'')+RUL[k]+'   ';"
"if(RUL[0]!=null&&RUL[1]!=null)t+='| фронт A→B '+ds(RUL[0],RUL[1])+'   ';"
"if(RUL[1]!=null&&RUL[2]!=null)t+='| спад B→C '+ds(RUL[1],RUL[2])+'   ';"
"if(RUL[0]!=null&&RUL[2]!=null)t+='| всего A→C '+ds(RUL[0],RUL[2])+'   ';"
"var ib=RUL[1]!=null?RUL[1]+g.z:-1;"
"if(ib>=0&&ib<g.n)t+='| высота B '+Math.round(g.sg*(g.a[ib]-g.bl))+' кодов от базы';"
"document.getElementById('rult').textContent=t||'меток нет: щёлкните по графику';"
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
"cv.addEventListener('pointerleave',function(){if(DRAG<0){HOV=null;scRedraw()}});"
/* Вкладки режимов и видимость управления: каждому режиму - свои поля. */
/* Авто и ждущий - один осциллограф: вкладка одна, режим развёртки
   переключается сегментом. SCM помнит последний выбранный. Режим 2
   (прежние «Импульсы») для прошивки тот же набор спектра - показываем
   его как вкладку «Спектр». */
/* Галочки «настройки» и «вся шкала»: выбор помнится в браузере. */
"function vis(){try{localStorage.setItem('mca_shset',document.getElementById('shset').checked?1:0);"
"localStorage.setItem('mca_fx',document.getElementById('fx').checked?1:0)}catch(e){}"
"window.LASTSIG='';showCtl()}"
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
"window.YREF=+(localStorage.getItem('mca_ref')||0);"
"window.XAX=+(localStorage.getItem('mca_xax')||0)}catch(e){window.YREF=0;window.XAX=0}"
"for(var i=0;i<3;i++)document.getElementById('rf'+i).className=i==window.YREF?'on':'';"
"for(var i=0;i<2;i++)document.getElementById('bx'+i).className=i==window.XAX?'on':'';"
"var SCM=1;"
"function showCtl(){var m=document.getElementById('md').value,sp=(m=='0'||m=='2'),sc=!sp;"
"cv.style.cursor=sc?'crosshair':'';cv.style.touchAction=sc?'none':'';"
"var v={g_sp:sp,g_sc:sc,g_zm:sc,g_leg:sc,g_rul:sc,g_big:sp,bclr:sp,st:sp,"
/* настройки - только на вкладке осциллографа: подбираются по импульсам */
"g_shs:sc,g_set:sc&&document.getElementById('shset').checked};"
"for(var k in v){var e=document.getElementById(k);if(e)e.style.display=v[k]?'':'none'}}"
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
"function poll(){"
"var md=document.getElementById('md').value;"
"if(md=='0'||md=='2'){fetch('/spectrum').then(r=>r.json()).then(function(j){"
"draw(j.d,document.getElementById('lg').checked);"
"var a=j.d,mx=0,tot=0,pk=0;"
"for(var i=0;i<a.length;i++){tot+=a[i];if(a[i]>mx){mx=a[i];pk=i}}"
"var s='всего в спектре: '+tot+'   максимум '+mx+' в канале '+pk+"
"'   каналов: '+a.length;"
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
"'<span class=w>'+j.ev+' <span class=mut>событий</span></span><span class=sep>|</span>'+"
"'<span title=\"зависит от Перезапуск и Поиск пика\"><span class=mut>мёртвое</span> '+dt.toFixed(1)+'%</span>'+"
"'<span class=sep>|</span><span><span class=mut>истинно</span> '+"
"(j.cps/Math.max(0.01,1-dt/100)).toFixed(0)+' имп/с</span>'+"
/* загрузка обработки: около 100% - не успевает, пойдут потерянные чанки.
   На вкладке «Конфиг MCA» спектр не набирается, и нагрузки нет. */
"'<span class=sep>|</span><span title=\"Какую долю ядра занимает обработка потока. '+"
"'Около 100% - не успевает, пойдут потерянные чанки: снизьте частоту.\"><span class=mut>загрузка</span> '+"
"(md=='1'||md=='3'?'—':((j.load||0)/10).toFixed(0)+'%')+'</span>'+"
/* векторная обработка не прошла самопроверку при старте - предупредить */
"(j.vec===0?'<span class=sep>|</span><span style=color:#f0c45a title=\"Векторные команды не прошли '+"
"'самопроверку при старте, обработка идёт обычным кодом - медленнее.\">векторы выкл.</span>':'');"
"document.getElementById('bt').textContent=hms(j.ms/1000);"
"document.getElementById('bc').textContent=j.cps;"
"document.getElementById('be').textContent=j.ev;"
"if(!(window.RLOCK>Date.now())){if(j.srun!==undefined)window.SRUN=j.srun;"
"if(isScope())runChip(j.srun,1);else runChip(j.run,0)}"
"document.getElementById('fchip').textContent=(j.freq/1e6).toFixed(2)+' МГц';"
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
"P.forEach(function(k){document.getElementById('p_'+k).value=k=='cpc'?p[k]/1000:p[k]});"
"showFields();cpcTop()})}})}"
/* ОСЦИЛЛОГРАФ опрашивается отдельно от спектра и статуса: следующий
   запрос уходит, как только отрисован предыдущий, но не чаще раза в
   SPER мс. Частота сама подстраивается под длину развёртки и скорость
   WiFi, и запросы не копятся в очередь. Просим только кусок под
   развёртку плюс запас L+G+16 слева, чтобы трапеция у края экрана
   успела установиться. Ответ двоичный: семь int32 и отсчёты по 2 байта. */
"var SPER=100;"
"function sparse(b){var v=new DataView(b),n=(b.byteLength-28)>>1,d=new Array(n);"
"for(var i=0;i<n;i++)d[i]=v.getUint16(28+2*i,true);"
"return {tg:v.getInt32(0,true),rise:v.getInt32(4,true),age:v.getInt32(8,true),"
"lvl:v.getInt32(12,true),len:v.getInt32(16,true),amp:v.getInt32(20,true),"
"rej:v.getInt32(24,true),d:d}}"
"function sloop(){var md=document.getElementById('md').value;"
"if(md!='1'&&md!='3'){window.SLAST=0;setTimeout(sloop,250);return}"
/* пауза: прибор не опрашиваем, на экране последний кадр */
"if(window.SRUN===0&&window.LASTJ){window.SLAST=0;window.SFPS=0;setTimeout(sloop,250);return}"
"var t0=Date.now(),Wn=+document.getElementById('zm').value||512,"
"M=(+document.getElementById('p_trap_L').value|0)+(+document.getElementById('p_trap_G').value|0)+16;"
"fetch('/scope?lvl='+(+document.getElementById('tl').value||30)+'&n='+(Wn+M)+'&pre='+(Math.round(Wn*0.2)+M)+"
"'&amin='+(+document.getElementById('amin').value||0)+'&amax='+(+document.getElementById('amax').value||0))"
".then(function(r){return r.arrayBuffer()}).then(function(b){"
"if(b.byteLength>=28)drawScope(sparse(b),md=='3');"
/* фактическая частота обновления, сглаженная - показывается под графиком */
"var now=Date.now();if(window.SLAST)window.SFPS=(window.SFPS||1000/(now-window.SLAST))*0.8+200/(now-window.SLAST);"
"window.SLAST=now})"
".catch(function(){}).then(function(){setTimeout(sloop,Math.max(10,SPER-(Date.now()-t0)))})}"
"setInterval(poll,1000);poll();sloop();"
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

static esp_err_t h_scope(httpd_req_t *r)
{
    /* Осциллограмма с синхронизацией по фронту. lvl в запросе -
     * уровень синхронизации, коды АЦП. Режим (авто / ждущий) задаётся
     * выбором режима прибора, а не этим запросом. */
    mca_prof_web_begin(PROF_EP_SCOPE);
    /* n - сколько отсчётов нужно странице, pre - сколько из них до
     * момента синхронизации. Отдаём только кусок под развёртку: время
     * ответа растёт с его длиной (см. LWIP_TCP_SND_BUF_DEFAULT). */
    char q[128], v[12];
    size_t want = DIAG_SCOPE_LEN, pre = DIAG_SCOPE_PRE;
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "lvl", v, sizeof(v)) == ESP_OK)
            mca_diag_set_trig_level(atoi(v));
        if (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
            int x = atoi(v);
            if (x > 0 && x < DIAG_SCOPE_LEN) want = (size_t)x;
        }
        if (httpd_query_key_value(q, "pre", v, sizeof(v)) == ESP_OK) {
            int x = atoi(v);
            if (x >= 0) pre = (size_t)x;
        }
        /* синхронизация по амплитуде: amin..amax кодов, amax=0 - выкл */
        int32_t lo = 0, hi = 0;
        if (httpd_query_key_value(q, "amin", v, sizeof(v)) == ESP_OK)
            lo = atoi(v);
        if (httpd_query_key_value(q, "amax", v, sizeof(v)) == ESP_OK)
            hi = atoi(v);
        mca_diag_set_amp_window(lo, hi);
    }
    /* прибор соберёт окно ровно под эту развёртку */
    mca_diag_set_scope_want(want);

    /* Ответ двоичный, little-endian: семь int32 (tg, rise, age, lvl,
     * длина всего окна, высота импульса, отброшено фильтром амплитуды),
     * затем отсчёты по uint16. Втрое короче JSON, и прибору не нужно
     * форматировать десятки тысяч чисел через snprintf - от этого и
     * зависит, сколько раз в секунду обновляется картинка. Буфер до 64 КБ
     * берём из PSRAM один раз (веб-сервер обрабатывает запросы по одному,
     * общий буфер безопасен). */
    enum { HDR = 7 * sizeof(int32_t) };
    static uint8_t *raw;
    /* В PSRAM. Во внутренней памяти пробовали - стало хуже: 32 КБ
     * отнимались у WiFi, а копия конкурировала с DMA захвата, и вернулись
     * потерянные чанки. */
    if (!raw) raw = heap_caps_malloc(HDR + DIAG_SCOPE_LEN * 2, MALLOC_CAP_SPIRAM);
    if (!raw) raw = malloc(HDR + DIAG_SCOPE_LEN * 2);
    if (!raw) {
        mca_prof_web_end(PROF_EP_SCOPE);
        return httpd_resp_send_500(r);
    }
    uint16_t *d = (uint16_t *)(raw + HDR);

    int32_t tg = -1, rise = 0, age = -1, amp = -1;
    uint32_t rej = 0;
    size_t full = 0;
    size_t n = mca_diag_get_scope(d, want, pre, &tg, &rise, &age, &full,
                                  &amp, &rej);
    for (size_t i = 0; i < n; i++) d[i] &= MCA_DATA_MASK;

    int32_t h[7] = { tg, rise, age, mca_diag_get_trig_level(), (int32_t)full,
                     amp, (int32_t)rej };
    memcpy(raw, h, HDR);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, (const char *)raw, HDR + n * 2);
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
        "\"srun\":%d,\"cap\":%d}",
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
        mca_scope_run ? 1 : 0, adc_cap_is_running() ? 1 : 0);
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
        "\"pileup_post_pct\":%ld,\"fl\":[",
        (long)p.threshold, (long)p.cpc_milli, (long)p.nch,
        (long)p.algo, (long)p.polarity, (long)p.int_rise, (long)p.int_fall,
        (long)p.hysteresis, (long)p.trap_L,
        (long)p.trap_G, (long)p.rearm, (long)p.search,
        (long)p.baseline_shift, (long)p.baseline_win,
        (long)p.flat_avg,
        (long)p.pileup_pre_pct,
        (long)p.pileup_post_pct);
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
    esp_wifi_connect();
}

static void sta_retry_later(void)
{
    if (!s_sta_ssid[0] || !s_sta_timer) return;  /* сети нет - не лезем в эфир */
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

static void wifi_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    wifi_creds_load();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_ev, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_ev, NULL, NULL);

    /* AP: открытый (без пароля) - прибор в лаборатории, не в поле.
     * Если нужен пароль - раскомментировать .authmode и .password ниже. */
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, MCA_AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len       = strlen(MCA_AP_SSID);
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.channel        = 1;

    const esp_timer_create_args_t ta = {
        .callback = sta_try_cb,
        .name     = "sta_try",
    };
    esp_timer_create(&ta, &s_sta_timer);

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

/* ---- шапка и навигация вспомогательных страниц ---- */
/* Та же шапка, что на главной, но без индикаторов состояния. */
#define SUB_HEAD(TITLE) \
"<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>" \
"<meta name=viewport content='width=device-width,initial-scale=1'>" \
"<title>" TITLE " · MCA</title><link rel=stylesheet href=/s.css></head><body>" \
"<header><div class=brand><img src=/logo.png?v=2 alt=''>MCA " \
"<span style=color:var(--accent)>AD9226</span></div></header>"
#define SUB_NAV(D, W, H) "<nav><a href=/>Спектр</a>" D W H "</nav>"
#define N_DIAG    "<a href=/diag>Диагностика</a>"
#define N_DIAG_ON "<span class=on>Диагностика</span>"
#define N_WIFI    "<a href=/wifi>WiFi</a>"
#define N_WIFI_ON "<span class=on>WiFi</span>"
#define N_HELP    "<a href=/help>Справка</a>"
#define N_HELP_ON "<span class=on>Справка</span>"

/* ---- страница/обработчики настройки WiFi ---- */
static const char WIFI_PAGE[] =
SUB_HEAD("WiFi") SUB_NAV(N_DIAG, N_WIFI_ON, N_HELP)
"<div class=wrap><section class='panel pad' style=max-width:520px>"
"<h3>Настройка WiFi прибора</h3>"
"<div id=cur class=n style=margin-bottom:12px></div>"
"<div class=lbl>Сеть (SSID)</div><input id=s style='width:100%;margin:4px 0 10px'>"
"<div class=lbl>Пароль</div><input id=p type=password style='width:100%;margin:4px 0 12px'>"
"<button class='btn green' onclick=go()>Сохранить и подключиться</button>"
"<p class=n>Прибор сохранит данные и попробует "
"подключиться, не отключая точку доступа " MCA_AP_SSID ".</p>"
"</section></div>"
"<script>"
"fetch('/wifi/status').then(r=>r.json()).then(j=>{"
"document.getElementById('cur').innerHTML="
"'Сейчас: <b>'+j.ssid+'</b> &mdash; '+(j.connected?'подключено, IP '+j.ip:'нет связи');"
"document.getElementById('s').value=j.ssid});"
"function go(){var s=document.getElementById('s').value,"
"p=document.getElementById('p').value;"
"fetch('/wifi/set?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))"
".then(()=>alert('Сохранено, подключаюсь...'))}"
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
    bool connected = sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
                     ip.ip.addr != 0;
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
        "{\"ssid\":\"%s\",\"connected\":%s,\"ip\":\"" IPSTR "\"}",
        s_sta_ssid, connected ? "true" : "false", IP2STR(&ip.ip));
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


/* ---- диагностика тракта АЦП ---- */
static const char DIAG_PAGE[] =
SUB_HEAD("Диагностика") SUB_NAV(N_DIAG_ON, N_WIFI, N_HELP)
"<div class=wrap><section class='panel pad'>"
"<h3>Диагностика тракта АЦП</h3>"
"<label><input type=checkbox id=en onchange=tog()> "
"побитовая статистика (разбор 1 чанка/с)</label>"
"<div id=out></div>"
"<button class='btn sm' onclick=probe() style=margin-top:10px>Проверить линии данных</button>"
"<div id=pr style=margin-top:8px;font-size:12.5px></div></section></div>"
"<script>"
"function tog(){fetch('/diag/set?en='+(document.getElementById('en').checked?1:0))}"
"function f(v,c){return '<span class='+c+'>'+v+'</span>'}"
"function upd(){fetch('/diag/data').then(r=>r.json()).then(function(j){"
"document.getElementById('en').checked=j.enabled;"
"var h='<table><tr><th>Параметр</th><th>Значение</th></tr>';"
"h+='<tr><td>Поток данных</td><td>'+(j.flow?f('идёт','ok'):f('НЕТ ДАННЫХ','bad'))+'</td></tr>';"
"h+='<tr><td>Чанков всего</td><td>'+j.chunks+'</td></tr>';"
"h+='<tr><td>Отсчётов всего</td><td>'+j.samples+'</td></tr>';"
"var ec=Math.abs(j.err)<=2?'ok':(Math.abs(j.err)<=10?'warn':'bad');"
"h+='<tr><td>Скорость измеренная</td><td>'+(j.rate/1e6).toFixed(3)+' МГц</td></tr>';"
"h+='<tr><td>Скорость ожидаемая</td><td>'+(j.exp/1e6).toFixed(3)+' МГц</td></tr>';"
"h+='<tr><td>Отклонение</td><td>'+f(j.err+' %',ec)+'</td></tr>';"
"h+='<tr><td>Потеряно чанков</td><td>'+(j.lost>0?f(j.lost,'warn'):j.lost)+'</td></tr>';"
"var tot=j.chunks+j.lost;var lp=tot?100*j.lost/tot:0;"
"h+='<tr><td>Доля потерь</td><td>'+(lp<0.1?f(lp.toFixed(2)+' % — чисто','ok'):"
"f(lp.toFixed(1)+' % — обработка не успевает','bad'))+'</td></tr>';"
"if(lp>0.1){var can=j.exp*(1-lp/100);"
"h+='<tr><td>Потолок обработки</td><td>'+f('около '+(can/1e6).toFixed(1)+"
"' МГц — поставьте эту частоту или ниже','warn')+'</td></tr>'}"
"var cs=(j.ctrl>>>29)&3;var st=(j.ctrl1>>>29)&1;var bl=j.ctrl1&0xFFFF;"
"var tb=(j.ctrl1>>>24)&1;"
"h+='<tr><td>Такт модуля CAM</td><td>'+(cs?f('вкл (clk_sel='+cs+')','ok'):f('ВЫКЛЮЧЕН','bad'))+'</td></tr>';"
"h+='<tr><td>cam_start</td><td>'+(st?f('запущен','ok'):f('НЕ УСТАНОВЛЕН','bad'))+'</td></tr>';"
"h+='<tr><td>16-битный режим</td><td>'+(tb?f('да','ok'):f('нет','bad'))+'</td></tr>';"
"h+='<tr><td>Длина чанка, байт</td><td>'+(bl+1)+'</td></tr>';"
"h+='<tr><td>Уровень на PCLK</td><td>'+j.pclk+'</td></tr>';"
"h+='<tr><td>cam_ctrl / ctrl1</td><td>0x'+(j.ctrl>>>0).toString(16)+' / 0x'+(j.ctrl1>>>0).toString(16)+'</td></tr>';"
"h+='</table>';"
/* ПРОФИЛЬ: что было за прошлую секунду. Очередь 16 из 16 - следующий
   чанк потерян; долгие запросы - кандидаты в виновники. */
"var p=j.pf;if(p){"
"var qc=p.q>=14?'bad':(p.q>=8?'warn':'ok');"
"h+='<h4>Профиль за последнюю секунду</h4><table><tr><th>Что</th><th>Значение</th></tr>';"
"h+='<tr><td>Чанков обработано</td><td>'+p.ch+'</td></tr>';"
"h+='<tr><td>Пик очереди чанков</td><td>'+f(p.q+' из 16',qc)+"
"' <span style=opacity:.6>(16 - следующий теряется)</span></td></tr>';"
"h+='<tr><td>Самая долгая обработка чанка</td><td>'+(p.work/1000).toFixed(2)+' мс</td></tr>';"
"h+='<tr><td>Самое долгое ожидание чанка</td><td>'+(p.wait/1000).toFixed(2)+' мс</td></tr>';"
"h+='<tr><td>Потеряно за секунду</td><td>'+(p.lost?f(p.lost,'bad'):'0')+'</td></tr>';"
"var wn=['/spectrum','/scope','/stat'];"
"for(var k=0;k<3;k++)h+='<tr><td>Запрос '+wn[k]+'</td><td>'+p.wcnt[k]+' за с, самый долгий '+"
"(p.wmax[k]/1000).toFixed(1)+' мс</td></tr>';"
"h+='<tr><td>Свободно внутр. памяти</td><td>'+(p.heap/1024).toFixed(0)+' КБ (минимум '+"
"(p.hmin/1024).toFixed(0)+' КБ)</td></tr>';"
"h+='<tr><td>Сигнал WiFi</td><td>'+(p.rssi?p.rssi+' дБм':'нет связи с роутером')+'</td></tr>';"
"h+='</table>';"
/* Журнал потерь: что делал веб в момент потери чанка. Если потери
   совпадают с запросами - виноват веб, если нет - обработка. */
"var wn2=['нет','/spectrum','/scope','/stat'];"
"if(j.ev&&j.ev.length){"
"h+='<h4>Журнал потерь чанков</h4><table><tr><th>Время</th><th>Потеряно</th>'+"
"'<th>Очередь</th><th>Шёл запрос</th><th>Длился</th><th>Обработка</th></tr>';"
"j.ev.forEach(function(e){"
"h+='<tr><td>'+(e[0]/1000).toFixed(1)+' с</td><td>'+e[1]+'</td><td>'+e[2]+'</td>'+"
"'<td>'+(wn2[e[3]]||'?')+'</td><td>'+(e[4]/1000).toFixed(1)+' мс</td><td>'+"
"(e[5]/1000).toFixed(2)+' мс</td></tr>'});"
"h+='</table>'}"
"else h+='<p style=opacity:.6>Потерь чанков не было.</p>';"
"}"
"if(j.stats){"
"h+='<table><tr><th>Бит</th><th>Переключений</th><th>Состояние</th></tr>';"
"var nm=['D0','D1','D2','D3','D4','D5','D6','D7','D8','D9','D10','D11','OTR'];"
"for(var b=0;b<13;b++){var st='',c='ok';"
"if(j.a1&(1<<b)){st='всегда 1';c='bad'}"
"else if(j.a0&(1<<b)){st='всегда 0';c='bad'}"
"else st='меняется';"
"h+='<tr><td>'+nm[b]+'</td><td>'+j.tg[b]+'</td><td>'+f(st,c)+'</td></tr>'}"
"h+='</table>';"
"h+='<table><tr><th>Значения в чанке</th><th></th></tr>';"
"h+='<tr><td>min / max</td><td>'+j.vmin+' / '+j.vmax+'</td></tr>';"
"h+='<tr><td>среднее</td><td>'+j.vmean+'</td></tr>';"
"h+='<tr><td>размах</td><td>'+(j.vmax-j.vmin)+'</td></tr>';"
"h+='<tr><td>OTR (переполнение)</td><td>'+(j.otr>0?f(j.otr,'warn'):j.otr)+'</td></tr>';"
"h+='<tr><td>проанализировано</td><td>'+j.an+' отсчётов</td></tr>';"
"h+='</table>'}"
"else h+='<p style=opacity:.6>Побитовая статистика выключена.</p>';"
"document.getElementById('out').innerHTML=h})}"
"function probe(){document.getElementById('pr').textContent='измеряю 4 с...';"
"fetch('/diag/probe').then(r=>r.json()).then(function(j){"
"var nm=['D0','D1','D2','D3','D4','D5','D6','D7','D8','D9','D10','D11','OTR'];"
"var h='<table><tr><th>Линия</th><th>Состояние</th></tr>';var live=0;"
"for(var b=0;b<13;b++){var st,c;"
"if(j.ch&(1<<b)){st='меняется';c='ok';live++}"
"else if(j.hi&(1<<b)){st='залипла в 1';c='bad'}"
"else st='залипла в 0',c='bad';"
"h+='<tr><td>'+nm[b]+'</td><td>'+f(st,c)+'</td></tr>'}h+='</table>';"
"h+=live?f('Активных линий: '+live+' — АЦП преобразует','ok'):"
"f('НИ ОДНА линия не шевелится — АЦП не преобразует. Смотрите питание платы и приходит ли на неё такт.','bad');"
"document.getElementById('pr').innerHTML=h})}"
"setInterval(upd,1000);upd();"
"</script></body></html>";

static esp_err_t h_diag_page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, DIAG_PAGE, HTTPD_RESP_USE_STRLEN);
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
        ",\"pf\":{\"ch\":%lu,\"q\":%lu,\"wait\":%lu,\"work\":%lu,"
        "\"lost\":%lu,\"heap\":%lu,\"hmin\":%lu,\"rssi\":%ld,"
        "\"wmax\":[%lu,%lu,%lu],\"wcnt\":[%lu,%lu,%lu]}",
        (unsigned long)pf.chunks, (unsigned long)pf.q_max,
        (unsigned long)pf.wait_max_us, (unsigned long)pf.work_max_us,
        (unsigned long)pf.lost_1s, (unsigned long)pf.heap_now,
        (unsigned long)pf.heap_min, (long)pf.rssi,
        (unsigned long)pf.web_max_us[PROF_EP_SPEC],
        (unsigned long)pf.web_max_us[PROF_EP_SCOPE],
        (unsigned long)pf.web_max_us[PROF_EP_STAT],
        (unsigned long)pf.web_cnt[PROF_EP_SPEC],
        (unsigned long)pf.web_cnt[PROF_EP_SCOPE],
        (unsigned long)pf.web_cnt[PROF_EP_STAT]);

    mca_prof_ev_t ev[12];
    size_t ne = mca_prof_events(ev, 12);
    n += snprintf(buf + n, sizeof(buf) - n, ",\"ev\":[");
    for (size_t i = 0; i < ne && n < (int)sizeof(buf) - 96; i++)
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s[%lu,%lu,%lu,%u,%lu,%lu]", i ? "," : "",
                      (unsigned long)ev[i].t_ms, (unsigned long)ev[i].lost,
                      (unsigned long)ev[i].q_depth, (unsigned)ev[i].web_ep,
                      (unsigned long)ev[i].web_us,
                      (unsigned long)ev[i].work_us);
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
SUB_HEAD("Справка") SUB_NAV(N_DIAG, N_WIFI, N_HELP_ON)
"<style>td:first-child{white-space:nowrap;font-weight:600;width:1%}"
"table{margin:4px 0 14px}</style>"
"<div class=wrap><section class='panel pad' style=max-width:920px>"
"<h3>Справка по параметрам</h3>"

"<h4>Обнаружение события</h4><table>"
"<tr><th>Параметр</th><th>Что делает</th></tr>"
"<tr><td>Порог<br><code>threshold</code></td><td>На сколько кодов АЦП "
"отсчёт должен превысить базовую линию, чтобы считаться событием. "
"Ставится чуть выше шума &mdash; посмотрите шум на вкладке «Конфиг MCA».</td></tr>"
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
"компьютера &mdash; своих часов у прибора нет.</td></tr>"
"<tr><td>Конфиг MCA</td><td>Осциллограф и настройки обработки. Сырые отсчёты с синхронизацией "
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
"</table>"

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
    wifi_init();

    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 28;
    cfg.stack_size       = 8192;
    cfg.core_id          = 0;           /* веб на ядро 0, DSP на ядро 1 */
    cfg.lru_purge_enable = true;

    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) return err;

    httpd_uri_t u[] = {
        { "/",             HTTP_GET, h_root,        NULL },
        { "/s.css",        HTTP_GET, h_css,         NULL },
        { "/logo.png",     HTTP_GET, h_logo,        NULL },
        { "/spectrum",     HTTP_GET, h_spectrum,    NULL },
        { "/scope",        HTTP_GET, h_scope,       NULL },
        { "/stat",         HTTP_GET, h_stat,        NULL },
        { "/cfg",          HTTP_GET, h_cfg,         NULL },
        { "/cmd",          HTTP_GET, h_cmd,         NULL },
        { "/wifi",         HTTP_GET, h_wifi_page,   NULL },
        { "/wifi/status",  HTTP_GET, h_wifi_status, NULL },
        { "/wifi/set",     HTTP_GET, h_wifi_set,    NULL },
        { "/diag",         HTTP_GET, h_diag_page,   NULL },
        { "/diag/data",    HTTP_GET, h_diag_data,   NULL },
        { "/diag/set",     HTTP_GET, h_diag_set,    NULL },
        { "/diag/probe",   HTTP_GET, h_diag_probe,  NULL },
        { "/help",         HTTP_GET, h_help,        NULL },
        { "/export.xml",   HTTP_GET, h_export,      (void *)"xml" },
        { "/export.csv",   HTTP_GET, h_export,      (void *)"csv" },
        { "/export.n42",   HTTP_GET, h_export,      (void *)"n42" },
        { "/export.spe",   HTTP_GET, h_export,      (void *)"spe" },
    };
    for (int i = 0; i < sizeof(u) / sizeof(u[0]); i++)
        httpd_register_uri_handler(srv, &u[i]);

    ESP_LOGI(TAG, "веб-сервер запущен");
    return ESP_OK;
}
