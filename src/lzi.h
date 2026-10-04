/* lzi.h — LZI: свой LZ с hash-цепочками и двумя пайплайнами (текст/бинарник) */
#ifndef LZI_H
#define LZI_H
#include "iscf.h"
#include "bitio.h"
#include "huff.h"

typedef struct {
    int chain;   /* длина цепочки */
    int nice;    /*_stop поиска при такой длине */
    int lazy;    /* пробовать совпадение со следующей позиции */
    int dyn;     /* динамические деревья (иначе статические) */
} LZP;

typedef struct {
    u32 *tl, *td;
    size_t n, cap;
    u16 fl[LITN_SYMS], fd[DST_SLOTS];
} LZT;

extern u16 LEN_BASE[LEN_SLOTS];
extern u32 DST_BASE[DST_SLOTS];
extern u8  LEN_EBITS[LEN_SLOTS], DST_EBITS[DST_SLOTS];

void lzi_init(void);
void lzi_params(int level, int text, LZP *p);
int  lzi_parse(const u8 *in, size_t n, const LZP *p, LZT *t,
               i32 *head, i32 *prev);
int  lzi_emit(const LZT *t, BW *b);
int  lzi_decode(const u8 *pay, size_t psz, u8 *out, size_t nraw);

#endif
