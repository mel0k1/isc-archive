/* lzi.c — LZI: hash-цепочки по 4 байтам; LZ2 — rep-дистанции и RISC */
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "lzi.h"
#include "rle.h"

u16 LEN_BASE[LEN_SLOTS];
u32 DST_BASE[DST_SLOTS];
u8  LEN_EBITS[LEN_SLOTS], DST_EBITS[DST_SLOTS];

/* старт контекста экстра-битов слота: контекст бита = LCTX[s] + номер бита */
u16 LCTX[LEN_SLOTS], DCTX[DST_SLOTS];
#define EXC_L   98    /* сумма LEN_EBITS: 2*1 + 4*2 + 8*3 + 16*4 */
#define EXC_D   342   /* сумма DST_EBITS: 2*(1+2+...+18) */
#define EXC_ALL (EXC_L + EXC_D)

/* класс длины для таблицы дистанций: длинный матч — дальше дистанция */
static int lcls(int ls) { return ls < 4 ? 0 : ls < 10 ? 1 : ls < 18 ? 2 : 3; }

static int inited;

static u32 load32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u32 hpos(const u8 *in, size_t q) { return (load32(in + q) * 2654435761u) >> 16; }

void lzi_init(void)
{
    /* guard под потоки: воркеры зовут block_decode одновременно */
    if (__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) return;
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
    u16 c = 0;
    for (int i = 0; i < LEN_SLOTS; i++) { LCTX[i] = c; c = (u16)(c + LEN_EBITS[i]); }
    c = 0;
    for (int i = 0; i < DST_SLOTS; i++) { DCTX[i] = c; c = (u16)(c + DST_EBITS[i]); }
    __atomic_store_n(&inited, 1, __ATOMIC_RELEASE);
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
    p->opt = level >= 9;
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

/* --- rep-дистанции: MRU-очередь из 4 последних --- */

static void rep_push(u32 *rep, u32 D)     /* новая дистанция вперёд */
{
    if (D == rep[0]) return;
    for (int r = 1; r < 4; r++)
        if (rep[r] == D) { for (; r > 0; r--) rep[r] = rep[r - 1]; rep[0] = D; return; }
    rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = D;
}

static void rep_move(u32 *rep, int rn)    /* rep-матч: rep[rn] -> rep0 */
{
    u32 v = rep[rn];
    for (int r = rn; r > 0; r--) rep[r] = rep[r - 1];
    rep[0] = v;
}

/* лучшая из четырёх последних дистанций */
static u32 rep_best(const u8 *in, size_t n, size_t i, const u32 *rep, u32 *rn)
{
    u32 maxm = n - i;
    if (maxm > MAX_MATCH) maxm = MAX_MATCH;
    u32 bl = 0;
    *rn = 0;
    for (int r = 0; r < 4; r++) {
        if (rep[r] > i) continue;
        const u8 *a = in + i, *b = a - rep[r];
        u32 m = 0;
        while (m < maxm && a[m] == b[m]) m++;
        if (m > bl) { bl = m; *rn = (u32)r; }
    }
    return bl;
}

static int lz_copy(u8 *out, size_t *op, size_t nraw, u32 D, u32 L)
{
    if (D == 0 || D > *op || L > nraw - *op) return -1;
    if (D >= L) memcpy(out + *op, out + *op - D, L);
    else for (u32 k = 0; k < L; k++) out[*op + k] = out[*op - D + k];
    *op += L;
    return 0;
}

static int lzi_greedy(const u8 *in, size_t n, const LZP *p, LZT *t,
                      i32 *head, i32 *prev, int reps);

int lzi_parse(const u8 *in, size_t n, const LZP *p, LZT *t,
              i32 *head, i32 *prev, int reps)
{
    return lzi_greedy(in, n, p, t, head, prev, reps);
}

static int lzi_greedy(const u8 *in, size_t n, const LZP *p, LZT *t,
                      i32 *head, i32 *prev, int reps)
{
    u32 rep[4] = { 1, 1, 1, 1 };   /* MRU дистанций */
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
        int isr = 0;
        u32 rn = 0;
        if (reps) {
            u32 rlen = rep_best(in, n, i, rep, &rn);
            /* та же длина — rep дешевле: без слота дистанции и экстр */
            if (rlen >= MIN_MATCH && rlen >= blen) { blen = rlen; isr = 1; }
        }
        if (blen < MIN_MATCH) { t->tl[t->n] = in[i]; t->td[t->n] = 0; t->n++; t->fl[in[i]]++; i++; continue; }
        if (p->lazy && i + 1 + MIN_MATCH <= n) {
            u32 bd2, blen2 = find_best(in, n, i + 1, head, prev, p, &bd2);
            if (reps) {
                u32 rn2;
                u32 rlen2 = rep_best(in, n, i + 1, rep, &rn2);
                if (rlen2 >= MIN_MATCH && rlen2 > blen2) blen2 = rlen2;
            }
            insert(head, prev, in, i + 1);
            if (blen2 > blen) { t->tl[t->n] = in[i]; t->td[t->n] = 0; t->n++; t->fl[in[i]]++; i++; continue; }
        }
        t->tl[t->n] = blen + 256;   /* совпадение отличаем от литерала */
        t->fl[257 + lslot((int)blen)]++;
        if (isr) {
            t->td[t->n] = TD_REP | rn;
            t->fd[DST_SLOTS + rn]++;
            rep_move(rep, (int)rn);
        } else {
            t->td[t->n] = bd;
            t->fd[dslot(bd)]++;
            rep_push(rep, bd);
        }
        t->n++;
        size_t end = i + blen;
        for (size_t k = i + 1; k < end && k + MIN_MATCH <= n; k++) insert(head, prev, in, k);
        i = end;
    }
    return 0;
}

/* --- оптимальный парсинг (-9): DP по ценам из статистики потока --- */

#define PSC  64                  /* цена в 1/64 бита */
#define P_INF ((u64)-1)

static u32 pbits(u32 f)          /* цена символа с частотой f */
{
    return f ? (u32)(log2((double)RISC_TOTAL / f) * PSC) : 12 * PSC;
}

typedef struct {
    u32 pa[LITN_SYMS];
    u32 pb[4][DST_SYMS];
    u32 l0[EXC_L], l1[EXC_L], d0[EXC_D], d1[EXC_D];
    u32 le[LEN_SLOTS][16];
} Price;

/* цена экстра-битов дистанции по контекстам */
static u32 dex_price(int ds, u32 ev, const Price *pr)
{
    int e = DST_EBITS[ds];
    const u32 *p0 = pr->d0 + DCTX[ds], *p1 = pr->d1 + DCTX[ds];
    u32 c = 0;
    for (int k = 0; k < e; k++)
        c += (ev >> (e - 1 - k)) & 1 ? p1[k] : p0[k];
    return c;
}

/* модель цен из статистики потока; smooth — априорная подмешалка для поиска,
   для сравнения потоков нужна честная (как в lzi_emit2) */
static void price_build(const LZT *t, Price *pr, int smooth)
{
    u16 nfA[LITN_SYMS];
    if (risc_norm(t->fl, LITN_SYMS, nfA)) {
        for (int s = 0; s < LITN_SYMS; s++) pr->pa[s] = 12 * PSC;
    } else {
        for (int s = 0; s < LITN_SYMS; s++) pr->pa[s] = pbits(nfA[s]);
    }
    u16 cb[4][DST_SYMS];
    u32 n0[EXC_ALL], n1[EXC_ALL];
    memset(cb, 0, sizeof cb);
    memset(n0, 0, sizeof n0);
    memset(n1, 0, sizeof n1);
    for (size_t i = 0; i < t->n; i++) {
        u32 T = t->tl[i];
        if (T < 256) continue;
        int ls = lslot((int)(T - 256));
        u32 D = t->td[i];
        if (D & TD_REP) cb[lcls(ls)][DST_SLOTS + (D & 3)]++;
        else {
            int ds = dslot(D);
            int e = DST_EBITS[ds];
            u32 ev = D - DST_BASE[ds];
            cb[lcls(ls)][ds]++;
            for (int k = 0; k < e; k++)
                if ((ev >> (e - 1 - k)) & 1) n1[EXC_L + DCTX[ds] + k]++;
                else n0[EXC_L + DCTX[ds] + k]++;
        }
        int e = LEN_EBITS[ls];
        u32 ev = T - 256 - LEN_BASE[ls];
        for (int k = 0; k < e; k++)
            if ((ev >> (e - 1 - k)) & 1) n1[LCTX[ls] + k]++;
            else n0[LCTX[ls] + k]++;
    }
    for (int c = 0; c < 4; c++) {
        u32 tot = 0;
        for (int s = 0; s < DST_SYMS; s++) tot += cb[c][s];
        if (!tot) {
            /* класс пуст: дорого, чтобы DP не убегал туда */
            for (int s = 0; s < DST_SYMS; s++) pr->pb[c][s] = 8 * PSC;
            continue;
        }
        u16 cnt[DST_SYMS];
        for (int s = 0; s < DST_SYMS; s++)
            cnt[s] = smooth ? (u16)(cb[c][s] + (t->fd[s] + 8) / 16 + 1) : cb[c][s];
        u16 nf[DST_SYMS];
        if (risc_norm(cnt, DST_SYMS, nf)) {
            for (int s = 0; s < DST_SYMS; s++) pr->pb[c][s] = 8 * PSC;
            continue;
        }
        for (int s = 0; s < DST_SYMS; s++) pr->pb[c][s] = pbits(nf[s]);
    }
    for (int c = 0; c < EXC_ALL; c++) {
        u32 tot = n0[c] + n1[c];
        u32 v = 2048;
        if (tot) {
            v = (u32)((u64)n0[c] * RISC_TOTAL / tot);
            u32 q = (v + 8) >> 4;
            if (q > 255) q = 255;
            v = q * 16 + 8;
        }
        u32 z = pbits(v), o = pbits(RISC_TOTAL - v);
        if (c < EXC_L) { pr->l0[c] = z; pr->l1[c] = o; }
        else { pr->d0[c - EXC_L] = z; pr->d1[c - EXC_L] = o; }
    }
    for (int ls = 0; ls < LEN_SLOTS; ls++) {
        int e = LEN_EBITS[ls];
        for (int v = 0; v < (1 << e); v++) {
            u32 c = 0;
            for (int k = 0; k < e; k++)
                c += (v >> (e - 1 - k)) & 1 ? pr->l1[LCTX[ls] + k] : pr->l0[LCTX[ls] + k];
            pr->le[ls][v] = c;
        }
    }
}

static int mlen(const u8 *in, size_t n, size_t p, u32 D)
{
    size_t maxm = n - p;
    if (maxm > MAX_MATCH) maxm = MAX_MATCH;
    const u8 *a = in + p, *b = a - D;
    u32 m = 0;
    while (m < maxm && a[m] == b[m]) m++;
    return (int)m;
}

typedef struct { u32 src, d; u16 len; u8 isr, rn; } OptEd;

/* оптимальный парсинг: DP по ценам модели жадного потока.
   Выбор DP/жадный делает вызывающий — по фактическому размеру emit */
int lzi_parse_opt(const u8 *in, size_t n, const LZP *p, LZT *t,
            const i32 *head, const i32 *prev)
{
    Price P1;
    price_build(t, &P1, 1);

    /* DP вперёд: цена позиции, ребро назад, rep-очередь лучшего пути */
    u64 *cost = malloc((n + 1) * sizeof *cost);
    OptEd *ed = calloc(n + 1, sizeof *ed);
    u32 (*q)[4] = malloc((n + 1) * sizeof *q);
    if (!cost || !ed || !q) { free(cost); free(ed); free(q); return 0; }
    for (size_t j = 0; j <= n; j++) cost[j] = P_INF;
    cost[0] = 0;
    q[0][0] = q[0][1] = q[0][2] = q[0][3] = 1;

    for (size_t j = 0; j < n; j++) {
        u64 c0 = cost[j];
        u64 c = c0 + P1.pa[in[j]];   /* литерал */
        if (c < cost[j + 1]) {
            cost[j + 1] = c;
            ed[j + 1].src = (u32)j; ed[j + 1].len = 0;
            memcpy(q[j + 1], q[j], sizeof q[j + 1]);
        }
        if (n - j < MIN_MATCH) continue;
        u32 bm = 0, bD = 0, bisr = 0, brn = 0;
        u32 maxm = n - j > MAX_MATCH ? MAX_MATCH : (u32)(n - j);
        /* репы: без слота дистанции и экстр */
        for (int r = 0; r < 4; r++) {
            u32 D = q[j][r];
            if (D > j || in[j] != in[j - D]) continue;
            int m = mlen(in, n, j, D);
            if (m < MIN_MATCH) continue;
            int ls = lslot(m);
            u64 pc = c0 + P1.pa[257 + ls] + P1.le[ls][m - LEN_BASE[ls]] + P1.pb[lcls(ls)][DST_SLOTS + r];
            if (pc < cost[j + m]) {
                cost[j + m] = pc;
                ed[j + m].src = (u32)j; ed[j + m].len = (u16)m;
                ed[j + m].isr = 1; ed[j + m].rn = (u8)r; ed[j + m].d = D;
                memcpy(q[j + m], q[j], sizeof q[j + m]);
                rep_move(q[j + m], r);
            }
            if ((u32)m > bm) { bm = (u32)m; bD = D; bisr = 1; brn = (u32)r; }
        }
        /* цепочка хешей: кандидаты короче bm покрыты поддиапазонами лучшего */
        i32 cnd = head[hpos(in, j)];
        int tries = p->chain;
        while (cnd >= 0 && tries-- > 0) {
            size_t cand = (size_t)cnd;
            /* цепочки уже полные (жадный вставил весь блок) — впереди j мусор */
            if (cand >= j) { cnd = prev[cand]; continue; }
            u32 D = (u32)(j - cand);
            if (bm >= MIN_MATCH + 1 && in[cand + bm - 1] != in[j + bm - 1]) { cnd = prev[cand]; continue; }
            int m = mlen(in, n, j, D);
            if ((u32)m > bm) { bm = (u32)m; bD = D; bisr = 0; brn = 0; }
            if (m >= MIN_MATCH && (u32)m + 1 >= bm) {
                int ls = lslot(m);
                int ds = dslot(D);
                u64 pc = c0 + P1.pa[257 + ls] + P1.le[ls][m - LEN_BASE[ls]]
                       + P1.pb[lcls(ls)][ds] + dex_price(ds, D, &P1);
                if (pc < cost[j + m]) {
                    cost[j + m] = pc;
                    ed[j + m].src = (u32)j; ed[j + m].len = (u16)m;
                    ed[j + m].isr = 0; ed[j + m].d = D;
                    memcpy(q[j + m], q[j], sizeof q[j + m]);
                    rep_push(q[j + m], D);
                }
            }
            if ((u32)m >= maxm || (u32)m >= (u32)p->nice) break;
            cnd = prev[cand];
        }
        /* поддиапазоны лучшего матча: та же дистанция, короче (не глубже 64) */
        if (bm >= MIN_MATCH) {
            u32 lo = bm - 64;
            if (lo < MIN_MATCH) lo = MIN_MATCH;
            u32 dexc = bisr ? 0 : dex_price(dslot(bD), bD, &P1);
            for (u32 L = lo; L < bm; L++) {
                int l2 = lslot((int)L);
                u64 pc = c0 + P1.pa[257 + l2] + P1.le[l2][L - LEN_BASE[l2]]
                       + (bisr ? P1.pb[lcls(l2)][DST_SLOTS + brn] : P1.pb[lcls(l2)][dslot(bD)] + dexc);
                if (pc < cost[j + L]) {
                    cost[j + L] = pc;
                    ed[j + L].src = (u32)j; ed[j + L].len = (u16)L;
                    ed[j + L].isr = (u8)bisr; ed[j + L].rn = (u8)brn; ed[j + L].d = bD;
                    memcpy(q[j + L], q[j], sizeof q[j + L]);
                    if (bisr) rep_move(q[j + L], (int)brn); else rep_push(q[j + L], bD);
                }
            }
        }
    }

    /* реконструкция: сколько токенов, потом с хвоста */
    size_t cnt = 0;
    for (size_t j = n; j > 0; ) { j = ed[j].src; cnt++; }
    if (cnt > t->cap) { free(cost); free(ed); free(q); return -1; }
    t->n = cnt;
    memset(t->fl, 0, sizeof t->fl);
    memset(t->fd, 0, sizeof t->fd);
    size_t idx = cnt;
    for (size_t j = n; j > 0; ) {
        u32 s = ed[j].src;
        idx--;
        if (ed[j].len) {
            t->tl[idx] = (u32)ed[j].len + 256;
            t->td[idx] = ed[j].isr ? (TD_REP | ed[j].rn) : ed[j].d;
            t->fl[257 + lslot(ed[j].len)]++;
            if (ed[j].isr) t->fd[DST_SLOTS + ed[j].rn]++;
            else t->fd[dslot(ed[j].d)]++;
        } else {
            t->tl[idx] = in[s];
            t->td[idx] = 0;
            t->fl[in[s]]++;
        }
        j = s;
    }
    t->fl[EOB] = 1;
    free(cost);
    free(ed);
    free(q);
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

/* --- LZ2: rep-дистанции, энтрокодек выбирается в потоке --- */

/* режим 1: канонический хаффман, экстры в общем бит-потоке */
static int emit_huff2(const LZT *t, u8 *out, size_t cap, size_t *osize,
                      const HF *fl, const HF *fd, const u8 *arr)
{
    BW bw;
    bw_init(&bw, out, cap);
    bw_byte(&bw, 1);
    hf_store_arr(&bw, arr, TREE2_BYTES);
    for (size_t i = 0; i < t->n; i++) {
        u32 T = t->tl[i];
        if (T < 256) { bw_code(&bw, fl->codes[T], fl->lens[T]); continue; }
        u32 L = T - 256;
        int s = lslot((int)L);
        bw_code(&bw, fl->codes[257 + s], fl->lens[257 + s]);
        bw_bits(&bw, L - LEN_BASE[s], LEN_EBITS[s]);
        u32 D = t->td[i];
        if (D & TD_REP) {
            int ds = DST_SLOTS + (D & 3);
            bw_code(&bw, fd->codes[ds], fd->lens[ds]);
        } else {
            int ds = dslot(D);
            bw_code(&bw, fd->codes[ds], fd->lens[ds]);
            bw_bits(&bw, D - DST_BASE[ds], DST_EBITS[ds]);
        }
    }
    bw_code(&bw, fl->codes[EOB], fl->lens[EOB]);
    if (bw.err) return -1;
    *osize = bw_flush(&bw);
    return 0;
}

/* режим 2: RISC (rANS), два потока: A — токены, B — дистанции.
   Раскладка: [mode][табл A][табл B][u24 lenA][u24 lenB][экстры][A][B] */
static int emit_risc2(const LZT *t, u8 *out, size_t cap, size_t *osize,
                      const u16 *nfA, const u32 *cmlA, const u16 *nfB, const u32 *cmlB,
                      const u8 *tb, size_t tsz)
{
    BW bw, ex;
    bw_init(&bw, out, cap);
    bw_byte(&bw, 2);
    bw_raw(&bw, tb, tsz);
    size_t P = bw_flush(&bw);
    if (bw.err || P + 6 > cap) return -1;
    bw_init(&ex, out + P + 6, cap - P - 6);

    /* поток B: дистанции, с самого конца буфера.
       rANS декодирует в обратном порядке — кодируем от хвоста */
    RiscEnc eb;
    risc_enc_init(&eb, out + cap);
    for (size_t i = t->n; i-- > 0; ) {
        u32 T = t->tl[i];
        if (T < 256) continue;
        u32 D = t->td[i];
        int ds = (D & TD_REP) ? (int)(DST_SLOTS + (D & 3)) : dslot(D);
        risc_put(&eb, cmlB[ds], nfB[ds]);
    }
    risc_flush(&eb);
    size_t ptrB = (size_t)(eb.ptr - out);

    /* поток A: токены, сразу под B; EOB декодируется последним — кодируем первым */
    RiscEnc ea;
    risc_enc_init(&ea, out + ptrB);
    risc_put(&ea, cmlA[EOB], nfA[EOB]);
    for (size_t i = t->n; i-- > 0; ) {
        u32 T = t->tl[i];
        if (T < 256) { risc_put(&ea, cmlA[T], nfA[T]); continue; }
        u32 L = T - 256;
        int s = 257 + lslot((int)L);
        risc_put(&ea, cmlA[s], nfA[s]);
    }
    risc_flush(&ea);
    if (bw.err) return -1;
    size_t ptrA = (size_t)(ea.ptr - out);
    size_t lenA = ptrB - ptrA, lenB = cap - ptrB;

    /* экстры: декодер читает вперёд по токенам */
    for (size_t i = 0; i < t->n; i++) {
        u32 T = t->tl[i];
        if (T < 256) continue;
        u32 L = T - 256;
        int s = lslot((int)L);
        bw_bits(&ex, L - LEN_BASE[s], LEN_EBITS[s]);
        u32 D = t->td[i];
        if (!(D & TD_REP)) {
            int ds = dslot(D);
            bw_bits(&ex, D - DST_BASE[ds], DST_EBITS[ds]);
        }
    }
    if (ex.err) return -1;
    size_t ex_len = bw_flush(&ex);
    if (ptrA < P + 6 + ex_len) return -1;   /* переслись с экстрами */
    if ((lenA | lenB) >> 24) return -1;

    /* A и B лежат подряд — сдвигаем вниз вплотную к экстрам */
    memmove(out + P + 6 + ex_len, out + ptrA, cap - ptrA);
    out[P] = (u8)lenA;
    out[P + 1] = (u8)(lenA >> 8);
    out[P + 2] = (u8)(lenA >> 16);
    out[P + 3] = (u8)lenB;
    out[P + 4] = (u8)(lenB >> 8);
    out[P + 5] = (u8)(lenB >> 16);
    *osize = P + 6 + ex_len + (cap - ptrA);
    return 0;
}

/* режим 3: RISC v2 — таблица B по классам длины, экстра-биты бинарным rANS.
   режим 4: то же + контекстные таблицы на полный алфавит A (291) — таблица
   выбирается по контексту токена (старший ниббл prev-байта), иначе глобальная.
   Раскладка: [mode][табл A][mask][таблы B][карта+f0 контекстов]
              [режим 4: litmap + табл. 291 по контексту]
              [u24 lenA][u24 lenB][u24 lenC][C][A][B] */
static int emit_risc3(const LZT *t, u8 *out, size_t cap, size_t *osize,
                      const u16 *nfA, const u16 nfB[4][DST_SYMS], const u16 *f0,
                      const u8 *tb, size_t tsz,
                      const u16 *nfx, const u32 *cmlx, const u8 *litmap,
                      const u8 *tc, int cxe)
{
    u32 cmlA[LITN_SYMS], cmlB[4][DST_SYMS];
    risc_cml(nfA, LITN_SYMS, cmlA);
    for (int c = 0; c < 4; c++) risc_cml(nfB[c], DST_SYMS, cmlB[c]);

    BW bw;
    bw_init(&bw, out, cap);
    bw_byte(&bw, nfx ? 4 : 3);
    bw_raw(&bw, tb, tsz);
    size_t P = bw_flush(&bw);
    if (bw.err || P + 9 > cap) return -1;

    /* поток B: слоты дистанций, таблица по классу длины матча */
    RiscEnc eb;
    risc_enc_init(&eb, out + cap);
    for (size_t i = t->n; i-- > 0; ) {
        u32 T = t->tl[i];
        if (T < 256) continue;
        u32 D = t->td[i];
        int ds = (D & TD_REP) ? (int)(DST_SLOTS + (D & 3)) : dslot(D);
        int cls = lcls(lslot((int)(T - 256)));
        risc_put(&eb, cmlB[cls][ds], nfB[cls][ds]);
    }
    risc_flush(&eb);
    size_t ptrB = (size_t)(eb.ptr - out);

    /* поток C: экстра-биты. Декодер читает MSB вперёд — кодируем с хвоста,
       внутри матча сначала дистанция, потом длина (обратный порядок) */
    RiscEnc ec;
    risc_enc_init(&ec, out + ptrB);
    for (size_t i = t->n; i-- > 0; ) {
        u32 T = t->tl[i];
        if (T < 256) continue;
        int ls = lslot((int)(T - 256));
        u32 D = t->td[i];
        if (!(D & TD_REP)) {
            int ds = dslot(D);
            int e = DST_EBITS[ds];
            u32 ev = D - DST_BASE[ds];
            for (int k = 0; k < e; k++)
                riscb_put(&ec, f0[EXC_L + DCTX[ds] + (e - 1 - k)], (ev >> k) & 1);
        }
        int e = LEN_EBITS[ls];
        u32 ev = T - 256 - LEN_BASE[ls];
        for (int k = 0; k < e; k++)
            riscb_put(&ec, f0[LCTX[ls] + (e - 1 - k)], (ev >> k) & 1);
    }
    risc_flush(&ec);
    size_t ptrC = (size_t)(ec.ptr - out);

    /* поток A: токены, EOB декодируется последним — кодируем первым.
       режим 4: каждый токен через таблицу своего контекста */
    RiscEnc ea;
    risc_enc_init(&ea, out + ptrC);
    if (nfx) {
        if (litmap[cxe >> 3] >> (cxe & 7) & 1)
            risc_put(&ea, cmlx[cxe * LITN_SYMS + EOB], nfx[cxe * LITN_SYMS + EOB]);
        else
            risc_put(&ea, cmlA[EOB], nfA[EOB]);
        for (size_t i = t->n; i-- > 0; ) {
            u32 T = t->tl[i];
            int sym = T < 256 ? (int)T : 257 + lslot((int)(T - 256));
            int cx = tc[i];
            if (litmap[cx >> 3] >> (cx & 7) & 1)
                risc_put(&ea, cmlx[cx * LITN_SYMS + sym], nfx[cx * LITN_SYMS + sym]);
            else
                risc_put(&ea, cmlA[sym], nfA[sym]);
        }
    } else {
        risc_put(&ea, cmlA[EOB], nfA[EOB]);
        for (size_t i = t->n; i-- > 0; ) {
            u32 T = t->tl[i];
            if (T < 256) { risc_put(&ea, cmlA[T], nfA[T]); continue; }
            int s = 257 + lslot((int)(T - 256));
            risc_put(&ea, cmlA[s], nfA[s]);
        }
    }
    risc_flush(&ea);
    if (bw.err) return -1;
    size_t ptrA = (size_t)(ea.ptr - out);
    size_t lenA = ptrC - ptrA, lenB = cap - ptrB, lenC = ptrB - ptrC;
    if ((lenA | lenB | lenC) >> 24) return -1;

    /* три потока лежат подряд — сдвигаем вниз вплотную к заголовку */
    memmove(out + P + 9, out + ptrA, cap - ptrA);
    out[P] = (u8)lenA;
    out[P + 1] = (u8)(lenA >> 8);
    out[P + 2] = (u8)(lenA >> 16);
    out[P + 3] = (u8)lenB;
    out[P + 4] = (u8)(lenB >> 8);
    out[P + 5] = (u8)(lenB >> 16);
    out[P + 6] = (u8)lenC;
    out[P + 7] = (u8)(lenC >> 8);
    out[P + 8] = (u8)(lenC >> 16);
    *osize = P + 9 + (cap - ptrA);
    return 0;
}

/* хвост таблиц режима 3/4: mask, таблицы B по классам, карта + q экстра-битов */
static size_t r3tail(const u16 nfB[4][DST_SYMS], u8 mask, const u16 *f0,
                     const u8 *bm, u8 *dst, size_t cap)
{
    size_t o = 0;
    if (o + 1 > cap) return (size_t)-1;
    dst[o++] = mask;
    for (int c = 0; c < 4; c++) {
        if (!(mask >> c & 1)) continue;
        size_t q = risc_table_store(nfB[c], DST_SYMS, dst + o, cap - o);
        if (q == (size_t)-1) return (size_t)-1;
        o += q;
    }
    if (o + EXC_ALL / 8 > cap) return (size_t)-1;
    memcpy(dst + o, bm, EXC_ALL / 8);
    o += EXC_ALL / 8;
    for (int c = 0; c < EXC_ALL; c++)
        if (f0[c]) {
            if (o + 1 > cap) return (size_t)-1;
            dst[o++] = (u8)(f0[c] >> 4);
        }
    return o;
}

/* контекстных классов: старший ниббл prev-байта (0 для первого байта блока) */
#define LITC 16

int lzi_emit2(const u8 *in, LZT *t, u8 *out, size_t cap, size_t *osize)
{
    HF fl, fd;
    /* контекст токена — старший ниббл prev-байта, u8 на токен; нужен только
       режиму 4, остальным путям td не трогаем */
    u8 *tc = malloc(t->n ? t->n : 1);
    size_t op = 0;
    int cxe = 0, have_tc = 0;
    if (tc) {
        have_tc = 1;
        for (size_t i = 0; i < t->n; i++) {
            u32 T = t->tl[i];
            tc[i] = (u8)((op ? in[op - 1] : 0) >> 4);
            op += T < 256 ? 1 : T - 256;
        }
        cxe = (int)((op ? in[op - 1] : 0) >> 4);
    }
    /* поток без матчей: пустой алфавит дистанций недопустим */
    u16 fd2[DST_SYMS];
    memcpy(fd2, t->fd, sizeof fd2);
    int anyd = 0;
    for (int i = 0; i < DST_SYMS; i++) anyd |= fd2[i] != 0;
    if (!anyd) fd2[0] = 1;
    hf_from_freq(&fl, t->fl, LITN_SYMS);
    hf_from_freq(&fd, fd2, DST_SYMS);

    /* дерево длин + оценка хаффмана */
    u8 arr[TREE2_BYTES], tr[TREE2_BYTES + TREE2_BYTES / 127 + 8];
    memcpy(arr, fl.lens, LITN_SYMS);
    memcpy(arr + LITN_SYMS, fd.lens, DST_SYMS);
    size_t rs = rle_encode(arr, TREE2_BYTES, tr);
    size_t treesz = 5 + ((rs < TREE2_BYTES) ? rs : TREE2_BYTES);

    u64 hbits = fl.lens[EOB];
    u64 exb = 0;
    /* счётчики RISC v2: слоты дистанций по классам + контексты экстра-битов */
    u16 cb[4][DST_SYMS], nfB[4][DST_SYMS];
    u32 n0[EXC_ALL], n1[EXC_ALL];
    memset(cb, 0, sizeof cb);
    memset(n0, 0, sizeof n0);
    memset(n1, 0, sizeof n1);
    for (size_t i = 0; i < t->n; i++) {
        u32 T = t->tl[i];
        if (T < 256) { hbits += fl.lens[T]; continue; }
        u32 L = T - 256;
        int ls = lslot((int)L);
        int cls = lcls(ls);
        hbits += fl.lens[257 + ls];
        u32 D = t->td[i];
        if (D & TD_REP) {
            hbits += fd.lens[DST_SLOTS + (D & 3)];
            cb[cls][DST_SLOTS + (D & 3)]++;
        } else {
            int ds = dslot(D);
            int e = DST_EBITS[ds];
            u32 ev = D - DST_BASE[ds];
            hbits += fd.lens[ds] + e;
            exb += e;
            cb[cls][ds]++;
            u32 *dc = &n0[EXC_L + DCTX[ds]], *dn = &n1[EXC_L + DCTX[ds]];
            for (int k = 0; k < e; k++)
                if ((ev >> (e - 1 - k)) & 1) dn[k]++; else dc[k]++;
        }
        /* в режиме 1 экстры в общем бит-потоке, в режиме 3 — в потоке C */
        int e = LEN_EBITS[ls];
        u32 ev = L - LEN_BASE[ls];
        hbits += e;
        exb += e;
        u32 *lc = &n0[LCTX[ls]], *ln = &n1[LCTX[ls]];
        for (int k = 0; k < e; k++)
            if ((ev >> (e - 1 - k)) & 1) ln[k]++; else lc[k]++;
    }
    u64 hsz = 1 + treesz + (hbits + 7) / 8;

    /* частоты RISC v2 */
    u16 nfA[LITN_SYMS];
    if (risc_norm(t->fl, LITN_SYMS, nfA) != 0) { free(tc); return -1; }
    double rbA = 0;
    for (int s = 0; s < LITN_SYMS; s++)
        if (t->fl[s]) rbA += (double)t->fl[s] * log2((double)RISC_TOTAL / nfA[s]);

    /* режим 2: одна таблица B, экстры сырьём */
    u16 nfB2[DST_SYMS];
    int have2 = risc_norm(t->fd, DST_SYMS, nfB2) == 0;
    double rb2 = rbA;
    if (have2)
        for (int s = 0; s < DST_SYMS; s++)
            if (t->fd[s]) rb2 += (double)t->fd[s] * log2((double)RISC_TOTAL / nfB2[s]);

    memset(nfB, 0, sizeof nfB);
    u8 mask = 0;
    for (int c = 0; c < 4; c++) {
        u32 tot = 0;
        for (int s = 0; s < DST_SYMS; s++) tot += cb[c][s];
        if (!tot) continue;
        if (risc_norm(cb[c], DST_SYMS, nfB[c]) != 0) { free(tc); return -1; }
        mask |= (u8)(1u << c);
    }
    /* f0 контекстов, 0 = не встречался; в потоке — байт q, f0 = 16q+8 */
    u16 f0[EXC_ALL];
    u32 nused = 0;
    for (int c = 0; c < EXC_ALL; c++) {
        u32 tot = n0[c] + n1[c];
        if (!tot) { f0[c] = 0; continue; }
        u32 v = (u32)((u64)n0[c] * RISC_TOTAL / tot);
        if (!v) v = 1;
        if (v >= RISC_TOTAL) v = RISC_TOTAL - 1;
        u32 q = (v + 8) >> 4;
        if (q > 255) q = 255;
        f0[c] = (u16)(q * 16 + 8);
        nused++;
    }

    /* оценка RISC v2: потоки A/B плюс бинарный C */
    double rb = 0, rbC = 0;
    for (int c = 0; c < 4; c++)
        if (mask >> c & 1)
            for (int s = 0; s < DST_SYMS; s++)
                if (cb[c][s]) rb += (double)cb[c][s] * log2((double)RISC_TOTAL / nfB[c][s]);
    for (int c = 0; c < EXC_ALL; c++)
        if (f0[c]) {
            rbC += (double)n0[c] * log2((double)RISC_TOTAL / f0[c]);
            rbC += (double)n1[c] * log2((double)RISC_TOTAL / (RISC_TOTAL - f0[c]));
        }
    double rbB = rb;
    rb += rbA + rbC;

    /* таблицы режима 3: A + хвост (mask, B классы, карта+q) */
    u8 tb[5200];
    u8 bm[EXC_ALL / 8];
    memset(bm, 0, sizeof bm);
    for (int c = 0; c < EXC_ALL; c++)
        if (f0[c]) bm[c >> 3] |= (u8)(1u << (c & 7));
    size_t tsz = risc_table_store(nfA, LITN_SYMS, tb, sizeof tb);
    if (tsz == (size_t)-1) { free(tc); return -1; }
    size_t tl3 = r3tail(nfB, mask, f0, bm, tb + tsz, sizeof tb - tsz);
    if (tl3 == (size_t)-1) { free(tc); return -1; }
    tsz += tl3;
    size_t tszA = risc_table_size(nfA, LITN_SYMS);   /* для tb2 режима 2 */
    u64 rsz = 1 + tsz + 9 + (u64)(rb + 7) / 8 + 16;

    /* таблицы режима 2: A + одна B (до режима 4 — тот не трогает tb2) */
    u8 tb2[1100];
    size_t tsz2 = 0;
    if (have2) {
        memcpy(tb2, tb, tszA);
        tsz2 = tszA;
        size_t q2 = risc_table_store(nfB2, DST_SYMS, tb2 + tsz2, sizeof tb2 - tsz2);
        if (q2 == (size_t)-1) have2 = 0;
        else tsz2 += q2;
    }
    u64 rsz2 = have2 ? 1 + tsz2 + 6 + (exb + 7) / 8 + (u64)(rb2 + 7) / 8 + 8 : (u64)-1;

    /* --- режим 4: контекстные таблицы полного алфавита A поверх режима 3 --- */
    u16 nfx[LITC][LITN_SYMS];
    u32 cmlx[LITC][LITN_SYMS];
    u32 c291[LITC][LITN_SYMS];
    u8 litmap[2] = { 0, 0 };
    double litA = 0;
    int nch = 0;
    memset(c291, 0, sizeof c291);
    if (have_tc) {
        for (size_t i = 0; i < t->n; i++) {
            u32 T = t->tl[i];
            c291[tc[i]][T < 256 ? (int)T : 257 + lslot((int)(T - 256))]++;
        }
        c291[cxe][EOB]++;
        for (int cx = 0; cx < LITC; cx++) {
            u32 tot = 0;
            for (int s = 0; s < LITN_SYMS; s++) tot += c291[cx][s];
            if (!tot) continue;
            double bef = 0;
            for (int s = 0; s < LITN_SYMS; s++)
                if (c291[cx][s]) bef += (double)c291[cx][s] * log2((double)RISC_TOTAL / nfA[s]);
            u16 nf2[LITN_SYMS];
            if (risc_norm32(c291[cx], LITN_SYMS, nf2) != 0) continue;
            double aft = 0;
            for (int s = 0; s < LITN_SYMS; s++)
                if (c291[cx][s]) aft += (double)c291[cx][s] * log2((double)RISC_TOTAL / nf2[s]);
            size_t tl2 = risc_table_size(nf2, LITN_SYMS);
            if (bef - aft <= (double)(tl2 * 8 + 32)) continue;   /* таблица дороже выигрыша */
            litmap[cx >> 3] |= (u8)(1u << (cx & 7));
            memcpy(nfx[cx], nf2, sizeof nf2);
            risc_cml(nf2, LITN_SYMS, cmlx[cx]);
            litA += aft;
            nch++;
        }
    }
    u64 rsz4 = (u64)-1;
    u16 nfA4[LITN_SYMS];
    u8 tb4[6400];
    size_t tsz4 = 0;
    if (nch) {
        /* глобальная таблица режима 4 — по остатку: токены выбранных
           контекстов уходят из неё */
        u16 restA[LITN_SYMS];
        memcpy(restA, t->fl, sizeof restA);
        for (int cx = 0; cx < LITC; cx++)
            if (litmap[cx >> 3] >> (cx & 7) & 1)
                for (int s = 0; s < LITN_SYMS; s++)
                    restA[s] = (u16)(restA[s] - c291[cx][s]);
        if (risc_norm(restA, LITN_SYMS, nfA4) == 0) {
            double rbA4 = 0;
            for (int s = 0; s < LITN_SYMS; s++)
                if (restA[s]) rbA4 += (double)restA[s] * log2((double)RISC_TOTAL / nfA4[s]);
            double rb4 = rbB + rbA4 + litA + rbC;
            /* tb4: своя таблица A + хвост + litmap + контекстные таблицы */
            size_t o4 = risc_table_store(nfA4, LITN_SYMS, tb4, sizeof tb4);
            if (o4 != (size_t)-1) {
                size_t tl4 = r3tail(nfB, mask, f0, bm, tb4 + o4, sizeof tb4 - o4);
                if (tl4 == (size_t)-1 || o4 + tl4 + 2 > sizeof tb4) nch = 0;
                else {
                    o4 += tl4;
                    tb4[o4++] = litmap[0];
                    tb4[o4++] = litmap[1];
                    for (int cx = 0; cx < LITC && nch; cx++)
                        if (litmap[cx >> 3] >> (cx & 7) & 1) {
                            size_t q4 = risc_table_store(nfx[cx], LITN_SYMS, tb4 + o4, sizeof tb4 - o4);
                            if (q4 == (size_t)-1) nch = 0;
                            else o4 += q4;
                        }
                    if (nch) {
                        tsz4 = o4;
                        rsz4 = 1 + tsz4 + 9 + (u64)(rb4 + 7) / 8 + 16;
                    }
                }
            } else nch = 0;
        }
    }

    int best = 1;
    u64 bs = hsz;
    if (have2 && rsz2 < bs) { best = 2; bs = rsz2; }
    if (rsz < bs) { best = 3; bs = rsz; }
    if (nch && rsz4 < bs) { best = 4; bs = rsz4; }
    if (best == 2) {
        u32 cmlA2[LITN_SYMS], cmlB2[DST_SYMS];
        risc_cml(nfA, LITN_SYMS, cmlA2);
        risc_cml(nfB2, DST_SYMS, cmlB2);
        free(tc);
        return emit_risc2(t, out, cap, osize, nfA, cmlA2, nfB2, cmlB2, tb2, tsz2);
    }
    if (best == 3) {
        int rc = emit_risc3(t, out, cap, osize, nfA, (const u16 (*)[DST_SYMS])nfB, f0, tb, tsz,
                            NULL, NULL, NULL, tc, cxe);
        free(tc);
        return rc == 0 ? 0 : emit_huff2(t, out, cap, osize, &fl, &fd, arr);
    }
    if (best == 4) {
        int rc = emit_risc3(t, out, cap, osize, nfA4, (const u16 (*)[DST_SYMS])nfB, f0, tb4, tsz4,
                            &nfx[0][0], &cmlx[0][0], litmap, tc, cxe);
        free(tc);
        return rc == 0 ? 0 : emit_huff2(t, out, cap, osize, &fl, &fd, arr);
    }
    free(tc);
    return emit_huff2(t, out, cap, osize, &fl, &fd, arr);
}

int lzi_decode2(const u8 *pay, size_t psz, u8 *out, size_t nraw)
{
    if (psz < 2) return -1;
    u8 mode = pay[0];
    u32 rep[4] = { 1, 1, 1, 1 };
    size_t op = 0;

    if (mode == 1) {
        BR br;
        br_init(&br, pay + 1, psz - 1);
        u8 arr[TREE2_BYTES];
        if (hf_load_arr(&br, arr, TREE2_BYTES)) return -1;
        for (int i = 0; i < TREE2_BYTES; i++) if (arr[i] > HLIM) return -1;
        HF fl, fd;
        hf_from_lens(&fl, arr, LITN_SYMS);
        hf_from_lens(&fd, arr + LITN_SYMS, DST_SYMS);
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
                if (ds < 0 || ds >= DST_SYMS) return -1;
                u32 D;
                if (ds >= DST_SLOTS) { D = rep[ds - DST_SLOTS]; rep_move(rep, ds - DST_SLOTS); }
                else { D = DST_BASE[ds] + br_bits(&br, DST_EBITS[ds]); rep_push(rep, D); }
                if (lz_copy(out, &op, nraw, D, L)) return -1;
            }
        }
    } else if (mode == 2) {
        const u8 *p = pay + 1, *end = pay + psz;
        u16 nfA[LITN_SYMS], nfB[DST_SYMS];
        size_t used;
        if (risc_table_load(p, (size_t)(end - p), nfA, LITN_SYMS, &used)) return -1;
        p += used;
        if (risc_table_load(p, (size_t)(end - p), nfB, DST_SYMS, &used)) return -1;
        p += used;
        if (end - p < 6) return -1;
        size_t lenA = (size_t)p[0] | (size_t)p[1] << 8 | (size_t)p[2] << 16;
        size_t lenB = (size_t)p[3] | (size_t)p[4] << 8 | (size_t)p[5] << 16;
        p += 6;
        if (lenA + lenB > (size_t)(end - p)) return -1;
        const u8 *aB0 = end - lenB, *aA0 = aB0 - lenA;
        BR ex;
        br_init(&ex, p, (size_t)(aA0 - p));
        RiscDec rdA, rdB;   /* 40К на стеке — ок */
        u32 cmlA[LITN_SYMS], cmlB[DST_SYMS];
        risc_build(&rdA, nfA, LITN_SYMS, cmlA);
        risc_build(&rdB, nfB, DST_SYMS, cmlB);
        const u8 *aA = aA0, *aB = aB0;
        /* aA/aB сдвинутся на 4 — границы держим отдельно (aB0) */
        if (risc_dec_init(&rdA, &aA, aB0) || risc_dec_init(&rdB, &aB, end)) return -1;
        for (;;) {
            int s = risc_get(&rdA, &aA, aB);
            if (s < 0) return -1;
            if (s < 256) {
                if (op >= nraw) return -1;
                out[op++] = (u8)s;
            } else if (s == 256) {
                break;
            } else {
                int ls = s - 257;
                if (ls >= LEN_SLOTS) return -1;
                u32 L = LEN_BASE[ls] + br_bits(&ex, LEN_EBITS[ls]);
                int ds = risc_get(&rdB, &aB, end);
                if (ds < 0 || ds >= DST_SYMS) return -1;
                u32 D;
                if (ds >= DST_SLOTS) { D = rep[ds - DST_SLOTS]; rep_move(rep, ds - DST_SLOTS); }
                else { D = DST_BASE[ds] + br_bits(&ex, DST_EBITS[ds]); rep_push(rep, D); }
                if (lz_copy(out, &op, nraw, D, L)) return -1;
            }
        }
    } else if (mode == 3) {
        /* RISC v2: B по классам длины, экстра-биты бинарным rANS (поток C) */
        const u8 *p = pay + 1, *end = pay + psz;
        u16 nfA[LITN_SYMS], nfB[4][DST_SYMS];
        u32 cmlA[LITN_SYMS], cmlB[4][DST_SYMS];
        size_t used;
        if (risc_table_load(p, (size_t)(end - p), nfA, LITN_SYMS, &used)) return -1;
        p += used;
        if (end - p < 1) return -1;
        u8 mask = *p++;
        for (int c = 0; c < 4; c++) {
            if (mask >> c & 1) {
                if (risc_table_load(p, (size_t)(end - p), nfB[c], DST_SYMS, &used)) return -1;
                p += used;
            } else {
                /* класс не встречался — заглушка, никогда не декодируется */
                memset(nfB[c], 0, sizeof nfB[c]);
                nfB[c][0] = RISC_TOTAL;
            }
            risc_cml(nfB[c], DST_SYMS, cmlB[c]);
        }
        /* контексты: биткарта, за ней байты q (f0 = 16q+8) по порядку */
        if (end - p < EXC_ALL / 8) return -1;
        u16 f0[EXC_ALL];
        memset(f0, 0, sizeof f0);
        const u8 *q = p + EXC_ALL / 8;
        for (int c = 0; c < EXC_ALL; c++) {
            if (!(p[c >> 3] >> (c & 7) & 1)) continue;
            if (q >= end) return -1;
            f0[c] = (u16)((u32)*q << 4 | 8);
            q++;
        }
        p = q;
        if (end - p < 9) return -1;
        size_t lenA = (size_t)p[0] | (size_t)p[1] << 8 | (size_t)p[2] << 16;
        size_t lenB = (size_t)p[3] | (size_t)p[4] << 8 | (size_t)p[5] << 16;
        size_t lenC = (size_t)p[6] | (size_t)p[7] << 8 | (size_t)p[8] << 16;
        p += 9;
        if (lenA + lenB + lenC != (size_t)(end - p)) return -1;
        /* потоки лежат [A][C][B] */
        const u8 *aA0 = p, *aC0 = aA0 + lenA, *aB0 = aC0 + lenC;
        RiscDec rdA;        /* LUT нужен только потоку A */
        RiscB rbB, rbc;
        risc_build(&rdA, nfA, LITN_SYMS, cmlA);
        const u8 *aA = aA0, *aB = aB0, *aC = aC0;
        if (risc_dec_init(&rdA, &aA, aC0)) return -1;
        if (riscb_init(&rbB, &aB, end)) return -1;
        if (riscb_init(&rbc, &aC, aB0)) return -1;
        for (;;) {
            int s = risc_get(&rdA, &aA, aC0);
            if (s < 0) return -1;
            if (s < 256) {
                if (op >= nraw) return -1;
                out[op++] = (u8)s;
            } else if (s == 256) {
                break;
            } else {
                int ls = s - 257;
                if (ls >= LEN_SLOTS) return -1;
                u32 L = LEN_BASE[ls], lev = 0;
                for (int k = 0; k < LEN_EBITS[ls]; k++) {
                    int b = riscb_get(&rbc, &aC, aB0, f0[LCTX[ls] + k]);
                    if (b < 0) return -1;
                    lev = (lev << 1) | (u32)b;
                }
                L += lev;
                int cls = lcls(ls);
                int ds = risc_get_sl(nfB[cls], cmlB[cls], DST_SYMS, &rbB.x, &aB, end);
                if (ds < 0 || ds >= DST_SYMS) return -1;
                u32 D;
                if (ds >= DST_SLOTS) { D = rep[ds - DST_SLOTS]; rep_move(rep, ds - DST_SLOTS); }
                else {
                    int e = DST_EBITS[ds];
                    u32 ev = 0;
                    for (int k = 0; k < e; k++) {
                        int b = riscb_get(&rbc, &aC, aB0, f0[EXC_L + DCTX[ds] + k]);
                        if (b < 0) return -1;
                        ev = (ev << 1) | (u32)b;
                    }
                    D = DST_BASE[ds] + ev;
                    rep_push(rep, D);
                }
                if (lz_copy(out, &op, nraw, D, L)) return -1;
            }
        }
    } else if (mode == 4) {
        /* режим 3 + контекстные таблицы алфавита A: litmap + таблицы после q */
        const u8 *p = pay + 1, *end = pay + psz;
        u16 nfA[LITN_SYMS], nfB[4][DST_SYMS], nfx[16][LITN_SYMS];
        u32 cmlA[LITN_SYMS], cmlB[4][DST_SYMS], cmlx[16][LITN_SYMS];
        size_t used;
        if (risc_table_load(p, (size_t)(end - p), nfA, LITN_SYMS, &used)) return -1;
        p += used;
        if (end - p < 1) return -1;
        u8 mask = *p++;
        for (int c = 0; c < 4; c++) {
            if (mask >> c & 1) {
                if (risc_table_load(p, (size_t)(end - p), nfB[c], DST_SYMS, &used)) return -1;
                p += used;
            } else {
                memset(nfB[c], 0, sizeof nfB[c]);
                nfB[c][0] = RISC_TOTAL;
            }
            risc_cml(nfB[c], DST_SYMS, cmlB[c]);
        }
        if (end - p < EXC_ALL / 8) return -1;
        u16 f0[EXC_ALL];
        memset(f0, 0, sizeof f0);
        const u8 *q = p + EXC_ALL / 8;
        for (int c = 0; c < EXC_ALL; c++) {
            if (!(p[c >> 3] >> (c & 7) & 1)) continue;
            if (q >= end) return -1;
            f0[c] = (u16)((u32)*q << 4 | 8);
            q++;
        }
        p = q;
        if (end - p < 2) return -1;
        u8 lm0 = p[0], lm1 = p[1];
        p += 2;
        for (int cx = 0; cx < 16; cx++) {
            if (!((cx < 8 ? lm0 : lm1) >> (cx & 7) & 1)) continue;
            if (risc_table_load(p, (size_t)(end - p), nfx[cx], LITN_SYMS, &used)) return -1;
            p += used;
            risc_cml(nfx[cx], LITN_SYMS, cmlx[cx]);
        }
        if (end - p < 9) return -1;
        size_t lenA = (size_t)p[0] | (size_t)p[1] << 8 | (size_t)p[2] << 16;
        size_t lenB = (size_t)p[3] | (size_t)p[4] << 8 | (size_t)p[5] << 16;
        size_t lenC = (size_t)p[6] | (size_t)p[7] << 8 | (size_t)p[8] << 16;
        p += 9;
        if (lenA + lenB + lenC != (size_t)(end - p)) return -1;
        const u8 *aA0 = p, *aC0 = aA0 + lenA, *aB0 = aC0 + lenC;
        RiscDec *rd4 = NULL;
        if (lm0 | lm1) {
            rd4 = malloc(16 * sizeof *rd4);     /* 16 LUT по 48К — только с кучей */
            if (!rd4) return -1;
            for (int cx = 0; cx < 16; cx++)
                if ((cx < 8 ? lm0 : lm1) >> (cx & 7) & 1)
                    risc_build(&rd4[cx], nfx[cx], LITN_SYMS, cmlx[cx]);
        }
        RiscDec rdA;
        RiscB rbB, rbc;
        risc_build(&rdA, nfA, LITN_SYMS, cmlA);
        const u8 *aA = aA0, *aB = aB0, *aC = aC0;
        int rc = 0;
        if (risc_dec_init(&rdA, &aA, aC0) || riscb_init(&rbB, &aB, end) ||
            riscb_init(&rbc, &aC, aB0)) rc = -1;
        for (;;) {
            if (rc) break;
            int cx = op ? out[op - 1] >> 4 : 0;
            /* состояние одно на поток A, LUT свой на контекст */
            /* таблицу выбирает контекст токена; состояние одно на поток A */
            RiscDec *d = ((cx < 8 ? lm0 : lm1) >> (cx & 7) & 1) ? &rd4[cx] : &rdA;
            d->x = rdA.x;
            int s = risc_get(d, &aA, aC0);
            rdA.x = d->x;
            if (s < 0) { rc = -1; break; }
            if (s < 256) {
                if (op >= nraw) { rc = -1; break; }
                out[op++] = (u8)s;
            } else if (s == 256) {
                break;
            } else {
                int ls = s - 257;
                if (ls >= LEN_SLOTS) { rc = -1; break; }
                u32 L = LEN_BASE[ls], lev = 0;
                for (int k = 0; k < LEN_EBITS[ls]; k++) {
                    int b = riscb_get(&rbc, &aC, aB0, f0[LCTX[ls] + k]);
                    if (b < 0) { rc = -1; break; }
                    lev = (lev << 1) | (u32)b;
                }
                if (rc) break;
                L += lev;
                int cls = lcls(ls);
                int ds = risc_get_sl(nfB[cls], cmlB[cls], DST_SYMS, &rbB.x, &aB, end);
                if (ds < 0 || ds >= DST_SYMS) { rc = -1; break; }
                u32 D;
                if (ds >= DST_SLOTS) { D = rep[ds - DST_SLOTS]; rep_move(rep, ds - DST_SLOTS); }
                else {
                    int e = DST_EBITS[ds];
                    u32 ev = 0;
                    for (int k = 0; k < e; k++) {
                        int b = riscb_get(&rbc, &aC, aB0, f0[EXC_L + DCTX[ds] + k]);
                        if (b < 0) { rc = -1; break; }
                        ev = (ev << 1) | (u32)b;
                    }
                    if (rc) break;
                    D = DST_BASE[ds] + ev;
                    rep_push(rep, D);
                }
                if (lz_copy(out, &op, nraw, D, L)) rc = -1;
            }
        }
        free(rd4);
        if (rc) return -1;
    } else return -1;

    return op == nraw ? 0 : -1;
}
