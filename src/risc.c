/* risc.c — RISC: нормализация частот, LUT, таблица в потоке */
#include <string.h>
#include "risc.h"

/* частоты -> натуральные, сумма ровно 4096; вариант под u32-счёты */
static void norm_any(u64 tot, const u32 *freq, int nsyms, u16 *nf)
{
    if (!tot) { nf[0] = RISC_TOTAL; return; }
    u32 sum = 0;
    for (int i = 0; i < nsyms; i++) {
        u32 q = (u32)((u64)freq[i] * RISC_TOTAL / tot);
        if (!q && freq[i]) q = 1;
        nf[i] = (u16)q;
        sum += q;
    }
    if (sum < RISC_TOTAL) {
        /* остаток — самому частому */
        int big = 0;
        for (int i = 1; i < nsyms; i++) if (freq[i] > freq[big]) big = i;
        nf[big] = (u16)(nf[big] + RISC_TOTAL - sum);
        return;
    }
    /* избыток — отнимаем у самых жирных nf, но не в ноль */
    while (sum > RISC_TOTAL) {
        int big = -1;
        for (int i = 0; i < nsyms; i++)
            if (nf[i] > 1 && (big < 0 || nf[i] > nf[big])) big = i;
        if (big < 0) return;
        nf[big]--;
        sum--;
    }
}

int risc_norm(const u16 *freq, int nsyms, u16 *nf)
{
    if (nsyms < 1 || nsyms > 512) return -1;
    u32 f[512];
    for (int i = 0; i < nsyms; i++) f[i] = freq[i];
    u64 tot = 0;
    for (int i = 0; i < nsyms; i++) tot += f[i];
    norm_any(tot, f, nsyms, nf);
    u32 sum = 0;
    for (int i = 0; i < nsyms; i++) sum += nf[i];
    return sum == RISC_TOTAL ? 0 : -1;
}

int risc_norm32(const u32 *freq, int nsyms, u16 *nf)
{
    u64 tot = 0;
    for (int i = 0; i < nsyms; i++) tot += freq[i];
    norm_any(tot, freq, nsyms, nf);
    u32 sum = 0;
    for (int i = 0; i < nsyms; i++) sum += nf[i];
    return sum == RISC_TOTAL ? 0 : -1;
}

/* LUT по слотам + исключающие префиксы для кодера */
void risc_build(RiscDec *d, const u16 *nf, int nsyms, u32 *cml)
{
    u32 c = 0;
    for (int s = 0; s < nsyms; s++) {
        cml[s] = c;
        for (u32 k = 0; k < nf[s]; k++) {
            d->sym[c + k] = (u16)s;
            d->fv[c + k] = nf[s];
            d->fc[c + k] = (u16)c;
        }
        c += nf[s];
    }
    for (u32 sl = c; sl < RISC_TOTAL; sl++) { d->sym[sl] = 0xff; d->fv[sl] = 0; d->fc[sl] = 0; }
}

/* таблица: на символ байт 0 = нет, 255 = u16 дальше, иначе частота */
size_t risc_table_store(const u16 *nf, int nsyms, u8 *dst, size_t cap)
{
    size_t o = 0;
    for (int i = 0; i < nsyms; i++) {
        if (o + 3 > cap) return (size_t)-1;
        u16 f = nf[i];
        if (!f) { dst[o++] = 0; }
        else if (f < 255) { dst[o++] = (u8)f; }
        else { dst[o] = 255; dst[o + 1] = (u8)f; dst[o + 2] = (u8)(f >> 8); o += 3; }
    }
    return o;
}

/* байт под таблицу без записи: 0 = нет, 255 = u16 дальше */
size_t risc_table_size(const u16 *nf, int nsyms)
{
    size_t o = 0;
    for (int i = 0; i < nsyms; i++) o += (nf[i] >= 255) ? 3 : 1;
    return o;
}

int risc_table_load(const u8 *src, size_t sz, u16 *nf, int nsyms, size_t *used)
{
    size_t i = 0;
    u32 sum = 0;
    for (int s = 0; s < nsyms; s++) {
        if (i >= sz) return -1;
        u8 b = src[i++];
        u32 f;
        if (!b) f = 0;
        else if (b < 255) f = b;
        else {
            if (i + 2 > sz) return -1;
            f = (u32)src[i] | (u32)src[i + 1] << 8;
            i += 2;
        }
        if (f > RISC_TOTAL) return -1;
        nf[s] = (u16)f;
        sum += f;
    }
    if (sum != RISC_TOTAL) return -1;
    *used = i;
    return 0;
}
