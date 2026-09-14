#include "mca_dsp.h"
#include "adc_cap.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"

static const char *TAG = "mca_dsp";

/* ======================================================================
 * АРХИТЕКТУРА (переработана после замеров реального сигнала)
 *
 * Сигнал с зарядочувствительного предусилителя: быстрый фронт 200-800 нс
 * и ДЛИННЫЙ экспоненциальный спад (tau ~ 21 мкс). При скорости счёта
 * 10 000/с новый импульс почти всегда приходит на недоспавший хвост
 * предыдущего - сигнал превращается в "лестницу", абсолютного нуля
 * между импульсами не существует.
 *
 * Поэтому порог сравнивается НЕ с сырым отсчётом, а с выходом
 * трапецеидального фильтра. Трапеция - это разность двух скользящих
 * окон: медленный хвост входит в оба почти одинаково и вычитается,
 * а ступенька нового импульса - нет. Дифференциальность получается
 * бесплатно, отдельный триггер не нужен.
 *
 * Следствие: трапецию надо считать на КАЖДОМ отсчёте, а не только
 * вокруг события. Поэтому используется рекурсивная форма Джорданова
 * (O(1) на отсчёт), а не пересуммирование окна:
 *
 *   trap[n] = trap[n-1] + v[n] - v[n-L] - v[n-G] + v[n-G-L]
 *
 * Это математически то же самое, что (сумма окна L) - (сумма окна L,
 * сдвинутого на G), но 4 обращения к кольцу вместо 2L.
 * ====================================================================== */

static uint32_t     *s_hist;
static mca_params_t  s_par;
static mca_stats_t   s_st;
static SemaphoreHandle_t s_lock;

/* --- кольцо сырых отсчётов + рекурсивный аккумулятор --- */
static int32_t  s_trap;          /* текущее значение трапеции */

/* Линейный рабочий буфер: хвост прошлого чанка + текущий чанк.
 * Раньше здесь было кольцо с маской, и каждое из трёх обращений
 * стоило вычитания, наложения маски и расчёта адреса - около
 * тридцати инструкций на отсчёт, потолок около 3.5 МГц. В линейном
 * буфере отсчёты берутся простым смещением по фиксированному сдвигу. */
#define DSP_TAIL (TRAP_L_MAX + TRAP_G_MAX + 8)
/* Выровнен на 16 байт: векторная разность (diff_vec) читает отсчёт
 * w[i] выровненными 128-битными словами, начиная с s_work + DSP_TAIL,
 * а переворот полярности пишет сюда 32-битными. Запас в 16 отсчётов
 * в конце: последняя восьмёрка читает до 7 отсчётов за краем чанка. */
static uint16_t s_work[DSP_TAIL + CAP_CHUNK_SAMPLES + 16] __attribute__((aligned(16)));
_Static_assert(DSP_TAIL % 8 == 0, "s_work + DSP_TAIL должен быть выровнен на 16 байт");

/* Разность трапеции d[i] = w[i] - w[i-L] - w[i-G] + w[i-G-L] для
 * отсчётов текущего чанка: s_d[k] относится к s_work[DSP_TAIL + k].
 * Считается сразу для всего чанка, дальше вся обработка берёт готовое
 * значение - одно чтение на отсчёт вместо четырёх. Значения в пределах
 * ±8190, в 16 бит помещаются. Запас в 16 - под последнюю восьмёрку. */
static int16_t s_d[CAP_CHUNK_SAMPLES + 16] __attribute__((aligned(16)));
#define DD(i) s_d[(i) - DSP_TAIL]

/* Векторный путь включается только после самопроверки при старте
 * (mca_dsp_vec_init): не прошла - считаем разность обычным кодом. */
static bool s_vec_ok;

/* Счётчики, которые копит только задача обработки, без мьютекса;
 * в статистику они уходят раз на чанк (см. конец mca_dsp_process). */
static uint32_t s_ev_pend, s_ovf_pend;   /* событий и зашкалов за чанк   */
static uint64_t s_busy_cyc;              /* тактов обработки с замера    */
static int64_t  s_load_t_us;             /* когда был прошлый замер      */

/* --- автомат детектора --- */
typedef enum { ST_IDLE = 0, ST_PEAK, ST_REARM } det_state_t;
static det_state_t s_state;
static int32_t  s_peak;          /* максимум трапеции в событии */
static int32_t  s_peak_pos;      /* индекс максимума в s_work   */
static int32_t  s_since;         /* отсчётов с начала фазы      */
static int32_t  s_pre_trap;      /* трапеция до события (для pileup) */
static int64_t  s_flat_sum;      /* сумма трапеции на плоской вершине */
static int32_t  s_flat_cnt;

/* --- baseline (для показа и диагностики; трапеция в нём не нуждается) --- */
static int32_t  s_base_fp;
static bool     s_base_init;
static int32_t  s_base_lost;

/* После сброса (разрыв потока, старт, смена L/G) буфер не описывает
 * непрерывный сигнал. Если сразу начать детектировать, скачок
 * от нулей к реальному уровню даст ложное событие с зашкальной
 * амплитудой - все такие валятся в последний канал и дают острый
 * пик у правого края спектра. Поэтому первые отсчёты после сброса
 * фильтр только прогревается, события не засчитываются. */
static bool     s_need_prime = true;
static int32_t  s_prime_left;

static uint64_t s_last_events;
static uint64_t s_t0_us;

void mca_dsp_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_hist = heap_caps_calloc(MCA_CHANNELS, sizeof(uint32_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hist) s_hist = calloc(MCA_CHANNELS, sizeof(uint32_t));

    /* Дефолты сняты с рабочей конфигурации другого прибора того же
     * класса (АЦП AD9235, тот же тип детектора):
     *   F 7000000  RISE 6  FALL 15  NOISE 30  HYST 1  STEP 5
     * отбраковка наложений там выключена - у нас тоже по умолчанию. */
    s_par = (mca_params_t){
        /* Порог задаётся в единицах ВЫХОДА ТРАПЕЦИИ, а не входного
         * шума. Проверено симуляцией: при шуме +-8 кодов трапеция
         * (L=6,G=15) даёт сигму ~17 и выбросы до 80, поэтому порог
         * должен быть 100-200. Значение NOISE 30 с того прибора
         * относится к их шумовым условиям и напрямую не переносится -
         * подбирать по реальному шуму (см. режим Осциллограф). */
        .threshold       = 20,
        .hysteresis      = 50,   /* % от порога для перезапуска   */
        /* ВАЖНО: у того прибора RISE=6 и FALL=15 - это части
         * ОДНОГО окна интегрирования, их сумма 21 отсчёт. У нас L и
         * есть это окно, поэтому переносить их числа как L=6, G=15
         * было неверно: мы интегрировали шестую часть от того, что
         * они. Берём L=20 (близко к их 21) и G с запасом на фронт. */
        .trap_L          = 20,
        .trap_G          = 44,
        .rearm           = 64,   /* L+G: трапеция возвращается к 0 */
        .search          = 70,   /* чуть больше L+G               */
        /* Способ измерения амплитуды. Обнаружение события в обоих
         * случаях одинаковое - по выходу трапеции: порог по сырому
         * сигналу при длинном хвосте не работает. Различается только
         * то, как считается величина уже найденного импульса. */
        .algo            = 0,
        .polarity        = 0,
        .int_rise        = 20,
        .int_fall        = 32,
        /* 1 код амплитуды на канал и 2048 каналов: при базе в середине
         * АЦП (запас вверх 2047 кодов) шкала занята целиком. */
        .cpc_milli       = 1000,
        .nch             = 2048,
        .baseline_shift  = 10,
        .baseline_win    = 150,
        .pileup_pre_pct  = 0,    /* 0 = выключено                 */
        .pileup_post_pct = 0,
        /* Усреднение по плато ВЫКЛЮЧЕНО по умолчанию.
         * Проверено симуляцией: выигрыш появляется только при длинном
         * плато (G-L >= ~19). При наших дефолтах (плато 9) максимум
         * даёт даже чуть меньший разброс - соседние отсчёты трапеции
         * сильно скоррелированы, усреднять почти нечего.
         * Смещение максимума (растёт с шумом) - постоянный сдвиг
         * для всех амплитуд, уходит в калибровку и ширину пиков
         * не портит. */
        .flat_avg        = 0,
    };

    memset(s_work, 0, sizeof(s_work));
    s_trap = 0; s_state = ST_IDLE;
    s_need_prime = true;
    s_base_fp = 0;
    s_t0_us = esp_timer_get_time();
    ESP_LOGI(TAG, "DSP готов: рекурсивная трапеция, порог по её выходу");
}

/* Очистить спектр и счётчики. Вызывать под s_lock. */
static void hist_clear_locked(void)
{
    memset(s_hist, 0, MCA_CHANNELS * sizeof(uint32_t));
    s_st.total_events      = 0;
    s_st.skipped_pileup    = 0;
    s_st.skipped_deadtime  = 0;
    s_st.samples_processed = 0;
    s_last_events = 0;
    s_base_init   = false;      /* перезахватить ноль заново */
    s_base_lost   = 0;
    s_t0_us = esp_timer_get_time();
}

void mca_dsp_reset_spectrum(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hist_clear_locked();
    xSemaphoreGive(s_lock);
}

/* ПРОСТОЕ ИНТЕГРИРОВАНИЕ.
 *
 * Сумма отсчётов от вершины минус rise до вершины плюс fall,
 * с вычтенной базовой линией. Для сцинтиллятора с ФЭУ это
 * физически корректная величина: энергия пропорциональна
 * полному заряду, то есть площади импульса.
 *
 * Проверено симуляцией: разброс около 2.5 % против 0.6 % у
 * трапеции с компенсацией. Разница возникает из-за дрожания
 * положения вершины: импульс несимметричный, и сдвиг окна на
 * отсчёт заметно меняет сумму. В итоговом спектре с NaI это
 * даёт 7.44 % против 7.02 % - почти неразличимо, зато настройка
 * куда понятнее: два числа прямо с экрана.
 *
 * Возвращает сумму, а число отсчётов - в *out_n. Амплитуда - их
 * отношение, средняя высота импульса в окне в кодах АЦП; делится она
 * уже при записи в канал, целиком, без округления до кода.
 */
static int64_t amp_integrate(int32_t peak_pos, int32_t rise, int32_t fall,
                             int32_t L, int32_t G, int32_t base,
                             int32_t *out_n)
{
    int total = DSP_TAIL + CAP_CHUNK_SAMPLES;

    /* Вершина трапеции отстаёт от вершины сигнала примерно на L,
     * поэтому настоящий максимум ищем левее. */
    int lo = peak_pos - L - G;
    int hi = peak_pos + 4;
    if (lo < 1) lo = 1;
    if (hi >= total) hi = total - 1;

    int32_t mx = -32768, pk = lo;
    for (int i = lo; i <= hi; i++)
        if (s_work[i] > mx) { mx = s_work[i]; pk = i; }

    int a = pk - rise, b = pk + fall;
    if (a < 0) a = 0;
    if (b >= total) b = total - 1;

    int64_t sum = 0;
    for (int i = a; i <= b; i++) sum += (int32_t)s_work[i] - base;

    int n = b - a + 1;
    *out_n = n > 0 ? n : 1;
    return n > 0 ? sum : 0;
}

/* Номер канала = амплитуда / «кодов на канал». Амплитуда приходит
 * дробью num/den (трапеция: вершина / L, интегрирование: сумма / число
 * отсчётов) и делится целиком в 64 битах. Округлять её до целого кода
 * заранее нельзя: при долях кода на канал (0.5, 0.25) заняты были бы
 * только каналы, кратные 2 или 4, и спектр стал бы «гребёнкой». */
static void record_event(int64_t num, int32_t den, const mca_params_t *p)
{
    int64_t d  = (int64_t)den * p->cpc_milli;
    int64_t ch = (d > 0 && num > 0) ? num * 1000 / d : 0;

    /* Зашкалившие события ВЫБРАСЫВАЕМ, а не прижимаем к последнему
     * каналу. Раньше они там копились и выглядели как настоящий
     * острый пик у правого края спектра. */
    /* Без мьютекса. Раньше он брался и отпускался на КАЖДОЕ событие -
     * сотни тактов. В гистограмму пишет только задача обработки, а
     * читателю (веб) недописанный на мгновение канал не страшен.
     * Счётчики копятся локально и уходят в статистику раз на чанк, под
     * той блокировкой, что и так берётся в конце mca_dsp_process. */
    if (ch >= MCA_CHANNELS) {
        s_ovf_pend++;
        return;
    }
    s_hist[ch]++;
    s_ev_pend++;
}

/* ---- РАЗНОСТЬ ТРАПЕЦИИ ДЛЯ ВСЕГО ЧАНКА ----
 * d[k] = w[k] - w[k-L] - w[k-G] + w[k-G-L], где w = s_work + DSP_TAIL. */

/* Обычным кодом - запасной путь, если векторный не прошёл самопроверку. */
static void IRAM_ATTR diff_scalar(int L, int G, int n)
{
    const uint16_t *w = s_work + DSP_TAIL;
    for (int k = 0; k < n; k++)
        s_d[k] = (int16_t)(w[k] - w[k - L] - w[k - G] + w[k - G - L]);
}

/* ВЕКТОРНО: 8 отсчётов за команду (ESP32-S3, расширение PIE).
 *
 * Поток w[k] выровнен на 16 байт (так подобраны s_work и DSP_TAIL),
 * три других сдвинуты на L, G и G+L отсчётов и читаются невыровненно:
 * EE.LD.128.USAR берёт выровненный блок и запоминает сдвиг в SAR_BYTE,
 * следующий блок читается обычной загрузкой, EE.SRC.Q склеивает из
 * двух нужные 16 байт. Каждый поток ставит свой сдвиг прямо перед своей
 * склейкой, поэтому один регистр SAR_BYTE на троих не мешает.
 *
 * Сложение и вычитание с насыщением 16 бит, но оно не наступает:
 * w от 0 до 4095, все промежуточные значения в пределах ±8190.
 * За концом чанка читается и пишется до 7 лишних отсчётов - на это
 * у s_work и s_d запас. Самое раннее чтение - блок, содержащий
 * w[-G-L] >= s_work + 8, - не выходит за начало массива.
 *
 * noinline: внутри аппаратный цикл loopgtz, а вложенных аппаратных
 * циклов не бывает - функция не должна оказаться внутри чужого. */
static void IRAM_ATTR __attribute__((noinline)) diff_vec(int L, int G, int n)
{
    const uint16_t *pa = s_work + DSP_TAIL;
    const uint16_t *pb = pa - L, *pc = pa - G, *pe = pa - G - L;
    int16_t *pd = s_d;
    int nv = (n + 7) >> 3;
    __asm__ volatile(
        "loopgtz %[nv], 1f\n\t"
        "ee.vld.128.ip     q0, %[pa], 16\n\t"   /* w[k..k+7]            */
        "ee.ld.128.usar.ip q1, %[pb], 16\n\t"   /* w[k-L..]: блок+сдвиг */
        "ee.vld.128.ip     q2, %[pb], 0\n\t"    /*   следующий блок     */
        "ee.src.q          q1, q1, q2\n\t"      /*   склейка            */
        "ee.vsubs.s16      q0, q0, q1\n\t"      /* w - w[-L]            */
        "ee.ld.128.usar.ip q3, %[pc], 16\n\t"
        "ee.vld.128.ip     q4, %[pc], 0\n\t"
        "ee.src.q          q3, q3, q4\n\t"
        "ee.vsubs.s16      q0, q0, q3\n\t"      /*   - w[-G]            */
        "ee.ld.128.usar.ip q5, %[pe], 16\n\t"
        "ee.vld.128.ip     q6, %[pe], 0\n\t"
        "ee.src.q          q5, q5, q6\n\t"
        "ee.vadds.s16      q0, q0, q5\n\t"      /*   + w[-G-L]          */
        "ee.vst.128.ip     q0, %[pd], 16\n\t"
        "1:\n\t"
        : [pa] "+r"(pa), [pb] "+r"(pb), [pc] "+r"(pc), [pe] "+r"(pe),
          [pd] "+r"(pd)
        : [nv] "r"(nv)
        : "memory");
}

/* Самопроверка векторной разности на самом приборе: случайные 12-битные
 * отсчёты и крайние 0/4095 (проверка, что насыщения нет), все сдвиги
 * выравнивания (L и G с разными остатками от деления на 8), разные длины
 * чанка (остаток последней восьмёрки). Сверка с обычной формулой по
 * каждому отсчёту. Не совпало хоть одно - работаем обычным кодом.
 * Вызывать из задачи обработки до первого mca_dsp_process. */
bool mca_dsp_vec_init(void)
{
    static const uint8_t Ls[] = { 1, 2, 3, 5, 7, 8, 13, 20, 33, 64 };
    const int total = (int)(sizeof(s_work) / sizeof(s_work[0]));
    bool ok = true;
    uint32_t x = 0x12345678u;

    for (int set = 0; set < 2 && ok; set++) {
        for (int k = 0; k < total; k++) {
            x = x * 1664525u + 1013904223u;
            s_work[k] = set == 0 ? (uint16_t)((x >> 20) & 0x0FFF)
                                 : (uint16_t)((x >> 31) ? MCA_ADC_MAX : 0);
        }
        for (size_t a = 0; a < sizeof(Ls) && ok; a++) {
            for (int g = 0; g < 3 && ok; g++) {
                int L = Ls[a];
                int G = g == 0 ? L + 3 : (g == 1 ? L + 44 : TRAP_G_MAX);
                if (G > TRAP_G_MAX) G = TRAP_G_MAX;
                if (L + G >= DSP_TAIL - 4) G = DSP_TAIL - 4 - L;
                int n = CAP_CHUNK_SAMPLES - (int)(a & 7);
                diff_vec(L, G, n);
                const uint16_t *w = s_work + DSP_TAIL;
                for (int k = 0; k < n; k++) {
                    int32_t ref = w[k] - w[k - L] - w[k - G] + w[k - G - L];
                    if (s_d[k] != ref) {
                        ESP_LOGE(TAG, "векторная разность разошлась: L=%d G=%d "
                                 "k=%d: %d вместо %ld", L, G, k, s_d[k],
                                 (long)ref);
                        ok = false;
                        break;
                    }
                }
            }
        }
    }
    memset(s_work, 0, sizeof(s_work));
    s_need_prime = true;
    s_vec_ok = ok;
    ESP_LOGI(TAG, "векторная разность трапеции: %s",
             ok ? "самопроверка пройдена, включена"
                : "самопроверка НЕ пройдена - работаем обычным кодом");
    return ok;
}

bool mca_dsp_vec_ok(void) { return s_vec_ok; }

/* Во ВНУТРЕННЕЙ памяти: иначе код горячего цикла тянется из флеш
 * через кэш, и каждый промах останавливает конвейер. Для цикла,
 * выполняемого миллионы раз в секунду, это заметно. */
void IRAM_ATTR mca_dsp_process(const uint16_t *data, size_t n)
{
    /* замер загрузки: такты этого вызова копятся в s_busy_cyc */
    const uint32_t cyc0 = esp_cpu_get_cycle_count();
    mca_params_t p;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    p = s_par;
    xSemaphoreGive(s_lock);

    if (n > CAP_CHUNK_SAMPLES) n = CAP_CHUNK_SAMPLES;

    const int32_t L = p.trap_L;
    const int32_t G = p.trap_G;
    /* НОРМИРОВКА НА ДЛИНУ ОКНА.
     * Выход трапеции пропорционален L, поэтому без нормировки
     * изменение окна сдвигает весь спектр и приходится заново
     * подбирать сжатие шкалы. Делить на каждом отсчёте дорого,
     * поэтому вместо этого домножаем ПОРОГ (один раз на чанк),
     * а амплитуду делим уже при регистрации события.
     * Так порог и каналы заданы в единицах, не зависящих от L. */
    const int32_t thr = p.threshold * L;

    /* Гистерезис - ДОЛЯ порога, а не вычитаемая единица.
     * Прежний вариант (порог минус 1) означал почти полное
     * отсутствие гистерезиса: повторный запуск разрешался,
     * едва трапеция опускалась на единицу. */
    int32_t hp = p.hysteresis;
    if (hp < 1) hp = 1;
    if (hp > 99) hp = 99;
    const int32_t thr_lo = (int32_t)((int64_t)thr * hp / 100);
    /* Прореживание проверки порога больше не нужно: в быстром цикле
     * сравнение стоит одну инструкцию, дешевле, чем счётчик. Параметр
     * оставлен в интерфейсе для совместимости, но не используется. */
    const bool pileup_on = (p.pileup_pre_pct > 0 && p.pileup_post_pct > 0);

    /* Границы плоской вершины (см. пояснение у flat_avg). */
    int32_t flat_len = 0, flat_from = 0, flat_to = 0;
    if (p.flat_avg && (G - L) >= 16) {
        int32_t plateau = G - L;
        int32_t margin  = plateau / 4;
        if (margin < 1) margin = 1;
        flat_from = L + margin;
        flat_to   = L + plateau - margin;
        if (flat_to > p.search) flat_to = p.search;
        flat_len  = flat_to - flat_from;
        if (flat_len < 1) flat_len = 0;
    }

    /* Склеиваем хвост и новый чанк. Импульсы вниз переворачиваем здесь,
     * один раз: дальше вся обработка видит их как импульсы вверх. Для
     * обычной полярности остаётся прежнее быстрое копирование. */
    if (p.polarity && ((uintptr_t)data & 3) == 0) {
        /* По два отсчёта за операцию: в 32-битном слове две половины,
         * в каждой 0x0FFF - (v & 0x0FFF). После маски половина не больше
         * 0x0FFF, поэтому вычитание не занимает из соседней половины и
         * результат совпадает с поштучным 4095 - (v & 0x0FFF) бит в бит
         * (проверено перебором, tools/test_dsp.py, уровень 8). Раньше
         * здесь был цикл по одному отсчёту - около 5 тактов на отсчёт,
         * почти треть бюджета на 13 МГц. */
        typedef uint32_t __attribute__((may_alias)) u32a_t;
        const u32a_t *src = (const u32a_t *)data;
        u32a_t *dst = (u32a_t *)(s_work + DSP_TAIL);
        const size_t n2 = n / 2;
        for (size_t k = 0; k < n2; k++)
            dst[k] = 0x0FFF0FFFu - (src[k] & 0x0FFF0FFFu);
        if (n & 1)
            s_work[DSP_TAIL + n - 1] =
                MCA_ADC_MAX - (data[n - 1] & MCA_DATA_MASK);
    } else if (p.polarity) {
        /* невыровненные данные (из DMA такого не бывает) - по одному */
        for (size_t k = 0; k < n; k++)
            s_work[DSP_TAIL + k] = MCA_ADC_MAX - (data[k] & MCA_DATA_MASK);
    } else {
        memcpy(s_work + DSP_TAIL, data, n * sizeof(uint16_t));
    }

    if (s_need_prime) {
        /* Хвост заполняем ПЕРВЫМ отсчётом чанка, а не нулями:
         * так на стыке нет скачка, и трапеция стартует с нуля
         * естественным образом. Берём уже из буфера - с учётом знака. */
        uint16_t v0 = s_work[DSP_TAIL];
        for (int k = 0; k < DSP_TAIL; k++) s_work[k] = v0;
        s_trap = 0;
        s_state = ST_IDLE;
        s_prime_left = L + G + p.search + 4;
        s_need_prime = false;
    }

    /* Разность трапеции для всего чанка сразу. Дальше и прогрев, и цикл
     * ожидания, и поиск вершины, и перезапуск берут готовое d. */
    if (s_vec_ok) diff_vec(L, G, (int)n);
    else          diff_scalar(L, G, (int)n);

    int32_t trap = s_trap;
    int32_t base_fp = s_base_fp;
    uint64_t pile = 0, dead = 0;

    const int end = DSP_TAIL + (int)n;
    int i = DSP_TAIL;

    /* ========================================================
     * ДВА ЦИКЛА ВМЕСТО ОДНОГО.
     *
     * Почти всё время мы просто ждём события, а переключатель
     * состояний выполнялся на КАЖДОМ отсчёте - лишняя загрузка
     * состояния и ветвление там, где нужно одно сравнение.
     * Теперь ожидание идёт отдельным коротким циклом: фильтр
     * плюс сравнение с порогом, и всё. Обработка события - в
     * медленной ветке, она выполняется редко.
     *
     * Маски тоже нет: бит переполнения больше не заводится
     * в камерный интерфейс, в слове чистые 12 бит.
     * ======================================================== */
    /* Прогрев прокручиваем ОТДЕЛЬНО, до основного цикла: проверять
     * счётчик на каждом отсчёте ради первых десятков - расточительно. */
    while (s_prime_left > 0 && i < end) {
        trap += DD(i);
        s_prime_left--;
        i++;
    }

    /* Базовая линия обновляется РЕДКО и отдельным проходом: раньше
     * условие проверялось на каждом отсчёте внутри горячего цикла. */
    for (int k = DSP_TAIL + 8; k < end; k += 64) {
        int32_t v = s_work[k];
        int32_t base = base_fp >> 8;
        int32_t dev  = v - base;
        if (!s_base_init) {
            base_fp = v << 8;
            s_base_init = true;
        } else if (dev <= p.baseline_win && dev >= -p.baseline_win) {
            base_fp += ((v << 8) - base_fp) >> p.baseline_shift;
            s_base_lost = 0;
        } else if (++s_base_lost > 64) {
            base_fp = v << 8;
            s_base_lost = 0;
        }
    }

    while (i < end) {

        if (s_state == ST_IDLE) {
            /* ---- БЫСТРЫЙ ЦИКЛ ОЖИДАНИЯ ----
             * Разность трапеции d для чанка уже посчитана (векторно,
             * diff_vec), здесь её остаётся накапливать: одно чтение и
             * одно сложение на отсчёт.
             *
             * ВОСЬМЁРКАМИ, порог - один раз по максимуму восьми значений
             * трапеции. Выход из цикла компилятор собирает переходом,
             * который в обычном случае выполняется (обход дальнего j), -
             * пусть это будет раз на восемь отсчётов, а не на четыре.
             * Условие цикла - счётчик, чтобы получился аппаратный loop.
             * Если максимум выше порога, восьмёрка разбирается поштучно:
             * первое пересечение и значение трапеции те же, что при
             * поштучной проверке (проверено на модели, уровень 8). */
            const int16_t *pd = &DD(i);
            const int cnt = end - i;
            const int16_t *p8 = pd;
            int q = cnt >> 3;
            for (; q > 0; q--) {
                const int32_t t0 = trap + p8[0], t1 = t0 + p8[1];
                const int32_t t2 = t1   + p8[2], t3 = t2 + p8[3];
                const int32_t t4 = t3   + p8[4], t5 = t4 + p8[5];
                const int32_t t6 = t5   + p8[6], t7 = t6 + p8[7];
                int32_t m = t0 > t1 ? t0 : t1;
                const int32_t m23 = t2 > t3 ? t2 : t3;
                const int32_t m45 = t4 > t5 ? t4 : t5;
                const int32_t m67 = t6 > t7 ? t6 : t7;
                m = m > m23 ? m : m23;
                m = m > m45 ? m : m45;
                m = m > m67 ? m : m67;
                if (m > thr) break;
                trap = t7;
                p8 += 8;
            }
            int k = (int)(p8 - pd);
            if (q > 0) {
                /* порог пробит внутри этой восьмёрки - ищем, на каком
                 * отсчёте это случилось */
                for (int o = 0; o < 8; o++) {
                    trap += p8[o];
                    if (trap > thr) break;
                    k++;
                }
            } else {
                /* остаток чанка, не кратный восьми */
                for (; k < cnt; k++) {
                    trap += pd[k];
                    if (trap > thr) break;
                }
            }
            i += k;
            if (i >= end) break;

            s_pre_trap = trap;
            s_peak     = trap;
            s_peak_pos = i;
            s_since    = 0;
            s_flat_sum = 0;
            s_flat_cnt = 0;
            s_state    = ST_PEAK;
            i++;
        }

        /* ---- медленная ветка: поиск вершины ---- */
        if (s_state == ST_PEAK) {
            bool done = false;
            if (flat_len == 0) {
                /* Без плато (обычный случай): до конца поиска осталось
                 * search - since отсчётов, их прогоняем коротким циклом -
                 * только трапеция и максимум, в локальных переменных.
                 * Счётчики прибавляются разом после цикла. */
                int m = p.search - s_since;
                if (m > end - i) m = end - i;
                int32_t pk = s_peak, pp = s_peak_pos;
                const int stop = i + m;
                for (; i < stop; i++) {
                    trap += DD(i);
                    if (trap > pk) { pk = trap; pp = i; }
                }
                s_peak = pk;
                s_peak_pos = pp;
                s_since += m;
                dead    += m;      /* поиск вершины - тоже мёртвое время */
                done = (s_since >= p.search);
            } else {
                /* с плато - по одному отсчёту, как раньше */
                while (i < end) {
                    trap += DD(i);
                    dead++;
                    if (trap > s_peak) { s_peak = trap; s_peak_pos = i; }
                    if (s_since >= flat_from && s_since < flat_to) {
                        s_flat_sum += trap;
                        s_flat_cnt++;
                    }
                    i++;
                    if (++s_since >= p.search) { done = true; break; }
                }
            }
            if (done) {
                    bool bad = false;
                    if (pileup_on) {
                        if (s_pre_trap > s_peak * p.pileup_pre_pct / 100)
                            bad = true;
                        if (!bad && trap > s_peak * p.pileup_post_pct / 100)
                            bad = true;
                    }
                    if (bad) {
                        pile++;
                    } else {
                        if (p.algo == 1) {
                            int32_t cnt;
                            int64_t s = amp_integrate(s_peak_pos, p.int_rise,
                                                      p.int_fall, L, G,
                                                      base_fp >> 8, &cnt);
                            record_event(s, cnt, &p);
                        } else {
                            int32_t amp = (s_flat_cnt > 0)
                                        ? (int32_t)(s_flat_sum / s_flat_cnt)
                                        : s_peak;
                            record_event(amp, L, &p);   /* вершина / L */
                        }
                    }
                    s_since = 0;
                    s_state = ST_REARM;
            }
            if (i >= end) break;
        }

        /* ---- медленная ветка: мёртвое время ---- */
        if (s_state == ST_REARM) {
            /* Пока с начала фазы меньше rearm отсчётов, условие выхода
             * заведомо ложно - эти отсчёты прогоняем без сравнений.
             * Проверка начинается с того отсчёта, на котором счётчик
             * доходит до rearm, - ровно как в поштучном цикле. */
            int m = p.rearm - 1 - s_since;
            if (m > end - i) m = end - i;
            if (m > 0) {
                const int stop = i + m;
                for (; i < stop; i++)
                    trap += DD(i);
                s_since += m;
                dead    += m;
            }
            for (; i < end; i++) {
                trap += DD(i);
                dead++;
                if (++s_since >= p.rearm && trap < thr_lo) {
                    s_state = ST_IDLE;
                    i++;
                    break;
                }
            }
        }
    }

    /* хвост -> начало буфера для следующего вызова */
    memmove(s_work, s_work + end - DSP_TAIL, DSP_TAIL * sizeof(uint16_t));

    /* Событие, не успевшее завершиться до конца чанка, продолжается
     * в следующем вызове. Но s_peak_pos - это ИНДЕКС В БУФЕРЕ, а
     * буфер только что сдвинулся: без пересчёта индекс указывал бы
     * на чужие отсчёты, и амплитуда считалась бы по пустому месту.
     * Такие события уходили в нулевой канал - на модели это 4 % при
     * search=70 и чанке 1024 (порог срабатывал ближе к концу чанка). */
    if (s_state == ST_PEAK) {
        s_peak_pos -= (int32_t)(end - DSP_TAIL);
        /* Если пик уехал за пределы сохранённого хвоста, мерить
         * нечего: событие бросаем, а не считаем по мусору. */
        if (s_peak_pos < L + G + 1) {
            s_state = ST_IDLE;
            s_since = 0;
        }
    }

    s_trap = trap;
    s_base_fp = base_fp;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.skipped_pileup    += pile;
    s_st.skipped_deadtime  += dead;
    s_st.samples_processed += n;
    s_st.baseline           = base_fp >> 8;
    s_st.total_events      += s_ev_pend;
    s_st.overflow          += s_ovf_pend;
    s_ev_pend = s_ovf_pend = 0;
    s_busy_cyc += (uint32_t)(esp_cpu_get_cycle_count() - cyc0);
    xSemaphoreGive(s_lock);
}

void mca_dsp_tick_1s(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_st.cps = (uint32_t)(s_st.total_events - s_last_events);
    s_last_events = s_st.total_events;
    s_st.run_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    s_st.chunks_lost = adc_cap_chunks_lost();
    /* Загрузка = доля тактов ядра, ушедших на обработку, за время с
     * прошлого замера. Близко к 100% - обработка не успевает, пойдут
     * потерянные чанки. */
    int64_t now = esp_timer_get_time();
    if (s_load_t_us > 0 && now > s_load_t_us)
        s_st.load_pm = (uint32_t)(s_busy_cyc * 1000ULL /
                       ((uint64_t)(now - s_load_t_us) *
                        CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ));
    s_busy_cyc  = 0;
    s_load_t_us = now;
    xSemaphoreGive(s_lock);
}

void mca_dsp_get_params(mca_params_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_par;
    xSemaphoreGive(s_lock);
}

void mca_dsp_set_params(const mca_params_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mca_params_t old = s_par;
    s_par = *in;

    if (s_par.trap_L < 1) s_par.trap_L = 1;
    if (s_par.trap_L > TRAP_L_MAX) s_par.trap_L = TRAP_L_MAX;
    if (s_par.trap_G < 1) s_par.trap_G = 1;
    if (s_par.trap_G > TRAP_G_MAX) s_par.trap_G = TRAP_G_MAX;
    /* Сумма окон не должна выходить за хвост рабочего буфера,
     * иначе чтение уйдёт за его начало. */
    /* Зазор ОБЯЗАН быть больше окна, иначе плоской вершины нет
     * и трапеция вырождается в треугольник. */
    if (s_par.trap_G <= s_par.trap_L + 2)
        s_par.trap_G = s_par.trap_L + 3;
    if (s_par.trap_L + s_par.trap_G >= DSP_TAIL - 4)
        s_par.trap_G = DSP_TAIL - 4 - s_par.trap_L;
    if (s_par.search < 1) s_par.search = 1;
    if (s_par.search > 256) s_par.search = 256;
    if (s_par.rearm < 0) s_par.rearm = 0;
    /* Каналов на экране - только 2048/4096/8192. Произвольный ввод
     * приводим к ближайшему разрешённому значению. */
    if (s_par.nch != 2048 && s_par.nch != 4096 && s_par.nch != 8192) {
        if (s_par.nch <= 3072)      s_par.nch = 2048;
        else if (s_par.nch <= 6144) s_par.nch = 4096;
        else                        s_par.nch = 8192;
    }
    if (s_par.cpc_milli < 10)     s_par.cpc_milli = 10;      /* 0.01 кода  */
    if (s_par.cpc_milli > 100000) s_par.cpc_milli = 100000;  /* 100 кодов  */
    if (s_par.algo != 1) s_par.algo = 0;
    s_par.polarity = s_par.polarity ? 1 : 0;
    if (s_par.int_rise < 0)  s_par.int_rise = 0;
    if (s_par.int_rise > 200) s_par.int_rise = 200;
    if (s_par.int_fall < 1)  s_par.int_fall = 1;
    if (s_par.int_fall > 400) s_par.int_fall = 400;
    if (s_par.baseline_win < 1)    s_par.baseline_win = 1;
    if (s_par.baseline_win > 4095) s_par.baseline_win = 4095;

    /* Смена L или G меняет саму передаточную функцию: накопленный
     * аккумулятор становится бессмысленным (в нём слагаемые от старых
     * окон). Сбрасываем - иначе трапеция уедет и не вернётся. */
    /* Смена полярности - то же самое: в буфере и в аккумуляторе лежит
     * сигнал старого знака, и базовую линию надо захватить заново. */
    if (s_par.trap_L != old.trap_L || s_par.trap_G != old.trap_G ||
        s_par.polarity != old.polarity) {
        s_trap       = 0;
        s_state      = ST_IDLE;
        s_need_prime = true;
        s_base_init  = false;
        ESP_LOGI(TAG, "L/G или полярность изменены -> фильтр перезапущен с прогревом");
    }
    /* «Кодов на канал» и способ измерения меняют саму раскладку событий
     * по каналам: старый спектр с новым не складывается - очищаем.
     * Число каналов раскладку не меняет - от него зависит только показ. */
    if (s_par.cpc_milli != old.cpc_milli || s_par.algo != old.algo) {
        hist_clear_locked();
        ESP_LOGI(TAG, "шкала или способ изменены -> спектр очищен");
    }
    xSemaphoreGive(s_lock);
}

/* Без блокировки. Страница спектра вызывает это до 16 раз на запрос,
 * каждый раз копируя 2 КБ из PSRAM, - под мьютексом, который задача
 * обработки берёт на каждом чанке: на 16 МГц это задерживало её и
 * давало потерянные чанки. В гистограмму пишет только задача обработки
 * (record_event, тоже без блокировки), слово читается целиком, а канал,
 * увеличенный на единицу мгновением позже, картинке не вредит. */
size_t mca_dsp_get_spectrum(uint32_t *dst, size_t from, size_t count)
{
    if (from >= MCA_CHANNELS) return 0;
    if (from + count > MCA_CHANNELS) count = MCA_CHANNELS - from;
    memcpy(dst, s_hist + from, count * sizeof(uint32_t));
    return count;
}

void mca_dsp_get_stats(mca_stats_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
}

void mca_dsp_flush(void)
{
    /* Буфер больше не описывает непрерывный сигнал: между старыми
     * и новыми отсчётами дыра. Не обнуляем его (это само создало бы
     * скачок), а помечаем, что следующий чанк начинается с прогрева. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state      = ST_IDLE;
    s_since      = 0;
    s_need_prime = true;
    xSemaphoreGive(s_lock);
}

/* Без мьютекса: одно слово читается атомарно, а знак синхронизации
 * осциллографу нужен на каждом чанке. */
bool mca_dsp_polarity_neg(void)
{
    return s_par.polarity != 0;
}
