/* lzi.c — LZI: hash-цепочки по 4 байтам; LZ2 — rep-дистанции и RISC */
#include <string.h>
#include <math.h>
#include "lzi.h"
#include "rle.h"

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

int lzi_parse(const u8 *in, size_t n, const LZP *p, LZT *t,
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

int lzi_emit2(const LZT *t, u8 *out, size_t cap, size_t *osize)
{
    HF fl, fd;
    hf_from_freq(&fl, t->fl, LITN_SYMS);
    hf_from_freq(&fd, t->fd, DST_SYMS);

    /* дерево длин + оценка хаффмана */
    u8 arr[TREE2_BYTES], tr[TREE2_BYTES + TREE2_BYTES / 127 + 8];
    memcpy(arr, fl.lens, LITN_SYMS);
    memcpy(arr + LITN_SYMS, fd.lens, DST_SYMS);
    size_t rs = rle_encode(arr, TREE2_BYTES, tr);
    size_t treesz = 5 + ((rs < TREE2_BYTES) ? rs : TREE2_BYTES);

    u64 hbits = fl.lens[EOB];
    u64 exb = 0;
    for (size_t i = 0; i < t->n; i++) {
        u32 T = t->tl[i];
        if (T < 256) { hbits += fl.lens[T]; continue; }
        u32 L = T - 256;
        int s = lslot((int)L);
        hbits += fl.lens[257 + s];
        u32 D = t->td[i];
        if (D & TD_REP) hbits += fd.lens[DST_SLOTS + (D & 3)];
        else {
            int ds = dslot(D);
            hbits += fd.lens[ds] + DST_EBITS[ds];
            exb += DST_EBITS[ds];
        }
        /* в режиме 1 экстры идут в общий бит-поток, в режиме 2 — отдельно */
        hbits += LEN_EBITS[s];
        exb += LEN_EBITS[s];
    }
    u64 hsz = 1 + treesz + (hbits + 7) / 8;

    /* оценка RISC: два отдельных алфавита, как у хаффмана */
    u16 nfA[LITN_SYMS], nfB[DST_SYMS];
    u32 cmlA[LITN_SYMS], cmlB[DST_SYMS];
    if (risc_norm(t->fl, LITN_SYMS, nfA) != 0) return -1;
    if (risc_norm(t->fd, DST_SYMS, nfB) != 0) return -1;
    u32 cc = 0;
    for (int s = 0; s < LITN_SYMS; s++) { cmlA[s] = cc; cc += nfA[s]; }
    cc = 0;
    for (int s = 0; s < DST_SYMS; s++) { cmlB[s] = cc; cc += nfB[s]; }
    double rb = 0;
    for (int s = 0; s < LITN_SYMS; s++)
        if (t->fl[s]) rb += (double)t->fl[s] * log2((double)RISC_TOTAL / nfA[s]);
    for (int s = 0; s < DST_SYMS; s++)
        if (t->fd[s]) rb += (double)t->fd[s] * log2((double)RISC_TOTAL / nfB[s]);
    u8 tb[1050];
    size_t tsz = risc_table_store(nfA, LITN_SYMS, tb, sizeof tb);
    if (tsz == (size_t)-1) return -1;
    size_t tsz2 = risc_table_store(nfB, DST_SYMS, tb + tsz, sizeof tb - tsz);
    if (tsz2 == (size_t)-1) return -1;
    tsz += tsz2;
    u64 rsz = 1 + tsz + 6 + (exb + 7) / 8 + (u64)(rb + 7) / 8 + 8;

    if (rsz < hsz) return emit_risc2(t, out, cap, osize, nfA, cmlA, nfB, cmlB, tb, tsz);
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
    } else return -1;

    return op == nraw ? 0 : -1;
}
