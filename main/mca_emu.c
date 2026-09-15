#include "mca_emu.h"
#include "mca_config.h"
#include "mca_dsp.h"
#include "mca_web.h"
#include "adc_clk.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "mca_emu";

/* ================= ЭМУЛЯЦИЯ MCA ПО ПРОТОКОЛУ SHPROTO =================
 *
 * Порт - UART0, тот же, что у консоли (мост USB-UART на плате). Формат
 * пакета, экранирование, CRC и разбор повторяют эталонный shproto
 * байт в байт:
 *
 *   0xFF 0xFE  <cmd> <данные...>  <CRC16 lo> <CRC16 hi>  0xA5
 *
 *  - CRC16 Modbus (полином 0xA001, начало 0xFFFF) по cmd и данным;
 *  - байты 0xFE, 0xA5 и 0xFD внутри пакета передаются как 0xFD, ~байт
 *    (0xFF не экранируется);
 *  - приёмник начинает пакет на каждом 0xFE; пакет цел, если CRC по
 *    cmd + данным + двум байтам CRC равна 0.
 *
 * Команды с ПК приходят текстом в пакете 0x03, с нулём на конце.
 * Ответы: 0x03 - текст, 0x04 - статус, 0x01 - спектр кусками по 64
 * канала (как у настоящего прибора). Во время набора каждую секунду -
 * статус, затем весь спектр: приёмники копят статус и применяют его в
 * момент, когда проход по спектру закончен, поэтому статус - первым.
 *
 * ОТВЕТЫ НА КОМАНДЫ. Программы ждут подтверждение отдельным текстовым
 * пакетом. BecqMoni после команды берёт ПЕРВЫЙ пришедший текстовый пакет,
 * обрезает его по первому \r и сравнивает целиком: на -sto, -rst, -sta
 * ждёт "-ok" (1 с), а на -sta при 38400 и 115200 - ровно
 * "Warning: silent mode forced due to low interface speed-ok" (2 с). Не
 * дождалась - «не удаётся прочитать данные из порта». Поэтому:
 *  - на каждую выполненную команду - "-ok";
 *  - команды разбираются и между пакетами спектра: на медленном порту
 *    проход длится секунды, и ответ не должен ждать его конца;
 *  - буфер передачи небольшой (1 КБ): ответ встаёт в очередь за ним, и
 *    на 38400 это ~0.27 с, а не секунда с лишним.
 * На 38400 и 115200 настоящий прибор не выгружает спектр каждую секунду
 * («silent»). Здесь спектр идёт и на этих скоростях, просто медленнее:
 * иначе программа, не опрашивающая прибор, спектра не увидела бы.
 *
 * Заводские команды настройки (-ris, -fall, -U, -V, -nos, -hyst, -step,
 * -t..., -pileup...) не выполняются, на них ответ "-err not supported":
 * программа на ПК не должна сбивать настройки обработки этого прибора -
 * они задаются на его странице. */

#define EMU_UART        UART_NUM_0
#define EMU_NS          "mca_cfg"
#define CMD_HIST        0x01
#define CMD_TEXT        0x03
#define CMD_STAT        0x04
#define CMD_REBOOT      0xF3
#define CHUNK_BINS      64          /* каналов в пакете спектра */
#define N_REGS          40          /* регистры -cal            */
#define REG_CRC         10          /* CRC32 регистров 0..9     */
#define REG_SERIAL      39
#define TX_RING         1024        /* см. «ответы на команды»  */

#define ANSWER_OK       "-ok"
#define ANSWER_SLOW     "Warning: silent mode forced due to low interface speed-ok"
#define ANSWER_UNSUPP   "-err not supported"

_Static_assert(MCA_CHANNELS % CHUNK_BINS == 0, "спектр не делится на пакеты");
_Static_assert(MCA_CHANNELS <= 65535, "смещение в пакете 16-битное");

static const uint32_t BAUDS[] = { 38400, 115200, 460800, 600000, 921600 };

static volatile uint32_t s_baud;          /* 0 - выключено            */
static TaskHandle_t      s_task;
static volatile bool     s_stop_req;
static vprintf_like_t    s_prev_vprintf;
static uint32_t          s_regs[N_REGS];
static bool              s_silent;        /* -sta -s: без выгрузки     */
static uint32_t          s_limit_s;       /* -sta xx: время набора, с  */
static bool              s_in_sweep;      /* идёт проход по спектру    */

static void rx_poll(TickType_t wait);

static bool baud_ok(uint32_t b)
{
    for (size_t i = 0; i < sizeof(BAUDS) / sizeof(BAUDS[0]); i++)
        if (BAUDS[i] == b) return true;
    return false;
}

/* ---------------- CRC ---------------- */
static uint16_t crc16_modbus(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    return crc;
}

/* Стандартная CRC32 (как у zip): по ней приёмники проверяют регистры
 * калибровки 0..9 - в ASCII, склеенными подряд. */
static uint32_t crc32_std(const char *s)
{
    uint32_t c = 0xFFFFFFFF;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return c ^ 0xFFFFFFFF;
}

/* ---------------- передача ---------------- */
/* с запасом на худший случай: всё экранировано, плюс заголовок и хвост */
static uint8_t  s_tx[1100];
_Static_assert(sizeof(s_tx) >= 2 * (1 + 2 + CHUNK_BINS * 4 + 2) + 8,
               "буфер передачи мал для пакета спектра");
static size_t   s_tx_len;
static uint16_t s_tx_crc;

static void tx_esc(uint8_t b)
{
    if (s_tx_len + 2 > sizeof(s_tx)) return;
    if (b == 0xFE || b == 0xA5 || b == 0xFD) {
        s_tx[s_tx_len++] = 0xFD;
        b = (uint8_t)~b;
    }
    s_tx[s_tx_len++] = b;
}

static void pkt_add(uint8_t b)
{
    s_tx_crc = crc16_modbus(s_tx_crc, b);
    tx_esc(b);
}

static void pkt_begin(uint8_t cmd)
{
    s_tx_len = 0;
    s_tx[s_tx_len++] = 0xFF;
    s_tx[s_tx_len++] = 0xFE;
    s_tx_crc = 0xFFFF;
    pkt_add(cmd);
}

static void pkt_end(void)
{
    const uint16_t c = s_tx_crc;
    tx_esc(c & 0xFF);
    tx_esc(c >> 8);
    if (s_tx_len < sizeof(s_tx)) s_tx[s_tx_len++] = 0xA5;
    uart_write_bytes(EMU_UART, s_tx, s_tx_len);
}

static void pkt_add16(uint16_t v) { pkt_add(v & 0xFF); pkt_add(v >> 8); }
static void pkt_add32(uint32_t v)
{
    pkt_add(v & 0xFF); pkt_add((v >> 8) & 0xFF);
    pkt_add((v >> 16) & 0xFF); pkt_add(v >> 24);
}

static void send_text(const char *s)
{
    pkt_begin(CMD_TEXT);
    while (*s) pkt_add((uint8_t)*s++);
    pkt_end();
}

/* ---------------- регистры -cal ---------------- */
static void regs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(EMU_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "emu_regs", s_regs, sizeof(s_regs));
    nvs_commit(h);
    nvs_close(h);
}

/* По умолчанию: калибровки нет (регистры 0..9 нули, в 10 - их CRC32),
 * пустые слоты FFFFFFFF, серийник - из MAC прибора. Записанное
 * программой командой -cal хранится в памяти прибора. */
static void regs_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_regs);
    if (nvs_open(EMU_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_blob(h, "emu_regs", s_regs, &len);
        nvs_close(h);
        if (e == ESP_OK && len == sizeof(s_regs)) return;
    }
    char cat[10 * 8 + 1] = { 0 };
    for (int i = 0; i < N_REGS; i++) s_regs[i] = 0xFFFFFFFF;
    for (int i = 0; i < REG_CRC; i++) {
        s_regs[i] = 0;
        snprintf(cat + i * 8, 9, "%08lX", (unsigned long)s_regs[i]);
    }
    s_regs[REG_CRC] = crc32_std(cat);
    uint8_t m[6] = { 0 };
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    s_regs[REG_SERIAL] = ((uint32_t)m[2] << 24) | ((uint32_t)m[3] << 16) |
                         ((uint32_t)m[4] << 8) | m[5];
    if (s_regs[REG_SERIAL] == 0xFFFFFFFF) s_regs[REG_SERIAL] = 1;
}

/* 40 строк по 8 hex с \r\n - ровно 400 байт, как у настоящего прибора. */
static void send_cal(void)
{
    static char b[N_REGS * 10 + 1];
    size_t pos = 0;
    for (int i = 0; i < N_REGS; i++)
        pos += snprintf(b + pos, sizeof(b) - pos, "%08lX\r\n",
                        (unsigned long)s_regs[i]);
    send_text(b);
}

/* ---------------- -inf ---------------- */
/* Мёртвое время на импульс программы на ПК считают как (RISE+FALL+1)/F.
 * У этого прибора импульс занимает окно трапеции (L) и перезапуск после
 * пика - их и отдаём как RISE и FALL. NOISE - порог, MAX - амплитуда,
 * попадающая в последний канал. */
static void send_inf(void)
{
    mca_params_t p;
    mca_stats_t  st;
    mca_dsp_get_params(&p);
    mca_dsp_get_stats(&st);
    const uint32_t max = (uint32_t)((uint64_t)MCA_CHANNELS *
                                    (uint32_t)p.cpc_milli / 1000);
    static char b[480];
    snprintf(b, sizeof(b),
        "VERSION 13 RISE %ld FALL %ld NOISE %ld F %.2f MAX %lu HYST 1 "
        "MODE 0 STEP 1 t %lu\r\n"
        "POT 0 POT2 0 T1 OFF T2 OFF T3 OFF OUT 0..0/1 Prise 0 Srise 0 "
        "Pfall 0 Sfall 0\r\n"
        "TC OFF TCpot OFF Tco [0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0] "
        "TP 1000 PileUp [] PileUpThr 8192",
        (long)p.trap_L, (long)p.rearm, (long)p.threshold,
        (double)adc_clk_get_freq(), (unsigned long)max,
        (unsigned long)(st.run_ms / 1000));
    send_text(b);
}

/* ---------------- статус и спектр ---------------- */
static void send_sweep(void)
{
    if (s_in_sweep) return;             /* -sho посреди прохода - он и идёт */
    s_in_sweep = true;

    mca_stats_t st;
    mca_dsp_get_stats(&st);

    /* статус: время, загрузка, CPS, отброшенные импульсы, ширина (0) */
    pkt_begin(CMD_STAT);
    pkt_add32(st.run_ms / 1000);
    pkt_add16((uint16_t)(st.load_pm / 10));
    pkt_add32(st.cps);
    pkt_add32((uint32_t)(st.skipped_deadtime + st.skipped_pileup));
    pkt_add32(0);
    pkt_end();

    static uint32_t v[CHUNK_BINS];
    for (uint32_t off = 0; off < MCA_CHANNELS && !s_stop_req; off += CHUNK_BINS) {
        const size_t got = mca_dsp_get_spectrum(v, off, CHUNK_BINS);
        pkt_begin(CMD_HIST);
        pkt_add16((uint16_t)off);
        for (size_t i = 0; i < CHUNK_BINS; i++) pkt_add32(i < got ? v[i] : 0);
        pkt_end();
        /* команды - и между пакетами спектра, см. «ответы на команды» */
        rx_poll(0);
    }
    s_in_sweep = false;
}

/* ---------------- скорость ---------------- */
static void baud_save(uint32_t b)
{
    nvs_handle_t h;
    if (nvs_open(EMU_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "emu_baud", b);
    nvs_commit(h);
    nvs_close(h);
}

static void baud_live(uint32_t b)
{
    uart_wait_tx_done(EMU_UART, pdMS_TO_TICKS(1000));
    uart_set_baudrate(EMU_UART, b);
    s_baud = b;
    baud_save(b);
}

/* ---------------- команды ---------------- */
static void handle_cmd(char *s)
{
    /* обрезать ноль, пробелы и переводы строки на концах */
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    while (*s == ' ') s++;

    char *save = NULL;
    char *cmd = strtok_r(s, " ", &save);
    if (!cmd) return;

    if (!strcmp(cmd, "-inf")) {
        send_inf();
    } else if (!strcmp(cmd, "-cal")) {
        char *a = strtok_r(NULL, " ", &save);
        char *v = strtok_r(NULL, " ", &save);
        if (!a) {
            send_cal();
        } else if (v) {
            const int r = atoi(a);
            if (r >= 0 && r < N_REGS) {
                s_regs[r] = (uint32_t)strtoul(v, NULL, 16);
                regs_save();
                send_text(ANSWER_OK);
            } else {
                send_text(ANSWER_UNSUPP);
            }
        }
    } else if (!strcmp(cmd, "-sta")) {
        s_silent  = false;
        s_limit_s = 0;
        for (char *t; (t = strtok_r(NULL, " ", &save)); ) {
            if (!strcmp(t, "-r"))      mca_cmd_clear = true;
            else if (!strcmp(t, "-s")) s_silent = true;
            else if (atoi(t) > 0)      s_limit_s = (uint32_t)atoi(t);
        }
        mca_mode     = MCA_MODE_SPECTRUM;
        mca_spec_run = true;
        /* так отвечает настоящий прибор на медленном порту - и именно эту
         * строку ждёт BecqMoni на 38400 и 115200 */
        send_text((s_baud == 38400 || s_baud == 115200) ? ANSWER_SLOW
                                                        : ANSWER_OK);
    } else if (!strcmp(cmd, "-sto")) {
        mca_spec_run = false;
        send_text(ANSWER_OK);
    } else if (!strcmp(cmd, "-sho")) {
        send_sweep();
    } else if (!strcmp(cmd, "-stt")) {
        send_text(mca_spec_run ? "collecting" : "stopped");
    } else if (!strcmp(cmd, "-rst")) {
        mca_cmd_clear = true;
        send_text(ANSWER_OK);
    } else if (!strcmp(cmd, "-mode")) {
        char *a = strtok_r(NULL, " ", &save);
        if (a && atoi(a) == 0) {
            mca_mode = MCA_MODE_SPECTRUM;
            send_text(ANSWER_OK);
        } else {
            send_text(ANSWER_UNSUPP);   /* осциллограф и импульсы - нет */
        }
    } else if (!strcmp(cmd, "-spd")) {
        char *a = strtok_r(NULL, " ", &save);
        const uint32_t b = a ? (uint32_t)strtoul(a, NULL, 10) : 0;
        if (baud_ok(b)) {
            send_text(ANSWER_OK);       /* ещё на прежней скорости */
            baud_live(b);
        } else {
            send_text(ANSWER_UNSUPP);
        }
    } else if (!strcmp(cmd, "-frq")) {
        /* ближайшая большая доступная частота, как у настоящего прибора */
        char *a = strtok_r(NULL, " ", &save);
        const uint32_t hz = a ? (uint32_t)strtoul(a, NULL, 10) : 0;
        int idx = MCA_FREQ_COUNT - 1;
        for (int i = 0; i < MCA_FREQ_COUNT; i++)
            if (mca_freq_table[i] >= hz) { idx = i; break; }
        if (hz) {
            mca_cmd_freq_idx = idx;
            send_text(ANSWER_OK);
        } else {
            send_text(ANSWER_UNSUPP);
        }
    } else if (!strcmp(cmd, "-reboot")) {
        send_text(ANSWER_OK);
        uart_wait_tx_done(EMU_UART, pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        /* заводская настройка и прочее - см. в начале файла */
        send_text(ANSWER_UNSUPP);
    }
}

/* ---------------- приём ---------------- */
static uint8_t s_rx[1100];
static size_t  s_rx_len;
static bool    s_rx_started, s_rx_esc;

static void rx_byte(uint8_t b)
{
    if (b == 0xFE) {                    /* начало пакета */
        s_rx_len = 0;
        s_rx_started = true;
        s_rx_esc = false;
        return;
    }
    if (!s_rx_started) return;          /* в т.ч. одиночный 0xFF раз в секунду */
    if (b == 0xFD) { s_rx_esc = true; return; }
    if (b == 0xA5) {                    /* конец пакета */
        s_rx_started = false;
        if (s_rx_len < 3) return;
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < s_rx_len; i++) crc = crc16_modbus(crc, s_rx[i]);
        if (crc != 0) return;           /* битый - как в эталоне, молча */
        const uint8_t cmd = s_rx[0];
        const size_t  len = s_rx_len - 3;
        if (cmd == CMD_TEXT && len > 0) {
            static char text[sizeof(s_rx)];
            memcpy(text, s_rx + 1, len);
            text[len] = 0;
            handle_cmd(text);
        } else if (cmd == CMD_REBOOT) {
            uart_wait_tx_done(EMU_UART, pdMS_TO_TICKS(200));
            esp_restart();
        }
        return;
    }
    if (s_rx_esc) { b = (uint8_t)~b; s_rx_esc = false; }
    if (s_rx_len < sizeof(s_rx)) s_rx[s_rx_len++] = b;
}

/* Разобрать всё, что пришло; wait - сколько ждать первого байта. */
static void rx_poll(TickType_t wait)
{
    uint8_t buf[128];
    int n;
    while ((n = uart_read_bytes(EMU_UART, buf, sizeof(buf), wait)) > 0) {
        for (int i = 0; i < n; i++) rx_byte(buf[i]);
        wait = 0;
    }
}

static void emu_task(void *arg)
{
    int64_t next = esp_timer_get_time() + 1000000;
    while (!s_stop_req) {
        rx_poll(pdMS_TO_TICKS(20));

        const int64_t now = esp_timer_get_time();
        if (now < next) continue;
        /* на медленном порту проход дольше секунды - не копим долг */
        next = (now - next > 2000000) ? now + 1000000 : next + 1000000;

        if (s_limit_s && mca_spec_run) {
            mca_stats_t st;
            mca_dsp_get_stats(&st);
            if (st.run_ms / 1000 >= s_limit_s) {
                mca_spec_run = false;
                s_limit_s = 0;
            }
        }
        if (mca_spec_run && !s_silent) send_sweep();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------- включение / выключение ---------------- */
static int null_vprintf(const char *fmt, va_list ap) { return 0; }

static void emu_start(uint32_t baud)
{
    ESP_LOGW(TAG, "эмуляция MCA на UART0, %lu бод - дальше консоль молчит",
             (unsigned long)baud);
    vTaskDelay(pdMS_TO_TICKS(30));      /* дать сообщению уйти */
    /* журнал - в никуда: иначе он смешался бы с пакетами протокола */
    s_prev_vprintf = esp_log_set_vprintf(null_vprintf);

    if (!uart_is_driver_installed(EMU_UART))
        uart_driver_install(EMU_UART, 1024, TX_RING, 0, NULL, 0);
    uart_set_baudrate(EMU_UART, baud);
    uart_flush_input(EMU_UART);

    s_rx_started = false;
    s_in_sweep = false;
    s_silent  = false;
    s_limit_s = 0;
    s_stop_req = false;
    s_baud = baud;
    xTaskCreatePinnedToCore(emu_task, "mca_emu", 6144, NULL, 4, &s_task, 0);
}

static void emu_stop(void)
{
    s_stop_req = true;
    for (int i = 0; i < 300 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    uart_wait_tx_done(EMU_UART, pdMS_TO_TICKS(500));
    uart_set_baudrate(EMU_UART, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    uart_driver_delete(EMU_UART);
    esp_log_set_vprintf(s_prev_vprintf);
    s_baud = 0;
    ESP_LOGI(TAG, "эмуляция MCA выключена, консоль снова работает");
}

void mca_emu_init(void)
{
    regs_load();
    nvs_handle_t h;
    uint32_t b = 0;
    if (nvs_open(EMU_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "emu_baud", &b);
        nvs_close(h);
    }
    if (baud_ok(b)) emu_start(b);
}

esp_err_t mca_emu_set(uint32_t baud)
{
    if (baud && !baud_ok(baud)) return ESP_ERR_INVALID_ARG;
    baud_save(baud);
    if (!baud) {
        if (s_baud) emu_stop();
    } else if (!s_baud) {
        emu_start(baud);
    } else if (baud != s_baud) {
        baud_live(baud);
    }
    return ESP_OK;
}

uint32_t mca_emu_baud(void) { return s_baud; }
