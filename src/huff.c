/* huff.c — построение деревьев, канонические коды, LUT на 9 бит */
#include <string.h>
#include "huff.h"
#include "rle.h"

static u32 revbits(u32 v, int n)
{
    u32 r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

/* канонические коды + LUT по готовым длинам */
static void hf_canon(HF *h, int nsyms)
{
    u32 code = 0, idx = 0, next[HLIM + 1];
    memset(h->ccount, 0, sizeof h->ccount);
    for (int s = 0; s < nsyms; s++) if (h->lens[s]) h->ccount[h->lens[s]]++;
    for (int l = 1; l <= HLIM; l++) {
        h->cfirst[l] = code;
        h->cindex[l] = idx;
        code = (code + h->ccount[l]) << 1;
        idx += h->ccount[l];
        next[l] = h->cfirst[l];
    }
    int i = 0;
    for (int l = 1; l <= HLIM; l++)
        for (int s = 0; s < nsyms; s++)
            if (h->lens[s] == l) { h->slist[i++] = (u16)s; h->codes[s] = (u16)next[l]++; }
    memset(h->lut_len, 0, sizeof h->lut_len);
    for (int s = 0; s < nsyms; s++) {
        int l = h->lens[s];
        if (!l || l > LUTB) continue;
        u32 rev = revbits(h->codes[s], l);
        int cnt = 1 << (LUTB - l);
        for (int j = 0; j < cnt; j++) {
            u32 id = rev | ((u32)j << l);
            h->lut_sym[id] = (u16)s;
            h->lut_len[id] = (u8)l;
        }
    }
}

int hf_from_lens(HF *h, const u8 *lens, int nsyms)
{
    memcpy(h->lens, lens, (size_t)nsyms);
    hf_canon(h, nsyms);
    return 0;
}

/* глубины через O(n^2) слияния; при выходе за HLIM частоты масштабируются */
static int hf_depths(const u16 *freq, int nsyms, u8 *lens)
{
    u32 f[2 * ALPH_MAX];
    int par[2 * ALPH_MAX];
    int sym[2 * ALPH_MAX];   /* лист -> символ */
    int act[ALPH_MAX];
    int na = 0, nn = 0;
    for (int s = 0; s < nsyms; s++) {
        if (!freq[s]) continue;
        f[nn] = freq[s]; par[nn] = -1; sym[nn] = s; act[na++] = nn; nn++;
    }
    if (na == 0) return -1;
    if (na == 1) { lens[sym[act[0]]] = 1; return 0; }
    int m = na;
    while (m > 1) {
        int i1 = -1, i2 = -1;
        for (int k = 0; k < m; k++) {
            if (i1 < 0 || f[act[k]] < f[act[i1]]) { i2 = i1; i1 = k; }
            else if (i2 < 0 || f[act[k]] < f[act[i2]]) i2 = k;
        }
        int a = act[i1], b = act[i2];
        f[nn] = f[a] + f[b];
        par[a] = par[b] = nn;
        par[nn] = -1;
        if (i1 == m - 1) { int t = i1; i1 = i2; i2 = t; }
        int tmp = act[m - 1];
        act[i1] = nn;
        act[i2] = tmp;
        m--; nn++;
    }
    int k = 0, maxd = 0;
    for (int s = 0; s < nsyms; s++) {
        if (!freq[s]) { lens[s] = 0; continue; }
        int d = 0;
        for (int j = k; par[j] >= 0; j = par[j]) d++;
        lens[s] = (u8)d;
        if (d > maxd) maxd = d;
        k++;
    }
    return maxd > HLIM ? -2 : 0;
}

int hf_from_freq(HF *h, const u16 *freq, int nsyms)
{
    u16 f[ALPH_MAX];
    memcpy(f, freq, sizeof(u16) * (size_t)nsyms);
    memset(h->lens, 0, sizeof h->lens);
    for (;;) {
        int rc = hf_depths(f, nsyms, h->lens);
        if (rc == 0) break;
        for (int s = 0; s < nsyms; s++) if (f[s]) f[s] = (u16)((f[s] + 1) >> 1);
    }
    hf_canon(h, nsyms);
    return 0;
}

int hf_dec(const HF *h, BR *b)
{
    u32 v = br_peek(b, LUTB);
    int l = h->lut_len[v];
    if (l) { br_drop(b, l); return h->lut_sym[v]; }
    u32 code = 0;
    for (int l2 = 1; l2 <= HLIM; l2++) {
        code = (code << 1) | br_bits(b, 1);
        u32 fc = h->cfirst[l2];
        if (h->ccount[l2] && code >= fc && code - fc < h->ccount[l2])
            return h->slist[h->cindex[l2] + (code - fc)];
    }
    return -1;
}

/* деревья: [u16 total][u8 flag][u16 rlen][payload] — payload жмётся нашим RLE */
void hf_store_trees(BW *b, const u8 *ll, const u8 *dl)
{
    u8 arr[TREE_BYTES], tmp[TREE_BYTES + TREE_BYTES / 127 + 4];
    memcpy(arr, ll, LITN_SYMS);
    memcpy(arr + LITN_SYMS, dl, DST_SLOTS);
    bw_byte(b, TREE_BYTES & 0xff);
    bw_byte(b, TREE_BYTES >> 8);
    size_t rs = rle_encode(arr, TREE_BYTES, tmp);
    if (rs < TREE_BYTES) {
        bw_byte(b, 1);
        bw_byte(b, (u8)rs);
        bw_byte(b, (u8)(rs >> 8));
        bw_raw(b, tmp, rs);
    } else {
        bw_byte(b, 0);
        bw_byte(b, TREE_BYTES & 0xff);
        bw_byte(b, TREE_BYTES >> 8);
        bw_raw(b, arr, TREE_BYTES);
    }
}

int hf_load_trees(BR *b, u8 *ll, u8 *dl)
{
    u8 arr[TREE_BYTES], tmp[TREE_BYTES + TREE_BYTES / 127 + 4];
    int total = (int)(br_bits(b, 8) | br_bits(b, 8) << 8);
    if (total != TREE_BYTES) return -1;
    u8 flag = (u8)br_bits(b, 8);
    size_t rs;
    if (flag == 0) {
        rs = (size_t)br_bits(b, 8) | (size_t)br_bits(b, 8) << 8;
        if (rs != TREE_BYTES) return -1;
        for (size_t i = 0; i < rs; i++) arr[i] = (u8)br_bits(b, 8);
    } else {
        rs = (size_t)br_bits(b, 8) | (size_t)br_bits(b, 8) << 8;
        if (rs > sizeof tmp) return -1;
        for (size_t i = 0; i < rs; i++) tmp[i] = (u8)br_bits(b, 8);
        size_t dc = rle_decode(tmp, rs, arr, TREE_BYTES);
        if (dc != TREE_BYTES) return -1;
    }
    memcpy(ll, arr, LITN_SYMS);
    memcpy(dl, arr + LITN_SYMS, DST_SLOTS);
    for (int i = 0; i < TREE_BYTES; i++) if (arr[i] > HLIM) return -1;
    return 0;
}
