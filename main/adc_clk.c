#include "adc_clk.h"
#include "mca_config.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_clk_tree.h"
#include "esp_private/periph_ctrl.h"
#include "soc/lcd_cam_struct.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "adc_clk";

/* ТАБЛИЦА ЧАСТОТ ДЛЯ ВЕБА. Страница берёт её из /cfg (поле fl), так
 * что свой список у каждого генератора.
 *
 * ПОЧЕМУ ВООБЩЕ ВАЖЕН ГЕНЕРАТОР. Замер на приборе: с CLK 16 МГц от LEDC
 * (делитель 5 нечётный, скважность 40/60) гармоника 154 x 16 = 2464 МГц
 * садилась в полосу канала 10 роутера, пакеты WiFi терялись, и
 * обновление осциллографа рвалось даже при остановленном захвате. Смена
 * канала роутера на 6 помогла. */
#if MCA_CLK_GEN_CAM
/* CAM_CLK: целые чётные делители от 160 или 240 МГц (в скобках).
 * На 17.14 МГц связь пропала - см. mca_config.h, MCA_CLK_GEN_CAM. */
const uint32_t mca_freq_table[MCA_FREQ_COUNT] = {
    1000000,   /* 160/160 */
    2000000,   /* 160/80  */
    4000000,   /* 160/40  */
    5000000,   /* 160/32  */
    6666667,   /* 160/24  */
    8000000,   /* 160/20 - по умолчанию, см. MCA_FREQ_DEFAULT_IDX */
    8888889,   /* 160/18  */
    10000000,  /* 160/16  */
    11428571,  /* 160/14  */
    12000000,  /* 240/20  */
    13333333,  /* 160/12  */
    15000000,  /* 240/16  */
    16000000,  /* 160/10  */
    17142857,  /* 240/14 - обработка спектра на пределе          */
    20000000,  /* 160/8  - спектр не успевает, для осциллографа  */
};
#else
/* LEDC: только 80 МГц / K (в скобках K). При чётном K скважность 50 %,
 * при нечётном - нет (для 16 МГц это 40/60). */
const uint32_t mca_freq_table[MCA_FREQ_COUNT] = {
    1000000,   /* 80  */
    2000000,   /* 40  */
    4000000,   /* 20  */
    5000000,   /* 16  */
    6666667,   /* 12  */
    8000000,   /* 10 - по умолчанию, см. MCA_FREQ_DEFAULT_IDX */
    8888889,   /* 9   */
    10000000,  /* 8   */
    11428571,  /* 7   */
    13333333,  /* 6   */
    16000000,  /* 5   */
    20000000,  /* 4 - спектр не успевает, для осциллографа */
};
#endif

static uint32_t s_freq = 0;

#if MCA_CLK_GEN_CAM
/* ================= CLK ОТ БЛОКА LCD_CAM =================
 *
 * У блока камеры есть свой делитель такта и выход CAM_CLK - через него
 * камерам подают такт (XCLK), так делает и драйвер esp32-camera. Мы
 * выводим CAM_CLK на GPIO5 вместо LEDC.
 *
 * ОН ЖЕ - ТАКТ САМОГО МОДУЛЯ CAM. Раньше модуль шёл от 240/2 = 120 МГц,
 * теперь идёт на частоте АЦП. Данные защёлкиваются по внешнему PCLK
 * (перемычка на GPIO18), так что это должно работать, но проверено
 * только замером на приборе: если захват сбоит, верните
 * MCA_CLK_GEN_CAM 0 в mca_config.h - вернётся LEDC.
 *
 * Регистры делителя сбрасывает periph_module_reset в adc_cap.c, поэтому
 * хозяин у них один - этот файл: adc_cap после сброса зовёт
 * adc_clk_cam_apply(). */

/* Значения поля cam_clk_sel (hal/esp32s3 cam_ll.h). */
#define SEL_PLL240 2        /* PLL_D2: половина BBPLL, 240 МГц при CPU 240 */
#define SEL_PLL160 3        /* PLL_F160M                                  */

static uint8_t s_sel = SEL_PLL160;
static uint8_t s_div = 20;

void adc_clk_cam_apply(void)
{
    LCD_CAM.cam_ctrl.cam_clk_sel      = s_sel;
    LCD_CAM.cam_ctrl.cam_clkm_div_num = s_div;
    LCD_CAM.cam_ctrl.cam_clkm_div_a   = 0;     /* без дробной части: */
    LCD_CAM.cam_ctrl.cam_clkm_div_b   = 0;     /* она дала бы дрожание */
    LCD_CAM.cam_ctrl.cam_update       = 1;
}

/* Подобрать источник и делитель. Сначала чётный делитель (скважность
 * 50 %), потом любой, и только если точно не выходит - ближайшее.
 * Частоту источника спрашиваем у IDF, а не зашиваем: PLL_D2 равен 240
 * только при частоте процессора 240 МГц. */
static bool cam_pick(uint32_t hz, uint8_t *sel, uint8_t *div, uint32_t *real)
{
    const struct { uint8_t sel; soc_module_clk_t clk; } src[2] = {
        { SEL_PLL160, SOC_MOD_CLK_PLL_F160M },
        { SEL_PLL240, SOC_MOD_CLK_PLL_D2 },
    };
    uint32_t f[2] = { 0, 0 };
    for (int s = 0; s < 2; s++)
        if (esp_clk_tree_src_get_freq_hz(src[s].clk,
                ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &f[s]) != ESP_OK)
            f[s] = 0;

    uint32_t best_err = UINT32_MAX;
    for (int pass = 0; pass < 3; pass++) {
        for (int s = 0; s < 2; s++) {
            if (!f[s]) continue;
            for (uint32_t d = 2; d <= 255; d++) {
                if (pass == 0 && (d & 1)) continue;
                const uint32_t r   = (f[s] + d / 2) / d;
                const uint32_t err = r > hz ? r - hz : hz - r;
                /* в таблице частоты округлены до герца */
                if (pass < 2 ? err <= 2 : err < best_err) {
                    *sel = src[s].sel;
                    *div = (uint8_t)d;
                    *real = r;
                    if (pass < 2) return true;
                    best_err = err;
                }
            }
        }
    }
    return false;
}

static esp_err_t cam_set(uint32_t freq_hz, bool first)
{
    uint8_t  sel, div;
    uint32_t real = 0;
    const bool exact = cam_pick(freq_hz, &sel, &div, &real);
    if (!real) {
        ESP_LOGE(TAG, "не удалось узнать частоту источников такта");
        return ESP_FAIL;
    }
    s_sel = sel;
    s_div = div;

    if (first) {
        /* Блок включаем здесь: CLK нужен раньше, чем захват. adc_cap
         * включит его ещё раз (счётчик ссылок) и сбросит - после сброса
         * он сам вызовет adc_clk_cam_apply(). */
        periph_module_enable(PERIPH_LCD_CAM_MODULE);
    }
    adc_clk_cam_apply();

    if (first) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << PIN_ADC_CLK_OUT,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        esp_rom_gpio_connect_out_signal(PIN_ADC_CLK_OUT, CAM_CLK_IDX,
                                        false, false);
        /* Ток выхода - как был у LEDC: фронты на перемычке иначе вялые. */
        gpio_set_drive_capability(PIN_ADC_CLK_OUT, GPIO_DRIVE_CAP_3);
    }

    s_freq = real;
    ESP_LOGI(TAG, "CLK %.3f МГц = %u МГц / %u%s, от блока CAM на GPIO%d%s",
             real / 1e6, sel == SEL_PLL160 ? 160 : 240, div,
             (div & 1) ? " (делитель нечётный - скважность не 50%)" : "",
             PIN_ADC_CLK_OUT,
             exact ? "" : " - ТОЧНО ЗАПРОШЕННУЮ НЕ ПОЛУЧИТЬ, взята ближайшая");
    return ESP_OK;
}

esp_err_t adc_clk_start(uint32_t freq_hz)    { return cam_set(freq_hz, true);  }
esp_err_t adc_clk_set_freq(uint32_t freq_hz) { return cam_set(freq_hz, false); }

#else  /* ================= ЗАПАСНОЙ ВАРИАНТ: LEDC ================= */

/* Только частоты вида 80 МГц / K: период складывается из целого числа
 * тактов APB (12.5 нс). Частоты таблицы, которые так не получить
 * (12, 15, 17.14 МГц), встанут на ближайшую достижимую - прибор
 * покажет, какая вышла на самом деле. При нечётном K скважность не
 * 50 % (для K=5 это 40/60), и гармоники CLK сильнее бьют по WiFi. */

#define CLK_TIMER   LEDC_TIMER_0
#define CLK_CHANNEL LEDC_CHANNEL_0
#define CLK_MODE    LEDC_LOW_SPEED_MODE

/* На высоких частотах LEDC остаётся мало бит разрешения:
 * duty_res <= log2(80MHz / freq). Скважность 50% = половина от (1<<res). */
static ledc_timer_bit_t pick_resolution(uint32_t freq_hz)
{
    uint32_t div = 80000000u / freq_hz;
    int bits = 0;
    while ((1u << (bits + 1)) <= div && bits < 14) bits++;
    if (bits < 1) bits = 1;
    return (ledc_timer_bit_t)bits;
}

/* ближайшая 80/K */
static uint32_t ledc_snap(uint32_t hz)
{
    uint32_t k = (80000000u + hz / 2) / hz;
    if (k < 4) k = 4;
    return 80000000u / k;
}

static esp_err_t ledc_set(uint32_t freq_hz, bool first)
{
    freq_hz = ledc_snap(freq_hz);
    ledc_timer_bit_t res = pick_resolution(freq_hz);

    ledc_timer_config_t tcfg = {
        .speed_mode      = CLK_MODE,
        .timer_num       = CLK_TIMER,
        .duty_resolution = res,
        .freq_hz         = freq_hz,
        /* APB как источник. PM отключён (CONFIG_PM_ENABLE=n),
         * поэтому частота APB фиксирована и CLK не поплывёт. */
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer_config %lu Hz res=%d: %s",
                 (unsigned long)freq_hz, res, esp_err_to_name(err));
        return err;
    }

    if (first) {
        ledc_channel_config_t ccfg = {
            .gpio_num   = PIN_ADC_CLK_OUT,
            .speed_mode = CLK_MODE,
            .channel    = CLK_CHANNEL,
            .timer_sel  = CLK_TIMER,
            .duty       = (1u << res) / 2,
            .hpoint     = 0,
            .intr_type  = LEDC_INTR_DISABLE,
        };
        err = ledc_channel_config(&ccfg);
        if (err != ESP_OK) return err;
    } else {
        ledc_set_duty(CLK_MODE, CLK_CHANNEL, (1u << res) / 2);
        ledc_update_duty(CLK_MODE, CLK_CHANNEL);
    }

    gpio_set_drive_capability(PIN_ADC_CLK_OUT, GPIO_DRIVE_CAP_3);

    /* НЕ ВЕРИМ запрошенному значению - спрашиваем, что вышло. */
    s_freq = ledc_get_freq(CLK_MODE, CLK_TIMER);
    if (s_freq == 0) s_freq = freq_hz;
    ESP_LOGI(TAG, "CLK %.3f МГц от LEDC на GPIO%d (res=%d бит)",
             s_freq / 1e6, PIN_ADC_CLK_OUT, res);
    return ESP_OK;
}

/* Модуль CAM при LEDC тактируется как раньше: 240/2 = 120 МГц. */
void adc_clk_cam_apply(void)
{
    LCD_CAM.cam_ctrl.cam_clk_sel      = 2;
    LCD_CAM.cam_ctrl.cam_clkm_div_num = 2;
    LCD_CAM.cam_ctrl.cam_clkm_div_a   = 0;
    LCD_CAM.cam_ctrl.cam_clkm_div_b   = 0;
}

esp_err_t adc_clk_start(uint32_t freq_hz)    { return ledc_set(freq_hz, true);  }
esp_err_t adc_clk_set_freq(uint32_t freq_hz) { return ledc_set(freq_hz, false); }

#endif

uint32_t adc_clk_get_freq(void) { return s_freq; }
