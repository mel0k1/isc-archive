/* lzi.c — совпадения ищем hash-цепочками по 4 байтам, окно = блок */
#include <string.h>
#include "lzi.h"

u16 LEN_BASE[LEN_SLOTS];
u32 DST_BASE[DST_SLOTS];
u8  LEN_EBITS[LEN_SLOTS], DST_EBITS[DST_SLOTS];

static int inited;

static u32 load32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u32 hpos(const u8 *in, size_t q) { return (load32(in + q) * 2654435761u) >> 16; }

void lzi_init(void)
{
    if (inited) return;
    inited = 1;
    /* длины 4..347: слоты 4+2+4+8+16 */
    for (int i = 0; i < 4; i++) { LEN_BASE[i] = (u16)(4 + i); LEN_EBITS[i] = 0; }
    for (int i = 4; i < 6; i++)  { LEN_BASE[i] = (u16)(8 + 2 * (i - 4)); LEN_EBITS[i] = 1; }
    for (int i = 6; i < 10; i++) { LEN_BASE[i] = (u16)(12 + 4 * (i - 6)); LEN_EBITS[i] = 2; }
    for (int i = 10; i < 18; i++){ LEN_BASE[i] = (u16)(28 + 8 * (i - 10)); LEN_EBITS[i] = 3; }
    for (int i = 18; i < 34; i++){ LEN_BASE[i] = (u16)(92 + 16 * (i - 18)); LEN_EBITS[i] = 4; }
    /* дистанции 1..2^20: слоты 4 + пары */
    for (int i = 0; i < 4; i++) { DST_BASE[i] = (u16)(1 + i); DST_EBITS[i] = 0; }
    u32 base = 5;
    int e = 1;
    for (int i = 4; i < DST_SLOTS; i += 2) {
        DST_BASE[i] = base;
        DST_BASE[i + 1] = base + (1u << e);
        DST_EBITS[i] = DST_EBITS[i + 1] = (u8)e;
        base += 2u << e;
        e++;
    }
}

static int lslot(int len)
{
    int v = len - 4;
    if (v < 4) return v;
    if (v < 8) return 4 + (v - 4) / 2;
    if (v < 24) return 6 + (v - 8) / 4;
    if (v < 88) return 10 + (v - 24) / 8;
    return 18 + (v - 88) / 16;
}

static int dslot(u32 dist)
{
    u32 v = dist - 1;
    if (v < 4) return (int)v;
    int hi = 2;
    while ((v >> (hi + 1)) != 0) hi++;
    int e = hi - 1;
    return 4 + 2 * (e - 1) + (int)((v >> e) & 1);
}

void lzi_params(int level, int text, LZP *p)
{
    typedef struct { u16 chain, nice; u8 lazy, dyn; } Par;
    static const Par T[9] = {
        {   2,  12, 0, 0 }, {   4,  16, 0, 0 }, {   8,  24, 0, 1 },
        {  16,  32, 1, 1 }, {  24,  48, 1, 1 }, {  48,  64, 1, 1 },
        {  96,  96, 1, 1 }, { 256, 160, 1, 1 }, {1024, 347, 1, 1 },
    };
    const Par *t = &T[level - 1];
    p->chain = t->chain;
    p->nice = t->nice + (text ? 32 : 0);
    if (p->nice > MAX_MATCH) p->nice = MAX_MATCH;
    /* тексту лень раньше, бинарнику — только с 6-го уровня */
    p->lazy = t->lazy && (text ? level >= 4 : level >= 6);
    p->dyn = t->dyn;
}

static u32 find_best(const u8 *in, size_t n, size_t p, const i32 *head, const i32 *prev,
                     const LZP *lp, u32 *bdist)
{
    u32 maxm = n - p;
    if (maxm > MAX_MATCH) maxm = MAX_MATCH;
    if (maxm < MIN_MATCH) { *bdist = 0; return 0; }
    u32 blen = 0, bd = 0;
    i32 c = head[hpos(in, p)];
    int tries = lp->chain;
    while (c >= 0 && tries-- > 0) {
        size_t cand = (size_t)c;
        if (cand == p) { c = prev[cand]; continue; }
        if (blen && in[cand + blen] != in[p + blen]) { c = prev[cand]; continue; }
        u32 m = 0;
        while (m < maxm && in[cand + m] == in[p + m]) m++;
        if (m > blen) {
            blen = m; bd = (u32)(p - cand);
            if (m >= maxm || (int)m >= lp->nice) break;
        }
        c = prev[cand];
    }
    *bdist = bd;
    return blen;
}

static void insert(i32 *head, i32 *prev, const u8 *in, size_t q)
{
    u32 h = hpos(in, q);
    if (head[h] == (i32)q) return;  /* уже в цепочке */
    prev[q] = head[h];
    head[h] = (i32)q;
}

int lzi_parse(const u8 *in, size_t n, const LZP *p, LZT *t, i32 *head, i32 *prev)
{
    t->n = 0;
    memset(t->fl, 0, sizeof t->fl);
    memset(t->fd, 0, sizeof t->fd);
    for (int i = 0; i < 1 << 16; i++) head[i] = -1;
    t->fl[EOB] = 1;
    size_t i = 0;
    while (i < n) {
        if (n - i < 4) { t->tl[t->n] = in[i]; t->td[t->n] = 0; t->n++; t->fl[in[i]]++; i++; continue; }
        insert(head, prev, in, i);
        u32 bd, blen = find_best(in, n, i, head, prev, p, &bd);
        if (blen < MIN_MATCH) { t->tl[t->n] = in[i]; t->td[t->n] = 0; t->n++; t->fl[in[i]]++; i++; continue; }
        if (p->lazy && i + 1 + MIN_MATCH <= n) {
            u32 bd2, blen2 = find_best(in, n, i + 1, head, prev, p, &bd2);
            insert(head, prev, in, i + 1);
            if (blen2 > blen) { t->tl[t->n] = in[i]; t->td[t->n] = 0; t->n++; t->fl[in[i]]++; i++; continue; }
        }
        t->tl[t->n] = blen + 256;   /* совпадение отличаем от литерала */
        t->td[t->n] = bd;
        t->n++;
        t->fl[257 + lslot((int)blen)]++;
        t->fd[dslot(bd)]++;
        size_t end = i + blen;
        for (size_t k = i + 1; k < end && k + MIN_MATCH <= n; k++) insert(head, prev, in, k);
        i = end;
    }
    return 0;
}

int lzi_emit(const LZT *t, BW *b)
{
    HF fl, fd;
    u16 fd2[DST_SLOTS];
    memcpy(fd2, t->fd, sizeof fd2);
    int anyd = 0;
    for (int i = 0; i < DST_SLOTS; i++) anyd |= fd2[i] != 0;
    if (!anyd) fd2[0] = 1;  /* пустой алфавит дистанций */
    hf_from_freq(&fl, t->fl, LITN_SYMS);
    hf_from_freq(&fd, fd2, DST_SLOTS);
    bw_byte(b, 1);
    hf_store_trees(b, fl.lens, fd.lens);
    for (size_t i = 0; i < t->n; i++) {
        u32 T = t->tl[i];
        if (T < 256) {
            bw_code(b, fl.codes[T], fl.lens[T]);
            continue;
        }
        u32 L = T - 256;
        int s = lslot((int)L);
        bw_code(b, fl.codes[257 + s], fl.lens[257 + s]);
        bw_bits(b, L - LEN_BASE[s], LEN_EBITS[s]);
        int ds = dslot(t->td[i]);
        bw_code(b, fd.codes[ds], fd.lens[ds]);
        bw_bits(b, t->td[i] - DST_BASE[ds], DST_EBITS[ds]);
    }
    bw_code(b, fl.codes[EOB], fl.lens[EOB]);
    return b->err ? -1 : 0;
}

int lzi_decode(const u8 *pay, size_t psz, u8 *out, size_t nraw)
{
    BR br;
    br_init(&br, pay, psz);
    u8 dyn = (u8)br_bits(&br, 8);
    HF fl, fd;
    u8 ll[LITN_SYMS], dl[DST_SLOTS];
    if (dyn) {
        if (hf_load_trees(&br, ll, dl)) return -1;
    } else {
        memset(ll, 9, sizeof ll);
        memset(dl, 6, sizeof dl);
    }
    hf_from_lens(&fl, ll, LITN_SYMS);
    hf_from_lens(&fd, dl, DST_SLOTS);
    size_t op = 0;
    for (;;) {
        int s = hf_dec(&fl, &br);
        if (s < 0) return -1;
        if (s < 256) {
            if (op >= nraw) return -1;
            out[op++] = (u8)s;
        } else if (s == 256) {
            break;
        } else {
            int ls = s - 257;
            if (ls >= LEN_SLOTS) return -1;
            u32 L = LEN_BASE[ls] + br_bits(&br, LEN_EBITS[ls]);
            int ds = hf_dec(&fd, &br);
            if (ds < 0 || ds >= DST_SLOTS) return -1;
            u32 D = DST_BASE[ds] + br_bits(&br, DST_EBITS[ds]);
            if (D == 0 || D > op || L > nraw - op) return -1;
            if (D >= L) memcpy(out + op, out + op - D, L);
            else for (u32 k = 0; k < L; k++) out[op + k] = out[op - D + k];
            op += L;
        }
    }
    return op == nraw ? 0 : -1;
}
