// GX video: decodes the GPU command stream (FIFO + display lists) and renders it with a software
// rasterizer at any internal resolution. Displayed frames (EFB -> XFB copies) can be saved as images.
// This is the reference renderer; a GPU backend can reuse the decoder.
//
// Command stream:  0x00 NOP | 0x08 CP reg (u8 reg, u32) | 0x10 XF regs (u32 (n-1)<<16|addr, n x u32)
//   0x20/28/30/38 indexed XF load A-D (u32) | 0x40 call display list (u32 addr, u32 size) | 0x48 inval
//   0x61 BP reg (u32: reg<<24 | value) | 0x80-0xBF draw: prim = (op>>3)&7, vat = op&7, u16 count, vertices
// Environment: TTPC_FRAMES=<dir> saves displayed frames as PPM, TTPC_FRAME_EVERY=N, TTPC_SCALE=S.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------- state
static u32 cp[256];                 // CP registers
static u32 xfmem[0x1000];           // XF memory (matrices, lights) as raw float bits
static u32 xfreg[0x100];            // XF registers 0x1000-0x10FF
static u32 bp[256];                 // BP registers
static u8 *tmem;                    // texture memory (TLUTs)
static int g_scale = 1;
static int EW, EH;                  // EFB size in host pixels
static u32 *efb_c; static float *efb_z;
static const char *g_dump_dir; static int g_dump_every = 30; static u64 g_copies;
static int g_render = 1;
u64 g_video_draws;
static u64 st_tri, st_w, st_area, st_cull, st_empty, st_px, st_alpha, st_z; static int st_samples;

// Reader for the parser: addresses 0xF0000000+ are the buffered FIFO bytes, anything else is guest memory.
static u8 g_buf[1 << 21]; static u32 g_len;
// Side-effect-free read of guest RAM: the renderer must never touch hardware registers (a stray
// array address in the register range would otherwise pop mailboxes etc.). Non-RAM reads as 0.
static inline u32 M8(u32 a) {
    u32 p = (a >> 28) < 8 ? a : (a & 0x1FFFFFFF);   // physical (array/FIFO registers) or virtual
    if (p < 0x01800000u || (p - 0x10000000u) < 0x04000000u) return g_mem[p];
    return 0;
}
static inline u32 M16(u32 a) { return M8(a) << 8 | M8(a + 1); }
static inline u32 M32(u32 a) { return M16(a) << 16 | M16(a + 2); }
static inline u32 V8(u32 a)  { return (a >> 28) == 0xF ? g_buf[a & 0x1FFFFF] : M8(a); }
static inline u32 V16(u32 a) { return V8(a) << 8 | V8(a + 1); }
static inline u32 V32(u32 a) { return V16(a) << 16 | V16(a + 2); }

static float xf_f(u32 addr) { u32 v = addr < 0x1000 ? xfmem[addr] : xfreg[addr & 0xFF]; float f; memcpy(&f, &v, 4); return f; }

extern int g_video_on;
void video_init(void) {
    if (getenv("TTPC_NOVIDEO")) g_video_on = 0;
    const char *s = getenv("TTPC_SCALE"); if (s) g_scale = atoi(s); if (g_scale < 1) g_scale = 1;
    EW = 640 * g_scale; EH = 528 * g_scale;
    efb_c = calloc((size_t)EW * EH, 4); efb_z = malloc((size_t)EW * EH * 4);
    for (long k = 0; k < (long)EW * EH; k++) efb_z[k] = 1.0f;
    tmem = calloc(1, 1 << 20);
    g_dump_dir = getenv("TTPC_FRAMES");
    if (getenv("TTPC_FRAME_EVERY")) g_dump_every = atoi(getenv("TTPC_FRAME_EVERY"));
    g_render = g_dump_dir != NULL;   // rasterizing is only needed when frames are being looked at
}

// ---------------------------------------------------------------- vertex formats
typedef struct { float pos[3]; float nrm[3]; float col[2][4]; float tex[8][2]; int pnmtx; int texmtx[8]; int has_col[2]; } Vtx;

static int fmt_size(int fmt) { return fmt == 0 || fmt == 1 ? 1 : fmt == 2 || fmt == 3 ? 2 : 4; }
static float read_comp(u32 *a, int fmt, int frac) {
    float v;
    switch (fmt) {
    case 0: v = (u8)V8(*a); *a += 1; break;
    case 1: v = (s8)V8(*a); *a += 1; break;
    case 2: v = (u16)V16(*a); *a += 2; break;
    case 3: v = (s16)V16(*a); *a += 2; break;
    default: { u32 b = V32(*a); *a += 4; memcpy(&v, &b, 4); return v; }
    }
    return ldexpf(v, -frac);
}
static void read_color(u32 *a, int fmt, float out[4]) {
    u32 r = 255, g = 255, b = 255, al = 255;
    switch (fmt) {
    case 0: { u32 v = V16(*a); *a += 2; r = (v >> 11) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31; break; }
    case 1: r = V8(*a); g = V8(*a + 1); b = V8(*a + 2); *a += 3; break;
    case 2: r = V8(*a); g = V8(*a + 1); b = V8(*a + 2); *a += 4; break;
    case 3: { u32 v = V16(*a); *a += 2; r = (v >> 12) * 17; g = ((v >> 8) & 15) * 17; b = ((v >> 4) & 15) * 17; al = (v & 15) * 17; break; }
    case 4: { u32 v = V8(*a) << 16 | V8(*a + 1) << 8 | V8(*a + 2); *a += 3;
              r = (v >> 18) * 255 / 63; g = ((v >> 12) & 63) * 255 / 63; b = ((v >> 6) & 63) * 255 / 63; al = (v & 63) * 255 / 63; break; }
    default: r = V8(*a); g = V8(*a + 1); b = V8(*a + 2); al = V8(*a + 3); *a += 4; break;
    }
    out[0] = r / 255.f; out[1] = g / 255.f; out[2] = b / 255.f; out[3] = al / 255.f;
}

// Attribute data may be inline (direct) or fetched from an array by 8/16-bit index.
static u32 attr_addr(u32 *p, int mode, int arr) {
    if (mode == 1) return 0;
    u32 idx = mode == 2 ? V8(*p) : V16(*p); *p += mode == 2 ? 1 : 2;
    return (cp[0xA0 + arr] & 0x3FFFFFFF) + idx * (cp[0xB0 + arr] & 0xFF);
}
static void read_vertex(u32 *p, int vat, Vtx *v) {
    u32 vcd_lo = cp[0x50], vcd_hi = cp[0x60], va = cp[0x70 + vat], vb = cp[0x80 + vat], vc = cp[0x90 + vat];
    u32 mia = cp[0x30], mib = cp[0x40];
    v->pnmtx = mia & 63;
    for (int t = 0; t < 8; t++) v->texmtx[t] = t < 4 ? (mia >> (6 + 6 * t)) & 63 : (mib >> (6 * (t - 4))) & 63;
    if (vcd_lo & 1) { v->pnmtx = V8(*p) & 63; *p += 1; }
    for (int t = 0; t < 8; t++) if (vcd_lo & (2 << t)) { v->texmtx[t] = V8(*p) & 63; *p += 1; }
    // position
    int pm = (vcd_lo >> 9) & 3;
    if (pm) {
        int cnt = va & 1, fmt = (va >> 1) & 7, frac = (va >> 4) & 31;
        u32 a = pm == 1 ? 0 : attr_addr(p, pm, 0); u32 *src = pm == 1 ? p : &a;
        v->pos[0] = read_comp(src, fmt, frac); v->pos[1] = read_comp(src, fmt, frac);
        v->pos[2] = cnt ? read_comp(src, fmt, frac) : 0.f;
    }
    // normal (with optional binormal/tangent: NBT)
    int nm = (vcd_lo >> 11) & 3;
    if (nm) {
        int nbt = (va >> 9) & 1, fmt = (va >> 10) & 7, frac = fmt == 1 ? 6 : fmt == 3 ? 14 : 0;
        u32 a = nm == 1 ? 0 : attr_addr(p, nm, 1); u32 *src = nm == 1 ? p : &a;
        for (int k = 0; k < 3; k++) v->nrm[k] = read_comp(src, fmt, frac);
        if (nbt) for (int k = 0; k < 6; k++) read_comp(src, fmt, frac);
    }
    // colors
    for (int ci = 0; ci < 2; ci++) {
        int cm = (vcd_lo >> (13 + 2 * ci)) & 3;
        v->has_col[ci] = cm != 0;
        if (!cm) { v->col[ci][0] = v->col[ci][1] = v->col[ci][2] = v->col[ci][3] = 1.f; continue; }
        int fmt = (va >> (14 + 4 * ci)) & 7;
        u32 a = cm == 1 ? 0 : attr_addr(p, cm, 2 + ci); u32 *src = cm == 1 ? p : &a;
        read_color(src, fmt, v->col[ci]);
    }
    // texture coordinates
    for (int t = 0; t < 8; t++) {
        int tm = (vcd_hi >> (2 * t)) & 3;
        if (!tm) { v->tex[t][0] = v->tex[t][1] = 0; continue; }
        int cnt, fmt, frac;
        switch (t) {
        case 0: cnt = (va >> 21) & 1; fmt = (va >> 22) & 7; frac = (va >> 25) & 31; break;
        case 1: cnt = vb & 1; fmt = (vb >> 1) & 7; frac = (vb >> 4) & 31; break;
        case 2: cnt = (vb >> 9) & 1; fmt = (vb >> 10) & 7; frac = (vb >> 13) & 31; break;
        case 3: cnt = (vb >> 18) & 1; fmt = (vb >> 19) & 7; frac = (vb >> 22) & 31; break;
        case 4: cnt = (vb >> 27) & 1; fmt = (vb >> 28) & 7; frac = vc & 31; break;
        case 5: cnt = (vc >> 5) & 1; fmt = (vc >> 6) & 7; frac = (vc >> 9) & 31; break;
        case 6: cnt = (vc >> 14) & 1; fmt = (vc >> 15) & 7; frac = (vc >> 18) & 31; break;
        default: cnt = (vc >> 23) & 1; fmt = (vc >> 24) & 7; frac = (vc >> 27) & 31; break;
        }
        u32 a = tm == 1 ? 0 : attr_addr(p, tm, 4 + t); u32 *src = tm == 1 ? p : &a;
        v->tex[t][0] = read_comp(src, fmt, frac);
        v->tex[t][1] = cnt ? read_comp(src, fmt, frac) : 0.f;
    }
}

// ---------------------------------------------------------------- transform
typedef struct { float x, y, z, w; float col[2][4]; float uv[8][3]; } Out;

static void mtx_apply(u32 base, const float in[3], float out[3], int rows) {
    for (int r = 0; r < rows; r++)
        out[r] = xf_f(base + 4 * r) * in[0] + xf_f(base + 4 * r + 1) * in[1] + xf_f(base + 4 * r + 2) * in[2] + xf_f(base + 4 * r + 3);
}
static void light_color(const Vtx *v, int ch, float out[4]) {
    // XF 0x100E/0x100F: color channel control, 0x1010/0x1011 alpha. bit0 = material from vertex,
    // bit1 = lighting enabled. Material registers 0x100C/0x100D, ambient 0x100A/0x100B (RGBA8).
    u32 cc = xfreg[0x0E + ch], ac = xfreg[0x10 + ch];
    u32 mat = xfreg[0x0C + ch], amb = xfreg[0x0A + ch];
    float m[4] = { (mat >> 24) / 255.f, ((mat >> 16) & 255) / 255.f, ((mat >> 8) & 255) / 255.f, (mat & 255) / 255.f };
    float am[4] = { (amb >> 24) / 255.f, ((amb >> 16) & 255) / 255.f, ((amb >> 8) & 255) / 255.f, (amb & 255) / 255.f };
    float vc[4]; memcpy(vc, v->col[ch], sizeof vc);
    for (int k = 0; k < 3; k++) {
        float base = (cc & 1) ? vc[k] : m[k];
        // lighting: approximate with ambient + full diffuse (keeps lit models visible until lights are done)
        out[k] = (cc & 2) ? fminf(1.f, base * fminf(1.f, am[k] + 0.75f)) : base;
    }
    float ba = (ac & 1) ? vc[3] : m[3];
    out[3] = (ac & 2) ? fminf(1.f, ba * fminf(1.f, am[3] + 0.75f)) : ba;
}
static void transform(const Vtx *v, Out *o) {
    float p[3]; mtx_apply((u32)v->pnmtx * 4, v->pos, p, 3);
    float P[6]; for (int k = 0; k < 6; k++) P[k] = xf_f(0x1020 + k);
    int ortho = xfreg[0x26] & 1;
    if (!ortho) { o->x = P[0] * p[0] + P[1] * p[2]; o->y = P[2] * p[1] + P[3] * p[2]; o->z = P[4] * p[2] + P[5]; o->w = -p[2]; }
    else        { o->x = P[0] * p[0] + P[1]; o->y = P[2] * p[1] + P[3]; o->z = P[4] * p[2] + P[5]; o->w = 1.f; }
    int nchan = xfreg[0x09] & 3;
    for (int ch = 0; ch < 2; ch++) {
        if (ch < nchan || v->has_col[ch]) light_color(v, ch, o->col[ch]);
        else for (int k = 0; k < 4; k++) o->col[ch][k] = 1.f;
    }
    // texture coordinate generation (XF 0x1040+n): source row bits 7-11, ST/STQ bit 1
    int ntex = xfreg[0x3F] & 15;
    for (int t = 0; t < 8; t++) {
        o->uv[t][0] = o->uv[t][1] = 0; o->uv[t][2] = 1;
        if (t >= ntex) continue;
        u32 tg = xfreg[0x40 + t];
        int src = (tg >> 7) & 31, stq = (tg >> 1) & 1, type = (tg >> 4) & 7;
        float in[3] = { 0, 0, 1 };
        if (type == 0 || type == 1) {
            if (src == 0) { in[0] = v->pos[0]; in[1] = v->pos[1]; in[2] = v->pos[2]; }
            else if (src == 1) { in[0] = v->nrm[0]; in[1] = v->nrm[1]; in[2] = v->nrm[2]; }
            else if (src >= 5 && src <= 12) { in[0] = v->tex[src - 5][0]; in[1] = v->tex[src - 5][1]; in[2] = 1; }
            float r[3]; u32 m = (u32)v->texmtx[t] * 4;
            mtx_apply(m, in, r, stq ? 3 : 2);
            o->uv[t][0] = r[0]; o->uv[t][1] = r[1]; o->uv[t][2] = stq ? r[2] : 1.f;
        } else {   // color-texgen and others: pass through texture coords
            o->uv[t][0] = v->tex[t][0]; o->uv[t][1] = v->tex[t][1];
        }
    }
}

// ---------------------------------------------------------------- textures
typedef struct { u32 addr; int w, h, fmt, wrap_s, wrap_t, tlut_off, tlut_fmt; } Tex;
static void get_tex(int map, Tex *t) {
    int b = map < 4 ? 0 : 0x20, m = map & 3;
    u32 img0 = bp[0x88 + b + m], img3 = bp[0x94 + b + m], mode0 = bp[0x80 + b + m], tlut = bp[0x98 + b + m];
    t->w = (img0 & 0x3FF) + 1; t->h = ((img0 >> 10) & 0x3FF) + 1; t->fmt = (img0 >> 20) & 15;
    t->addr = 0x80000000u | ((img3 & 0x00FFFFFF) << 5);
    t->wrap_s = mode0 & 3; t->wrap_t = (mode0 >> 2) & 3;
    t->tlut_off = (tlut & 0x3FF) << 9; t->tlut_fmt = (tlut >> 10) & 3;
}
static u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r << 24 | g << 16 | b << 8 | a; }
static u32 c565(u32 v) { return rgba((v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255); }
static u32 c5a3(u32 v) {
    if (v & 0x8000) return rgba(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31, 255);
    return rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 / 7);
}
static u32 tlut_color(const Tex *t, u32 idx) {
    u32 o = (t->tlut_off + idx * 2) & 0xFFFFF;
    u32 v = (u32)tmem[o] << 8 | tmem[o + 1];
    if (t->tlut_fmt == 0) return rgba(v & 255, v & 255, v & 255, v >> 8);
    return t->tlut_fmt == 1 ? c565(v) : c5a3(v);
}
// Fetch one texel (x, y already wrapped). Formats are stored in tiles (block-linear).
static u32 texel(const Tex *t, int x, int y) {
    static const int bw[16] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0 };
    static const int bh[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };
    int f = t->fmt; if (!bw[f]) return 0xFF00FFFF;
    int bx = x / bw[f], by = y / bh[f], ix = x % bw[f], iy = y % bh[f];
    int bpr = (t->w + bw[f] - 1) / bw[f];
    u32 blk = (u32)(by * bpr + bx);
    switch (f) {
    case 0: { u32 a = t->addr + blk * 32 + iy * 4 + ix / 2; u32 v = M8(a); v = (ix & 1) ? v & 15 : v >> 4; v *= 17; return rgba(v, v, v, v); }
    case 1: { u32 v = M8(t->addr + blk * 32 + iy * 8 + ix); return rgba(v, v, v, v); }
    case 2: { u32 v = M8(t->addr + blk * 32 + iy * 8 + ix); u32 i = (v & 15) * 17, a = (v >> 4) * 17; return rgba(i, i, i, a); }
    case 3: { u32 v = M16(t->addr + blk * 32 + (iy * 4 + ix) * 2); return rgba(v & 255, v & 255, v & 255, v >> 8); }
    case 4: return c565(M16(t->addr + blk * 32 + (iy * 4 + ix) * 2));
    case 5: return c5a3(M16(t->addr + blk * 32 + (iy * 4 + ix) * 2));
    case 6: { u32 a = t->addr + blk * 64 + (iy * 4 + ix) * 2;
              return rgba(M8(a + 1), M8(a + 32), M8(a + 33), M8(a)); }
    case 8: { u32 v = M8(t->addr + blk * 32 + iy * 4 + ix / 2); v = (ix & 1) ? v & 15 : v >> 4; return tlut_color(t, v); }
    case 9: return tlut_color(t, M8(t->addr + blk * 32 + iy * 8 + ix));
    case 10: return tlut_color(t, M16(t->addr + blk * 32 + (iy * 4 + ix) * 2) & 0x3FFF);
    case 14: {   // CMPR: 8x8 tile of four DXT1 4x4 sub-blocks
        int sub = (iy / 4) * 2 + (ix / 4); u32 a = t->addr + blk * 32 + sub * 8;
        u32 c0 = M16(a), c1 = M16(a + 2), bits = M32(a + 4);
        u32 p0 = c565(c0), p1 = c565(c1), pal[4] = { p0, p1, 0, 0 };
        #define CH(c, s) (((c) >> (s)) & 255)
        if (c0 > c1) {
            pal[2] = rgba((2 * CH(p0, 24) + CH(p1, 24)) / 3, (2 * CH(p0, 16) + CH(p1, 16)) / 3, (2 * CH(p0, 8) + CH(p1, 8)) / 3, 255);
            pal[3] = rgba((CH(p0, 24) + 2 * CH(p1, 24)) / 3, (CH(p0, 16) + 2 * CH(p1, 16)) / 3, (CH(p0, 8) + 2 * CH(p1, 8)) / 3, 255);
        } else {
            pal[2] = rgba((CH(p0, 24) + CH(p1, 24)) / 2, (CH(p0, 16) + CH(p1, 16)) / 2, (CH(p0, 8) + CH(p1, 8)) / 2, 255);
            pal[3] = 0;
        }
        int sel = (bits >> (30 - 2 * ((iy & 3) * 4 + (ix & 3)))) & 3;
        return pal[sel];
    }
    }
    return 0xFF00FFFF;
}
static int wrap(int v, int n, int mode) {
    if (mode == 1) { v %= n; return v < 0 ? v + n : v; }
    if (mode == 2) { int p = 2 * n; v %= p; if (v < 0) v += p; return v < n ? v : p - 1 - v; }
    return v < 0 ? 0 : v >= n ? n - 1 : v;
}
static void sample(int map, float s, float tc, float out[4]) {
    Tex t; get_tex(map, &t);
    float fx = s * t.w - 0.5f, fy = tc * t.h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float ax = fx - x0, ay = fy - y0;
    float acc[4] = { 0 };
    for (int k = 0; k < 4; k++) {
        int xx = wrap(x0 + (k & 1), t.w, t.wrap_s), yy = wrap(y0 + (k >> 1), t.h, t.wrap_t);
        u32 c = texel(&t, xx, yy);
        float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
        acc[0] += wgt * (c >> 24); acc[1] += wgt * ((c >> 16) & 255); acc[2] += wgt * ((c >> 8) & 255); acc[3] += wgt * (c & 255);
    }
    for (int k = 0; k < 4; k++) out[k] = acc[k] / 255.f;
}

// ---------------------------------------------------------------- TEV (texture environment) combiner
static void tev_reg(int r, float out[4]) {   // C0..C2 / PREV registers set via BP 0xE0-0xE7
    u32 lo = bp[0xE0 + 2 * r], hi = bp[0xE1 + 2 * r];
    int ra = lo & 0x7FF, a = (lo >> 12) & 0x7FF, b = hi & 0x7FF, g = (hi >> 12) & 0x7FF;
    #define S11(v) ((v) & 0x400 ? (int)(v) - 0x800 : (int)(v))
    out[0] = S11(ra) / 255.f; out[3] = S11(a) / 255.f; out[2] = S11(b) / 255.f; out[1] = S11(g) / 255.f;
}
static float g_konst[4][4];          // konst registers (BP 0xE0-0xE7 with bit 23 set)
static float g_reg[4][4];            // PREV, C0, C1, C2 (initial values from BP)
static void konst_value(int stage, int alpha, float out[4]) {
    u32 ks = bp[0xF6 + stage / 2];
    int sel = alpha ? (ks >> ((stage & 1) ? 19 : 9)) & 31 : (ks >> ((stage & 1) ? 14 : 4)) & 31;
    if (sel <= 7) { float v = (8 - sel) / 8.f; out[0] = out[1] = out[2] = out[3] = v; return; }
    if (sel >= 12 && sel <= 15) { memcpy(out, g_konst[sel - 12], 16); return; }
    int k = (sel - 16) & 3, comp = (sel - 16) >> 2;   // 16-31: K0_R,K1_R,K2_R,K3_R,K0_G,...
    float v = g_konst[k][comp]; out[0] = out[1] = out[2] = out[3] = v;
}
static void tev(const float ras[2][4], const float tex[8][4], int texmap_of_stage[16], float out[4]) {
    int nst = ((bp[0x00] >> 10) & 15) + 1;
    float R[4][4]; memcpy(R, g_reg, sizeof R);
    for (int s = 0; s < nst; s++) {
        u32 order = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        int map = order & 7, en = (order >> 6) & 1, chan = (order >> 7) & 7;
        float T[4] = { 1, 1, 1, 1 }, C[4] = { 0, 0, 0, 0 };
        if (en && texmap_of_stage[s] >= 0) memcpy(T, tex[s], 16);
        if (chan == 0 || chan == 2 || chan == 4) memcpy(C, ras[0], 16);
        else if (chan == 1 || chan == 3 || chan == 5) memcpy(C, ras[1], 16);
        float K[4], KA[4]; konst_value(s, 0, K); konst_value(s, 1, KA);
        u32 ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
        // color
        float in[4][3];
        for (int i = 0; i < 4; i++) {
            int sel = (ce >> (12 - 4 * i)) & 15; float *dst = in[i];   // a(12-15), b(8-11), c(4-7), d(0-3)
            switch (sel) {
            case 0: case 2: case 4: case 6: { int r = sel / 2; dst[0] = R[r][0]; dst[1] = R[r][1]; dst[2] = R[r][2]; break; }
            case 1: case 3: case 5: case 7: { int r = sel / 2; dst[0] = dst[1] = dst[2] = R[r][3]; break; }
            case 8: dst[0] = T[0]; dst[1] = T[1]; dst[2] = T[2]; break;
            case 9: dst[0] = dst[1] = dst[2] = T[3]; break;
            case 10: dst[0] = C[0]; dst[1] = C[1]; dst[2] = C[2]; break;
            case 11: dst[0] = dst[1] = dst[2] = C[3]; break;
            case 12: dst[0] = dst[1] = dst[2] = 1; break;
            case 13: dst[0] = dst[1] = dst[2] = 0.5f; break;
            case 14: dst[0] = K[0]; dst[1] = K[1]; dst[2] = K[2]; break;
            default: dst[0] = dst[1] = dst[2] = 0; break;
            }
        }
        float ain[4];
        for (int i = 0; i < 4; i++) {
            int sel = (ae >> (13 - 3 * i)) & 7;   // a(13-15), b(10-12), c(7-9), d(4-6)
            switch (sel) {
            case 0: case 1: case 2: case 3: ain[i] = R[sel][3]; break;
            case 4: ain[i] = T[3]; break;
            case 5: ain[i] = C[3]; break;
            case 6: ain[i] = KA[3]; break;
            default: ain[i] = 0; break;
            }
        }
        static const float bias_t[4] = { 0, 0.5f, -0.5f, 0 }, scale_t[4] = { 1, 2, 4, 0.5f };
        int cb = (ce >> 16) & 3, csub = (ce >> 18) & 1, cclamp = (ce >> 19) & 1, cscale = (ce >> 20) & 3, cdest = (ce >> 22) & 3;
        int ab = (ae >> 16) & 3, asub = (ae >> 18) & 1, aclamp = (ae >> 19) & 1, ascale = (ae >> 20) & 3, adest = (ae >> 22) & 3;
        float res[4];
        for (int k = 0; k < 3; k++) {
            float l = in[0][k] * (1 - in[2][k]) + in[1][k] * in[2][k];
            float v = cb == 3 ? in[3][k] + l : (in[3][k] + (csub ? -l : l) + bias_t[cb]) * scale_t[cscale];
            res[k] = cclamp ? fminf(1, fmaxf(0, v)) : fminf(4, fmaxf(-4, v));
        }
        {
            float l = ain[0] * (1 - ain[2]) + ain[1] * ain[2];
            float v = ab == 3 ? ain[3] + l : (ain[3] + (asub ? -l : l) + bias_t[ab]) * scale_t[ascale];
            res[3] = aclamp ? fminf(1, fmaxf(0, v)) : fminf(4, fmaxf(-4, v));
        }
        R[cdest][0] = res[0]; R[cdest][1] = res[1]; R[cdest][2] = res[2]; R[adest][3] = res[3];
        if (s == nst - 1) { out[0] = R[cdest][0]; out[1] = R[cdest][1]; out[2] = R[cdest][2]; out[3] = R[adest][3]; }
    }
    for (int k = 0; k < 4; k++) out[k] = fminf(1, fmaxf(0, out[k]));
}

// ---------------------------------------------------------------- rasterizer
static int alpha_test(float a) {
    u32 ac = bp[0xF3];
    int r0 = ac & 255, r1 = (ac >> 8) & 255, c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, op = (ac >> 22) & 3;
    int v = (int)(a * 255 + 0.5f);
    #define CMP(c, r) ((c) == 0 ? 0 : (c) == 1 ? v < (r) : (c) == 2 ? v == (r) : (c) == 3 ? v <= (r) : (c) == 4 ? v > (r) : (c) == 5 ? v != (r) : (c) == 6 ? v >= (r) : 1)
    int a0 = CMP(c0, r0), a1 = CMP(c1, r1);
    return op == 0 ? a0 && a1 : op == 1 ? a0 || a1 : op == 2 ? a0 != a1 : a0 == a1;
}
static float blend_factor(int f, int src, const float s[4], const float d[4], int k) {
    switch (f) {
    case 0: return 0; case 1: return 1;
    case 2: return src ? d[k] : s[k];            // DSTCLR / SRCCLR
    case 3: return src ? 1 - d[k] : 1 - s[k];
    case 4: return s[3]; case 5: return 1 - s[3];
    case 6: return d[3]; case 7: return 1 - d[3];
    }
    return 1;
}
static void put_pixel(int x, int y, float z, const float c[4]) {
    long i = (long)y * EW + x;
    u32 zm = bp[0x40];
    if (zm & 1) {
        int fn = (zm >> 1) & 7; float dz = efb_z[i];
        int pass = fn == 0 ? 0 : fn == 1 ? z < dz : fn == 2 ? z == dz : fn == 3 ? z <= dz : fn == 4 ? z > dz : fn == 5 ? z != dz : fn == 6 ? z >= dz : 1;
        if (!pass) return;
        if (zm & 16) efb_z[i] = z;
    }
    u32 bm = bp[0x41];
    u32 dc = efb_c[i];
    float d[4] = { (dc >> 24) / 255.f, ((dc >> 16) & 255) / 255.f, ((dc >> 8) & 255) / 255.f, (dc & 255) / 255.f };
    float o[4]; memcpy(o, c, 16);
    if (bm & 1) {
        int sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
        for (int k = 0; k < 4; k++) {
            float v = (bm & 0x800) ? d[k] - c[k] : c[k] * blend_factor(sf, 1, c, d, k) + d[k] * blend_factor(df, 0, c, d, k);
            o[k] = fminf(1, fmaxf(0, v));
        }
    }
    if (!(bm & 8)) { o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; }   // color update disabled
    if (!(bm & 16)) o[3] = d[3];                                 // alpha update disabled
    efb_c[i] = rgba((u32)(o[0] * 255 + .5f), (u32)(o[1] * 255 + .5f), (u32)(o[2] * 255 + .5f), (u32)(o[3] * 255 + .5f));
}

static void to_screen(const Out *o, float *sx, float *sy, float *sz) {
    float wd = xf_f(0x101A), ht = xf_f(0x101B), zr = xf_f(0x101C), xo = xf_f(0x101D), yo = xf_f(0x101E), fz = xf_f(0x101F);
    float iw = 1.f / o->w;
    *sx = (o->x * iw * wd + xo - 342.f) * g_scale;
    *sy = (o->y * iw * ht + yo - 342.f) * g_scale;
    *sz = (o->z * iw * zr + fz) / 16777215.f;
}

static void draw_tri(const Out *a, const Out *b, const Out *c) {
    st_tri++;
    if (a->w <= 1e-6f || b->w <= 1e-6f || c->w <= 1e-6f) { st_w++; return; }   // TODO: proper near-plane clipping
    const Out *v[3] = { a, b, c };
    float X[3], Y[3], Z[3], IW[3];
    for (int k = 0; k < 3; k++) { to_screen(v[k], &X[k], &Y[k], &Z[k]); IW[k] = 1.f / v[k]->w; }
    float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (st_samples < 6) { st_samples++; fprintf(stderr, "[video] tri screen (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) z %.3f w %.3f | vp %g %g %g %g proj %s\n",
        X[0], Y[0], X[1], Y[1], X[2], Y[2], Z[0], a->w, xf_f(0x101A), xf_f(0x101B), xf_f(0x101D), xf_f(0x101E), (xfreg[0x26] & 1) ? "ortho" : "persp"); }
    if (fabsf(area) < 1e-9f) { st_area++; return; }
    // Front faces are clockwise on screen (y down) = positive area here. GENMODE cull: 1 = back, 2 = front.
    int cull = (bp[0x00] >> 14) & 3;
    if (cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0)) { st_cull++; return; }
    // scissor (BP 0x20/0x21, offset by 342)
    int stl = bp[0x20], sbr = bp[0x21];
    int sx0 = ((stl >> 12) & 0x7FF) - 342, sy0 = (stl & 0x7FF) - 342, sx1 = ((sbr >> 12) & 0x7FF) - 342, sy1 = (sbr & 0x7FF) - 342;
    int minx = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), maxx = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
    int miny = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), maxy = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
    if (minx < sx0 * g_scale) minx = sx0 * g_scale; if (miny < sy0 * g_scale) miny = sy0 * g_scale;
    if (maxx > (sx1 + 1) * g_scale) maxx = (sx1 + 1) * g_scale; if (maxy > (sy1 + 1) * g_scale) maxy = (sy1 + 1) * g_scale;
    if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx > EW) maxx = EW; if (maxy > EH) maxy = EH;
    if (minx >= maxx || miny >= maxy) { st_empty++; return; }
    int nst = ((bp[0x00] >> 10) & 15) + 1;
    int stage_map[16], stage_coord[16];
    for (int s = 0; s < nst; s++) { u32 o = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0); stage_map[s] = (o >> 6) & 1 ? (int)(o & 7) : -1; stage_coord[s] = (o >> 3) & 7; }
    for (int y = miny; y < maxy; y++) {
        for (int x = minx; x < maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((X[1] - px) * (Y[2] - py) - (X[2] - px) * (Y[1] - py)) / area;
            float w1 = ((X[2] - px) * (Y[0] - py) - (X[0] - px) * (Y[2] - py)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2];
            float iw = w0 * IW[0] + w1 * IW[1] + w2 * IW[2];
            float pw[3] = { w0 * IW[0] / iw, w1 * IW[1] / iw, w2 * IW[2] / iw };   // perspective-correct weights
            float ras[2][4], tex[8][4];
            for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 4; k++)
                ras[ch][k] = pw[0] * v[0]->col[ch][k] + pw[1] * v[1]->col[ch][k] + pw[2] * v[2]->col[ch][k];
            for (int s = 0; s < nst; s++) {
                if (stage_map[s] < 0) continue;
                int tc = stage_coord[s];
                float u = 0, vv = 0, q = 0;
                for (int k = 0; k < 3; k++) { u += pw[k] * v[k]->uv[tc][0]; vv += pw[k] * v[k]->uv[tc][1]; q += pw[k] * v[k]->uv[tc][2]; }
                if (q == 0) q = 1;
                sample(stage_map[s], u / q, vv / q, tex[s]);
            }
            float col[4]; tev(ras, tex, stage_map, col);
            if (!alpha_test(col[3])) { st_alpha++; continue; }
            st_px++;
            put_pixel(x, y, z, col);
        }
    }
}

// ---------------------------------------------------------------- frame output
static void dump_frame(void) {
    if (!g_dump_dir) return;
    u32 src = bp[0x49], wh = bp[0x4A];
    int x0 = (src & 0x3FF) * g_scale, y0 = ((src >> 10) & 0x3FF) * g_scale;
    int w = ((wh & 0x3FF) + 1) * g_scale, h = (((wh >> 10) & 0x3FF) + 1) * g_scale;
    if (x0 + w > EW) w = EW - x0; if (y0 + h > EH) h = EH - y0;
    char path[512]; snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g_dump_dir, (unsigned long long)g_copies);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        u32 c = efb_c[(long)(y0 + y) * EW + x0 + x]; u8 px[3] = { c >> 24, c >> 16, c >> 8 }; fwrite(px, 1, 3, f);
    }
    fclose(f);
}
static void efb_copy(u32 v) {
    int to_xfb = (v >> 14) & 1, clear = (v >> 11) & 1;
    if (to_xfb) {
        g_copies++;
        if (g_render && g_copies % g_dump_every == 0) dump_frame();
        g_render = g_dump_dir && ((g_copies + 1) % g_dump_every == 0);   // only rasterize frames we save
    }
    if (clear) {
        u32 ar = bp[0x4F], gb = bp[0x50];
        u32 c = rgba(ar & 255, (gb >> 8) & 255, gb & 255, (ar >> 8) & 255);
        float z = (bp[0x51] & 0xFFFFFF) / 16777215.f;
        u32 src = bp[0x49], wh = bp[0x4A];
        int x0 = (src & 0x3FF) * g_scale, y0 = ((src >> 10) & 0x3FF) * g_scale;
        int w = ((wh & 0x3FF) + 1) * g_scale, h = (((wh >> 10) & 0x3FF) + 1) * g_scale;
        for (int y = y0; y < y0 + h && y < EH; y++) for (int x = x0; x < x0 + w && x < EW; x++) {
            long i = (long)y * EW + x;
            if (bp[0x41] & 8) efb_c[i] = c;
            if (bp[0x40] & 16) efb_z[i] = z;
        }
    }
}

// ---------------------------------------------------------------- register writes
static void bp_write(u32 v) {
    u32 reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) { bp[0xFE] = val; return; }                  // mask for the next write
    if (bp[0xFE]) { val = (bp[reg] & ~bp[0xFE]) | (val & bp[0xFE]); bp[0xFE] = 0; }
    bp[reg] = val;
    if (reg >= 0xE0 && reg <= 0xE7) {                               // TEV color / konst registers
        int r = (reg - 0xE0) / 2;
        float c[4]; tev_reg(r, c);
        int konst = (bp[0xE0 + 2 * r] >> 23) & 1;
        if (konst) memcpy(g_konst[r], c, 16); else memcpy(g_reg[r], c, 16);
    }
    if (reg == 0x65) {                                              // load TLUT into TMEM
        u32 src = 0x80000000u | ((bp[0x64] & 0x00FFFFFF) << 5);
        u32 dst = (val & 0x3FF) << 9, count = ((val >> 10) & 0x7FF) * 16;
        for (u32 k = 0; k < count * 2 && dst + k < (1u << 20); k++) tmem[dst + k] = (u8)V8(src + k);
    }
    if (reg == 0x52) efb_copy(val);
}
static void xf_write(u32 addr, u32 v) {
    if (addr < 0x1000) xfmem[addr] = v;
    else if (addr < 0x1100) xfreg[addr & 0xFF] = v;
    if (addr == 0x1018) cp[0x30] = v;     // XF copies of the matrix index registers
    if (addr == 0x1019) cp[0x40] = v;
}

// ---------------------------------------------------------------- primitives
static void draw_prims(int prim, int vat, int n, u32 *p) {
    static Vtx vin[4096]; static Out vout[4096];
    if (n > 4096) n = 4096;
    for (int k = 0; k < n; k++) { memset(&vin[k], 0, sizeof vin[k]); read_vertex(p, vat, &vin[k]); }
    g_video_draws++;
    if (!g_render) return;
    for (int k = 0; k < n; k++) transform(&vin[k], &vout[k]);
    switch (prim) {
    case 0: case 1: for (int k = 0; k + 3 < n; k += 4) { draw_tri(&vout[k], &vout[k + 1], &vout[k + 2]); draw_tri(&vout[k], &vout[k + 2], &vout[k + 3]); } break;
    case 2: for (int k = 0; k + 2 < n; k += 3) draw_tri(&vout[k], &vout[k + 1], &vout[k + 2]); break;
    case 3: for (int k = 0; k + 2 < n; k++) { if (k & 1) draw_tri(&vout[k + 1], &vout[k], &vout[k + 2]); else draw_tri(&vout[k], &vout[k + 1], &vout[k + 2]); } break;
    case 4: for (int k = 1; k + 1 < n; k++) draw_tri(&vout[0], &vout[k], &vout[k + 1]); break;
    default: break;   // lines/points: not drawn yet
    }
}

// Parse commands from guest memory [p, end). Returns bytes consumed (stops at an incomplete command).
static u32 parse(u32 p, u32 end, int depth);
static u32 run_display_list(u32 addr, u32 size, int depth) { if (depth < 4) parse(addr, addr + size, depth + 1); return 0; }

static u32 parse(u32 p, u32 end, int depth) {
    u32 start = p;
    while (p < end) {
        u32 op = V8(p), s = p;
        if (op == 0x00 || op == 0x48) { p += 1; continue; }
        if (op == 0x08) { if (p + 6 > end) break; cp[V8(p + 1)] = V32(p + 2); p += 6; continue; }
        if (op == 0x10) {
            if (p + 5 > end) break;
            u32 h = V32(p + 1), n = (h >> 16) + 1, a = h & 0xFFFF;
            if (p + 5 + 4 * n > end) break;
            for (u32 k = 0; k < n; k++) xf_write(a + k, V32(p + 5 + 4 * k));
            p += 5 + 4 * n; continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {   // indexed XF load from array 12-15
            if (p + 5 > end) break;
            u32 h = V32(p + 1), idx = h >> 16, len = ((h >> 12) & 15) + 1, a = h & 0xFFF, arr = 12 + ((op >> 3) & 3);
            u32 src = (cp[0xA0 + arr] & 0x3FFFFFFF) + idx * (cp[0xB0 + arr] & 0xFF);
            for (u32 k = 0; k < len; k++) xf_write(a + k, V32(0x80000000u | (src + 4 * k)));
            p += 5; continue;
        }
        if (op == 0x40) { if (p + 9 > end) break; run_display_list(0x80000000u | (V32(p + 1) & 0x1FFFFFFF), V32(p + 5), depth); p += 9; continue; }
        if (op == 0x61) { if (p + 5 > end) break; bp_write(V32(p + 1)); p += 5; continue; }
        if (op >= 0x80 && op < 0xC0) {
            if (p + 3 > end) break;
            int n = V16(p + 1); u32 q = p + 3;
            // vertices may still be arriving: measure the size with a dry run over a scratch copy
            draw_prims((op >> 3) & 7, op & 7, n, &q);
            if (q > end) { p = s; break; }
            p = q; continue;
        }
        // unknown opcode: skip a byte so we can resynchronise
        static int warned; if (warned++ < 5) fprintf(stderr, "[video] unknown GX opcode %02X at %08X\n", op, p);
        p += 1;
    }
    return p - start;
}

// ---------------------------------------------------------------- FIFO input (from gx.c)
void video_fifo(u32 v, int size) {          // bytes written to the write-gather pipe, big-endian
    if (g_len + 4 > sizeof g_buf) g_len = 0;
    for (int k = size - 1; k >= 0; k--) g_buf[g_len++] = (u8)(v >> (8 * k));
}
void video_flush(void) {                    // decode every complete command buffered so far
    if (!g_len) return;
    u32 used = parse(0xF0000000u, 0xF0000000u + g_len, 0);
    if (used) { memmove(g_buf, g_buf + used, g_len - used); g_len -= used; }
}
void video_dump_stats(void) {
    fprintf(stderr, "[video] tris %llu: w<=0 %llu, degenerate %llu, culled %llu, off-screen %llu | pixels %llu, alpha-rejected %llu\n",
            (unsigned long long)st_tri, (unsigned long long)st_w, (unsigned long long)st_area, (unsigned long long)st_cull,
            (unsigned long long)st_empty, (unsigned long long)st_px, (unsigned long long)st_alpha);
    fprintf(stderr, "[video] %llu draw calls, %llu display copies, %u bytes buffered, first bytes %02X %02X %02X %02X\n",
            (unsigned long long)g_video_draws, (unsigned long long)g_copies, g_len, g_buf[0], g_buf[1], g_buf[2], g_buf[3]);
}
