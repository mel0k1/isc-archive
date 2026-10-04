/* isum.c — isum32: FNV-1a со своей базой 0x1CA7C0DE, не криптография */
#include "isum.h"

#define P32 0x01000193u
#define B32 0x1CA7C0DEu
#define P64 0x100000001b3ull
#define B64 0x1CA7C0DEDEC0DE17ull

u32 isum32_u(u32 h, const void *p, size_t n)
{
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= P32; }
    return h;
}

u32 isum32(const void *p, size_t n) { return isum32_u(B32, p, n); }

u64 isum64_u(u64 h, const void *p, size_t n)
{
    const u8 *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= P64; }
    return h;
}

u64 isum64(const void *p, size_t n) { return isum64_u(B64, p, n); }
