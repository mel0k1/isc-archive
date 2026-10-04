/* archive.c — сборка архива (с дедупом), чтение, проверка, распаковка */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "archive.h"
#include "isum.h"
#include "block.h"
#include "lzi.h"
#include "util.h"

#define FX_SIZE 36   /* фиксированная часть записи без esum */
#define FX_ALL  40   /* с esum */
#define PAY_CAP (2 * BLOCK_RAW + 65536)

/* ============================ запись ============================ */

static int wr_at(FILE *f, u64 off, const void *buf, size_t n)
{
    if (fseeko(f, (off_t)off, SEEK_SET)) return -1;
    return fwrite(buf, 1, n, f) == n ? 0 : -1;
}

typedef struct { u64 fp, sz; u32 idx; u8 used; } FPE;

static u64 fp_slot(const FPE *map, u32 cap, u64 fp, u64 sz)
{
    u64 h = (fp * 0x9E3779B97F4A7C15ull ^ sz) & (cap - 1);
    while (map[h].used && !(map[h].fp == fp && map[h].sz == sz)) h = (h + 1) & (cap - 1);
    return h;
}

static void patch_entry(FILE *f, u64 ent_off, size_t nlen, u8 type, u8 recipe, u8 level,
                        u64 raw, u64 comp, u64 fp, u32 nb, u32 link)
{
    u8 fx[FX_ALL] = { 0 };
    fx[0] = type; fx[1] = recipe; fx[2] = level;
    st64(fx + 4, raw);
    st64(fx + 12, comp);
    st64(fx + 20, fp);
    st32(fx + 28, nb);
    st32(fx + 32, link);
    st32(fx + 36, isum32(fx, FX_SIZE));
    wr_at(f, ent_off + 2 + nlen, fx, FX_ALL);
}

int arch_write(const char *out, char **paths, size_t npaths, int level, int mode)
{
    char **names = NULL;
    size_t cnt = 0, cap = 0;
    for (size_t i = 0; i < npaths; i++)
        if (collect_path(paths[i], &names, &cnt, &cap)) {
            fprintf(stderr, "isc: не открыть '%s'\n", paths[i]);
            return 2;
        }

    u64 *sizes = calloc(cnt ? cnt : 1, sizeof(u64));
    size_t keep = 0;
    for (size_t i = 0; i < cnt; i++) {
        char nm[4096];
        snprintf(nm, sizeof nm, "%s", names[i]);
        if (sane_name(nm) || !strcmp(nm, out)) { free(names[i]); continue; }
        struct stat st;
        if (stat(names[i], &st) || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "isc: пропускаю '%s'\n", names[i]);
            free(names[i]);
            continue;
        }
        sizes[keep] = (u64)st.st_size;
        free(names[keep]);
        names[keep] = strdup(nm);
        keep++;
    }
    cnt = keep;

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "isc: не создать '%s'\n", out); return 2; }

    /* буферы кодека */
    u8 *in = malloc(BLOCK_RAW), *outb = malloc(PAY_CAP), *dbuf = malloc(BLOCK_RAW);
    i32 *head = malloc((size_t)(1 << 16) * sizeof(i32));
    i32 *prev = malloc((size_t)BLOCK_RAW * sizeof(i32));
    u32 *tl = malloc((size_t)BLOCK_RAW * sizeof(u32));
    u32 *td = malloc((size_t)BLOCK_RAW * sizeof(u32));
    if (!in || !outb || !head || !prev || !tl || !td || !dbuf) return 2;
    lzi_init();

    u32 mcap = 16;
    while (mcap < (u32)(cnt + 1) * 2) mcap <<= 1;
    FPE *map = calloc(mcap, sizeof(FPE));
    u64 *fps = calloc(cnt ? cnt : 1, sizeof(u64));
    u32 *link_to = calloc(cnt ? cnt : 1, sizeof(u32));   /* 0 = не ссылка, иначе индекс цели + 1 */

    /* проход 1: отпечатки и дедуп — до записи, чтобы точно знать число блоков */
    u64 nb_est = 0;
    for (size_t i = 0; i < cnt; i++) {
        FILE *src = fopen(names[i], "rb");
        if (!src) { fprintf(stderr, "isc: пропал '%s'\n", names[i]); continue; }
        u64 fp = 0x1CA7C0DEDEC0DE17ull;
        size_t rn;
        while ((rn = fread(in, 1, BLOCK_RAW, src)) > 0) fp = isum64_u(fp, in, rn);
        fclose(src);
        fps[i] = fp;
        u64 slot = fp_slot(map, mcap, fp, sizes[i]);
        if (map[slot].used) { link_to[i] = map[slot].idx + 1; continue; }
        map[slot].used = 1; map[slot].fp = fp; map[slot].sz = sizes[i]; map[slot].idx = (u32)i;
        nb_est += (sizes[i] + BLOCK_RAW - 1) / BLOCK_RAW;
    }

    u8 flags = 0;
    for (size_t i = 0; i < cnt; i++) if (link_to[i]) flags |= 1;

    u8 hdr[HDR_SIZE] = { 0 };
    memcpy(hdr, ISCF_MAGIC, 4);
    hdr[4] = ISCF_VERSION;
    hdr[5] = flags;
    st32(hdr + 8, (u32)cnt);
    st32(hdr + 12, (u32)nb_est);
    st64(hdr + 16, rnd64());
    st32(hdr + 24, isum32(hdr, 24));
    fwrite(hdr, 1, HDR_SIZE, f);

    u64 *ent_off = calloc(cnt ? cnt : 1, sizeof(u64));
    size_t *nlen = calloc(cnt ? cnt : 1, sizeof(size_t));
    for (size_t i = 0; i < cnt; i++) {
        ent_off[i] = (u64)ftello(f);
        nlen[i] = strlen(names[i]);
        u8 b[2] = { (u8)nlen[i], (u8)(nlen[i] >> 8) };
        fwrite(b, 1, 2, f);
        fwrite(names[i], 1, nlen[i], f);
        u8 fx[FX_ALL] = { 0 };
        fwrite(fx, 1, FX_ALL, f);
    }
    u64 index_off = (u64)ftello(f);
    u8 zero[16] = { 0 };
    for (u64 i = 0; i < nb_est; i++) fwrite(zero, 1, 16, f);
    u64 data_off = (u64)ftello(f);

    u64 raw_sum = 0, data_pos = data_off;
    u32 gblock = 0;

    /* проход 2: сжимаем уникальные, пишем блоки */
    for (size_t i = 0; i < cnt; i++) {
        if (link_to[i]) {
            patch_entry(f, ent_off[i], nlen[i], T_LINK, 0, (u8)level,
                        sizes[i], 0, fps[i], 0, link_to[i] - 1);
            continue;
        }
        FILE *src = fopen(names[i], "rb");
        if (!src) continue;
        u64 dstart = data_pos;
        u32 nb = 0;
        u8 rec_first = R_STORE;
        size_t rn;
        while ((rn = fread(in, 1, BLOCK_RAW, src)) > 0) {
            if (gblock >= nb_est) { fprintf(stderr, "isc: файл изменился при упаковке\n"); return 2; }
            u8 rec = 0;
            size_t psz = block_encode(in, rn, level, mode, outb, PAY_CAP,
                                      &rec, head, prev, tl, td, dbuf);
            if (wr_at(f, data_pos, outb, psz)) return 2;   /* по смещению: патчи сбивают позицию потока */
            u8 bi[16];
            st32(bi, (u32)psz);
            st32(bi + 4, (u32)rn);
            st32(bi + 8, isum32(in, rn));
            bi[12] = rec;
            wr_at(f, index_off + (u64)gblock * 16, bi, 16);
            if (nb == 0) rec_first = rec;
            gblock++;
            nb++;
            raw_sum = isum32_u(raw_sum, in, rn);
            data_pos += psz;
        }
        fclose(src);
        patch_entry(f, ent_off[i], nlen[i], T_FILE, rec_first, (u8)level,
                    sizes[i], data_pos - dstart, fps[i], nb, 0);
    }

    u8 ftr[FTR_SIZE] = { 0 };
    memcpy(ftr, "FSCI", 4);
    st32(ftr + 4, (u32)raw_sum);
    st32(ftr + 12, isum32(ftr, 12));
    if (wr_at(f, data_pos, ftr, FTR_SIZE)) return 2;

    fclose(f);
    return 0;
}

/* ============================ чтение ============================ */

static int rd_exact(FILE *f, void *buf, size_t n)
{
    return fread(buf, 1, n, f) == n ? 0 : -1;
}

int arch_open(Arch *a, const char *path)
{
    memset(a, 0, sizeof *a);
    snprintf(a->path, sizeof a->path, "%s", path);
    a->f = fopen(path, "rb");
    if (!a->f) return -1;
    u8 hdr[HDR_SIZE];
    if (rd_exact(a->f, hdr, HDR_SIZE) || memcmp(hdr, ISCF_MAGIC, 4) || hdr[4] != ISCF_VERSION) {
        fprintf(stderr, "isc: это не ISCF v1 архив\n");
        return -1;
    }
    if (le32(hdr + 24) != isum32(hdr, 24)) {
        fprintf(stderr, "isc: битый заголовок\n");
        return -1;
    }
    a->nent = le32(hdr + 8);
    a->nblocks = le32(hdr + 12);
    a->flags = hdr[5];
    a->arch_id = le64(hdr + 16);
    a->ents = calloc(a->nent ? a->nent : 1, sizeof(AEnt));
    u32 nb_sum = 0;
    for (u32 i = 0; i < a->nent; i++) {
        AEnt *e = &a->ents[i];
        u8 b[2];
        if (rd_exact(a->f, b, 2)) return -1;
        size_t nl = le16(b);
        if (nl > 4095) return -1;
        e->name = malloc(nl + 1);
        if (rd_exact(a->f, e->name, nl)) return -1;
        e->name[nl] = 0;
        if (!name_ok(e->name)) { fprintf(stderr, "isc: опасное имя '%s'\n", e->name); return -1; }
        u8 fx[FX_ALL];
        if (rd_exact(a->f, fx, FX_ALL)) return -1;
        if (le32(fx + 36) != isum32(fx, FX_SIZE)) {
            fprintf(stderr, "isc: битая запись #%u\n", i);
            return -1;
        }
        e->type = fx[0]; e->recipe = fx[1]; e->level = fx[2];
        e->raw = le64(fx + 4);
        e->comp = le64(fx + 12);
        e->fp = le64(fx + 20);
        e->nblocks = le32(fx + 28);
        e->link = le32(fx + 32);
        if (e->type == T_FILE) {
            e->bcomp = malloc((size_t)e->nblocks * 4 + 4);
            e->braw = malloc((size_t)e->nblocks * 4 + 4);
            e->bsum = malloc((size_t)e->nblocks * 4 + 4);
            e->brec = malloc((size_t)e->nblocks + 4);
            nb_sum += e->nblocks;
        }
    }
    if (nb_sum != a->nblocks) {
        fprintf(stderr, "isc: индекс не сходится\n");
        return -1;
    }
    for (u32 i = 0; i < a->nent; i++) {
        AEnt *e = &a->ents[i];
        for (u32 b = 0; b < e->nblocks; b++) {
            u8 bi[16];
            if (rd_exact(a->f, bi, 16)) return -1;
            e->bcomp[b] = le32(bi);
            e->braw[b] = le32(bi + 4);
            e->bsum[b] = le32(bi + 8);
            e->brec[b] = bi[12];
            if (e->bcomp[b] > PAY_CAP || e->braw[b] > BLOCK_RAW) {
                fprintf(stderr, "isc: мусор в индексе\n");
                return -1;
            }
        }
    }
    a->data_off = (u64)ftello(a->f);
    u64 cur = a->data_off;
    for (u32 i = 0; i < a->nent; i++) {
        a->ents[i].data_base = cur;
        cur += a->ents[i].comp;
    }
    /* подвал */
    u8 ftr[FTR_SIZE];
    if (fseeko(a->f, 0, SEEK_END)) return -1;
    u64 fsz = (u64)ftello(a->f);
    if (fsz < HDR_SIZE + FTR_SIZE || fseeko(a->f, (off_t)(fsz - FTR_SIZE), SEEK_SET)) return -1;
    if (rd_exact(a->f, ftr, FTR_SIZE)) return -1;
    if (memcmp(ftr, "FSCI", 4) || le32(ftr + 12) != isum32(ftr, 12)) {
        fprintf(stderr, "isc: битый подвал — архив обрезан?\n");
        return -1;
    }
    a->raw_sum = le32(ftr + 4);
    return 0;
}

void arch_close(Arch *a)
{
    if (a->f) fclose(a->f);
    for (u32 i = 0; i < a->nent; i++) {
        free(a->ents[i].name);
        free(a->ents[i].bcomp);
        free(a->ents[i].braw);
        free(a->ents[i].bsum);
        free(a->ents[i].brec);
    }
    free(a->ents);
    memset(a, 0, sizeof *a);
}

/* ======================= параллельный конвейер ======================= */

#include <pthread.h>
#include <unistd.h>

/* задание: один блок одной записи */
typedef struct {
    u32 ent, blk;        /* для сообщений об ошибках */
    u32 psz, rn, sum, rec;
    u64 aoff;            /* payload в архиве */
} Job;

/* мини-семафор: sem_t не переносим (macOS) */
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int v; } Ssem;

static void ssem_init(Ssem *s, int v)
{
    pthread_mutex_init(&s->mu, 0);
    pthread_cond_init(&s->cv, 0);
    s->v = v;
}
static void ssem_kill(Ssem *s)
{
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
}
static void ssem_wait(Ssem *s)
{
    pthread_mutex_lock(&s->mu);
    while (!s->v) pthread_cond_wait(&s->cv, &s->mu);
    s->v--;
    pthread_mutex_unlock(&s->mu);
}
static void ssem_post(Ssem *s)
{
    pthread_mutex_lock(&s->mu);
    s->v++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

/* слоты: воркер t владеет слотом t и обрабатывает свои задания (t, t+nslots, ...)
   строго по возрастанию — главный забирает блоки в порядке заданий (isum32_u
   некоммутативен), а fslot[t] не даёт воркеру обогнать чтение результата */
typedef struct {
    Arch *a;
    Job *jobs;
    u32 njobs, nslots;
    u8 *spay, *sdec;     /* буферы payload и декода на слот */
    u32 *ssum, *sjid;    /* чексум декода и номер задания в слоте */
    u8 *serr;
    Ssem *fslot, *ready; /* per-slot: свободен / готов */
    int stop;
} Par;

typedef struct { Par *p; u32 t; } Warg;

static void *par_worker(void *arg)
{
    Warg *w = arg;
    Par *p = w->p;
    int fd = fileno(p->a->f);
    u32 t = w->t;
    u8 *pay = p->spay + (size_t)t * PAY_CAP;
    u8 *dec = p->sdec + (size_t)t * BLOCK_RAW;
    for (u32 i = t; i < p->njobs; i += p->nslots) {
        if (__atomic_load_n(&p->stop, __ATOMIC_RELAXED)) break;
        Job *j = &p->jobs[i];
        ssem_wait(&p->fslot[t]);
        if (__atomic_load_n(&p->stop, __ATOMIC_RELAXED)) { ssem_post(&p->fslot[t]); break; }
        int err = pread(fd, pay, j->psz, (off_t)j->aoff) != (ssize_t)j->psz;
        if (!err && block_decode(pay, j->psz, j->rec, dec, j->rn)) err = 1;
        else if (!err) p->ssum[t] = isum32(dec, j->rn);
        p->sjid[t] = i;
        p->serr[t] = (u8)err;
        ssem_post(&p->ready[t]);
    }
    return NULL;
}

static int copy_file(const char *dst, const char *src)
{
    FILE *a = fopen(src, "rb"), *b = fopen(dst, "wb");
    if (!a || !b) { if (a) fclose(a); if (b) fclose(b); return -1; }
    u8 buf[65536];
    size_t rn;
    while ((rn = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, rn, b) != rn) break;
    int rc = ferror(a) || ferror(b) ? -1 : 0;
    fclose(a);
    if (fclose(b)) rc = -1;
    return rc;
}

static void parent_mkdir(const char *path)
{
    const char *q = strrchr(path, '/');
    if (!q) return;
    size_t l = (size_t)(q - path);
    char *d = malloc(l + 1);
    memcpy(d, path, l);
    d[l] = 0;
    mkdir_p(d);
    free(d);
}

/* все блоки через пул; writeout = 0 — только проверка (используется isc t) */
static int par_pass(Arch *a, const char *dir, int jobs, int writeout, u64 *raw_sum)
{
    u32 njobs = 0;
    for (u32 i = 0; i < a->nent; i++)
        if (a->ents[i].type == T_FILE) njobs += a->ents[i].nblocks;

    int rc = 0;
    u64 *edone = calloc(a->nent ? a->nent : 1, sizeof(u64));
    Job *jl = malloc((njobs ? njobs : 1) * sizeof(Job));
    if (!edone || !jl) { free(edone); free(jl); return 2; }

    u32 k = 0;
    for (u32 i = 0; i < a->nent; i++) {
        AEnt *e = &a->ents[i];
        if (e->type != T_FILE) continue;
        u64 aoff = e->data_base;
        for (u32 b = 0; b < e->nblocks; b++) {
            jl[k].ent = i;
            jl[k].blk = b;
            jl[k].psz = e->bcomp[b];
            jl[k].rn = e->braw[b];
            jl[k].sum = e->bsum[b];
            jl[k].rec = e->brec[b];
            jl[k].aoff = aoff;
            aoff += e->bcomp[b];
            k++;
        }
    }

    if (jobs < 1) jobs = 1;
    u32 nwork = (u32)jobs;
    if (nwork > njobs) nwork = njobs;
    if (nwork < 1) nwork = 1;
    u32 nslots = nwork;

    Par p;
    memset(&p, 0, sizeof p);
    p.a = a;
    p.jobs = jl;
    p.njobs = njobs;
    p.nslots = nslots;
    p.spay = malloc((size_t)nslots * PAY_CAP);
    p.sdec = malloc((size_t)nslots * BLOCK_RAW);
    p.ssum = calloc(nslots, sizeof(u32));
    p.sjid = calloc(nslots, sizeof(u32));
    p.serr = calloc(nslots, 1);
    p.ready = malloc(nslots * sizeof(Ssem));
    p.fslot = malloc(nslots * sizeof(Ssem));
    if (!p.spay || !p.sdec || !p.ssum || !p.sjid || !p.serr || !p.ready || !p.fslot) {
        free(p.spay); free(p.sdec); free(p.ssum); free(p.sjid); free(p.serr); free(p.ready); free(p.fslot);
        free(edone); free(jl);
        return 2;
    }
    for (u32 s = 0; s < nslots; s++) {
        ssem_init(&p.fslot[s], 1);   /* слот свободен */
        ssem_init(&p.ready[s], 0);
    }

    pthread_t *th = calloc(nwork ? nwork : 1, sizeof(pthread_t));
    Warg *wa = calloc(nwork ? nwork : 1, sizeof(Warg));
    u32 started = 0;
    for (u32 t = 0; t < nwork; t++) {
        wa[t].p = &p;
        wa[t].t = t;
        if (pthread_create(&th[t], 0, par_worker, &wa[t])) break;
        started++;
    }
    if (!started)
        for (u32 t = 0; t < nwork; t++) {   /* потоков нет — декодируем прямо тут */
            Warg w = { &p, t };
            par_worker(&w);
        }

    /* главный поток: пишет блоки по порядку и ведёт сквозной isum */
    FILE *o = NULL;
    u32 cur = 0xFFFFFFFFu;
    for (u32 i = 0; i < njobs; i++) {
        Job *j = &jl[i];
        if (writeout && j->ent != cur) {
            if (o) {
                fclose(o);
                printf("распаковано: %s\n", a->ents[cur].name);
            }
            cur = j->ent;
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", dir, a->ents[cur].name);
            parent_mkdir(path);
            o = fopen(path, "wb");
            if (!o) {
                fprintf(stderr, "isc: не создать '%s'\n", path);
                rc = 2;
                break;
            }
        }
        u32 t = i % nslots;
        ssem_wait(&p.ready[t]);
        if (p.sjid[t] != i) {   /* рассинхрон слотов — внутренней ошибки быть не должно */
            rc = 2;
            ssem_post(&p.fslot[t]);
            break;
        }
        if (p.serr[t]) {
            fprintf(stderr, "isc: блок %u записи '%s' не декодируется\n", j->blk, a->ents[j->ent].name);
            rc = 3;
        } else if (p.ssum[t] != j->sum) {
            fprintf(stderr, "isc: isum не сошёлся в блоке %u записи '%s'\n", j->blk, a->ents[j->ent].name);
            rc = 3;
        } else {
            u8 *dec = p.sdec + (size_t)t * BLOCK_RAW;
            if (writeout && fwrite(dec, 1, j->rn, o) != j->rn) {
                fprintf(stderr, "isc: не записать '%s'\n", a->ents[j->ent].name);
                rc = 2;
            } else {
                *raw_sum = isum32_u(*raw_sum, dec, j->rn);
                edone[j->ent] += j->rn;
            }
        }
        ssem_post(&p.fslot[t]);
        if (rc) break;
    }
    if (writeout && o) {
        fclose(o);
        printf("распаковано: %s\n", a->ents[cur].name);
    }
    if (rc) {
        p.stop = 1;
        /* разбудить воркеров, висящих на своих слотах */
        for (u32 s = 0; s < nslots; s++) ssem_post(&p.fslot[s]);
    }

    for (u32 t = 0; t < started; t++) pthread_join(th[t], 0);
    free(th);
    for (u32 s = 0; s < nslots; s++) {
        ssem_kill(&p.fslot[s]);
        ssem_kill(&p.ready[s]);
    }
    free(p.spay); free(p.sdec); free(p.ssum); free(p.sjid); free(p.serr); free(p.ready); free(p.fslot);

    for (u32 i = 0; i < a->nent && !rc; i++)
        if (a->ents[i].type == T_FILE && edone[i] != a->ents[i].raw) {
            fprintf(stderr, "isc: запись '%s' распалась\n", a->ents[i].name);
            rc = 3;
        }
    free(edone);
    free(jl);
    return rc;
}

int arch_test(Arch *a, int jobs)
{
    int rc = 0;
    for (u32 i = 0; i < a->nent && !rc; i++) {
        AEnt *e = &a->ents[i];
        if (e->type == T_FILE) {
            u64 comp_sum = 0;
            for (u32 b = 0; b < e->nblocks; b++) comp_sum += e->bcomp[b];
            if (comp_sum != e->comp) {
                fprintf(stderr, "isc: индекс записи '%s' не сходится\n", e->name);
                rc = 3;
            }
        } else if (e->type == T_LINK) {
            if (e->link >= i || a->ents[e->link].type != T_FILE ||
                a->ents[e->link].fp != e->fp || a->ents[e->link].raw != e->raw) {
                fprintf(stderr, "isc: битая ссылка #%u\n", i);
                rc = 3;
            }
        }
    }
    u64 raw = 0;
    if (!rc) rc = par_pass(a, 0, jobs, 0, &raw);
    if (!rc && raw != a->raw_sum) {
        fprintf(stderr, "isc: общая сумма не сошлась\n");
        rc = 3;
    }
    if (!rc) printf("OK: %u записей, блоки целы\n", a->nent);
    return rc;
}

int arch_extract(Arch *a, const char *dir, int jobs)
{
    mkdir_p(dir);
    for (u32 i = 0; i < a->nent; i++)
        if (a->ents[i].type == T_DIR) {
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", dir, a->ents[i].name);
            mkdir_p(path);
        }
    /* пустые файлы: блоков нет — создаём напрямую */
    for (u32 i = 0; i < a->nent; i++) {
        AEnt *e = &a->ents[i];
        if (e->type != T_FILE || e->nblocks) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->name);
        parent_mkdir(path);
        FILE *o = fopen(path, "wb");
        if (!o) {
            fprintf(stderr, "isc: не создать '%s'\n", path);
            return 2;
        }
        fclose(o);
        printf("распаковано: %s\n", e->name);
    }
    u64 raw = 0;
    int rc = par_pass(a, dir, jobs, 1, &raw);
    if (rc) return rc;

    /* дедуп-ссылки: файл-оригинал уже распакован — копируем */
    for (u32 i = 0; i < a->nent && !rc; i++) {
        AEnt *e = &a->ents[i];
        if (e->type != T_LINK) continue;
        char dst[4096], src[4096];
        snprintf(dst, sizeof dst, "%s/%s", dir, e->name);
        snprintf(src, sizeof src, "%s/%s", dir, a->ents[e->link].name);
        parent_mkdir(dst);
        if (copy_file(dst, src)) {
            fprintf(stderr, "isc: не создать '%s'\n", dst);
            rc = 2;
            break;
        }
        printf("распаковано: %s\n", e->name);
    }
    return rc;
}
