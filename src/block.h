/* block.h — кодек одного блока: выбор рецепта + конвейер */
#ifndef BLOCK_H
#define BLOCK_H
#include "iscf.h"

#define M_AUTO  0
#define M_STORE 1
#define M_LZ    2

size_t block_encode(const u8 *in, size_t n, int level, int mode,
                    u8 *out, size_t cap, u8 *recipe, i32 *head, i32 *prev,
                    u32 *tl, u32 *td);
int    block_decode(const u8 *pay, size_t psz, u8 recipe, u8 *out, size_t n);

#endif
