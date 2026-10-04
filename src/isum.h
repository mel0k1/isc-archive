/* isum.h — свой чексум ISCF */
#ifndef ISUM_H
#define ISUM_H
#include "iscf.h"

u32 isum32(const void *p, size_t n);
u32 isum32_u(u32 h, const void *p, size_t n);   /* продолжить сумму */
u64 isum64(const void *p, size_t n);
u64 isum64_u(u64 h, const void *p, size_t n);   /* продолжить отпечаток */

#endif
