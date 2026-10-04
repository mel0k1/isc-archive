/* block.c — конвейер блока: метрики -> рецепт -> сжатие -> защитный откат в store */
#include <string.h>
#include "block.h"
#include "isum.h"
#include "recipe.h"
#include "rle.h"
#include "lzi.h"

/* порог «и так сжато»: энтропия почти предельная */
#define H_STORE 7.85f
/* минимальный размер блока, ради которого стоит включать конвейер */
#define N_MIN   192
/* считаем сжатием выигрыш >= 1.5% */
#define GAIN_NUM 63   /* (n * 63) / 64 ~ n - n/64 */
/* дельта-кандидат: обвал энтропии больше этого */
#define D_GAIN  0.35f

static int worthy(size_t sz, size_t n) { return sz <= n - n / 64; }

/* out[i] = in[i] - in[i-step]: звук, счётчики, массивы чисел */
static void delta_apply(const u8 *in, size_t n, int step, u8 *out)
{
    for (int i = 0; i < step; i++) out[i] = in[i];
    for (size_t i = (size_t)step; i < n; i++) out[i] = (u8)(in[i] - in[i - step]);
}

size_t block_encode(const u8 *in, size_t n, int level, int mode,
                    u8 *out, size_t cap, u8 *recipe, i32 *head, i32 *prev,
                    u32 *tl, u32 *td, u8 *dbuf)
{
    lzi_init();
    Metrics m;
    if (mode == M_STORE) { memcpy(out, in, n); *recipe = R_STORE; return n; }
    if (n < N_MIN) { memcpy(out, in, n); *recipe = R_STORE; return n; }
    recipe_metrics(in, n, &m);
    if (m.h > H_STORE) { memcpy(out, in, n); *recipe = R_STORE; return n; }

    /* повторяющиеся данные — сначала быстрый RLE */
    if (mode == M_AUTO && m.r >= 0.55f) {
        size_t rs = rle_encode(in, n, out);
        if (worthy(rs, n)) { *recipe = R_RLE; return rs; }
    }

    int text = recipe_is_text(&m);
    LZP lp;
    lzi_params(level, text, &lp);
    LZT t;
    t.tl = tl; t.td = td; t.cap = n;

    /* базовый LZI-поток */
    lzi_parse(in, n, &lp, &t, head, prev);
    BW bw;
    bw_init(&bw, out, cap);
    if (lzi_emit(&t, &bw) != 0) { memcpy(out, in, n); *recipe = R_STORE; return n; }
    size_t ls = bw_flush(&bw);

    /* дельта: шаг с самым низким обвалом энтропии, потом честное сравнение */
    if (mode != M_STORE && !text && m.r < 0.55f && n >= 1024 && dbuf) {
        int step = 0;
        float best = m.h;
        for (int s = 1; s <= 8; s <<= 1) {
            float hd = recipe_delta_h(in, n, s);
            if (hd < best - D_GAIN) { best = hd; step = s; }
        }
        if (step) {
            delta_apply(in, n, step, dbuf);
            lzi_parse(dbuf, n, &lp, &t, head, prev);
            BW bw2;
            bw_init(&bw2, dbuf, n);   /* dbuf уже прочитан парсером — переиспользуем */
            if (lzi_emit(&t, &bw2) == 0 && !bw2.err) {
                size_t ds = bw_flush(&bw2);
                if (ds + 1 < ls && worthy(ds + 1, n)) {
                    out[0] = (u8)step;
                    memcpy(out + 1, dbuf, ds);
                    *recipe = R_DLT;
                    return ds + 1;
                }
            }
        }
    }

    if (bw.err || !worthy(ls, n)) { memcpy(out, in, n); *recipe = R_STORE; return n; }
    *recipe = text ? R_LZT : R_LZB;
    return ls;
}

int block_decode(const u8 *pay, size_t psz, u8 recipe, u8 *out, size_t n)
{
    lzi_init();
    switch (recipe) {
    case R_STORE:
        if (psz != n) return -1;
        memcpy(out, pay, n);
        return 0;
    case R_RLE:
        return rle_decode(pay, psz, out, n) == n ? 0 : -1;
    case R_LZT:
    case R_LZB:
        return lzi_decode(pay, psz, out, n);
    case R_DLT:
        if (psz < 1) return -1;
        int st = pay[0];
        if (st != 1 && st != 2 && st != 4 && st != 8) return -1;
        if (lzi_decode(pay + 1, psz - 1, out, n)) return -1;
        for (size_t i = (size_t)st; i < n; i++) out[i] = (u8)(out[i] + out[i - st]);
        return 0;
    default:
        return -1;
    }
}
