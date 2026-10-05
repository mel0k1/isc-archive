/* lzi.h — LZI: свой LZ с hash-цепочками; LZ2 добавляет rep-дистанции */
#ifndef LZI_H
#define LZI_H
#include "iscf.h"
#include "bitio.h"
#include "huff.h"
#include "risc.h"

typedef struct {
    int chain;   /* длина цепочки */
    int nice;    /*_stop поиска при такой длине */
    int lazy;    /* пробовать совпадение со следующей позиции */
    int dyn;     /* динамические деревья (иначе статические) */
    int opt;     /* оптимальный парсинг (уровень 9) */
} LZP;

typedef struct {
    u32 *tl, *td;
    size_t n, cap;
    u16 fl[LITN_SYMS], fd[DST_SYMS];
} LZT;

extern u16 LEN_BASE[LEN_SLOTS];
extern u32 DST_BASE[DST_SLOTS];
extern u8  LEN_EBITS[LEN_SLOTS], DST_EBITS[DST_SLOTS];

void lzi_init(void);
void lzi_params(int level, int text, LZP *p);
int  lzi_parse(const u8 *in, size_t n, const LZP *p, LZT *t,
               i32 *head, i32 *prev, int reps);
int  lzi_parse_opt(const u8 *in, size_t n, const LZP *p, LZT *t,
                   const i32 *head, const i32 *prev);   /* оптимальный (-9) */
int  lzi_emit(const LZT *t, BW *b);            /* старый поток (рецепты 2/3/4) */
int  lzi_decode(const u8 *pay, size_t psz, u8 *out, size_t nraw);
int  lzi_emit2(const u8 *in, LZT *t, u8 *out, size_t cap, size_t *osize);  /* LZ2 */
int  lzi_decode2(const u8 *pay, size_t psz, u8 *out, size_t nraw);

#endif
