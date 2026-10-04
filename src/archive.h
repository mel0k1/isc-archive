/* archive.h — контейнер ISCF: сборка, чтение, списки, проверка, распаковка */
#ifndef ARCHIVE_H
#define ARCHIVE_H
#include "iscf.h"

typedef struct {
    char *name;
    u8 type, recipe, level;
    u64 raw, comp, fp;
    u32 nblocks, link;
    u32 *bcomp, *braw, *bsum;
    u8 *brec;
    u64 ent_off;
    u64 data_base;
} AEnt;

typedef struct {
    FILE *f;
    char path[4096];
    u32 nent, nblocks;
    u8 flags;
    u64 arch_id, raw_sum;
    AEnt *ents;
    u64 data_off;
} Arch;

int  arch_write(const char *out, char **paths, size_t npaths, int level, int mode);
int  arch_open(Arch *a, const char *path);
void arch_close(Arch *a);
int  arch_list(Arch *a);
int  arch_test(Arch *a, int jobs);
int  arch_extract(Arch *a, const char *dir, int jobs);

#endif
