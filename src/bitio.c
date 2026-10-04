/* bitio.c — LSB-first: младший бит потока идёт первым */
#include "bitio.h"

void br_init(BR *b, const void *buf, size_t size)
{
    b->buf = buf; b->size = size; b->pos = 0; b->acc = 0; b->n = 0; b->over = 0;
}

static void br_fill(BR *b, int want)
{
    while (b->n < want) {
        if (b->pos >= b->size) { b->over = 1; return; }  /* дальше читаются нули */
        b->acc |= (u64)b->buf[b->pos++] << b->n;
        b->n += 8;
    }
}

u32 br_peek(BR *b, int n) { br_fill(b, n); return (n < 32) ? (u32)(b->acc & ((1ull << n) - 1)) : (u32)b->acc; }

void br_drop(BR *b, int n) { b->acc >>= n; b->n -= n; }

u32 br_bits(BR *b, int n) { u32 v = br_peek(b, n); br_drop(b, n); return v; }

void bw_init(BW *b, void *buf, size_t cap)
{
    b->buf = buf; b->cap = cap; b->pos = 0; b->acc = 0; b->n = 0; b->err = 0;
}

void bw_bits(BW *b, u32 v, int n)
{
    if (n < 32) v &= (1u << n) - 1;
    b->acc |= (u64)v << b->n;
    b->n += n;
    while (b->n >= 8) {
        if (b->pos >= b->cap) { b->err = 1; return; }
        b->buf[b->pos++] = (u8)b->acc;
        b->acc >>= 8;
        b->n -= 8;
    }
}

void bw_code(BW *b, u32 code, int len)
{
    /* код Хаффмана старшими битами вперёд */
    for (int i = len - 1; i >= 0; i--) bw_bits(b, (code >> i) & 1, 1);
}

void bw_align(BW *b)
{
    if (b->n > 0) {
        if (b->pos >= b->cap) { b->err = 1; return; }
        b->buf[b->pos++] = (u8)b->acc;
        b->acc = 0; b->n = 0;
    }
}

void bw_byte(BW *b, u8 v) { bw_align(b); if (b->pos < b->cap) b->buf[b->pos++] = v; else b->err = 1; }

void bw_raw(BW *b, const void *p, size_t n)
{
    bw_align(b);
    if (b->pos + n > b->cap) { b->err = 1; return; }
    for (size_t i = 0; i < n; i++) b->buf[b->pos++] = ((const u8 *)p)[i];
}

size_t bw_flush(BW *b) { bw_align(b); return b->pos; }
