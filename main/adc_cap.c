#include "adc_cap.h"
#include "adc_clk.h"
#include "mca_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/gdma.h"
#include "hal/dma_types.h"
#include "hal/gpio_hal.h"
#include "soc/lcd_cam_struct.h"
#include "soc/lcd_cam_reg.h"
#include "soc/gpio_sig_map.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static const char *TAG = "adc_cap";

/* Виртуальные "пины" GPIO-матрицы ESP32-S3: подать на вход периферии
 * постоянный уровень, не занимая физическую ножку. */
#define GPIO_MATRIX_CONST_ZERO  0x30
#define GPIO_MATRIX_CONST_ONE   0x38

/* ---- состояние ---- */
static uint16_t          *s_buf[CAP_N_CHUNKS];
static dma_descriptor_t  *s_desc;
static gdma_channel_handle_t s_dma;
static QueueHandle_t      s_queue;
static volatile bool      s_running;
static volatile uint64_t  s_lost;
static volatile bool      s_gap;      /* был пропуск с прошлой выдачи */
static volatile uint64_t  s_chunks;      /* всего чанков от DMA */
static volatile int       s_last_ready = -1;

/* РАБОЧИЙ РЕЖИМ, найден автоподбором на железе (вариант 16).
 * Все полярности прямые, а кадровая синхронизация должна быть
 * прибита к ПОСТОЯННОМУ НУЛЮ - изначально я прибил её к единице
 * (данные, мол, всегда валидны), и это было неверно.
 * Автоподбор остаётся в прошивке на случай другой платы. */
static cam_mode_t s_mode = { .vh_de_mode = 0, .vsync_inv = 0,
                             .hsync_inv = 0, .de_inv = 0, .vsync_src = 1 };

typedef struct { int idx; } cap_msg_t;

/* ---------- ISR: чанк заполнен ---------- */
static IRAM_ATTR bool on_recv_eof(gdma_channel_handle_t ch,
                                  gdma_event_data_t *ev, void *user)
{
    BaseType_t hp = pdFALSE;
    /* ev->rx_eof_desc_addr указывает на завершившийся дескриптор */
    dma_descriptor_t *d = (dma_descriptor_t *)ev->rx_eof_desc_addr;
    int idx = -1;
    for (int i = 0; i < CAP_N_CHUNKS; i++) {
        if (s_desc[i].buffer == (void *)s_buf[i] && &s_desc[i] == d) {
            idx = i; break;
        }
    }
    if (idx < 0) return false;

    s_last_ready = idx;
    s_chunks++;
    cap_msg_t m = { .idx = idx };
    if (xQueueSendFromISR(s_queue, &m, &hp) != pdTRUE) {
        s_lost++;               /* DSP не успевает - чанк потерян */
        s_gap = true;           /* поток разорван, фильтру нужен сброс */
    }
    return hp == pdTRUE;
}

/* ---------- настройка GPIO ---------- */
static void cap_gpio_setup(void)
{
    const int pins[16] = MCA_DATA_PINS;

    /* Линии данных -> CAM_DATA_IN0..15.
     *
     * Шина у CAM 16-битная, а у нас задействовано только 13 линий
     * (D0..D11 + OTR). Оставшиеся ТРИ нельзя бросать висеть: вход
     * периферии, не привязанный ни к чему, ловит наводки и сыплет
     * мусор в старшие биты слова. Прибиваем их к логическому нулю
     * через GPIO-матрицу.
     *
     * Константы матрицы: 0x30 = постоянный ноль, 0x38 = постоянная
     * единица. Подтверждено рабочим кодом проекта espScope
     * (там же и приём с VSYNC/HREF, см. ниже). */
    for (int i = 0; i < 16; i++) {
        /* Бит переполнения (OTR) в камерный интерфейс НЕ заводим:
         * обработке он не нужен, а его наличие заставляло накладывать
         * маску на каждое из четырёх обращений в горячем цикле.
         * Прибиваем к нулю - тогда слово в памяти это чистые 12 бит
         * данных, и маска не нужна нигде. */
        if (i >= MCA_ADC_BITS || pins[i] < 0) {
            /* неиспользуемая линия -> жёсткий ноль, не "в воздухе" */
            esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO,
                                           CAM_DATA_IN0_IDX + i, false);
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        esp_rom_gpio_connect_in_signal(pins[i], CAM_DATA_IN0_IDX + i, false);
    }

    /* PCLK: вход с перемычки GPIO5 -> GPIO18 */
    gpio_config_t pclk = {
        .pin_bit_mask = 1ULL << PIN_CAM_PCLK_IN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pclk);
    esp_rom_gpio_connect_in_signal(PIN_CAM_PCLK_IN, CAM_PCLK_IDX, false);

    /* Сенсора нет - кадров и строк не существует. В параллельном
     * камерном режиме данные принимаются только когда
     * V_SYNC = H_SYNC = H_ENABLE = 1, поэтому прибиваем их
     * к постоянной единице. */
    esp_rom_gpio_connect_in_signal(
        s_mode.vsync_src == 1 ? GPIO_MATRIX_CONST_ZERO
                              : GPIO_MATRIX_CONST_ONE,
        CAM_V_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE,
                                   CAM_H_SYNC_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE,
                                   CAM_H_ENABLE_IDX, false);
}

/* Программный фронт на входе кадровой синхронизации.
 * Физического вывода не требует - дёргаем константы GPIO-матрицы. */
static void cam_vsync_pulse(void)
{
    /* Паузы обязательны. Три записи подряд идут за десятки наносекунд,
     * и модуль может просто не разглядеть такой короткий перепад -
     * он опрашивает вход своим тактом, да ещё и через фильтр. */
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE,
                                   CAM_V_SYNC_IDX, false);
    esp_rom_delay_us(50);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO,
                                   CAM_V_SYNC_IDX, false);
    esp_rom_delay_us(50);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE,
                                   CAM_V_SYNC_IDX, false);
    esp_rom_delay_us(50);
}

/* ---------- настройка CAM ---------- */
static void cap_cam_setup(void)
{
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    periph_module_reset(PERIPH_LCD_CAM_MODULE);

    LCD_CAM.cam_ctrl.val  = 0;
    LCD_CAM.cam_ctrl1.val = 0;

    /* ТАКТИРОВАНИЕ САМОГО МОДУЛЯ CAM.
     *
     * Это отдельная вещь от внешнего PCLK! Данные защёлкиваются по
     * PCLK, но внутренняя логика блока должна быть запитана своим
     * тактом. cam_clk_sel == 0 означает "тактирование выключено",
     * и тогда модуль молчит, сколько ни подавай PCLK снаружи.
     * Обнулив cam_ctrl.val выше, мы как раз обнулили и это поле.
     *
     * Значения: 0 = выкл, 1 = XTAL, 2 = PLL_D2 (240 МГц), 3 = PLL_F160M.
     *
     * Источник и делитель задаёт adc_clk: при MCA_CLK_GEN_CAM этот же
     * делитель даёт CLK для АЦП (выход CAM_CLK), и сброс выше его
     * остановил - здесь он запускается снова. */
    adc_clk_cam_apply();

    LCD_CAM.cam_ctrl.cam_stop_en          = 0;
    LCD_CAM.cam_ctrl.cam_vsync_filter_thres = 0;
    LCD_CAM.cam_ctrl.cam_update           = 0;
    LCD_CAM.cam_ctrl.cam_byte_order       = 0;
    LCD_CAM.cam_ctrl.cam_bit_order        = 0;
    LCD_CAM.cam_ctrl.cam_line_int_en      = 0;
    LCD_CAM.cam_ctrl.cam_vs_eof_en        = 0;  /* EOF по счётчику байт,
                                                   а не по VSYNC        */

    /* 16-битный режим: за один такт PCLK забираем слово целиком. */
    LCD_CAM.cam_ctrl1.cam_2byte_en        = 1;
    LCD_CAM.cam_ctrl1.cam_clk_inv         = 0;  /* <-- ПОДБИРАЕТСЯ:
                                                   фронт защёлкивания   */
    LCD_CAM.cam_ctrl1.cam_vsync_filter_en = 0;
    LCD_CAM.cam_ctrl1.cam_vh_de_mode_en   = s_mode.vh_de_mode;
    LCD_CAM.cam_ctrl1.cam_de_inv          = s_mode.de_inv;
    LCD_CAM.cam_ctrl1.cam_hsync_inv       = s_mode.hsync_inv;
    LCD_CAM.cam_ctrl1.cam_vsync_inv       = s_mode.vsync_inv;

    /* Через сколько байт CAM выставит in_suc_eof. */
    LCD_CAM.cam_ctrl1.cam_rec_data_bytelen = CAP_CHUNK_BYTES - 1;

    LCD_CAM.cam_ctrl.cam_update = 1;
}

/* ---------- DMA ---------- */
static esp_err_t cap_dma_setup(void)
{
    /* Буферы во ВНУТРЕННЕЙ RAM: PSRAM не тянет 30 МБ/с потока
     * и добавляет непредсказуемые задержки. */
    for (int i = 0; i < CAP_N_CHUNKS; i++) {
        s_buf[i] = heap_caps_aligned_alloc(64, CAP_CHUNK_BYTES,
                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "нет памяти под чанк %d", i);
            return ESP_ERR_NO_MEM;
        }
        memset(s_buf[i], 0, CAP_CHUNK_BYTES);
    }

    s_desc = heap_caps_aligned_alloc(64,
                 sizeof(dma_descriptor_t) * CAP_N_CHUNKS,
                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_desc) return ESP_ERR_NO_MEM;

    /* Кольцо дескрипторов: последний ссылается на первый.
     * Захват идёт непрерывно, старые данные затираются новыми -
     * это и есть наш ring buffer с предысторией для pre-trigger. */
    /* Страховка на будущее: поле размера 12-битное. */
    _Static_assert(CAP_CHUNK_BYTES <= 4095,
                   "размер чанка не влезает в 12-битное поле дескриптора GDMA");

    for (int i = 0; i < CAP_N_CHUNKS; i++) {
        s_desc[i].dw0.size     = CAP_CHUNK_BYTES;
        s_desc[i].dw0.length   = 0;
        s_desc[i].dw0.owner    = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        s_desc[i].dw0.suc_eof  = 0;
        s_desc[i].buffer       = s_buf[i];
        s_desc[i].next         = &s_desc[(i + 1) % CAP_N_CHUNKS];
    }

    gdma_channel_alloc_config_t cfg = {
        .direction = GDMA_CHANNEL_DIRECTION_RX,
    };
    ESP_ERROR_CHECK(gdma_new_ahb_channel(&cfg, &s_dma));
    gdma_connect(s_dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_CAM, 0));

    gdma_strategy_config_t strat = {
        .owner_check      = false,   /* кольцо - владельца не проверяем */
        .auto_update_desc = true,
    };
    gdma_apply_strategy(s_dma, &strat);

    gdma_rx_event_callbacks_t cbs = { .on_recv_eof = on_recv_eof };
    gdma_register_rx_event_callbacks(s_dma, &cbs, NULL);

    return ESP_OK;
}

esp_err_t adc_cap_init(void)
{
    /* Глубина очереди = числу буферов, и не больше.
     * Раньше очередь была в восемь раз глубже, но номер в ней
     * ссылается на буфер, который к моменту разбора уже перезаписан
     * контроллером - мы бы молча обрабатывали затёртые данные,
     * считая их целыми. Теперь переполнение честно фиксируется
     * как потеря чанка. */
    s_queue = xQueueCreate(CAP_N_CHUNKS, sizeof(cap_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    cap_gpio_setup();
    cap_cam_setup();
    ESP_ERROR_CHECK(cap_dma_setup());

    ESP_LOGI(TAG, "захват готов: %d чанков x %d отсчётов",
             CAP_N_CHUNKS, CAP_CHUNK_SAMPLES);
    return ESP_OK;
}

/* Голый запуск без проверок и логов - для перебора вариантов. */
static void adc_cap_start_quiet(void)
{
    xQueueReset(s_queue);
    for (int i = 0; i < CAP_N_CHUNKS; i++) {
        s_desc[i].dw0.owner  = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        s_desc[i].dw0.length = 0;
    }

    /* ПОСЛЕДОВАТЕЛЬНОСТЬ ПО ДОКУМЕНТАЦИИ.
     * Раньше я делал по-своему и упускал сброс контроллера DMA,
     * а регистры обновлял уже после запуска передачи. Правильный
     * порядок: сброс DMA -> обновление регистров -> сброс приёмного
     * буфера -> запуск DMA -> запуск камеры.
     *
     * Сигналы сброса камеры и её буфера в документации помечены
     * как "только запись" и снимаются аппаратно сами: писать в них
     * ноль не нужно, а чтение-модификация-запись всего регистра
     * ради этого могла задевать соседние биты. */
    gdma_reset(s_dma);
    LCD_CAM.cam_ctrl.cam_update       = 1;
    LCD_CAM.cam_ctrl1.cam_afifo_reset = 1;

    gdma_start(s_dma, (intptr_t)&s_desc[0]);
    LCD_CAM.cam_ctrl1.cam_start       = 1;

    if (s_mode.vsync_src == 2) cam_vsync_pulse();
    s_running = true;
}

esp_err_t adc_cap_start(void)
{
    if (s_running) return ESP_OK;

    xQueueReset(s_queue);
    for (int i = 0; i < CAP_N_CHUNKS; i++) {
        s_desc[i].dw0.owner  = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        s_desc[i].dw0.length = 0;
    }

    /* Порядок по документации (см. комментарий в adc_cap_start_quiet) */
    gdma_reset(s_dma);
    LCD_CAM.cam_ctrl.cam_update       = 1;
    LCD_CAM.cam_ctrl1.cam_afifo_reset = 1;

    gdma_start(s_dma, (intptr_t)&s_desc[0]);
    LCD_CAM.cam_ctrl1.cam_start       = 1;

    /* ПРОВЕРЯЕМ, ЧТО БИТ ЗАПУСКА УДЕРЖАЛСЯ.
     * Из дампа регистров на реальном железе выяснилось, что cam_start
     * не оставался установленным - поэтому не верим записи на слово,
     * а читаем обратно и пробуем ещё раз в другом порядке. */
    if (LCD_CAM.cam_ctrl1.cam_start == 0) {
        ESP_LOGW(TAG, "cam_start не удержался, пробую иначе");
        /* вариант 2: сначала start, потом update */
        LCD_CAM.cam_ctrl1.cam_start = 1;
        LCD_CAM.cam_ctrl.cam_update = 1;
    }
    if (LCD_CAM.cam_ctrl1.cam_start == 0) {
        /* вариант 3: запись всего слова целиком, без чтения-модификации */
        uint32_t v = LCD_CAM.cam_ctrl1.val | (1u << 29);
        LCD_CAM.cam_ctrl1.val = v;
        LCD_CAM.cam_ctrl.cam_update = 1;
    }
    /* ===== ЗАПУСК КАДРА =====
     *
     * Камерный интерфейс рассчитан на настоящий сенсор: захват
     * начинается ПО ФРОНТУ кадровой синхронизации. Мы прибили VSYNC
     * к постоянной единице (чтобы данные всегда считались валидными) -
     * и тем самым лишили модуль фронта, которого он ждёт. Он честно
     * запущен и вечно стоит в ожидании начала кадра.
     *
     * Фронт делаем программно, не занимая ни одной ножки: переключаем
     * вход VSYNC между константным нулём и единицей прямо в
     * GPIO-матрице. Для периферии это выглядит как настоящий перепад.
     */
    cam_vsync_pulse();

    /* захват включается и выключается при каждой смене вкладки, поэтому
     * эти сообщения - на отладочном уровне (по умолчанию не печатаются) */
    ESP_LOGD(TAG, "после запуска: ctrl=0x%08lx ctrl1=0x%08lx cam_start=%d",
             (unsigned long)LCD_CAM.cam_ctrl.val,
             (unsigned long)LCD_CAM.cam_ctrl1.val,
             (int)LCD_CAM.cam_ctrl1.cam_start);

    s_running = true;

    /* ПРОВЕРКА, А НЕ РАПОРТ.
     * Записать биты в регистры мало - это ничего не говорит о том,
     * идут ли данные с платы. Ждём реальных чанков от DMA. */
    uint64_t before = s_chunks;

    /* ПРОВЕРКА PCLK. Считаем переходы на входном пине за 20 мс.
     * CAM тактируется ИМЕННО ЭТИМ пином, и если на нём тишина -
     * дальше искать бессмысленно, дело в перемычке, а не в регистрах. */
    int pclk_seen = 0;
    {
        int prev = gpio_get_level(PIN_CAM_PCLK_IN);
        int64_t t_end = esp_timer_get_time() + 20000;
        while (esp_timer_get_time() < t_end) {
            int cur = gpio_get_level(PIN_CAM_PCLK_IN);
            if (cur != prev) { pclk_seen++; prev = cur; if (pclk_seen > 8) break; }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(150));

    if (s_chunks == before) {
        if (pclk_seen == 0) {
            ESP_LOGE(TAG, "НА ВХОДЕ PCLK (GPIO%d) НЕТ ТАКТА.",
                     PIN_CAM_PCLK_IN);
            ESP_LOGE(TAG, "  Поставьте перемычку GPIO%d -> GPIO%d.",
                     PIN_ADC_CLK_OUT, PIN_CAM_PCLK_IN);
            ESP_LOGE(TAG, "  Без неё камерный интерфейс не тактируется "
                          "и не выдаст ни одного отсчёта.");
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGE(TAG, "PCLK на GPIO%d ЕСТЬ, но чанков нет - значит дело "
                      "в настройке LCD_CAM, а не в монтаже.",
                 PIN_CAM_PCLK_IN);
        ESP_LOGE(TAG, "  Запустите автоподбор режима на /diag (кнопка "
                      "подбора управляющих сигналов камеры).");
        ESP_LOGE(TAG, "ЗАХВАТ НЕ ПОШЁЛ: за 150 мс не пришло ни одного чанка");
        ESP_LOGE(TAG, "  проверьте: перемычка GPIO%d->GPIO%d (CLK->PCLK),",
                 PIN_ADC_CLK_OUT, PIN_CAM_PCLK_IN);
        ESP_LOGE(TAG, "  питание платы AD9226, перемычки R32-R35 на плате,");
        ESP_LOGE(TAG, "  подключение линий данных D0-D11");
        return ESP_ERR_TIMEOUT;      /* захват оставляем включённым,
                                        чтобы /diag мог наблюдать */
    }

    ESP_LOGD(TAG, "захват идёт: %llu чанков за 150 мс",
             (unsigned long long)(s_chunks - before));
    return ESP_OK;
}

esp_err_t adc_cap_stop(void)
{
    if (!s_running) return ESP_OK;
    LCD_CAM.cam_ctrl1.cam_start = 0;
    gdma_stop(s_dma);
    s_running = false;
    ESP_LOGD(TAG, "захват остановлен");
    return ESP_OK;
}

bool adc_cap_is_running(void) { return s_running; }

size_t adc_cap_snapshot(uint16_t *dst, size_t max_samples)
{
    int idx = s_last_ready;
    if (idx < 0 || idx >= CAP_N_CHUNKS) return 0;
    size_t n = max_samples < CAP_CHUNK_SAMPLES ? max_samples
                                               : CAP_CHUNK_SAMPLES;
    memcpy(dst, s_buf[idx], n * 2);
    return n;
}

bool adc_cap_wait_chunk(const uint16_t **out, size_t *n, uint32_t ms)
{
    cap_msg_t m;
    if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(ms)) != pdTRUE)
        return false;
    *out = s_buf[m.idx];
    *n   = CAP_CHUNK_SAMPLES;
    return true;
}

/* Был ли разрыв потока с прошлого вызова. Сбрасывает признак.
 * Нужно, чтобы фильтр не принял скачок на стыке за импульс. */
bool adc_cap_take_gap(void)
{
    bool g = s_gap;
    s_gap = false;
    return g;
}

/* Сколько чанков уже ждёт разбора. Равно числу буферов - значит
 * следующий чанк будет потерян. */
size_t adc_cap_queue_depth(void)
{
    return s_queue ? uxQueueMessagesWaiting(s_queue) : 0;
}

uint64_t adc_cap_chunks_lost(void)  { return s_lost; }
uint64_t adc_cap_chunks_total(void) { return s_chunks; }

/* Повторить фронт кадровой синхронизации.
 * Нужно, если один фронт запускает ровно один кадр: после его
 * окончания поток встанет, и модуль снова будет ждать начала. */
void adc_cap_kick(void)
{
    if (!s_running) return;
    cam_vsync_pulse();
}

/* Проверка линий данных БЕЗ осциллографа.
 * Читаем порт много раз подряд и смотрим, какие биты меняются.
 * Если АЦП преобразует, младшие разряды дёргаются от шума даже
 * при отсутствии сигнала на входе. Если всё замерло - АЦП не
 * работает (питание, такт до платы), и камерный интерфейс ни при чём. */
uint32_t adc_cap_probe_data_lines(uint32_t *stuck_hi, uint32_t *stuck_lo)
{
    uint32_t changed = 0, and_acc = 0xFFFFFFFF, or_acc = 0;
    for (int i = 0; i < 4000; i++) {
        uint32_t v = (REG_READ(GPIO_IN_REG) >> MCA_DATA_SHIFT) & 0x1FFF;
        and_acc &= v;
        or_acc  |= v;
        esp_rom_delay_us(1);
    }
    changed  = or_acc & ~and_acc;      /* биты, которые менялись   */
    *stuck_hi = and_acc;               /* всегда 1                 */
    *stuck_lo = (~or_acc) & 0x1FFF;    /* всегда 0                 */
    return changed;
}

void adc_cap_dump_regs(uint32_t *ctrl, uint32_t *ctrl1, int *pclk_lvl)
{
    *ctrl     = LCD_CAM.cam_ctrl.val;
    *ctrl1    = LCD_CAM.cam_ctrl1.val;
    *pclk_lvl = gpio_get_level(PIN_CAM_PCLK_IN);
}

/* ================= АВТОПОДБОР РЕЖИМА ================= */

static const uint8_t TUNE_VSRC[3] = { 0, 1, 2 };
#define TUNE_TOTAL (2 * 2 * 2 * 2 * 3)      /* 48 вариантов */

static volatile bool s_tune_busy;
static volatile int  s_tune_idx;
static volatile int  s_tune_found = -1;

static void tune_decode(int i, cam_mode_t *m)
{
    m->vh_de_mode = i & 1;              i >>= 1;
    m->vsync_inv  = i & 1;              i >>= 1;
    m->hsync_inv  = i & 1;              i >>= 1;
    m->de_inv     = i & 1;              i >>= 1;
    m->vsync_src  = TUNE_VSRC[i % 3];
}

void adc_cap_set_mode(const cam_mode_t *m) { s_mode = *m; }
void adc_cap_get_mode(cam_mode_t *m)       { *m = s_mode; }

void adc_cap_tune_request(void)
{
    s_tune_idx   = 0;
    s_tune_found = -1;
    s_tune_busy  = true;
}
bool adc_cap_tune_busy(void)     { return s_tune_busy; }
int  adc_cap_tune_result(void)   { return s_tune_found; }
int  adc_cap_tune_total(void)    { return TUNE_TOTAL; }
int  adc_cap_tune_progress(void)
{
    return s_tune_busy ? (s_tune_idx * 100 / TUNE_TOTAL) : 100;
}

/* Один шаг перебора. Вызывается из главной задачи, чтобы не держать
 * обработчик HTTP и не ловить таймаут соединения. */
void adc_cap_tune_step(void)
{
    if (!s_tune_busy) return;

    if (s_tune_idx >= TUNE_TOTAL) {
        s_tune_busy = false;
        if (s_tune_found < 0) {
            ESP_LOGE(TAG, "автоподбор: рабочего варианта НЕ НАЙДЕНО");
            /* вернуть исходный режим, а не последний перебранный */
            cam_mode_t d = { .vh_de_mode = 0, .vsync_inv = 0,
                             .hsync_inv = 0, .de_inv = 0, .vsync_src = 1 };
            s_mode = d;
            LCD_CAM.cam_ctrl1.cam_vh_de_mode_en = 0;
            LCD_CAM.cam_ctrl1.cam_de_inv        = 0;
            LCD_CAM.cam_ctrl1.cam_hsync_inv     = 0;
            LCD_CAM.cam_ctrl1.cam_vsync_inv     = 0;
            LCD_CAM.cam_ctrl.cam_update = 1;
        }
        return;
    }

    cam_mode_t m;
    tune_decode(s_tune_idx, &m);

    adc_cap_stop();
    s_mode = m;

    /* Только поля режима, без полного сброса периферии: повторный
     * periph_module_reset в цикле оставлял бы модуль в неясном
     * состоянии. */
    LCD_CAM.cam_ctrl1.cam_vh_de_mode_en = m.vh_de_mode;
    LCD_CAM.cam_ctrl1.cam_de_inv        = m.de_inv;
    LCD_CAM.cam_ctrl1.cam_hsync_inv     = m.hsync_inv;
    LCD_CAM.cam_ctrl1.cam_vsync_inv     = m.vsync_inv;
    esp_rom_gpio_connect_in_signal(
        m.vsync_src == 1 ? GPIO_MATRIX_CONST_ZERO : GPIO_MATRIX_CONST_ONE,
        CAM_V_SYNC_IDX, false);
    LCD_CAM.cam_ctrl.cam_update = 1;

    uint64_t before = s_chunks;
    adc_cap_start_quiet();
    vTaskDelay(pdMS_TO_TICKS(120));

    if (s_chunks > before) {
        s_tune_found = s_tune_idx;
        s_tune_busy  = false;
        ESP_LOGI(TAG, "автоподбор НАШЁЛ вариант %d: vh_de=%d vs_inv=%d "
                      "hs_inv=%d de_inv=%d vsrc=%d (чанков %llu)",
                 s_tune_idx, m.vh_de_mode, m.vsync_inv, m.hsync_inv,
                 m.de_inv, m.vsync_src,
                 (unsigned long long)(s_chunks - before));
        return;
    }
    adc_cap_stop();
    s_tune_idx++;
}
