/* bitio.h — битовые потоки LSB-first; коды Хаффмана пишутся MSB-вперёд */
#ifndef BITIO_H
#define BITIO_H
#include "iscf.h"

typedef struct { const u8 *buf; size_t size, pos; u64 acc; int n; int over; } BR;

void br_init(BR *b, const void *buf, size_t size);
u32  br_peek(BR *b, int n);
void br_drop(BR *b, int n);
u32  br_bits(BR *b, int n);

typedef struct { u8 *buf; size_t cap, pos; u64 acc; int n; int err; } BW;

void   bw_init(BW *b, void *buf, size_t cap);
void   bw_bits(BW *b, u32 v, int n);            /* n <= 32, младшие биты вперёд */
void   bw_code(BW *b, u32 code, int len);       /* код Хаффмана, старший бит вперёд */
void   bw_align(BW *b);
void   bw_byte(BW *b, u8 v);                    /* выровнять и записать байт */
void   bw_raw(BW *b, const void *p, size_t n);
size_t bw_flush(BW *b);                         /* выровнять, вернуть размер */

#endif
