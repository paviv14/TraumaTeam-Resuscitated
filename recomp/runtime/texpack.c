// Texture packs: dump the game's textures as PNG files and load higher-resolution replacements.
//
// Every texture is identified by a 64-bit hash of its source data as the game stores it (texel bytes
// plus the palette for paletted formats) - the same value for the same texture on every run and
// every PC, wherever the game happens to load it in memory.
//
//   Dump:    TTPC_DUMP_TEXTURES=1 (or a folder name) saves each new texture once, to textures_dump/
//            tt_<hash>_<w>x<h>_f<format>.png   (format = GX texture format number)
//   Replace: put PNGs in textures/ (or TTPC_TEXTURES=<folder>, subfolders allowed); a file whose name
//            starts with tt_<hash> replaces that texture. Any size works - typically 2x-8x the original,
//            keeping the aspect ratio; alpha is used as-is.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_FALLBACK
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

void host_mkdir(const char *path);

typedef struct { u64 hash; char *path; u8 *rgba; int w, h, tried; u8 *half[2]; } Repl;
static Repl *g_repl; static int g_nrepl, g_caprepl;
static char g_dump_dir[512]; static int g_dump = -1;
static u64 *g_dumped; static int g_ndumped, g_capdumped;
int g_texpack_count, g_texpack_dumped;

static int parse_hash(const char *name, u64 *h) {
    if (strncmp(name, "tt_", 3) != 0) return 0;
    char *end; *h = strtoull(name + 3, &end, 16);
    return end - (name + 3) == 16;
}
static void scan(const char *dir, int depth) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 6) scan(p, depth + 1); continue; }
        u64 h; size_t n = strlen(e->d_name);
        if (n < 4 || (strcmp(e->d_name + n - 4, ".png") && strcmp(e->d_name + n - 4, ".PNG"))) continue;
        if (!parse_hash(e->d_name, &h)) continue;
        if (g_nrepl == g_caprepl) { g_caprepl = g_caprepl ? g_caprepl * 2 : 256; g_repl = realloc(g_repl, sizeof *g_repl * g_caprepl); }
        g_repl[g_nrepl++] = (Repl){ h, strdup(p), NULL, 0, 0, 0, { NULL, NULL } };
    }
    closedir(d);
}

void texpack_init(void) {
    const char *dir = getenv("TTPC_TEXTURES"); if (!dir) dir = "textures";
    scan(dir, 0);
    g_texpack_count = g_nrepl;
    if (g_nrepl) fprintf(stderr, "[textures] %d replacement textures in %s\n", g_nrepl, dir);
    const char *dd = getenv("TTPC_DUMP_TEXTURES");
    g_dump = dd && *dd && strcmp(dd, "0") != 0;
    if (g_dump) {
        snprintf(g_dump_dir, sizeof g_dump_dir, "%s", strcmp(dd, "1") == 0 ? "textures_dump" : dd);
        host_mkdir(g_dump_dir);
        // already-dumped files are not written again
        DIR *d = opendir(g_dump_dir); struct dirent *e;
        while (d && (e = readdir(d))) { u64 h; if (parse_hash(e->d_name, &h)) {
            if (g_ndumped == g_capdumped) { g_capdumped = g_capdumped ? g_capdumped * 2 : 1024; g_dumped = realloc(g_dumped, 8 * g_capdumped); }
            g_dumped[g_ndumped++] = h; } }
        if (d) closedir(d);
        fprintf(stderr, "[textures] dumping new textures to %s/ (%d already there)\n", g_dump_dir, g_ndumped);
    }
}

// Replacement for a texture hash: RGBA bytes (loaded on first use), or NULL.
const u8 *texpack_lookup(u64 hash, int *w, int *h) {
    for (int k = 0; k < g_nrepl; k++) {
        Repl *r = &g_repl[k];
        if (r->hash != hash) continue;
        if (!r->tried) {
            r->tried = 1; int n;
            r->rgba = stbi_load(r->path, &r->w, &r->h, &n, 4);
            if (!r->rgba) fprintf(stderr, "[textures] cannot read %s: %s\n", r->path, stbi_failure_reason());
        }
        if (!r->rgba) return NULL;
        *w = r->w; *h = r->h; return r->rgba;
    }
    return NULL;
}

// Key of a dual-palette image (see resolve_pairs in video.c): A = red/green half, B = blue/alpha half.
u64 texpack_pair_key(u64 a, u64 b) {
    u64 k = a ^ (b * 0x9E3779B97F4A7C15ull);
    k ^= k >> 31; k *= 0xBF58476D1CE4E5B9ull; k ^= k >> 29;
    return k;
}
// One half of a full-colour replacement, in the form the game's TEV stages expect from that map:
// which 0 -> (R, R, R, G), which 1 -> (B, B, B, A) - an IA8 texel is (I, I, I, A).
const u8 *texpack_lookup_half(u64 key, int which, int *w, int *h) {
    if (!texpack_lookup(key, w, h)) return NULL;
    for (int k = 0; k < g_nrepl; k++) {
        Repl *r = &g_repl[k];
        if (r->hash != key || !r->rgba) continue;
        if (!r->half[which]) {
            size_t n = (size_t)r->w * r->h; u8 *o = malloc(n * 4);
            for (size_t i = 0; i < n; i++) { const u8 *p = r->rgba + 4 * i;
                u8 c = which ? p[2] : p[0], a = which ? p[3] : p[1];
                o[4 * i] = o[4 * i + 1] = o[4 * i + 2] = c; o[4 * i + 3] = a; }
            r->half[which] = o;
        }
        return r->half[which];
    }
    return NULL;
}

// Save a decoded texture (R<<24|G<<16|B<<8|A pixels) once per hash.
void texpack_dump(u64 hash, int w, int h, int fmt, const u32 *px) {
    if (g_dump <= 0 || w <= 0 || h <= 0) return;
    for (int k = 0; k < g_ndumped; k++) if (g_dumped[k] == hash) return;
    if (g_ndumped == g_capdumped) { g_capdumped = g_capdumped ? g_capdumped * 2 : 1024; g_dumped = realloc(g_dumped, 8 * g_capdumped); }
    g_dumped[g_ndumped++] = hash;
    u8 *rgba = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) { u32 c = px[i]; rgba[4*i] = c >> 24; rgba[4*i+1] = c >> 16; rgba[4*i+2] = c >> 8; rgba[4*i+3] = (u8)c; }
    char p[700]; snprintf(p, sizeof p, "%s/tt_%016llX_%dx%d_f%d.png", g_dump_dir, (unsigned long long)hash, w, h, fmt);
    if (stbi_write_png(p, w, h, 4, rgba, w * 4)) g_texpack_dumped++;
    free(rgba);
}

int texpack_write_png(const char *path, int w, int h, const u8 *rgb) { return stbi_write_png(path, w, h, 3, rgb, w * 3); }
