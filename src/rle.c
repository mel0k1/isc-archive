/* rle.c — токены: [0..127] литералы, [0x80|k] серия из k+4 байт, 0 = конец */
#include "rle.h"

#define RLE_MIN 4
#define RLE_MAX (127 + RLE_MIN)

size_t rle_encode(const u8 *in, size_t n, u8 *out)
{
    size_t o = 0, i = 0, lits = 0;
    while (i < n) {
        /* измеряем серию */
        size_t j = i + 1;
        while (j < n && in[j] == in[i] && j - i < RLE_MAX) j++;
        if (j - i >= RLE_MIN) {
            /* сброс накопленных литералов */
            while (lits > 0) {
                size_t c = lits > 127 ? 127 : lits;
                out[o++] = (u8)c;
                for (size_t k = i - lits; k < i - lits + c; k++) out[o++] = in[k];
                lits -= c;
            }
            out[o++] = (u8)(0x80 | ((j - i) - RLE_MIN));
            out[o++] = in[i];
            i = j;
        } else {
            lits++;
            i++;
            if (lits == 127) {
                out[o++] = 127;
                for (size_t k = i - lits; k < i; k++) out[o++] = in[k];
                lits = 0;
            }
        }
    }
    while (lits > 0) {
        size_t c = lits > 127 ? 127 : lits;
        out[o++] = (u8)c;
        for (size_t k = i - lits; k < i - lits + c; k++) out[o++] = in[k];
        lits -= c;
    }
    out[o++] = 0;
    return o;
}

size_t rle_decode(const u8 *in, size_t n, u8 *out, size_t cap)
{
    size_t o = 0, i = 0;
    while (i < n) {
        u8 c = in[i++];
        if (c == 0) return o;
        if (c & 0x80) {
            if (i >= n || o + (size_t)(c & 0x7f) + RLE_MIN > cap) return (size_t)-1;
            u8 v = in[i++];
            for (int k = 0; k < (c & 0x7f) + RLE_MIN; k++) out[o++] = v;
        } else {
            if (i + c > n || o + c > cap) return (size_t)-1;
            for (size_t k = 0; k < c; k++) out[o++] = in[i++];
        }
    }
    return o;   /* без терминатора тоже считаем успехом (обрезанный поток отдали как есть) */
}
