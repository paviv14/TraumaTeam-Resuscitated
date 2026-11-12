// Disc stand-in: serves the drive's read commands from an extracted disc folder
// (Dolphin "Extract Entire Disc" layout: <root>/sys/{boot.bin,bi2.bin,apploader.img,main.dol,fst.bin},
// <root>/files/...), and loads the file table into memory like the apploader does.
//
// FST entry (12 bytes): u8 is_dir, u24 name offset, u32 file offset >> 2 (Wii) / parent, u32 size / next.
// Drive read offsets are also in 4-byte units, relative to the game partition.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u64 off, size; char path[256]; int logged; } DiscFile;
static DiscFile *g_files; static int g_nfiles;
static char g_root[512];
static int g_log = 1;

static u8 *read_all(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(*n); if (fread(d, 1, *n, f) != (size_t)*n) { free(d); fclose(f); return NULL; } fclose(f); return d;
}
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void add(u64 off, u64 size, const char *path) {
    g_files = realloc(g_files, (g_nfiles + 1) * sizeof *g_files);
    g_files[g_nfiles].off = off; g_files[g_nfiles].size = size;
    snprintf(g_files[g_nfiles].path, sizeof g_files[g_nfiles].path, "%s", path); g_nfiles++;
}

// Walk the FST, recording the host path of every file (recursive over directory ranges).
static void walk(const u8 *fst, u32 n, const char *names, u32 *i, u32 end, const char *dir) {
    while (*i < end) {
        const u8 *e = fst + 12 * *i;
        char p[512]; snprintf(p, sizeof p, "%s/%s", dir, names + (be32(e) & 0xFFFFFF));
        if (e[0]) { u32 next = be32(e + 8); (*i)++; walk(fst, n, names, i, next, p); }
        else { add((u64)be32(e + 4) << 2, be32(e + 8), p); (*i)++; }
    }
}

// Load fst.bin into the top of MEM1 and point the OS at it (0x80000038/0x8000003C), as the apploader does.
void disc_init(const char *root) {
    snprintf(g_root, sizeof g_root, "%s", root);
    char p[600]; long n;
    snprintf(p, sizeof p, "%s/sys/fst.bin", root);
    u8 *fst = read_all(p, &n);
    if (!fst) { fprintf(stderr, "[disc] no %s - disc access disabled\n", p); return; }
    u32 count = be32(fst + 8);
    u32 i = 1; walk(fst, count, (const char *)fst + 12 * count, &i, count, "files");
    // system area files at their fixed positions
    long bn; snprintf(p, sizeof p, "%s/sys/boot.bin", root); u8 *boot = read_all(p, &bn);
    add(0, 0x440, "sys/boot.bin"); add(0x440, 0x2000, "sys/bi2.bin"); add(0x2440, 0x40000, "sys/apploader.img");
    if (boot) { add((u64)be32(boot + 0x420) << 2, 0x500000, "sys/main.dol"); add((u64)be32(boot + 0x424) << 2, n, "sys/fst.bin"); free(boot); }
    u32 at = (0x81800000u - (u32)((n + 31) & ~31)) & ~31u;
    for (long k = 0; k < n; k++) W8(at + k, fst[k]);
    W32(0x80000038, at); W32(0x8000003C, (u32)n);
    if (R32(0x80000034) > at) W32(0x80000034, at);   // arena must end below the FST
    free(fst);
    fprintf(stderr, "[disc] %d files, FST at %08X (%ld bytes), root %s\n", g_nfiles, at, n, root);
}

// ---------------------------------------------------------------- reading
// Files stay open (a few at a time): opening a file per read is slow on Windows, where every open can
// also go through the antivirus. Reads can run on a worker thread (disc_read_async) so a slow first
// read from the host disk stalls only the game's loading, not the frame loop.
#include <pthread.h>
static pthread_mutex_t g_io = PTHREAD_MUTEX_INITIALIZER;
typedef struct { DiscFile *f; FILE *h; u64 used; } Open;
static Open g_open[16]; static u64 g_use;
static FILE *open_file(DiscFile *f) {   // caller holds g_io
    Open *slot = &g_open[0];
    for (int k = 0; k < 16; k++) {
        if (g_open[k].f == f && g_open[k].h) { g_open[k].used = ++g_use; return g_open[k].h; }
        if (g_open[k].used < slot->used) slot = &g_open[k];
    }
    if (slot->h) fclose(slot->h);
    char p[800]; snprintf(p, sizeof p, "%s/%s", g_root, f->path);
    slot->f = f; slot->h = fopen(p, "rb"); slot->used = ++g_use;
    return slot->h;
}
static DiscFile *find_file(u64 off) {   // g_files is sorted by offset
    int lo = 0, hi = g_nfiles - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2; DiscFile *f = &g_files[mid];
        if (off < f->off) hi = mid - 1; else if (off >= f->off + f->size) lo = mid + 1; else return f;
    }
    return NULL;
}
static int cmp_off(const void *a, const void *b) { u64 x = ((const DiscFile *)a)->off, y = ((const DiscFile *)b)->off; return x < y ? -1 : x > y; }

// Read into a host buffer. Returns 1 (drive success) or 2 (a file is missing).
static int read_host(u64 off, u32 len, u8 *buf) {
    static int sorted; if (!sorted) { sorted = 1; qsort(g_files, g_nfiles, sizeof *g_files, cmp_off); }
    while (len) {
        DiscFile *f = find_file(off);
        if (!f) { static int np; if (np++ < 5) fprintf(stderr, "[disc] read %llX: past the end of a file (padding) - zeros\n", (unsigned long long)off);
                  memset(buf, 0, len); return 1; }
        u64 in = off - f->off; u32 n = (u32)(f->size - in < len ? f->size - in : len);
        pthread_mutex_lock(&g_io);
        FILE *h = open_file(f);
        size_t got = 0;
        if (h) { fseek(h, (long)in, SEEK_SET); got = fread(buf, 1, n, h); }
        pthread_mutex_unlock(&g_io);
        if (!h) { fprintf(stderr, "[disc] MISSING %s (needed by the game)\n", f->path); return 2; }
        if (got < n) memset(buf + got, 0, n - got);
        {   // VR: which forensics case is loaded (autopsy/<lang>/<stage>/...) -> its autopsy report on the desk
            extern int g_vr_case; const char *q = strstr(f->path, "autopsy/");
            if (q && (q[8] == 'e' || q[8] == 'j') && q[11] == '/' && q[12] >= '0' && q[12] <= '9' && q[15] == '/') { int c = atoi(q + 12); if (c > 0) g_vr_case = c; }
        }
        if (g_log && !f->logged) { f->logged = 1; fprintf(stderr, "[disc] %s +%llX (%u bytes)\n", f->path, (unsigned long long)in, n); }
        off += n; len -= n; buf += n;
    }
    return 1;
}
static void to_guest(u32 dst, const u8 *buf, u32 len) {
    u32 p = dst & 0x1FFFFFFF;
    if (p + len <= 0x01800000u || (p >= 0x10000000u && p + len <= 0x14000000u)) memcpy(g_mem + p, buf, len);
    else for (u32 k = 0; k < len; k++) W8(dst + k, buf[k]);
}

// Read `len` bytes at disc byte offset `off` into guest memory at `dst`. Returns 1 (drive success) or 2.
int disc_read(u64 off, u32 len, u32 dst) {
    u8 *buf = malloc(len ? len : 1);
    int r = read_host(off, len, buf);
    if (r == 1) to_guest(dst, buf, len);
    free(buf);
    return r;
}

// Asynchronous read: the worker thread fills a host buffer; disc_job_finish copies it into guest memory
// (on the game thread) once done.
struct DiscJob { u64 off; u32 len, dst; u8 *buf; volatile int done; int result; struct DiscJob *next; };
typedef struct DiscJob DiscJob;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_qc = PTHREAD_COND_INITIALIZER;
static DiscJob *g_q, **g_qtail = &g_q;
static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_qm);
        while (!g_q) pthread_cond_wait(&g_qc, &g_qm);
        DiscJob *j = g_q; g_q = j->next; if (!g_q) g_qtail = &g_q;
        pthread_mutex_unlock(&g_qm);
        j->result = read_host(j->off, j->len, j->buf);
        __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}
DiscJob *disc_read_async(u64 off, u32 len, u32 dst) {
    static int started; static pthread_t th;
    if (!started) { started = 1; pthread_create(&th, NULL, worker, NULL); }
    DiscJob *j = calloc(1, sizeof *j);
    j->off = off; j->len = len; j->dst = dst; j->buf = malloc(len ? len : 1);
    pthread_mutex_lock(&g_qm);
    *g_qtail = j; g_qtail = &j->next;
    pthread_cond_signal(&g_qc);
    pthread_mutex_unlock(&g_qm);
    return j;
}
int disc_job_done(DiscJob *j) { return __atomic_load_n(&j->done, __ATOMIC_ACQUIRE); }
int disc_job_finish(DiscJob *j) {   // copy into guest memory, free; returns the drive result
    int r = j->result;
    if (r == 1) to_guest(j->dst, j->buf, j->len);
    free(j->buf); free(j);
    return r;
}
