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

static int worthy(size_t sz, size_t n) { return sz <= n - n / 64; }

size_t block_encode(const u8 *in, size_t n, int level, int mode,
                    u8 *out, size_t cap, u8 *recipe, i32 *head, i32 *prev,
                    u32 *tl, u32 *td)
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

    /* LZI: текстовый или бинарный пайплайн */
    int text = recipe_is_text(&m);
    LZP lp;
    lzi_params(level, text, &lp);
    LZT t;
    t.tl = tl; t.td = td; t.cap = n;
    lzi_parse(in, n, &lp, &t, head, prev);
    BW bw;
    bw_init(&bw, out, cap);
    if (lzi_emit(&t, &bw) != 0) { memcpy(out, in, n); *recipe = R_STORE; return n; }
    size_t ls = bw_flush(&bw);
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
    default:
        return -1;
    }
}
