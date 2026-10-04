/* huff.h — канонический Хаффман с быстрым LUT-декодированием */
#ifndef HUFF_H
#define HUFF_H
#include "iscf.h"
#include "bitio.h"

#define ALPH_MAX 291
#define LUTB     9
#define LUT_SIZE (1 << LUTB)
#define TREE_BYTES (LITN_SYMS + DST_SLOTS)   /* 331 */

typedef struct {
    u8  lens[ALPH_MAX];
    u16 codes[ALPH_MAX];
    u16 slist[ALPH_MAX];
    u32 ccount[HLIM + 1];
    u32 cfirst[HLIM + 1];
    u32 cindex[HLIM + 1];
    u16 lut_sym[LUT_SIZE];
    u8  lut_len[LUT_SIZE];
} HF;

int  hf_from_freq(HF *h, const u16 *freq, int nsyms);
int  hf_from_lens(HF *h, const u8 *lens, int nsyms);
int  hf_dec(const HF *h, BR *b);
void hf_store_arr(BW *b, const u8 *arr, size_t nb);   /* [u16 nb][флаг][rle|raw] */
int  hf_load_arr(BR *b, u8 *arr, size_t nb);
void hf_store_trees(BW *b, const u8 *ll, const u8 *dl);
int  hf_load_trees(BR *b, u8 *ll, u8 *dl);

#endif
