/* risc.h — RISC: наш rANS-кодер. Состояние 32 бита, scale 12, байты назад */
#ifndef RISC_H
#define RISC_H
#include "iscf.h"

#define RISC_SCALE  12
#define RISC_TOTAL  (1 << RISC_SCALE)   /* 4096, сумма частот */
#define RISC_LOW    (1u << 23)          /* нижняя граница состояния */

/* LUT декодера + состояние. sym — u16: алфавит до 291 символа! */
typedef struct {
    u32 x;
    u16 sym[RISC_TOTAL];
    u16 fv[RISC_TOTAL];
    u16 fc[RISC_TOTAL];
} RiscDec;

/* кодер: x растёт вверх, байты пишем назад от конца буфера */
typedef struct { u32 x; u8 *ptr; } RiscEnc;

/* бинарный декод: без LUT, только состояние */
typedef struct { u32 x; } RiscB;

/* префиксы без LUT — для маленьких алфавитов */
static inline void risc_cml(const u16 *nf, int nsyms, u32 *cml)
{
    u32 c = 0;
    for (int s = 0; s < nsyms; s++) { cml[s] = c; c += nf[s]; }
}

int    risc_norm(const u16 *freq, int nsyms, u16 *nf);              /* сумма = 4096 */
void   risc_build(RiscDec *d, const u16 *nf, int nsyms, u32 *cml);  /* LUT + префиксы */
size_t risc_table_store(const u16 *nf, int nsyms, u8 *dst, size_t cap);
int    risc_table_load(const u8 *src, size_t sz, u16 *nf, int nsyms, size_t *used);

static inline void risc_enc_init(RiscEnc *e, u8 *buf_end)
{
    e->x = RISC_LOW;
    e->ptr = buf_end;
}

static inline void risc_put(RiscEnc *e, u32 c, u32 f)
{
    u32 xmax = ((RISC_LOW >> RISC_SCALE) << 8) * f;
    while (e->x >= xmax) { *--e->ptr = (u8)e->x; e->x >>= 8; }
    /* скобки обязательны: << ниже + по приоритету */
    e->x = (((e->x / f) << RISC_SCALE) + (e->x % f) + c);
}

static inline void risc_flush(RiscEnc *e)
{
    /* старший байт первым — в памяти получится little-endian */
    for (int i = 0; i < 4; i++) { *--e->ptr = (u8)(e->x >> 24); e->x <<= 8; }
}

static inline int risc_dec_init(RiscDec *d, const u8 **pp, const u8 *end)
{
    if (end - *pp < 4) return -1;
    d->x = le32(*pp);
    *pp += 4;
    return d->x >= RISC_LOW ? 0 : -1;
}

static inline int riscb_init(RiscB *b, const u8 **pp, const u8 *end)
{
    if (end - *pp < 4) return -1;
    b->x = le32(*pp);
    *pp += 4;
    return b->x >= RISC_LOW ? 0 : -1;
}

/* декод без LUT: бинарный поиск по префиксам, алфавит небольшой */
static inline int risc_get_sl(const u16 *nf, const u32 *cml, int nsyms,
                              u32 *x, const u8 **pp, const u8 *end)
{
    u32 sl = *x & (RISC_TOTAL - 1);
    int lo = 0, hi = nsyms - 1;
    while (lo < hi) {
        int m = (lo + hi + 1) >> 1;
        if (cml[m] <= sl) lo = m; else hi = m - 1;
    }
    u32 f = nf[lo], c = cml[lo];
    if (!f || sl >= c + f) return -1;
    *x = f * (*x >> RISC_SCALE) + sl - c;
    while (*x < RISC_LOW) {
        if (*pp >= end) return -1;
        *x = (*x << 8) | *(*pp)++;
    }
    return lo;
}

/* бит с частотой нуля f0 из [1..4095] */
static inline int riscb_get(RiscB *b, const u8 **pp, const u8 *end, u32 f0)
{
    if (!f0 || f0 >= RISC_TOTAL) return -1;
    u32 sl = b->x & (RISC_TOTAL - 1);
    int r;
    if (sl < f0) { r = 0; b->x = f0 * (b->x >> RISC_SCALE) + sl; }
    else { r = 1; b->x = (RISC_TOTAL - f0) * (b->x >> RISC_SCALE) + sl - f0; }
    while (b->x < RISC_LOW) {
        if (*pp >= end) return -1;
        b->x = (b->x << 8) | *(*pp)++;
    }
    return r;
}

static inline void riscb_put(RiscEnc *e, u32 f0, int bit)
{
    if (bit) risc_put(e, f0, RISC_TOTAL - f0);
    else risc_put(e, 0, f0);
}

/* символ из потока; pp движется вперёд по байтам ренормализации */
static inline int risc_get(RiscDec *d, const u8 **pp, const u8 *end)
{
    u32 sl = d->x & (RISC_TOTAL - 1);
    int s = d->sym[sl];
    if (d->fv[sl] == 0) return -1;
    d->x = d->fv[sl] * (d->x >> RISC_SCALE) + sl - d->fc[sl];
    while (d->x < RISC_LOW) {
        if (*pp >= end) return -1;
        d->x = (d->x << 8) | *(*pp)++;
    }
    return s;
}

#endif
