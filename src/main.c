/* main.c — isc: утилита для формата ISCF (.isc) */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE   /* иначе мак спрячет _SC_NPROCESSORS_ONLN */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "archive.h"
#include "block.h"
#include "lzi.h"
#include "util.h"

static void usage(void)
{
    puts(
        "isc — Its So Cool archiver (ISCF v1)\n"
        "\n"
        "  isc a [-1..-9] [-m auto|store|lz] <архив.isc> <путь>...\n"
        "  isc x [-j N] <архив.isc> [куда]\n"
        "  isc l <архив.isc>\n"
        "  isc t [-j N] <архив.isc>\n"
        "  isc i <архив.isc>\n"
        "\n"
        "  -1..-9     уровень сжатия (по умолчанию 6)\n"
        "  -m auto    маршрутизатор рецептов (по умолчанию)\n"
        "  -m store   всё хранить без сжатия\n"
        "  -m lz      без RLE и дельты, только LZI/store\n"
        "  -j N       потоки распаковки (по умолчанию — все ядра)");
}

static int parse_level(const char *s, int *level)
{
    if (s[0] != '-' || s[1] < '1' || s[1] > '9' || s[2]) return -1;
    *level = s[1] - '0';
    return 0;
}

/* -j N, -jN; 0 = авто (все ядра) */
static int parse_jobs(int argc, char **argv, int *i, int *jobs)
{
    const char *s = argv[*i];
    if (strncmp(s, "-j", 2)) return -1;
    if (s[2]) {
        int v = 0;
        for (const char *p = s + 2; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            v = v * 10 + (*p - '0');
            if (v > 256) return -1;
        }
        *jobs = v;
        return 0;
    }
    if (*i + 1 >= argc) return -1;
    const char *n = argv[++(*i)];
    int v = 0;
    for (const char *p = n; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > 256) return -1;
    }
    *jobs = v;
    return 0;
}

static int auto_jobs(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return (int)n;
}

static const char *recipe_name(u8 r)
{
    switch (r) {
    case R_STORE: return "store";
    case R_RLE: return "rle";
    case R_LZT: return "lz-text";
    case R_LZB: return "lz-bin";
    case R_DLT: return "delta";
    case R_LZT2: return "lz2-text";
    case R_LZB2: return "lz2-bin";
    case R_DLT2: return "lz2-delta";
    }
    return "?";
}

static int cmd_add(int argc, char **argv)
{
    int level = 6, mode = M_AUTO;
    int i = 0;
    for (; i < argc; i++) {
        if (!parse_level(argv[i], &level)) continue;
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "auto")) mode = M_AUTO;
            else if (!strcmp(m, "store")) mode = M_STORE;
            else if (!strcmp(m, "lz")) mode = M_LZ;
            else { fprintf(stderr, "isc: неизвестный режим '%s'\n", m); return 1; }
            continue;
        }
        break;
    }
    if (argc - i < 2) { usage(); return 1; }
    return arch_write(argv[i], argv + i + 1, (size_t)(argc - i - 1), level, mode);
}

static int cmd_x(int argc, char **argv)
{
    int jobs = 0, i = 0;
    for (; i < argc; i++) {
        if (!parse_jobs(argc, argv, &i, &jobs)) continue;
        break;
    }
    if (argc - i < 1) { usage(); return 1; }
    Arch a;
    if (arch_open(&a, argv[i])) return 2;
    int rc = arch_extract(&a, i + 1 < argc ? argv[i + 1] : ".", jobs ? jobs : auto_jobs());
    arch_close(&a);
    return rc;
}

static int cmd_l(int argc, char **argv)
{
    if (argc < 1) { usage(); return 1; }
    Arch a;
    if (arch_open(&a, argv[0])) return 2;
    printf("%4s %2s %-7s %12s %12s  %s\n", "#", "тип", "рецепт", "сырой", "сжатый", "имя");
    for (u32 i = 0; i < a.nent; i++) {
        AEnt *e = &a.ents[i];
        char rs[16], cs[16];
        human(e->raw, rs, sizeof rs);
        human(e->comp, cs, sizeof cs);
        char extra[64] = "";
        if (e->type == T_LINK) snprintf(extra, sizeof extra, " -> #%u", e->link);
        printf("%4u %2s %-7s %12s %12s  %s%s\n",
               i, e->type == T_LINK ? "l" : "f", recipe_name(e->recipe),
               rs, cs, e->name, extra);
    }
    arch_close(&a);
    return 0;
}

static int cmd_t(int argc, char **argv)
{
    int jobs = 0, i = 0;
    for (; i < argc; i++) {
        if (!parse_jobs(argc, argv, &i, &jobs)) continue;
        break;
    }
    if (argc - i < 1) { usage(); return 1; }
    Arch a;
    if (arch_open(&a, argv[i])) return 2;
    int rc = arch_test(&a, jobs ? jobs : auto_jobs());
    arch_close(&a);
    return rc;
}

static int cmd_i(int argc, char **argv)
{
    if (argc < 1) { usage(); return 1; }
    Arch a;
    if (arch_open(&a, argv[0])) return 2;
    u64 raw = 0, comp = 0;
    u32 files = 0, dirs = 0, links = 0;
    u32 rec_cnt[8] = { 0 };
    for (u32 i = 0; i < a.nent; i++) {
        AEnt *e = &a.ents[i];
        if (e->type == T_FILE) { files++; raw += e->raw; comp += e->comp; rec_cnt[e->recipe & 7]++; }
        else if (e->type == T_LINK) links++;
        else dirs++;
    }
    char raws[16], comps[16];
    human(raw, raws, sizeof raws);
    human(comp, comps, sizeof comps);
    printf("формат:      ISCF v%d\n", ISCF_VERSION);
    printf("архив id:    %016llx\n", (unsigned long long)a.arch_id);
    printf("записей:     %u (файлов %u, ссылок %u, каталогов %u)\n", a.nent, files, links, dirs);
    printf("блоков:      %u по %u КиБ\n", a.nblocks, BLOCK_RAW / 1024);
    printf("сырой объём: %s\n", raws);
    printf("сжатый:      %s\n", comps);
    if (raw) printf("степень:     %.3fx (%.1f%% от исходного)\n", (double)raw / (double)comp, 100.0 * (double)comp / (double)raw);
    printf("рецепты:     store %u, rle %u, lz %u/%u, delta %u/%u\n",
           rec_cnt[R_STORE], rec_cnt[R_RLE], rec_cnt[R_LZT], rec_cnt[R_LZB],
           rec_cnt[R_DLT], rec_cnt[R_DLT2]);
    printf("lz2:         text %u, bin %u, delta %u\n",
           rec_cnt[R_LZT2], rec_cnt[R_LZB2], rec_cnt[R_DLT2]);
    printf("дедуп:       %s\n", (a.flags & 1) ? "есть" : "нет");
    arch_close(&a);
    return 0;
}

int main(int argc, char **argv)
{
    lzi_init();
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "a") || !strcmp(cmd, "add")) return cmd_add(argc - 2, argv + 2);
    if (!strcmp(cmd, "x") || !strcmp(cmd, "extract")) return cmd_x(argc - 2, argv + 2);
    if (!strcmp(cmd, "l") || !strcmp(cmd, "list")) return cmd_l(argc - 2, argv + 2);
    if (!strcmp(cmd, "t") || !strcmp(cmd, "test")) return cmd_t(argc - 2, argv + 2);
    if (!strcmp(cmd, "i") || !strcmp(cmd, "info")) return cmd_i(argc - 2, argv + 2);
    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(); return 0; }
    fprintf(stderr, "isc: неизвестная команда '%s'\n", cmd);
    usage();
    return 1;
}
