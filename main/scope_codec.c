#include "scope_codec.h"

#include <stdlib.h>
#include <string.h>

static inline void put(scodec_t *s, uint8_t **o, uint8_t nib)
{
    if (s->half) {
        *(*o)++ = (uint8_t)(s->acc | nib);
        s->half = false;
    } else {
        s->acc  = (uint8_t)(nib << 4);
        s->half = true;
    }
}

size_t scodec_encode(scodec_t *s, const uint16_t *src, size_t n,
                     uint8_t *out, size_t cap, size_t *used)
{
    uint8_t *o = out;
    const uint8_t *const lim = out + cap;
    size_t i = 0;
    /* не больше 4 полубайтов на отсчёт - это 2 байта выхода */
    for (; i < n && lim - o >= 2; i++) {
        const int32_t v = src[i] & 0x0FFF;
        const int32_t d = v - (int32_t)s->prev;
        if (s->started && d >= -7 && d <= 7) {
            put(s, &o, (uint8_t)(d & 0x0F));
        } else {
            if (s->started) put(s, &o, 8);
            put(s, &o, (uint8_t)(v >> 8));
            put(s, &o, (uint8_t)((v >> 4) & 0x0F));
            put(s, &o, (uint8_t)(v & 0x0F));
            s->started = true;
        }
        s->prev = (uint16_t)v;
    }
    *used = (size_t)(o - out);
    return i;
}

size_t scodec_finish(scodec_t *s, uint8_t *out)
{
    if (!s->half) return 0;
    out[0]  = s->acc;
    s->half = false;
    return 1;
}

size_t scodec_decode(const uint8_t *in, size_t len, uint16_t *dst, size_t n)
{
    size_t p = 0;                     /* номер полубайта */
    const size_t pn = len * 2;
#define NIB() ((p & 1) ? (in[p++ >> 1] & 0x0F) : (in[p++ >> 1] >> 4))
    if (!n || pn < 3) return 0;
    int32_t v = NIB() << 8;
    v |= NIB() << 4;
    v |= NIB();
    dst[0] = (uint16_t)v;
    size_t i = 1;
    for (; i < n && p < pn; i++) {
        const int32_t c = NIB();
        if (c == 8) {
            if (p + 3 > pn) break;
            v  = NIB() << 8;
            v |= NIB() << 4;
            v |= NIB();
        } else {
            v += c > 7 ? c - 16 : c;
        }
        dst[i] = (uint16_t)v;
    }
#undef NIB
    return i;
}

bool scodec_selftest(void)
{
    enum { N = 6000, OUT = N * 2 + 8 };
    uint16_t *src = malloc(N * sizeof(uint16_t));
    uint16_t *dst = malloc(N * sizeof(uint16_t));
    uint8_t  *buf = malloc(OUT);
    bool ok = src && dst && buf;

    uint32_t x = 0x2545F491u;
    int32_t v = 2048;
    for (int i = 0; ok && i < N; i++) {
        x = x * 1664525u + 1013904223u;
        const uint32_t r = x >> 16;
        if ((r & 63) == 0)      v = (r >> 6) & 1 ? 4095 : 0;   /* края шкалы */
        else if ((r & 15) == 1) v += (int32_t)((r >> 4) % 400) - 200;
        else                    v += (int32_t)((r >> 4) % 17) - 8;  /* -8..8 */
        if (v < 0)    v = 0;
        if (v > 4095) v = 4095;
        src[i] = (uint16_t)v;
    }

    /* куски выхода разной длины: состояние должно переживать границы */
    for (int pass = 0; ok && pass < 3; pass++) {
        static const size_t CH[3] = { 2, 7, 4096 };
        scodec_t st = { 0 };
        size_t pos = 0, len = 0;
        while (pos < N) {
            size_t cap = CH[pass];
            if (cap > OUT - len) cap = OUT - len;
            size_t used = 0;
            pos += scodec_encode(&st, src + pos, N - pos, buf + len, cap, &used);
            len += used;
            if (!used && pos < N) { ok = false; break; }
        }
        len += scodec_finish(&st, buf + len);
        if (!ok) break;
        memset(dst, 0, N * sizeof(uint16_t));
        ok = scodec_decode(buf, len, dst, N) == N &&
             memcmp(src, dst, N * sizeof(uint16_t)) == 0;
    }
    free(src);
    free(dst);
    free(buf);
    return ok;
}
