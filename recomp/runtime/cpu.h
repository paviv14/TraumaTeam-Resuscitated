// Guest CPU state and memory access for the recompiled game.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;

typedef struct { double ps0, ps1; } FPR;

typedef struct CPU {
    u32 r[32];
    FPR f[32];
    u32 cr, lr, ctr, msr, fpscr;
    u8 ca, so, ov;
    u32 gqr[8];
    u32 spr[1024];
} CPU;

// Generated functions are weak so recomp/runtime/hle.c can replace any of them with a native version.
#define WEAK __attribute__((weak))

// Ring buffer of recently entered guest functions, dumped when the game hangs or halts.
extern u32 g_trace[256]; extern u32 g_trace_pos;
extern u8 g_seen[0x340000 / 4];
#define TRACE(a) (g_trace[g_trace_pos++ & 255] = (a), g_seen[((a) - 0x80004000u) >> 2] = 1)

typedef void (*GuestFn)(CPU *);
typedef struct { u32 addr; GuestFn fn; } FuncEntry;
extern const FuncEntry g_funcs[];
extern const u32 g_nfuncs;
extern const u32 g_entry;

// ---------------------------------------------------------------- memory
// Physical map: MEM1 0x00000000 (24 MB), MEM2 0x10000000 (64 MB), MMIO 0x0C000000 / 0x0D000000.
// Virtual 0x8/0xC -> MEM1, 0x9/0xD -> MEM2 (cached/uncached), 0xE0000000 -> locked cache.
extern u8 *g_mem;   // 0x14000000 bytes of host memory indexed by physical address
extern u8 g_lc[0x4000];
u32 mmio_read(u32 phys, int size);
void mmio_write(u32 phys, u32 v, int size);
void rt_badaddr(u32 a, int write);

#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define LIKELY(x) __builtin_expect(!!(x), 1)

static inline u8 *HOSTPTR(u32 a, int write) {
    if ((a >> 28) == 0xE) return g_lc + (a & 0x3FFF);
    u32 p = a & 0x1FFFFFFF;
    if (LIKELY(p < 0x01800000u || (p - 0x10000000u) < 0x04000000u)) return g_mem + p;
    rt_badaddr(a, write);
    return g_mem;  // not reached
}
static inline int IS_MMIO(u32 a) { u32 p = a & 0x1FFFFFFF; return (p - 0x0C000000u) < 0x02000000u && (a >> 28) != 0xE; }

static inline u32 R32(u32 a) { if (IS_MMIO(a)) return mmio_read(a & 0x1FFFFFFF, 4); u32 v; memcpy(&v, HOSTPTR(a, 0), 4); return BSWAP32(v); }
static inline u32 R16(u32 a) { if (IS_MMIO(a)) return mmio_read(a & 0x1FFFFFFF, 2); u16 v; memcpy(&v, HOSTPTR(a, 0), 2); return BSWAP16(v); }
static inline u32 R8(u32 a)  { if (IS_MMIO(a)) return mmio_read(a & 0x1FFFFFFF, 1); return *HOSTPTR(a, 0); }
static inline void W32(u32 a, u32 v) { if (IS_MMIO(a)) { mmio_write(a & 0x1FFFFFFF, v, 4); return; } v = BSWAP32(v); memcpy(HOSTPTR(a, 1), &v, 4); }
static inline void W16(u32 a, u16 v) { if (IS_MMIO(a)) { mmio_write(a & 0x1FFFFFFF, v, 2); return; } v = BSWAP16(v); memcpy(HOSTPTR(a, 1), &v, 2); }
static inline void W8(u32 a, u8 v)   { if (IS_MMIO(a)) { mmio_write(a & 0x1FFFFFFF, v, 1); return; } *HOSTPTR(a, 1) = v; }
static inline float RF32(u32 a) { u32 v = R32(a); float f; memcpy(&f, &v, 4); return f; }
static inline void WF32(u32 a, float f) { u32 v; memcpy(&v, &f, 4); W32(a, v); }
static inline double RF64(u32 a) { u64 v = ((u64)R32(a) << 32) | R32(a + 4); double d; memcpy(&d, &v, 8); return d; }
static inline void WF64(u32 a, double d) { u64 v; memcpy(&v, &d, 8); W32(a, (u32)(v >> 32)); W32(a + 4, (u32)v); }

#define FBITS(d) ({ u64 _b; double _d = (d); memcpy(&_b, &_d, 8); _b; })
#define FSETBITS(dst, bits) do { u64 _b = (bits); memcpy(&(dst), &_b, 8); } while (0)

// ---------------------------------------------------------------- condition register
#define ROTL(x, n) ({ u32 _x = (x); u32 _n = (n) & 31; _n ? (_x << _n) | (_x >> (32 - _n)) : _x; })
#define CR_BIT(n) ((c->cr >> (31 - (n))) & 1)
#define SET_CR_BIT(n, v) do { u32 _m = 1u << (31 - (n)); c->cr = (v) ? (c->cr | _m) : (c->cr & ~_m); } while (0)
#define GET_CRF(f) ((c->cr >> (28 - 4 * (f))) & 0xF)
#define SET_CRF(f, v) do { c->cr = (c->cr & ~(0xFu << (28 - 4 * (f)))) | (((u32)(v) & 0xF) << (28 - 4 * (f))); } while (0)
#define SET_CRF_S(f, a, b) SET_CRF(f, ((a) < (b) ? 8 : (a) > (b) ? 4 : 2) | c->so)
#define SET_CRF_U(f, a, b) SET_CRF(f, ((u32)(a) < (u32)(b) ? 8 : (u32)(a) > (u32)(b) ? 4 : 2) | c->so)
#define SET_CR0(v) SET_CRF_S(0, (s32)(v), 0)
#define SET_CRF_F(f, a, b) do { double _a = (a), _b = (b); \
    SET_CRF(f, (isnan(_a) || isnan(_b)) ? 1 : _a < _b ? 8 : _a > _b ? 4 : 2); } while (0)

// ---------------------------------------------------------------- runtime services
void rt_call(CPU *c, u32 addr);
extern u32 g_irq_pending;          // set when an interrupt source may need servicing
void rt_poll(CPU *c);              // deliver pending interrupts (called on loop back-edges)
void rt_set_dec(u32 v); u32 rt_get_dec(void);
void orig_8021B5D0(CPU *c); void orig_802222E0(CPU *c);
void rt_syscall(CPU *c, u32 pc);
void rt_rfi(CPU *c);
void rt_trap(CPU *c, u32 pc);
void rt_unimpl(CPU *c, u32 pc, const char *what);
u32 rt_mfspr(CPU *c, int spr);
void rt_mtspr(CPU *c, int spr, u32 v);
u32 rt_mftb(CPU *c, int which);
void rt_mtmsr(CPU *c, u32 v);
void rt_dcbz(CPU *c, u32 ea);
void rt_psq_l(CPU *c, u32 ea, int fd, int w, int i);
void rt_psq_st(CPU *c, u32 ea, int fs, int w, int i);

static inline u32 rt_divw(u32 a, u32 b) {
    s32 x = (s32)a, y = (s32)b;
    if (y == 0 || (x == (s32)0x80000000 && y == -1)) return (x < 0 && y == 0) ? 0xFFFFFFFFu : 0;
    return (u32)(x / y);
}
static inline u32 rt_sraw(CPU *c, u32 s, u32 b) {
    s32 v = (s32)s;
    if (b & 0x20) { c->ca = v < 0; return v < 0 ? 0xFFFFFFFFu : 0; }
    int n = b & 31;
    c->ca = (v < 0) && n && (s & ((1u << n) - 1));
    return (u32)(v >> n);
}
static inline s32 rt_fctiw(double d, int toward_zero) {
    if (isnan(d)) return (s32)0x80000000;
    if (!toward_zero) d = nearbyint(d);
    if (d >= 2147483647.0) return 0x7FFFFFFF;
    if (d <= -2147483648.0) return (s32)0x80000000;
    return (s32)d;
}
