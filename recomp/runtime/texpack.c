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
//
// Streaming: lookups only find the file. Worker threads turn it into a GPU-ready image (scaled down
// to the size limit, mipmapped, DXT5-compressed) and keep that in a disk cache (texture_cache/), so
// the next time it is one quick read. Until it is ready the game's own texture is shown; gl.c
// uploads a few finished images per frame and evicts textures not used for a while.
#include "cpu.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#define STB_DXT_IMPLEMENTATION
#include "third_party/stb_dxt.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_FALLBACK
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

void host_mkdir(const char *path);


#include "platform.h"
#include "gx_state.h"
typedef struct {   // one replacement file; variants: 0 = the image, 1/2 = halves of a dual-palette pair
    u64 hash; char *path; long long fsize, mtime;
    int set;                      // 0 = the texture pack, 1.. = a button-prompt set (prompts/<set>/)
    int state[3];                 // 0 not requested, 1 queued/working, 2 ready (img), 3 taken by gl.c, 4 failed
    TPImage *img[3];
} Repl;
static Repl *g_repl; static int g_nrepl, g_caprepl;
static int *g_rmap; static int g_rmap_cap;   // hash -> index+1 (open addressing)
static char g_dump_dir[512]; static int g_dump = -1;
static u64 *g_dumped; static int g_ndumped, g_capdumped;
int g_texpack_count, g_texpack_dumped;
static char g_cache_dir[512] = "texture_cache";

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
        if (S_ISDIR(st.st_mode)) { if (depth < 8) scan(p, depth + 1); continue; }
        u64 h; size_t n = strlen(e->d_name);
        if (n < 4 || (strcmp(e->d_name + n - 4, ".png") && strcmp(e->d_name + n - 4, ".PNG") && strcmp(e->d_name + n - 4, ".jpg"))) continue;   // .jpg: the redrawn tutorial screenshots
        if (!parse_hash(e->d_name, &h)) continue;
        if (g_nrepl == g_caprepl) { g_caprepl = g_caprepl ? g_caprepl * 2 : 256; g_repl = realloc(g_repl, sizeof *g_repl * g_caprepl); }
        Repl r = { 0 }; r.hash = h; r.path = strdup(p); r.fsize = (long long)st.st_size; r.mtime = (long long)st.st_mtime;
        g_repl[g_nrepl++] = r;
    }
    closedir(d);
}
static int rmap_slot(u64 h) { return (int)((h ^ (h >> 29)) % (u64)g_rmap_cap); }
static void rmap_build(void) {
    g_rmap_cap = g_nrepl * 2 + 16; g_rmap = calloc((size_t)g_rmap_cap, sizeof *g_rmap);
    for (int i = 0; i < g_nrepl; i++) {
        int j = rmap_slot(g_repl[i].hash);
        while (g_rmap[j]) { if (g_repl[g_rmap[j] - 1].hash == g_repl[i].hash) goto dup; j = (j + 1) % g_rmap_cap; }
        g_rmap[j] = i + 1;
    dup:;
    }
}
// Button-prompt sets: prompts/<name>/tt_<id>_*.png replace the game's Wii Remote prompts while that set is active
// (keyboard & mouse, or a pad style); they win over the texture pack and work with the pack switched off.
static const char *PSET_NAME[] = { "", "keyboard", "xbox", "ps4", "ps5" };
#define NPSET 5
static struct { u64 hash; int id; } *g_pset_map[NPSET]; static int g_pset_n[NPSET], g_pset_cap[NPSET];
static int g_pset;
int texpack_prompt_set(void) { return g_pset; }
const char *texpack_path(int id) { return id >= 0 && id < g_nrepl ? g_repl[id].path : ""; }
int texpack_is_prompt(int id) { return id >= 0 && id < g_nrepl && g_repl[id].set != 0; }
void texpack_set_prompts(int set) { g_pset = set >= 0 && set < NPSET ? set : 0; }
int texpack_prompt_count(int set) { return set > 0 && set < NPSET ? g_pset_n[set] : 0; }
// Live prompts: prompts/base/ holds the pictures with blank button slots (the Wii Remote painted out), the same
// files for every set; prompts/slots.txt says where each slot is and which Wii action it shows; the buttons are
// drawn in when the picture loads, from the player's current bindings (prompt_compose).
typedef struct { u64 hash; float x0, y0, x1, y1; char act[16]; } Slot;
static Slot *g_slots; static int g_nslots;
static unsigned g_bind_sig;   // changes with the bindings: part of a composed picture's cache name
static void load_slots(const char *base) {
    char p[600]; snprintf(p, sizeof p, "%s/slots.txt", base);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        Slot sl = { 0 }; char hs[32];
        if (line[0] == '#' || sscanf(line, "%31s %f %f %f %f %15s", hs, &sl.x0, &sl.y0, &sl.x1, &sl.y1, sl.act) != 6) continue;
        sl.hash = strtoull(hs, NULL, 16);
        g_slots = realloc(g_slots, sizeof *g_slots * (size_t)(g_nslots + 1)); g_slots[g_nslots++] = sl;
    }
    fclose(f);
    fprintf(stderr, "[prompts] %d button slots\n", g_nslots);
}
static void scan_prompts(void) {
    const char *base = getenv("TTPC_PROMPTS"); if (!base) base = "prompts";
    load_slots(base);
    for (int s = 1; s < NPSET; s++) {
        char d[600]; snprintf(d, sizeof d, "%s/base", base);
        int from = g_nrepl; scan(d, 0);
        for (int i = from; i < g_nrepl; i++) {
            // a picture with no slot left in slots.txt (an older base file still in the folder): not used
            int has = 0; for (int k = 0; k < g_nslots && !has; k++) has = g_slots[k].hash == g_repl[i].hash;
            g_repl[i].set = s;
            if (!has) { g_repl[i].hash = 0; continue; }
            if (g_pset_n[s] == g_pset_cap[s]) { g_pset_cap[s] = g_pset_cap[s] ? g_pset_cap[s] * 2 : 64; g_pset_map[s] = realloc(g_pset_map[s], sizeof *g_pset_map[s] * g_pset_cap[s]); }
            g_pset_map[s][g_pset_n[s]].hash = g_repl[i].hash; g_pset_map[s][g_pset_n[s]].id = i; g_pset_n[s]++;
        }
    }
    if (g_pset_n[1]) fprintf(stderr, "[prompts] %d pictures with live button prompts\n", g_pset_n[1]);
}
int texpack_find(u64 hash) {   // replacement id, or -1
    if (g_pset) for (int i = 0; i < g_pset_n[g_pset]; i++) if (g_pset_map[g_pset][i].hash == hash) return g_pset_map[g_pset][i].id;
    if (!g_cfg.textures || !g_rmap) return -1;
    for (int j = rmap_slot(hash); g_rmap[j]; j = (j + 1) % g_rmap_cap) if (g_repl[g_rmap[j] - 1].hash == hash) return g_rmap[j] - 1;
    return -1;
}

// ---------------------------------------------------------------- worker threads
static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_tc = PTHREAD_COND_INITIALIZER;
static int *g_q; static int g_qn, g_qcap;   // queued (id << 2 | variant), taken from the end (newest first)
static int g_nworkers;
int g_tp_limit = 2048, g_tp_compress = 1;   // set by gl.c from the settings / GPU support
long long g_tp_decoded, g_tp_cached, g_tp_failed;

static void box_half(const u8 *src, int w, int h, u8 *dst) {   // 2x2 average (odd sizes: clamp)
    int W = w > 1 ? w / 2 : 1, H = h > 1 ? h / 2 : 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int x0 = 2 * x, y0 = 2 * y, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
        for (int c = 0; c < 4; c++)
            dst[4 * (y * W + x) + c] = (u8)((src[4 * (y0 * w + x0) + c] + src[4 * (y0 * w + x1) + c] + src[4 * (y1 * w + x0) + c] + src[4 * (y1 * w + x1) + c] + 2) >> 2);
    }
}
static TPImage *build_image(u8 *rgba, int w, int h, int compress) {   // takes rgba; mip chain, optionally DXT5
    int nl = 1; for (int a = w, b = h; a > 1 || b > 1; a = a > 1 ? a / 2 : 1, b = b > 1 ? b / 2 : 1) nl++;
    if (nl > 16) nl = 16;
    // fully opaque images use DXT1 (half the size of DXT5)
    if (compress) { int opaque = 1; for (size_t i = 0, n = (size_t)w * h; i < n && opaque; i++) opaque = rgba[4 * i + 3] == 255; if (opaque) compress = 2; }
    TPImage *im = calloc(1, sizeof *im); im->w = w; im->h = h; im->nlevels = nl; im->compressed = compress;
    u8 *cur = rgba; int cw = w, ch = h;
    for (int l = 0; l < nl; l++) {
        size_t sz;
        if (compress) {
            int bs = compress == 2 ? 8 : 16, bw = (cw + 3) / 4, bh = (ch + 3) / 4; sz = (size_t)bw * bh * bs;
            u8 *o = malloc(sz);
            for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
                u8 blk[64];
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
                    int sx = bx * 4 + x, sy = by * 4 + y; if (sx >= cw) sx = cw - 1; if (sy >= ch) sy = ch - 1;
                    memcpy(blk + 4 * (y * 4 + x), cur + 4 * ((size_t)sy * cw + sx), 4);
                }
                stb_compress_dxt_block(o + bs * ((size_t)by * bw + bx), blk, compress == 1, STB_DXT_HIGHQUAL);
            }
            im->data[l] = o;
        } else { sz = (size_t)cw * ch * 4; im->data[l] = malloc(sz); memcpy(im->data[l], cur, sz); }
        im->size[l] = (u32)sz; im->bytes += sz;
        if (l + 1 < nl) {
            int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
            u8 *n = malloc((size_t)nw * nh * 4); box_half(cur, cw, ch, n);
            if (cur != rgba) free(cur);
            cur = n; cw = nw; ch = nh;
        }
    }
    if (cur != rgba) free(cur);
    free(rgba);
    return im;
}
void texpack_free_image(TPImage *im) { if (!im) return; for (int l = 0; l < im->nlevels; l++) free(im->data[l]); free(im); }

// disk cache: "TTC1", u32 w, h, levels, compressed, limit, i64 source size, source mtime, then each level: u32 size + data
static void cache_path(const Repl *r, int var, char *p, size_t n) {
    if (r->set) snprintf(p, n, "%s/%016llX_%d_%d%s_p%d_%08X_v4.ttc", g_cache_dir, (unsigned long long)r->hash, var, g_tp_limit, g_tp_compress ? "" : "u", r->set, g_bind_sig);
    else snprintf(p, n, "%s/%016llX_%d_%d%s_e.ttc", g_cache_dir, (unsigned long long)r->hash, var, g_tp_limit, g_tp_compress ? "" : "u");
}
static TPImage *cache_read(const Repl *r, int var) {
    char p[700]; cache_path(r, var, p, sizeof p);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    u32 hd[6]; long long src[2]; TPImage *im = NULL;
    if (fread(hd, 4, 6, f) != 6 || memcmp(hd, "TTC2", 4) || fread(src, 8, 2, f) != 2 || src[0] != r->fsize || src[1] != r->mtime ||
        (hd[4] != 0) != (g_tp_compress != 0) || hd[4] > 2 || (int)hd[5] != g_tp_limit || hd[3] < 1 || hd[3] > 16) goto out;
    im = calloc(1, sizeof *im); im->w = (int)hd[1]; im->h = (int)hd[2]; im->nlevels = (int)hd[3]; im->compressed = (int)hd[4];
    for (int l = 0; l < im->nlevels; l++) {
        u32 sz; if (fread(&sz, 4, 1, f) != 1 || sz > (1u << 28)) { texpack_free_image(im); im = NULL; goto out; }
        im->data[l] = malloc(sz); im->size[l] = sz; im->bytes += sz;
        if (fread(im->data[l], 1, sz, f) != sz) { im->nlevels = l + 1; texpack_free_image(im); im = NULL; goto out; }
    }
out:
    fclose(f); return im;
}
static void cache_write(const Repl *r, int var, const TPImage *im) {
    char p[700], tmp[720]; cache_path(r, var, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp%d", p, var);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    u32 hd[6] = { 0, (u32)im->w, (u32)im->h, (u32)im->nlevels, (u32)im->compressed, (u32)g_tp_limit }; memcpy(hd, "TTC2", 4);
    long long src[2] = { r->fsize, r->mtime };
    int ok = fwrite(hd, 4, 6, f) == 6 && fwrite(src, 8, 2, f) == 2;
    for (int l = 0; ok && l < im->nlevels; l++) ok = fwrite(&im->size[l], 4, 1, f) == 1 && fwrite(im->data[l], 1, im->size[l], f) == im->size[l];
    fclose(f);
    if (ok) { remove(p); rename(tmp, p); } else remove(tmp);
}
static u8 *variant_of(const u8 *rgba, int w, int h, int var) {   // 1: (R,R,R,G)  2: (B,B,B,A)
    size_t n = (size_t)w * h; u8 *o = malloc(n * 4);
    for (size_t i = 0; i < n; i++) { const u8 *q = rgba + 4 * i; u8 c = var == 2 ? q[2] : q[0], a = var == 2 ? q[3] : q[1];
        o[4 * i] = o[4 * i + 1] = o[4 * i + 2] = c; o[4 * i + 3] = a; }
    return o;
}

// ---------------------------------------------------------------- live button prompts
#include "font_atlas.h"
typedef struct { int w, h; u8 *px; } Glyph;
static Glyph *glyph_get(int set, const char *name) {   // prompts/glyphs/<style>/<name>.png, kept once loaded
    static struct { char key[48]; Glyph g; } cache[160]; static int n; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static const char *STY[] = { "", "keyboard", "xbox", "ps4", "ps5" };
    char key[48]; snprintf(key, sizeof key, "%s/%s", STY[set], name);
    pthread_mutex_lock(&m);
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].key, key)) { pthread_mutex_unlock(&m); return cache[i].g.px ? &cache[i].g : NULL; }
    const char *base = getenv("TTPC_PROMPTS"); if (!base) base = "prompts";
    char p[700]; snprintf(p, sizeof p, "%s/glyphs/%s.png", base, key);
    int c; Glyph g = { 0 }; g.px = stbi_load(p, &g.w, &g.h, &c, 4);
    if (!g.px && set != 1 && strcmp(STY[set], "xbox")) { snprintf(p, sizeof p, "%s/glyphs/xbox/%s.png", base, name); g.px = stbi_load(p, &g.w, &g.h, &c, 4); }
    if (n < 160) { snprintf(cache[n].key, sizeof cache[n].key, "%s", key); cache[n].g = g; n++; }
    pthread_mutex_unlock(&m);
    return g.px ? &cache[n - 1].g : NULL;
}
static void blend_px(u8 *d, const float *c) {   // c: straight-alpha rgba 0..255
    float a = c[3] / 255.f;
    float da = d[3] / 255.f, oa = a + da * (1 - a);
    for (int k = 0; k < 3; k++) d[k] = (u8)(oa > 0 ? (c[k] * a + d[k] * da * (1 - a)) / oa + 0.5f : 0);
    d[3] = (u8)(oa * 255 + 0.5f);
}
static void draw_glyph(u8 *img, int W, int H, const Glyph *g, float x, float y, float w, float h) {   // bilinear, stretched
    for (int py = (int)y; py < (int)(y + h + 1); py++) for (int px = (int)x; px < (int)(x + w + 1); px++) {
        if (px < 0 || py < 0 || px >= W || py >= H) continue;
        float u = (px + 0.5f - x) / w * g->w - 0.5f, v = (py + 0.5f - y) / h * g->h - 0.5f;
        if (u < -0.5f || v < -0.5f || u > g->w - 0.5f || v > g->h - 0.5f) continue;
        int u0 = (int)floorf(u), v0 = (int)floorf(v); float fu = u - u0, fv = v - v0; float c[4] = { 0 }, wsum = 0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            int uu = u0 + i, vv = v0 + j; if (uu < 0 || vv < 0 || uu >= g->w || vv >= g->h) continue;
            float wt = (i ? fu : 1 - fu) * (j ? fv : 1 - fv); const u8 *s = g->px + 4 * (vv * g->w + uu);
            float al = s[3] / 255.f * wt; for (int k = 0; k < 3; k++) c[k] += s[k] * al; c[3] += al; wsum += wt; }
        if (c[3] <= 0) continue;
        for (int k = 0; k < 3; k++) c[k] /= c[3]; c[3] = c[3] / (wsum > 0 ? wsum : 1) * 255;
        blend_px(img + 4 * (py * W + px), c);
    }
}
static float text_w(const char *s, float px) { float k = px / FONT_LINE, w = 0; for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += g_font_glyph[c - 32][5] * k; } return w; }
static void draw_text(u8 *img, int W, int H, const char *s, float x, float y, float px, const u8 *rgb) {   // y = top of the line
    float k = px / FONT_LINE;
    for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?';
        const short *g = g_font_glyph[c - 32];
        for (int j = 0; j < (int)(g[3] * k) + 1; j++) for (int i = 0; i < (int)(g[2] * k) + 1; i++) {
            int X = (int)(x + g[4] * k) + i, Y = (int)y + j; if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            float fx = i / k, fy = j / k; int sx = g[0] + (int)fx, sy = g[1] + (int)fy;
            if (fx >= g[2] || fy >= g[3]) continue;
            float cov = g_font_px[sy * FONT_W + sx]; if (cov <= 0) continue;
            float col[4] = { rgb[0], rgb[1], rgb[2], cov }; blend_px(img + 4 * (Y * W + X), col);
        }
        x += g[5] * k; }
}
// the slots of one picture, drawn from the bindings (prompt_tokens, platform.c)
static void prompt_compose(u64 hash, int set, u8 *img, int W, int H) {
    for (int si = 0; si < g_nslots; si++) {
        Slot *sl = &g_slots[si]; if (sl->hash != hash) continue;
        PromptTok tk[8]; int n = prompt_tokens(set, sl->act, tk, 8); if (n <= 0) continue;
        float rx = sl->x0 * W, ry = sl->y0 * H, rw = (sl->x1 - sl->x0) * W, rh = (sl->y1 - sl->y0) * H;
        float gh = rh, ws[8], tot;
        for (int pass = 0; pass < 2; pass++) {   // widths at height gh; shrink to fit the slot's width
            tot = 0;
            for (int i = 0; i < n; i++) {
                if (tk[i].kind == 1) { Glyph *kg = glyph_get(set == 1 ? 1 : set, "key");
                    float tw = text_w(tk[i].name, gh * 0.42f) + gh * 0.42f; float mn = kg ? gh * kg->w / kg->h : gh * 0.8f; ws[i] = tw > mn ? tw : mn; }
                else if (tk[i].kind == 2) ws[i] = text_w(tk[i].name, gh * 0.6f) + gh * 0.08f;
                else { Glyph *g = glyph_get(set, tk[i].name); ws[i] = g ? gh * g->w / g->h : gh * 0.8f; }
                tot += ws[i];
            }
            if (tot <= rw * 1.0f) break;
            gh *= rw / tot;
        }
        float x = rx + (rw - tot) / 2, y = ry + (rh - gh) / 2;
        static const u8 white[3] = { 236, 236, 236 }, ink[3] = { 28, 28, 30 };
        for (int i = 0; i < n; i++) {
            if (tk[i].kind == 2) draw_text(img, W, H, tk[i].name, x + gh * 0.04f, y + gh * 0.18f, gh * 0.6f, white);
            else {
                Glyph *g = glyph_get(set, tk[i].kind == 1 ? "key" : tk[i].name);
                if (!g && tk[i].kind == 1) g = glyph_get(1, "key");
                if (g) draw_glyph(img, W, H, g, x, y, ws[i], gh);
                if (tk[i].kind == 1) { float ts = gh * 0.42f; draw_text(img, W, H, tk[i].name, x + (ws[i] - text_w(tk[i].name, ts)) / 2, y + gh * 0.27f, ts, ink); }
            }
            x += ws[i];
        }
    }
}
// bindings or device changed: composed pictures are made again (gl.c drops its copies: gpu_prompts_flush)
void texpack_prompts_changed(unsigned sig) {
    if (sig == g_bind_sig) return;
    g_bind_sig = sig;
    pthread_mutex_lock(&g_tm);
    for (int i = 0; i < g_nrepl; i++) if (g_repl[i].set) for (int v = 0; v < 3; v++) {
        if (g_repl[i].state[v] == 2) { texpack_free_image(g_repl[i].img[v]); g_repl[i].img[v] = NULL; }
        if (g_repl[i].state[v] != 1) g_repl[i].state[v] = 0;
    }
    pthread_mutex_unlock(&g_tm);
}
u64 host_now_ns(void);
// Upscalers darken the colour of a sprite's soft edge (its see-through pixels are black, and the edge blends towards
// them): drawn over something light, the edge showed as a grey line (the comic bubbles' tails). The colour of every
// partly see-through pixel near the opaque part is taken from its nearest opaque neighbours instead; its alpha stays.
static void bleed_edges(u8 *px, int w, int h) {
    size_t n = (size_t)w * h; u8 *done = malloc(n), *nd = malloc(n); int any = 0;
    for (size_t i = 0; i < n; i++) { done[i] = px[4 * i + 3] >= 240; any |= !done[i]; }
    if (any) for (int pass = 0; pass < 6; pass++) {
        int changed = 0; memcpy(nd, done, n);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x; if (done[i]) continue;
            int r = 0, g = 0, b = 0, c = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int X = x + dx, Y = y + dy; if ((dx | dy) == 0 || X < 0 || Y < 0 || X >= w || Y >= h) continue;
                size_t j = (size_t)Y * w + X; if (!done[j]) continue;
                r += px[4 * j]; g += px[4 * j + 1]; b += px[4 * j + 2]; c++; }
            if (c) { px[4 * i] = (u8)(r / c); px[4 * i + 1] = (u8)(g / c); px[4 * i + 2] = (u8)(b / c); nd[i] = 1; changed = 1; }
        }
        memcpy(done, nd, n); if (!changed) break;
    }
    free(done); free(nd);
}
static void work(int id, int var) {
    Repl *r = &g_repl[id];
    static int tlog = -1; if (tlog < 0) tlog = getenv("TTPC_TEXLOG") != NULL;
    u64 t0 = tlog ? host_now_ns() : 0;
    TPImage *im = cache_read(r, var);
    if (im) { __atomic_add_fetch(&g_tp_cached, 1, __ATOMIC_RELAXED); goto done; }
    int w, h, n; u8 *rgba = stbi_load(r->path, &w, &h, &n, 4);
    if (!rgba) { fprintf(stderr, "[textures] cannot read %s: %s\n", r->path, stbi_failure_reason()); __atomic_add_fetch(&g_tp_failed, 1, __ATOMIC_RELAXED); goto done; }
    while ((w > g_tp_limit || h > g_tp_limit) && w > 1 && h > 1) {   // size limit: halve (keeps the aspect ratio)
        int nw = w / 2, nh = h / 2; u8 *s2 = malloc((size_t)nw * nh * 4); box_half(rgba, w, h, s2); free(rgba); rgba = s2; w = nw; h = nh;
    }
    // (a dual-palette pair is one RGBA picture split in two, (R,G) and (B,A), and put back together by the game,
    // so the buttons are drawn onto the whole picture before it is split)
    bleed_edges(rgba, w, h);
    if (r->set) prompt_compose(r->hash, r->set, rgba, w, h);
    if (var) {   // dual-palette pair: make both halves now, cache the other one for later
        int other = var == 1 ? 2 : 1;
        TPImage *o = build_image(variant_of(rgba, w, h, other), w, h, g_tp_compress); cache_write(r, other, o);
        pthread_mutex_lock(&g_tm);
        if (r->state[other] == 1 && !r->img[other]) { r->img[other] = o; r->state[other] = 2; o = NULL; }
        pthread_mutex_unlock(&g_tm);
        texpack_free_image(o);
        u8 *v = variant_of(rgba, w, h, var); free(rgba); rgba = v;
    }
    im = build_image(rgba, w, h, g_tp_compress);
    cache_write(r, var, im);
    __atomic_add_fetch(&g_tp_decoded, 1, __ATOMIC_RELAXED);
done:
    if (tlog && im) fprintf(stderr, "[textures] %016llX/%d %dx%d %.0f KB in %.1f ms\n", (unsigned long long)r->hash, var, im->w, im->h, im->bytes / 1024.0, (host_now_ns() - t0) / 1e6);
    pthread_mutex_lock(&g_tm);
    if (r->state[var] == 1) { r->img[var] = im; r->state[var] = im ? 2 : 4; im = NULL; }
    pthread_mutex_unlock(&g_tm);
    texpack_free_image(im);
}
static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_tm);
        while (!g_qn) pthread_cond_wait(&g_tc, &g_tm);
        int job = g_q[--g_qn];
        int id = job >> 2, var = job & 3;
        int already = g_repl[id].state[var] != 1 || g_repl[id].img[var];
        pthread_mutex_unlock(&g_tm);
        if (!already) work(id, var);
    }
    return NULL;
}

// Finished image for (id, variant): ownership passes to the caller. Otherwise queues it (once) and
// returns NULL. *failed is set when it can never load.
TPImage *texpack_get(int id, int var, int *failed, int take);
TPImage *texpack_take(int id, int var, int *failed) { return texpack_get(id, var, failed, 1); }
TPImage *texpack_get(int id, int var, int *failed, int take) {   // take 0: only make sure it is on its way
    *failed = 0;
    if (id < 0 || id >= g_nrepl) { *failed = 1; return NULL; }
    Repl *r = &g_repl[id]; TPImage *im = NULL;
    pthread_mutex_lock(&g_tm);
    switch (r->state[var]) {
    case 2: if (take) { im = r->img[var]; r->img[var] = NULL; r->state[var] = 3; } break;
    case 4: *failed = 1; break;
    case 0: case 3:   // new, or evicted by gl.c and needed again
        r->state[var] = 1;
        if (g_qn == g_qcap) { g_qcap = g_qcap ? g_qcap * 2 : 256; g_q = realloc(g_q, sizeof *g_q * (size_t)g_qcap); }
        g_q[g_qn++] = id << 2 | var;
        pthread_cond_signal(&g_tc);
        break;
    }
    pthread_mutex_unlock(&g_tm);
    return im;
}
int texpack_pending(void) { pthread_mutex_lock(&g_tm); int n = g_qn; pthread_mutex_unlock(&g_tm); return n; }

void texpack_init(void) {
    const char *dir = getenv("TTPC_TEXTURES"); if (!dir) dir = "textures";
    scan(dir, 0);
    rmap_build();          // the pack only; the prompt sets have their own lists
    scan_prompts();
    g_texpack_count = g_nrepl;
    if (getenv("TTPC_TEXTURE_CACHE")) snprintf(g_cache_dir, sizeof g_cache_dir, "%s", getenv("TTPC_TEXTURE_CACHE"));
    if (g_nrepl) {
        host_mkdir(g_cache_dir);
        long cpus = 4;
#ifdef _WIN32
        { const char *e = getenv("NUMBER_OF_PROCESSORS"); if (e) cpus = atol(e); }
#else
        { long n = sysconf(_SC_NPROCESSORS_ONLN); if (n > 0) cpus = n; }
#endif
        g_nworkers = (int)(cpus / 3); if (g_nworkers < 2) g_nworkers = 2; if (g_nworkers > 8) g_nworkers = 8;
        for (int i = 0; i < g_nworkers; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_detach(t); }
        fprintf(stderr, "[textures] %d replacement textures in %s (%d loader threads, cache in %s/)\n", g_nrepl, dir, g_nworkers, g_cache_dir);
    }
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

// --prepare-textures: build the whole disk cache now, on every CPU core, so nothing has to be
// converted while playing. Images under a ".tpx" folder are the game's dual-palette 2D art (both
// halves are made); everything else is used as one image.
int texpack_prepare(void) {
    settings_load();
    static const int lim[3] = { 1024, 2048, 4096 };
    g_tp_limit = lim[g_cfg.tex_limit < 0 ? 0 : g_cfg.tex_limit > 2 ? 2 : g_cfg.tex_limit];
    g_tp_compress = g_cfg.tex_compress;
    int ok = g_cfg.textures; g_cfg.textures = 1;
    const char *dir = getenv("TTPC_TEXTURES"); if (!dir) dir = "textures";
    scan(dir, 0); rmap_build();
    if (getenv("TTPC_TEXTURE_CACHE")) snprintf(g_cache_dir, sizeof g_cache_dir, "%s", getenv("TTPC_TEXTURE_CACHE"));
    host_mkdir(g_cache_dir);
    printf("Texture pack: %d images in %s, size limit %d, %s. Cache: %s\n", g_nrepl, dir, g_tp_limit, g_tp_compress ? "compressed" : "uncompressed", g_cache_dir);
    if (!g_nrepl) { printf("No texture pack images found.\n"); return 1; }
    long cpus = 4;
#ifdef _WIN32
    { const char *e = getenv("NUMBER_OF_PROCESSORS"); if (e) cpus = atol(e); }
#else
    { long n = sysconf(_SC_NPROCESSORS_ONLN); if (n > 0) cpus = n; }
#endif
    if (cpus < 1) cpus = 1;
    if (cpus > 64) cpus = 64;
    int total = 0, failed;
    for (int i = 0; i < g_nrepl; i++) {
        int pair = strstr(g_repl[i].path, ".tpx/") || strstr(g_repl[i].path, ".tpx\\") || strstr(g_repl[i].path, ".TPX/");
        if (pair) { texpack_get(i, 1, &failed, 0); g_repl[i].state[2] = 1; }   // the job for half 1 makes half 2 too
        else texpack_get(i, 0, &failed, 0);
        total++;
    }
    u64 host_now_ns(void); u64 t0 = host_now_ns();
    for (long k = 0; k < cpus; k++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_detach(t); }
    for (;;) {
        int done = 0;
        pthread_mutex_lock(&g_tm);
        for (int i = 0; i < g_nrepl; i++) {
            Repl *r = &g_repl[i]; int var = r->state[0] ? 0 : 1;
            if (r->state[var] >= 2) {
                done++;
                for (int v = 0; v < 3; v++) if (r->img[v]) { texpack_free_image(r->img[v]); r->img[v] = NULL; }
            }
        }
        pthread_mutex_unlock(&g_tm);
        double secs = (host_now_ns() - t0) / 1e9;
        printf("\r  %d / %d  (%d%%)  %.0f s   ", done, total, 100 * done / total, secs); fflush(stdout);
        if (done >= total) break;
#ifdef _WIN32
        { void Sleep(unsigned long); Sleep(500); }
#else
        usleep(500000);
#endif
    }
    printf("\nDone: %lld converted, %lld already up to date, %lld could not be read.\n", g_tp_decoded, g_tp_cached, g_tp_failed);
    g_cfg.textures = ok;
    return 0;
}

// Key of a dual-palette image (see resolve_pairs in video.c): A = red/green half, B = blue/alpha half.
u64 texpack_pair_key(u64 a, u64 b) {
    u64 k = a ^ (b * 0x9E3779B97F4A7C15ull);
    k ^= k >> 31; k *= 0xBF58476D1CE4E5B9ull; k ^= k >> 29;
    return k;
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
