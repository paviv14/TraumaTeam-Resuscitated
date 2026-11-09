// Pipeline state decoded from the GX registers, shared by the software rasterizer (video.c) and the
// OpenGL backend (gl.c).
#pragma once
#include "cpu.h"
#include <stddef.h>

// A transformed vertex: clip-space position (GX convention, before the viewport), colors, texcoords.
typedef struct { float x, y, z, w; float col[2][4]; float uv[8][3]; } Out;

// A decoded texture (RGBA8 packed as R<<24|G<<16|B<<8|A), cached across frames and revalidated by
// hashing the source bytes once per displayed frame. `version` changes whenever px is re-decoded.
typedef struct {
    u32 addr, img0, tlut, gen; int w, h; u32 *px; size_t cap;
    u64 hash, data_hash; u32 version;
    u32 gl_tex, gl_version;            // owned by the GPU backend
    const u8 *hi; int hw, hh;          // replacement from a texture pack (RGBA bytes), or NULL
    int fmt;
    u64 partner;                       // dual-palette image: hash of the other half last paired with
} TexEntry;

typedef struct { TexEntry *e; const u32 *px; int w, h, ws, wt, linear; u32 gl_copy; } TexV;

typedef struct {
    int map, coord, chan, csel[4], asel[4];
    int cb, csub, cclamp, cscale, cdest, ab, asub, aclamp, ascale, adest;
    float K[4], KA;
    int rsw[4], tsw[4];   // TEV swap tables: output channel k takes input channel rsw[k] / tsw[k]
} Stage;

typedef struct {
    int nst; Stage st[16]; TexV tex[8];
    float reg[4][4];
    int ac_r0, ac_r1, ac_c0, ac_c1, ac_op;
    u32 zm, bm;
    int sx0, sy0, sx1, sy1, cull;
    float vp[6];
    // indirect texturing
    int nbump, is[4][4];          // per indirect stage: texmap, texcoord, scale s/t shift
    u32 indcmd[16];               // BP 0x10+n per TEV stage
    float im[6][4];               // 3 matrices x 2 rows: m0 m1 m2 (real values), scale exponent
    float cs[8][2];               // texcoord scale (texels) per texcoord, from BP 0x30-0x3F
} Setup;

// GPU backend (gl.c). Coordinates are EFB pixels at 1x (640x528); the backend applies its scale.
int  gpu_init(int scale);                       // after the window's GL context exists; 0 = unavailable
void gpu_draw(const Setup *s, const Out *tris, int nverts);   // triangle list, already culled
void gpu_clear(int x0, int y0, int w, int h, u32 rgba, float z, int color, int alpha, int depth);
void gpu_present(int x0, int y0, int w, int h);  // EFB rect -> window, then swap
int  gpu_copy_to_texture(u32 dst, int x0, int y0, int w, int h, int ow, int oh, int fmt, int inten);
u32  gpu_copy_lookup(u32 addr, int w, int h);   // GPU texture of an EFB copy, or 0
int  gpu_read_rgba(int x0, int y0, int w, int h, u32 *out);  // native-res R<<24|G<<16|B<<8|A, top row first
int  gpu_read_rgb(int x0, int y0, int w, int h, u8 *out);   // for frame dumps (top row first)
extern int g_gpu;
