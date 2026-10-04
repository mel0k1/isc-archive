/* recipe.c — метрики блока: энтропия, печатность (utf-8 учитываем), серии */
#include <math.h>
#include <string.h>
#include "recipe.h"

void recipe_metrics(const u8 *in, size_t n, Metrics *m)
{
    u32 hist[256] = { 0 };
    for (size_t i = 0; i < n; i++) hist[in[i]]++;
    double h = 0;
    for (int i = 0; i < 256; i++) {
        if (!hist[i]) continue;
        double p = (double)hist[i] / (double)n;
        h -= p * log2(p);
    }
    m->h = (float)h;

    size_t good = 0, i = 0;
    while (i < n) {
        u8 c = in[i];
        if (c < 0x80) {
            good += (c >= 0x20 && c < 0x7f) || c == 9 || c == 10 || c == 13;
            i++;
        } else if (c >= 0xc2 && c <= 0xdf && i + 1 < n && (in[i + 1] & 0xc0) == 0x80) {
            good += 2; i += 2;
        } else if (c >= 0xe0 && c <= 0xef && i + 2 < n && (in[i + 1] & 0xc0) == 0x80 && (in[i + 2] & 0xc0) == 0x80) {
            good += 3; i += 3;
        } else if (c >= 0xf0 && c <= 0xf4 && i + 3 < n && (in[i + 1] & 0xc0) == 0x80 && (in[i + 2] & 0xc0) == 0x80 && (in[i + 3] & 0xc0) == 0x80) {
            good += 4; i += 4;
        } else i++;
    }
    m->p = n ? (float)((double)good / (double)n) : 0;

    size_t cov = 0;
    i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && in[j] == in[i]) j++;
        if (j - i >= 4) cov += j - i;
        i = j;
    }
    m->r = n ? (float)((double)cov / (double)n) : 0;
}

int recipe_is_text(const Metrics *m) { return m->p >= 0.87f; }

/* энтропия разностей in[i] - in[i-step]: гистограмма на лету, без буфера */
float recipe_delta_h(const u8 *in, size_t n, int step)
{
    if (n <= (size_t)step) return 8.0f;
    u32 hist[256] = { 0 };
    size_t m = n - (size_t)step;
    for (size_t i = 0; i < m; i++) hist[(u8)(in[i + step] - in[i])]++;
    double h = 0;
    for (int i = 0; i < 256; i++) {
        if (!hist[i]) continue;
        double p = (double)hist[i] / (double)m;
        h -= p * log2(p);
    }
    return (float)h;
}
