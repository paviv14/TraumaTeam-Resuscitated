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
#include "platform.h"
#include "port.h"
#include "gx_state.h"

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
    g_render = g_dump_dir != NULL || g_window_open;   // rasterize only when frames are being looked at
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
    static const int bh[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };   // = TBW/TBH
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

// ---------------------------------------------------------------- decoded texture cache
// Textures are decoded to RGBA once and kept; once per displayed frame (or after a TLUT load) a
// cached texture is revalidated by hashing its source bytes, and re-decoded only if they changed.
static TexEntry g_tcache[1024];
static u32 g_tgen = 1;               // bumped when cached textures need revalidation
static const int TBW[16] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0 };
static const int TBH[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };
static u64 hash_bytes(const u8 *b, u32 n, u64 h) {
    u32 i = 0;
    for (; i + 8 <= n; i += 8) { u64 v; memcpy(&v, b + i, 8); h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static u64 tex_hash(const Tex *t) {
    int f = t->fmt; if (!TBW[f]) return 0;
    u32 blocks = (u32)((t->w + TBW[f] - 1) / TBW[f]) * (u32)((t->h + TBH[f] - 1) / TBH[f]);
    u32 bytes = blocks * (f == 6 ? 64 : 32), p = t->addr & 0x1FFFFFFF;
    if (!(p + bytes <= 0x01800000u || (p >= 0x10000000u && p + bytes <= 0x14000000u))) return 1;
    u64 h = hash_bytes(g_mem + p, bytes, 0x9E3779B97F4A7C15ull);
    if (f >= 8 && f <= 10) {
        u32 n = (f == 8 ? 16 : f == 9 ? 256 : 16384) * 2, o = (u32)t->tlut_off & 0xFFFFF;
        if (o + n > (1u << 20)) n = (1u << 20) - o;
        h = hash_bytes(tmem + o, n, h ^ (u64)t->tlut_fmt);
    }
    return h;
}
static TexEntry *tex_decode(const Tex *t, u32 img0, u32 tlutreg) {
    int pal = t->fmt >= 8 && t->fmt <= 10;
    u32 key = (t->addr >> 5) ^ (img0 * 2654435761u) ^ (pal ? tlutreg * 40503u : 0);
    TexEntry *e = NULL, *empty = NULL;
    for (int probe = 0; probe < 8; probe++) {
        TexEntry *c = &g_tcache[(key + probe) & 1023];
        if (c->px && c->addr == t->addr && c->img0 == img0 && (!pal || c->tlut == tlutreg)) { e = c; break; }
        if (!c->px && !empty) empty = c;
    }
    int fresh = !e;
    if (!e) e = empty ? empty : &g_tcache[(key + (g_tgen & 7)) & 1023];   // evict a slot if all are taken
    if (!fresh && e->gen == g_tgen) return e;
    u64 h = tex_hash(t);
    e->gen = g_tgen;
    if (!fresh && h == e->hash) return e;
    size_t n = (size_t)t->w * t->h;
    if (e->cap < n) { free(e->px); e->px = malloc(n * 4); e->cap = n; }
    for (int y = 0; y < t->h; y++) for (int x = 0; x < t->w; x++) e->px[(size_t)y * t->w + x] = texel(t, x, y);
    e->addr = t->addr; e->img0 = img0; e->tlut = tlutreg; e->w = t->w; e->h = t->h; e->hash = h; e->version++;
    return e;
}

// ---------------------------------------------------------------- per-draw pipeline state
// Everything the pixel loop (or the GPU) needs, decoded from BP registers once per draw call.
static Setup S;

static void setup_draw(void) {
    S.nst = ((bp[0x00] >> 10) & 15) + 1;
    S.cull = (bp[0x00] >> 14) & 3;
    memcpy(S.reg, g_reg, sizeof S.reg);
    int used[8] = { 0 };
    for (int s = 0; s < S.nst; s++) {
        Stage *g = &S.st[s];
        u32 o = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        g->map = (o >> 6) & 1 ? (int)(o & 7) : -1; g->coord = (o >> 3) & 7;
        int chan = (o >> 7) & 7;
        g->chan = (chan == 0 || chan == 2 || chan == 4) ? 0 : (chan == 1 || chan == 3 || chan == 5) ? 1 : -1;
        float K[4], KA[4]; konst_value(s, 0, K); konst_value(s, 1, KA);
        memcpy(g->K, K, 16); g->KA = KA[3];
        u32 ce = bp[0xC0 + 2 * s], ae = bp[0xC1 + 2 * s];
        for (int i = 0; i < 4; i++) { g->csel[i] = (ce >> (12 - 4 * i)) & 15; g->asel[i] = (ae >> (13 - 3 * i)) & 7; }
        g->cb = (ce >> 16) & 3; g->csub = (ce >> 18) & 1; g->cclamp = (ce >> 19) & 1; g->cscale = (ce >> 20) & 3; g->cdest = (ce >> 22) & 3;
        g->ab = (ae >> 16) & 3; g->asub = (ae >> 18) & 1; g->aclamp = (ae >> 19) & 1; g->ascale = (ae >> 20) & 3; g->adest = (ae >> 22) & 3;
        int rs = ae & 3, ts = (ae >> 2) & 3;
        for (int k = 0; k < 4; k++) {
            g->rsw[k] = (bp[0xF6 + 2 * rs + (k >> 1)] >> (2 * (k & 1))) & 3;
            g->tsw[k] = (bp[0xF6 + 2 * ts + (k >> 1)] >> (2 * (k & 1))) & 3;
        }
        if (g->map >= 0) used[g->map] = 1;
    }
    for (int m = 0; m < 8; m++) {
        S.tex[m].px = NULL; S.tex[m].e = NULL;
        if (!used[m]) continue;
        Tex t; get_tex(m, &t);
        int b = m < 4 ? 0 : 0x20, mm = m & 3;
        u32 img0 = bp[0x88 + b + mm], mode0 = bp[0x80 + b + mm], tlut = bp[0x98 + b + mm];
        S.tex[m].e = tex_decode(&t, img0, tlut); S.tex[m].px = S.tex[m].e->px;
        {   // debug: TTPC_TEXDUMP=<addr>,<frame> saves that texture (as decoded) once the frame is reached
            static u32 want, at; static int init, done;
            if (!init) { init = 1; const char *e = getenv("TTPC_TEXDUMP"); if (e) sscanf(e, "%x,%u", &want, &at); }
            if (want && !done && (t.addr & 0x1FFFFFFF) == (want & 0x1FFFFFFF) && g_copies >= at) {
                done = 1; char path[256]; snprintf(path, sizeof path, "texdump_%08X_f%llu.ppm", want, (unsigned long long)g_copies);
                FILE *f = fopen(path, "wb");
                if (f) { fprintf(f, "P6\n%d %d\n255\n", t.w, t.h);
                         for (int k = 0; k < t.w * t.h; k++) { u32 c = S.tex[m].px[k]; u8 rgb[3] = { (u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8) }; fwrite(rgb, 1, 3, f); }
                         fclose(f); fprintf(stderr, "[video] dumped texture %08X -> %s\n", want, path); }
            }
        }
        S.tex[m].w = t.w; S.tex[m].h = t.h; S.tex[m].ws = t.wrap_s; S.tex[m].wt = t.wrap_t;
        S.tex[m].linear = (mode0 >> 4) & 1;                       // magnification filter
    }
    u32 ac = bp[0xF3];
    S.ac_r0 = ac & 255; S.ac_r1 = (ac >> 8) & 255; S.ac_c0 = (ac >> 16) & 7; S.ac_c1 = (ac >> 19) & 7; S.ac_op = (ac >> 22) & 3;
    S.zm = bp[0x40]; S.bm = bp[0x41];
    int stl = bp[0x20], sbr = bp[0x21];
    S.sx0 = ((stl >> 12) & 0x7FF) - 342; S.sy0 = (stl & 0x7FF) - 342; S.sx1 = ((sbr >> 12) & 0x7FF) - 342; S.sy1 = (sbr & 0x7FF) - 342;
    for (int k = 0; k < 6; k++) S.vp[k] = xf_f(0x101A + k);
}

static inline void samp(const TexV *t, float s, float tc, float out[4]) {
    if (!t->px) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    if (!t->linear) {
        int x = wrap((int)floorf(s * t->w), t->w, t->ws), y = wrap((int)floorf(tc * t->h), t->h, t->wt);
        u32 c = t->px[y * t->w + x];
        out[0] = (c >> 24) * (1 / 255.f); out[1] = ((c >> 16) & 255) * (1 / 255.f); out[2] = ((c >> 8) & 255) * (1 / 255.f); out[3] = (c & 255) * (1 / 255.f);
        return;
    }
    float fx = s * t->w - 0.5f, fy = tc * t->h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy); float ax = fx - x0, ay = fy - y0;
    int xa = wrap(x0, t->w, t->ws), xb = wrap(x0 + 1, t->w, t->ws), ya = wrap(y0, t->h, t->wt), yb = wrap(y0 + 1, t->h, t->wt);
    u32 c00 = t->px[ya * t->w + xa], c10 = t->px[ya * t->w + xb], c01 = t->px[yb * t->w + xa], c11 = t->px[yb * t->w + xb];
    float w00 = (1 - ax) * (1 - ay), w10 = ax * (1 - ay), w01 = (1 - ax) * ay, w11 = ax * ay;
    for (int k = 0; k < 4; k++) {
        int sh = 24 - 8 * k;
        out[k] = (w00 * ((c00 >> sh) & 255) + w10 * ((c10 >> sh) & 255) + w01 * ((c01 >> sh) & 255) + w11 * ((c11 >> sh) & 255)) * (1 / 255.f);
    }
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void tev_fast(const float ras[2][4], const float tex[16][4], float out[4]) {
    static const float bias_t[4] = { 0, 0.5f, -0.5f, 0 }, scale_t[4] = { 1, 2, 4, 0.5f };
    float R[4][4]; memcpy(R, S.reg, sizeof R);
    for (int s = 0; s < S.nst; s++) {
        const Stage *g = &S.st[s];
        float T[4] = { 1, 1, 1, 1 }, C[4] = { 0, 0, 0, 0 };
        if (g->map >= 0) for (int k = 0; k < 4; k++) T[k] = tex[s][g->tsw[k]];
        if (g->chan >= 0) for (int k = 0; k < 4; k++) C[k] = ras[g->chan][g->rsw[k]];
        float in[4][3];
        for (int i = 0; i < 4; i++) {
            float *d = in[i]; int sel = g->csel[i];
            switch (sel) {
            case 0: case 2: case 4: case 6: { const float *r = R[sel / 2]; d[0] = r[0]; d[1] = r[1]; d[2] = r[2]; break; }
            case 1: case 3: case 5: case 7: d[0] = d[1] = d[2] = R[sel / 2][3]; break;
            case 8: d[0] = T[0]; d[1] = T[1]; d[2] = T[2]; break;
            case 9: d[0] = d[1] = d[2] = T[3]; break;
            case 10: d[0] = C[0]; d[1] = C[1]; d[2] = C[2]; break;
            case 11: d[0] = d[1] = d[2] = C[3]; break;
            case 12: d[0] = d[1] = d[2] = 1; break;
            case 13: d[0] = d[1] = d[2] = 0.5f; break;
            case 14: d[0] = g->K[0]; d[1] = g->K[1]; d[2] = g->K[2]; break;
            default: d[0] = d[1] = d[2] = 0; break;
            }
        }
        float ain[4];
        for (int i = 0; i < 4; i++) {
            int sel = g->asel[i];
            ain[i] = sel < 4 ? R[sel][3] : sel == 4 ? T[3] : sel == 5 ? C[3] : sel == 6 ? g->KA : 0;
        }
        for (int k = 0; k < 3; k++) {
            float l = in[0][k] * (1 - in[2][k]) + in[1][k] * in[2][k];
            float v = g->cb == 3 ? in[3][k] + l : (in[3][k] + (g->csub ? -l : l) + bias_t[g->cb]) * scale_t[g->cscale];
            R[g->cdest][k] = g->cclamp ? clampf(v, 0, 1) : clampf(v, -4, 4);
        }
        float l = ain[0] * (1 - ain[2]) + ain[1] * ain[2];
        float v = g->ab == 3 ? ain[3] + l : (ain[3] + (g->asub ? -l : l) + bias_t[g->ab]) * scale_t[g->ascale];
        R[g->adest][3] = g->aclamp ? clampf(v, 0, 1) : clampf(v, -4, 4);
        if (s == S.nst - 1) { out[0] = R[g->cdest][0]; out[1] = R[g->cdest][1]; out[2] = R[g->cdest][2]; out[3] = R[g->adest][3]; }
    }
    for (int k = 0; k < 4; k++) out[k] = clampf(out[k], 0, 1);
}

// ---------------------------------------------------------------- rasterizer
static inline int alpha_test(float a) {
    int v = (int)(a * 255 + 0.5f);
    #define CMP(c, r) ((c) == 0 ? 0 : (c) == 1 ? v < (r) : (c) == 2 ? v == (r) : (c) == 3 ? v <= (r) : (c) == 4 ? v > (r) : (c) == 5 ? v != (r) : (c) == 6 ? v >= (r) : 1)
    int a0 = CMP(S.ac_c0, S.ac_r0), a1 = CMP(S.ac_c1, S.ac_r1);
    int op = S.ac_op;
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
static inline void put_pixel(int x, int y, float z, const float c[4]) {
    long i = (long)y * EW + x;
    u32 zm = S.zm;
    if (zm & 1) {
        int fn = (zm >> 1) & 7; float dz = efb_z[i];
        int pass = fn == 0 ? 0 : fn == 1 ? z < dz : fn == 2 ? z == dz : fn == 3 ? z <= dz : fn == 4 ? z > dz : fn == 5 ? z != dz : fn == 6 ? z >= dz : 1;
        if (!pass) return;
        if (zm & 16) efb_z[i] = z;
    }
    u32 bm = S.bm;
    if (!(bm & 1) && (bm & 8) && (bm & 16)) {   // plain write: the common case
        efb_c[i] = rgba((u32)(c[0] * 255 + .5f), (u32)(c[1] * 255 + .5f), (u32)(c[2] * 255 + .5f), (u32)(c[3] * 255 + .5f));
        return;
    }
    u32 dc = efb_c[i];
    float d[4] = { (dc >> 24) / 255.f, ((dc >> 16) & 255) / 255.f, ((dc >> 8) & 255) / 255.f, (dc & 255) / 255.f };
    float o[4]; memcpy(o, c, 16);
    if (bm & 1) {
        int sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
        for (int k = 0; k < 4; k++) {
            float v = (bm & 0x800) ? d[k] - c[k] : c[k] * blend_factor(sf, 1, c, d, k) + d[k] * blend_factor(df, 0, c, d, k);
            o[k] = clampf(v, 0, 1);
        }
    }
    if (!(bm & 8)) { o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; }   // color update disabled
    if (!(bm & 16)) o[3] = d[3];                                 // alpha update disabled
    efb_c[i] = rgba((u32)(o[0] * 255 + .5f), (u32)(o[1] * 255 + .5f), (u32)(o[2] * 255 + .5f), (u32)(o[3] * 255 + .5f));
}

static void to_screen(const Out *o, float *sx, float *sy, float *sz) {
    float iw = 1.f / o->w;
    *sx = (o->x * iw * S.vp[0] + S.vp[3] - 342.f) * g_scale;
    *sy = (o->y * iw * S.vp[1] + S.vp[4] - 342.f) * g_scale;
    *sz = (o->z * iw * S.vp[2] + S.vp[5]) / 16777215.f;
}

static void draw_tri(const Out *a, const Out *b, const Out *c) {
    st_tri++;
    if (a->w <= 1e-6f || b->w <= 1e-6f || c->w <= 1e-6f) { st_w++; return; }   // TODO: proper near-plane clipping
    const Out *v[3] = { a, b, c };
    float X[3], Y[3], Z[3], IW[3];
    for (int k = 0; k < 3; k++) { to_screen(v[k], &X[k], &Y[k], &Z[k]); IW[k] = 1.f / v[k]->w; }
    float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (fabsf(area) < 1e-9f) { st_area++; return; }
    // Front faces are clockwise on screen (y down) = positive area here. GENMODE cull: 1 = back, 2 = front.
    int cull = S.cull;
    if (cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0)) { st_cull++; return; }
    int minx = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), maxx = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
    int miny = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), maxy = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
    if (minx < S.sx0 * g_scale) minx = S.sx0 * g_scale; if (miny < S.sy0 * g_scale) miny = S.sy0 * g_scale;
    if (maxx > (S.sx1 + 1) * g_scale) maxx = (S.sx1 + 1) * g_scale; if (maxy > (S.sy1 + 1) * g_scale) maxy = (S.sy1 + 1) * g_scale;
    if (minx < 0) minx = 0; if (miny < 0) miny = 0; if (maxx > EW) maxx = EW; if (maxy > EH) maxy = EH;
    if (minx >= maxx || miny >= maxy) { st_empty++; return; }
    // Edge functions are affine in x: step them instead of recomputing per pixel.
    float ia = 1.f / area;
    float A0 = -(Y[2] - Y[1]) * ia, B0 = (X[2] - X[1]) * ia;   // w0 = ((X1-px)(Y2-py) - (X2-px)(Y1-py)) / area
    float A1 = -(Y[0] - Y[2]) * ia, B1 = (X[0] - X[2]) * ia;
    int big = (maxx - minx) * (maxy - miny) > 2048;
    #pragma omp parallel for schedule(dynamic, 8) if (big)
    for (int y = miny; y < maxy; y++) {
        float py = y + 0.5f, px0 = minx + 0.5f;
        float w0 = ((X[1] - px0) * (Y[2] - py) - (X[2] - px0) * (Y[1] - py)) * ia;
        float w1 = ((X[2] - px0) * (Y[0] - py) - (X[0] - px0) * (Y[2] - py)) * ia;
        (void)B0; (void)B1;
        for (int x = minx; x < maxx; x++, w0 += A0, w1 += A1) {
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2];
            float iw = w0 * IW[0] + w1 * IW[1] + w2 * IW[2];
            float inv = 1.f / iw;
            float pw[3] = { w0 * IW[0] * inv, w1 * IW[1] * inv, w2 * IW[2] * inv };   // perspective-correct weights
            float ras[2][4], tex[16][4];
            for (int ch = 0; ch < 2; ch++) for (int k = 0; k < 4; k++)
                ras[ch][k] = pw[0] * v[0]->col[ch][k] + pw[1] * v[1]->col[ch][k] + pw[2] * v[2]->col[ch][k];
            for (int s = 0; s < S.nst; s++) {
                int m = S.st[s].map; if (m < 0) continue;
                int tc = S.st[s].coord;
                float u = pw[0] * v[0]->uv[tc][0] + pw[1] * v[1]->uv[tc][0] + pw[2] * v[2]->uv[tc][0];
                float vv = pw[0] * v[0]->uv[tc][1] + pw[1] * v[1]->uv[tc][1] + pw[2] * v[2]->uv[tc][1];
                float q = pw[0] * v[0]->uv[tc][2] + pw[1] * v[1]->uv[tc][2] + pw[2] * v[2]->uv[tc][2];
                if (q == 0) q = 1;
                samp(&S.tex[m], u / q, vv / q, tex[s]);
            }
            float col[4]; tev_fast(ras, tex, col);
            if (!alpha_test(col[3])) continue;
            put_pixel(x, y, z, col);
        }
    }
}

// ---------------------------------------------------------------- frame output
static void dump_frame(void) {
    if (!g_dump_dir) return;
    u32 src = bp[0x49], wh = bp[0x4A];
    if (g_gpu) {
        int x0 = src & 0x3FF, y0 = (src >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
        static u8 buf[640 * 8 * 528 * 8 * 3];
        int W, H; if (!(W = gpu_read_rgb(x0, y0, w, h, buf))) return; H = W >> 16; W &= 0xFFFF;
        char path[512]; snprintf(path, sizeof path, "%s/frame_%06llu.ppm", g_dump_dir, (unsigned long long)g_copies);
        FILE *f = fopen(path, "wb"); if (!f) return;
        fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(buf, 3, (size_t)W * H, f); fclose(f);
        return;
    }
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
// EFB -> texture copy: the copied rectangle (optionally 2x2 box-downscaled) is encoded into guest
// memory in the requested texture format, block-tiled like real texture data, so later draws (and the
// CPU) see it exactly as on hardware. BP 0x52 fields: bit 9 half scale, bit 15 intensity (YUV luma),
// bits 3-6 target format (fmt = tp >> 1 | (tp & 1) << 3). BP 0x4B dest >> 5, BP 0x4D row stride / 32.
static void efb_copy_to_texture(u32 v) {
    u32 src = bp[0x49], wh = bp[0x4A];
    int x0 = src & 0x3FF, y0 = (src >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    int tp = (v >> 3) & 15, fmt = (tp >> 1) | ((tp & 1) << 3), half = (v >> 9) & 1, inten = (v >> 15) & 1;
    u32 dst = (bp[0x4B] & 0xFFFFFF) << 5, stride = (bp[0x4D] & 0x3FF) << 5;
    if (x0 + w > 640) w = 640 - x0; if (y0 + h > 528) h = 528 - y0;
    if (w <= 0 || h <= 0) return;
    static u32 pix[640 * 528];                    // copied rect at native resolution, R<<24|G<<16|B<<8|A
    if (g_gpu) { if (!gpu_read_rgba(x0, y0, w, h, pix)) return; }
    else for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        long sx = (long)(x0 + x) * g_scale, sy = (long)(y0 + y) * g_scale;
        pix[y * w + x] = efb_c[sy * EW + sx];
    }
    int ow = half ? w / 2 : w, oh = half ? h / 2 : h;
    if (half) for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        u32 a = pix[2 * y * w + 2 * x], b = pix[2 * y * w + 2 * x + 1], c = pix[(2 * y + 1) * w + 2 * x], d = pix[(2 * y + 1) * w + 2 * x + 1];
        u32 o = 0;
        for (int sh = 0; sh < 32; sh += 8) o |= ((((a >> sh) & 255) + ((b >> sh) & 255) + ((c >> sh) & 255) + ((d >> sh) & 255) + 2) / 4) << sh;
        pix[y * ow + x] = o;   // in place: row y reads rows 2y, 2y+1 >= y, already-written cells are behind
    }
    int bw, bh, bsz;
    switch (fmt) {
    case 0: case 2: bw = 8; bh = fmt == 0 ? 8 : 4; bsz = 32; break;      // 4-bit
    case 1: case 7: case 8: case 9: case 10: bw = 8; bh = 4; bsz = 32; break;   // 8-bit
    case 6: bw = 4; bh = 4; bsz = 64; break;                            // RGBA8 (two 32-byte halves)
    default: bw = 4; bh = 4; bsz = 32; break;                           // 16-bit
    }
    int bpr = (ow + bw - 1) / bw;
    if (!stride) stride = (u32)bpr * bsz;
    u32 base = dst & 0x1FFFFFFF;
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        u32 c = pix[y * ow + x];
        u32 R = c >> 24, G = (c >> 16) & 255, B = (c >> 8) & 255, A = c & 255;
        u32 Y = (u32)(0.257f * R + 0.504f * G + 0.098f * B + 16.5f); if (Y > 255) Y = 255;
        u32 I = inten ? Y : R;
        int bx = x / bw, by = y / bh, ix = x % bw, iy = y % bh;
        u32 blk = base + (u32)by * stride + (u32)bx * bsz;
        if (blk + bsz > 0x01800000u && !(blk >= 0x10000000u && blk + bsz <= 0x14000000u)) continue;
        u8 *m = g_mem + blk;
        switch (fmt) {
        case 0: { u8 *q = m + iy * 4 + ix / 2; u8 n = (u8)(I >> 4); *q = (ix & 1) ? (*q & 0xF0) | n : (*q & 0x0F) | (n << 4); break; }
        case 2: { u8 *q = m + iy * 8 + ix; *q = (u8)((A >> 4) << 4 | (I >> 4)); break; }   // IA4 (8 wide)
        case 1: case 8: m[iy * 8 + ix] = (u8)I; break;
        case 7: m[iy * 8 + ix] = (u8)A; break;
        case 9: m[iy * 8 + ix] = (u8)G; break;
        case 10: m[iy * 8 + ix] = (u8)B; break;
        case 3: m[(iy * 4 + ix) * 2] = (u8)A; m[(iy * 4 + ix) * 2 + 1] = (u8)I; break;   // IA8
        case 11: m[(iy * 4 + ix) * 2] = (u8)G; m[(iy * 4 + ix) * 2 + 1] = (u8)R; break;  // RG8
        case 12: m[(iy * 4 + ix) * 2] = (u8)B; m[(iy * 4 + ix) * 2 + 1] = (u8)G; break;  // GB8
        case 4: { u16 p = (u16)((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3)); m[(iy * 4 + ix) * 2] = p >> 8; m[(iy * 4 + ix) * 2 + 1] = (u8)p; break; }
        case 5: { u16 p = A >= 0xE0 ? (u16)(0x8000 | (R >> 3) << 10 | (G >> 3) << 5 | (B >> 3))
                                    : (u16)((A >> 5) << 12 | (R >> 4) << 8 | (G >> 4) << 4 | (B >> 4));
                  m[(iy * 4 + ix) * 2] = p >> 8; m[(iy * 4 + ix) * 2 + 1] = (u8)p; break; }
        case 6: { int o = (iy * 4 + ix) * 2; m[o] = (u8)A; m[o + 1] = (u8)R; m[32 + o] = (u8)G; m[32 + o + 1] = (u8)B; break; }
        default: break;
        }
    }
    static int logged; if (!logged++) fprintf(stderr, "[video] EFB->texture copies active (first: %dx%d -> %dx%d fmt %d at %08X)\n", w, h, ow, oh, fmt, dst);
}

static void efb_copy(u32 v) {
    int to_xfb = (v >> 14) & 1, clear = (v >> 11) & 1;
    if (!to_xfb && g_render) efb_copy_to_texture(v);
    if (!to_xfb && getenv("TTPC_COPYLOG")) {
        static int n; if (n++ < 40) fprintf(stderr, "[efbcopy] tex copy v=%06X src %06X wh %06X dst %08X stride %u frame %llu\n",
            v, bp[0x49], bp[0x4A], (bp[0x4B] & 0xFFFFFF) << 5, bp[0x4D] & 0x3FF, (unsigned long long)g_copies);
    }
    if (to_xfb) {
        g_copies++;
        if (g_render && g_copies % g_dump_every == 0) dump_frame();
        if (g_gpu) {
            u32 src = bp[0x49], wh = bp[0x4A];
            gpu_present(src & 0x3FF, (src >> 10) & 0x3FF, (wh & 0x3FF) + 1, ((wh >> 10) & 0x3FF) + 1);
        } else if (g_window_open && g_render) {
            u32 src = bp[0x49], wh = bp[0x4A];
            int x0 = (src & 0x3FF) * g_scale, y0 = ((src >> 10) & 0x3FF) * g_scale;
            int w = ((wh & 0x3FF) + 1) * g_scale, h = (((wh >> 10) & 0x3FF) + 1) * g_scale;
            if (x0 + w > EW) w = EW - x0; if (y0 + h > EH) h = EH - y0;
            platform_present(efb_c, x0, y0, w, h, EW);
        }
        // Frame skipping: if drawing has fallen behind real time, skip rasterizing the next frame
        // (never two in a row) so the game keeps its normal speed.
        static u64 last; static int skipped;
        u64 now = host_now_ns();
        int skip = !g_gpu && g_window_open && !skipped && last && now - last > 18000000ull && !getenv("TTPC_NOSKIP");
        skipped = skip; last = now;
        g_render = g_gpu || (g_window_open && !skip) || (g_dump_dir && ((g_copies + 1) % g_dump_every == 0));
    }
    if (clear) {
        u32 ar = bp[0x4F], gb = bp[0x50];
        u32 c = rgba(ar & 255, (gb >> 8) & 255, gb & 255, (ar >> 8) & 255);
        float z = (bp[0x51] & 0xFFFFFF) / 16777215.f;
        u32 src = bp[0x49], wh = bp[0x4A];
        if (g_gpu) {
            gpu_clear(src & 0x3FF, (src >> 10) & 0x3FF, (wh & 0x3FF) + 1, ((wh >> 10) & 0x3FF) + 1, c, z,
                      (bp[0x41] & 8) != 0, (bp[0x41] & 16) != 0, (bp[0x40] & 16) != 0);
            return;
        }
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
    if (reg == 0x65) g_tgen++;
    if (reg == 0x52) { efb_copy(val); g_tgen++; }
}
static void xf_write(u32 addr, u32 v) {
    if (addr < 0x1000) xfmem[addr] = v;
    else if (addr < 0x1100) xfreg[addr & 0xFF] = v;
    if (addr == 0x1018) cp[0x30] = v;     // XF copies of the matrix index registers
    if (addr == 0x1019) cp[0x40] = v;
}

// ---------------------------------------------------------------- primitives
// GPU path: cull on the CPU (same rule as draw_tri) and collect a triangle list.
static Out g_tris[3 * 2 * 4096]; static int g_ntris;
static void collect_tri(const Out *a, const Out *b, const Out *c) {
    if (a->w > 1e-6f && b->w > 1e-6f && c->w > 1e-6f && S.cull) {
        if (S.cull == 3) return;
        float X[3], Y[3], Z; const Out *v[3] = { a, b, c };
        for (int k = 0; k < 3; k++) to_screen(v[k], &X[k], &Y[k], &Z);
        float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
        if ((S.cull == 1 && area < 0) || (S.cull == 2 && area > 0)) return;
    }
    if (g_ntris + 3 > (int)(sizeof g_tris / sizeof *g_tris)) return;
    g_tris[g_ntris++] = *a; g_tris[g_ntris++] = *b; g_tris[g_ntris++] = *c;
}
static void draw_prims(int prim, int vat, int n, u32 *p) {
    static Vtx vin[4096]; static Out vout[4096];
    if (n > 4096) n = 4096;
    for (int k = 0; k < n; k++) { memset(&vin[k], 0, sizeof vin[k]); read_vertex(p, vat, &vin[k]); }
    g_video_draws++;
    if (!g_render) return;
    setup_draw();
    for (int k = 0; k < n; k++) transform(&vin[k], &vout[k]);
    void (*tri)(const Out *, const Out *, const Out *) = g_gpu ? collect_tri : draw_tri;
    g_ntris = 0;
    switch (prim) {
    case 0: case 1: for (int k = 0; k + 3 < n; k += 4) { tri(&vout[k], &vout[k + 1], &vout[k + 2]); tri(&vout[k], &vout[k + 2], &vout[k + 3]); } break;
    case 2: for (int k = 0; k + 2 < n; k += 3) tri(&vout[k], &vout[k + 1], &vout[k + 2]); break;
    case 3: for (int k = 0; k + 2 < n; k++) { if (k & 1) tri(&vout[k + 1], &vout[k], &vout[k + 2]); else tri(&vout[k], &vout[k + 1], &vout[k + 2]); } break;
    case 4: for (int k = 1; k + 1 < n; k++) tri(&vout[0], &vout[k], &vout[k + 1]); break;
    default: break;   // lines/points: not drawn yet
    }
    if (g_gpu && g_ntris) gpu_draw(&S, g_tris, g_ntris);
    static long dl_from = -2; if (dl_from == -2) dl_from = getenv("TTPC_DRAWLOG") ? atol(getenv("TTPC_DRAWLOG")) : -1;
    if (dl_from >= 0 && (long)g_copies >= dl_from && (long)g_copies < dl_from + 2) {
        float X, Y, Z; to_screen(&vout[0], &X, &Y, &Z);
        fprintf(stderr, "[draw f%llu] prim %d n %d tris %d | nst %d | v0 scr (%.0f,%.0f) z %.3f w %.2f c0 %.2f %.2f %.2f %.2f uv %.3f %.3f | z %X blend %X ac %06X cull %d sc %d,%d-%d,%d\n",
            (unsigned long long)g_copies, prim, n, g_ntris / 3, S.nst, X, Y, Z, vout[0].w, vout[0].col[0][0], vout[0].col[0][1], vout[0].col[0][2], vout[0].col[0][3],
            vout[0].uv[0][0], vout[0].uv[0][1], S.zm, S.bm, bp[0xF3], S.cull, S.sx0, S.sy0, S.sx1, S.sy1);
        { u32 m = (u32)vin[0].pnmtx * 4;
          fprintf(stderr, "    pos %.3f %.3f %.3f pnmtx %d (vcd %08X vat %d %08X) | M %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f | P %.3f %.3f %.3f %.3f %.3f %.3f type %u\n",
            vin[0].pos[0], vin[0].pos[1], vin[0].pos[2], vin[0].pnmtx, cp[0x50], vat, cp[0x70 + vat],
            xf_f(m), xf_f(m + 1), xf_f(m + 2), xf_f(m + 3), xf_f(m + 4), xf_f(m + 5), xf_f(m + 6), xf_f(m + 7), xf_f(m + 8), xf_f(m + 9), xf_f(m + 10), xf_f(m + 11),
            xf_f(0x1020), xf_f(0x1021), xf_f(0x1022), xf_f(0x1023), xf_f(0x1024), xf_f(0x1025), xfreg[0x26]); }
        for (int st = 0; st < S.nst; st++) {
            const Stage *g = &S.st[st]; int m = g->map;
            fprintf(stderr, "    st%d map %d coord %d chan %d C[%d %d %d %d] b%d s%d c%d x%d d%d A[%d %d %d %d] d%d K %.2f %.2f %.2f %.2f KA %.2f",
                st, m, g->coord, g->chan, g->csel[0], g->csel[1], g->csel[2], g->csel[3], g->cb, g->csub, g->cclamp, g->cscale, g->cdest,
                g->asel[0], g->asel[1], g->asel[2], g->asel[3], g->adest, g->K[0], g->K[1], g->K[2], g->K[3], g->KA);
            if (m >= 0 && S.tex[m].e) fprintf(stderr, " | tex %08X %dx%d fmt %d", S.tex[m].e->addr, S.tex[m].w, S.tex[m].h, (S.tex[m].e->img0 >> 20) & 15);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "    reg PREV %.2f %.2f %.2f %.2f C0 %.2f %.2f %.2f %.2f C1 %.2f %.2f %.2f %.2f\n", S.reg[0][0], S.reg[0][1], S.reg[0][2], S.reg[0][3],
            S.reg[1][0], S.reg[1][1], S.reg[1][2], S.reg[1][3], S.reg[2][0], S.reg[2][1], S.reg[2][2], S.reg[2][3]);
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
            { static long dl = -2; if (dl == -2) dl = getenv("TTPC_DRAWLOG") ? atol(getenv("TTPC_DRAWLOG")) : -1;
              if (dl >= 0 && (long)g_copies == dl && a < 0x400) fprintf(stderr, "[xf] write %u words at %03X (first %08X)\n", n, a, V32(p + 5)); }
            for (u32 k = 0; k < n; k++) xf_write(a + k, V32(p + 5 + 4 * k));
            p += 5 + 4 * n; continue;
        }
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {   // indexed XF load from array 12-15
            if (p + 5 > end) break;
            u32 h = V32(p + 1), idx = h >> 16, len = ((h >> 12) & 15) + 1, a = h & 0xFFF, arr = 12 + ((op >> 3) & 3);
            u32 src = (cp[0xA0 + arr] & 0x3FFFFFFF) + idx * (cp[0xB0 + arr] & 0xFF);
            for (u32 k = 0; k < len; k++) xf_write(a + k, V32(0x80000000u | (src + 4 * k)));
            static long dl = -2; if (dl == -2) dl = getenv("TTPC_DRAWLOG") ? atol(getenv("TTPC_DRAWLOG")) : -1;
            if (dl >= 0 && (long)g_copies >= dl && (long)g_copies < dl + 1)
                fprintf(stderr, "[xfidx] op %02X idx %u len %u addr %03X arr %u base %08X stride %u src %08X first %08X %08X %08X %08X\n",
                        op, idx, len, a, arr, cp[0xA0 + arr], cp[0xB0 + arr], src, V32(0x80000000u | src), V32(0x80000000u | (src + 4)), V32(0x80000000u | (src + 8)), V32(0x80000000u | (src + 12)));
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
