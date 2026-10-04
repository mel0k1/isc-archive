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
