/* util.c — обход каталогов, имена, размеры, время */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include "util.h"

int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* mkdir уже существующего каталога — не ошибка */
static int errno_ok(void) { return errno != EEXIST; }

int mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t l = strlen(tmp);
    while (l > 1 && tmp[l - 1] == '/') tmp[--l] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; if (mkdir(tmp, 0755) && errno_ok()) return -1; *p = '/'; }
    }
    return mkdir(tmp, 0755) && errno_ok() ? -1 : 0;
}

int sane_name(char *name)
{
    for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
    char *s = name;
    while (*s == '/') s++;
    while (s[0] == '.' && s[1] == '/') s += 2;
    if (s != name) memmove(name, s, strlen(s) + 1);
    size_t l = strlen(name);
    while (l && name[l - 1] == '/') name[--l] = 0;
    if (!l) return -1;
    /* компоненты ".." запрещены */
    const char *p = name;
    while (p) {
        const char *q = strchr(p, '/');
        size_t cl = q ? (size_t)(q - p) : strlen(p);
        if (cl == 2 && p[0] == '.' && p[1] == '.') return -1;
        p = q ? q + 1 : NULL;
    }
    return 0;
}

int name_ok(const char *name)
{
    if (!name[0] || name[0] == '/' || strstr(name, "//")) return 0;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", name);
    return sane_name(tmp) == 0;
}

static void push(char ***names, size_t *cnt, size_t *cap, char *s)
{
    if (*cnt == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *names = realloc(*names, *cap * sizeof(char *));
        if (!*names) exit(2);
    }
    (*names)[(*cnt)++] = s;
}

int collect_path(const char *path, char ***names, size_t *cnt, size_t *cap)
{
    if (!is_dir(path)) {
        char *s = strdup(path);
        push(names, cnt, cap, s);
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[4096];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (is_dir(sub)) collect_path(sub, names, cnt, cap);
        else push(names, cnt, cap, strdup(sub));
    }
    closedir(d);
    /* сам каталог хранить не будем — имена файлов уже с префиксом */
    return 0;
}

void human(u64 n, char *out, size_t cap)
{
    const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int k = 0;
    while (v >= 1024 && k < 4) { v /= 1024; k++; }
    if (k) snprintf(out, cap, "%.2f %s", v, u[k]);
    else snprintf(out, cap, "%llu B", (unsigned long long)n);
}

u64 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

u64 rnd64(void)
{
    u64 v = (u64)time(NULL) * 0x9E3779B97F4A7C15ull ^ (u64)getpid() << 32;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { fread(&v, 8, 1, f); fclose(f); }
    return v;
}
