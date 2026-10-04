/* rle.h — свой RLE: он же упаковка деревьев Хаффмана */
#ifndef RLE_H
#define RLE_H
#include "iscf.h"

size_t rle_encode(const u8 *in, size_t n, u8 *out);            /* размер out >= n + n/127 + 2 */
size_t rle_decode(const u8 *in, size_t n, u8 *out, size_t cap); /* вернёт размер, (size_t)-1 при ошибке */

#endif
