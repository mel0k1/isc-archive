/* util.h — файловая мелочь */
#ifndef UTIL_H
#define UTIL_H
#include "iscf.h"

int  is_dir(const char *path);
int  mkdir_p(const char *path);
int  sane_name(char *name);                 /* нормализует имя записи, 0 = ok */
int  name_ok(const char *name);             /* проверка имени при распаковке */
int  collect_path(const char *path, char ***names, size_t *cnt, size_t *cap);
void human(u64 n, char *out, size_t cap);
u64  now_ms(void);
u64  rnd64(void);

#endif
