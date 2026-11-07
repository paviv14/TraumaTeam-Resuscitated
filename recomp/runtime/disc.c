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

typedef struct { u64 off, size; char path[256]; } DiscFile;
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

// Read `len` bytes at disc byte offset `off` into guest memory at `dst`. Returns 1 (drive success) or 2.
int disc_read(u64 off, u32 len, u32 dst) {
    while (len) {
        DiscFile *f = NULL;
        for (int k = 0; k < g_nfiles; k++) if (off >= g_files[k].off && off < g_files[k].off + g_files[k].size) { f = &g_files[k]; break; }
        if (!f) { fprintf(stderr, "[disc] read %llX: not inside any file (padding?) - zeros\n", (unsigned long long)off);
                  for (u32 k = 0; k < len; k++) W8(dst + k, 0); return 1; }
        u64 in = off - f->off; u32 n = (u32)(f->size - in < len ? f->size - in : len);
        char p[800]; snprintf(p, sizeof p, "%s/%s", g_root, f->path);
        FILE *h = fopen(p, "rb");
        if (!h) { fprintf(stderr, "[disc] MISSING %s (needed by the game)\n", f->path); return 2; }
        u8 *buf = malloc(n); fseek(h, (long)in, SEEK_SET); size_t got = fread(buf, 1, n, h); fclose(h);
        for (u32 k = 0; k < got; k++) W8(dst + k, buf[k]);
        for (u32 k = got; k < n; k++) W8(dst + k, 0);
        free(buf);
        if (g_log) { static int nl; if (nl++ < 40) fprintf(stderr, "[disc] %s +%llX (%u bytes)\n", f->path, (unsigned long long)in, n); }
        off += n; len -= n; dst += n;
    }
    return 1;
}
