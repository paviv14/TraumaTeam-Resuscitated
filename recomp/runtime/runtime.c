// Runtime for the recompiled game: memory, DOL loading, indirect calls, SPRs, quantized loads,
// hardware register (MMIO) stubs and boot.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"

u8 *g_mem;
u8 g_lc[0x4000];
static CPU g_cpu;
CPU *g_cpu_dbg = &g_cpu;   // for debug instrumentation outside runtime.c
u32 g_trace[256]; u32 g_trace_pos;
u8 g_seen[0x340000 / 4];
static void report_seen(void) {
    unsigned n = 0; for (unsigned k = 0; k < sizeof g_seen; k++) n += g_seen[k];
    fprintf(stderr, "distinct functions executed: %u / %u\n", n, g_nfuncs);
    if (getenv("TTPC_SEEN")) {   // list every game-code function that ran (0x80007360-0x80206000)
        FILE *f = fopen(getenv("TTPC_SEEN"), "w");
        for (u32 a = 0x80007360; a < 0x80206000; a += 4) if (g_seen[(a - 0x80004000u) >> 2]) fprintf(f, "%08X\n", a);
        fclose(f);
    }
    const char *pk = getenv("TTPC_PEEK");   // e.g. TTPC_PEEK=805842C5,805842C6 : print bytes at timeout
    while (pk && *pk) { u32 a = (u32)strtoul(pk, (char **)&pk, 16); fprintf(stderr, "  [%08X] = %02X %02X %02X %02X\n", a, R8(a), R8(a+1), R8(a+2), R8(a+3)); if (*pk == ',') pk++; else break; }
    const char *w = getenv("TTPC_WATCH");   // e.g. TTPC_WATCH=8022FCC0,8000C500
    while (w && *w) { u32 a = (u32)strtoul(w, (char **)&w, 16); fprintf(stderr, "  %08X ran: %s\n", a, g_seen[(a - 0x80004000u) >> 2] ? "yes" : "no"); if (*w == ',') w++; else break; }
}
static void dump_trace(void) {
    // print distinct recent functions, newest last
    fprintf(stderr, "recent functions:");
    u32 last = 0; int n = 0;
    for (int k = 0; k < 256; k++) {
        u32 a = g_trace[(g_trace_pos + k) & 255];
        if (a && a != last) { fprintf(stderr, " %08X", a); n++; }
        last = a;
    }
    fprintf(stderr, "\n");
}
void rt_dump_threads(void);
static void dump(CPU *c);
void launcher_error(const char *msg);
static void on_crash(void) {
    fprintf(stderr, "CRASH (host stack overflow or bad pointer)\n");
    dump(&g_cpu); dump_trace(); report_seen(); rt_dump_threads();
    launcher_error("Trauma Team Resuscitated stopped with an error.\n\nThe details are in ttpc.log next to Resuscitated.exe - please include it when you report the problem.");
}
void gx_dump(void); void video_dump_stats(void);
u32 g_wp = 0xFFFFFFFFu, g_wp_len = 0;   // TTPC_WP=<phys addr>[,<len>]
void rt_wp_hit(u32 a, u32 v, int size) {
    static u64 t0, after; static int init;
    if (!init) { init = 1; t0 = host_now_ns(); after = getenv("TTPC_WP_AFTER") ? (u64)atoi(getenv("TTPC_WP_AFTER")) * 1000000000ull : 0; }
    if (host_now_ns() - t0 < after) return;
    static int n; if (n++ > 3000) return;
    fprintf(stderr, "[wp] store%d %08X <- %08X | lr %08X fn %08X\n", size * 8, a, v, g_cpu.lr, g_trace[(g_trace_pos - 1) & 255]);
}
__attribute__((constructor)) static void wp_init(void) {
    const char *e = getenv("TTPC_WP"); if (!e) return;
    char *q; g_wp = (u32)strtoul(e, &q, 16) & 0x1FFFFFFFu; g_wp_len = *q == ',' ? (u32)strtoul(q + 1, 0, 16) : 4;
}
static void on_timeout(void) { fprintf(stderr, "TIMEOUT\n"); dump_trace(); report_seen(); rt_dump_threads(); gx_dump(); video_dump_stats(); fflush(stderr); if (getenv("TTPC_GMON")) exit(2); _Exit(2); }
static int g_trace_mmio = 1;
static unsigned g_mmio_count;

// ---------------------------------------------------------------- errors
static void dump(CPU *c) {
    fprintf(stderr, "  lr=%08X ctr=%08X cr=%08X msr=%08X\n", c->lr, c->ctr, c->cr, c->msr);
    for (int i = 0; i < 32; i += 8)
        fprintf(stderr, "  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->r[i], c->r[i + 1],
                c->r[i + 2], c->r[i + 3], c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
    // call stack from the guest back chain: each frame's saved LR sits at (caller's sp)+4
    fprintf(stderr, "  call stack:");
    u32 sp = c->r[1];
    for (int k = 0; k < 24 && sp >= 0x80000000u && sp < 0x81800000u; k++) {
        u32 next = R32(sp); if (next <= sp || next >= 0x81800000u) break;
        fprintf(stderr, " %08X", R32(next + 4)); sp = next;
    }
    fprintf(stderr, "\n");
}
static void halt(const char *why) { fprintf(stderr, "HALT: %s\n", why); dump(&g_cpu); dump_trace(); report_seen(); exit(1); }
void rt_halt(const char *why) { halt(why); }
CPU *g_cpup = &g_cpu;

void rt_badaddr(u32 a, int write) {
    char b[96]; snprintf(b, sizeof b, "bad %s at %08X", write ? "write" : "read", a); halt(b);
}
void rt_unimpl(CPU *c, u32 pc, const char *what) {
    char b[96]; snprintf(b, sizeof b, "unimplemented %s at %08X", what, pc); (void)c; halt(b);
}
void rt_trap(CPU *c, u32 pc) { char b[64]; snprintf(b, sizeof b, "trap at %08X", pc); (void)c; halt(b); }
// The SDK installs a system-call handler (__OSInitSystemCall) that only toggles HID0 to force a
// pipeline sync (used by PPCSync). Nothing to do on the host.
void rt_syscall(CPU *c, u32 pc) { (void)c; (void)pc; }
static int find_func(u32 addr);
// rfi jumps to SRR0 with MSR = SRR1. The SDK uses it for RealMode() (run a function with address
// translation off: entering = target is a function entry; leaving = target is a return address, i.e.
// unwind) and for thread switches (OSLoadContext, handled by HLE). Translation does not exist on PC.
void rt_rfi(CPU *c) {
    u32 t = c->spr[26]; c->msr = c->spr[27];
    if (t < 0x80000000u) t |= 0x80000000u;
    if (find_func(t) >= 0) { rt_call(c, t); return; }
    /* returning to a continuation: unwinding via the C return chain does that */
}

// ---------------------------------------------------------------- indirect calls
static int find_func(u32 addr) {
    u32 lo = 0, hi = g_nfuncs;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return (lo < g_nfuncs && g_funcs[lo].addr == addr) ? (int)lo : -1;
}
void rt_call(CPU *c, u32 addr) {
    int lo = find_func(addr);
    if (lo >= 0) {
        g_funcs[lo].fn(c);   // (no depth counter: fibers/longjmp unwind past returns; see on_segv)
        return;
    }
    // The console's Bluetooth stack (0x80262000-0x80290000) is bypassed by the Wii Remote HLE, so its
    // modules' callbacks are never registered; a stray timer event (seen in the developers' stage
    // select) would call a null one. On the console nothing would be listening either: skip it.
    if (addr == 0 && c->lr >= 0x80262000u && c->lr < 0x80290000u) {
        static int n; if (n++ < 3) fprintf(stderr, "[bt] skipped unregistered Bluetooth callback (lr %08X)\n", c->lr);
        c->r[3] = 0; return;
    }
    char b[64]; snprintf(b, sizeof b, "indirect call to unknown address %08X (lr %08X)", addr, c->lr); halt(b);
}

// ---------------------------------------------------------------- special registers
u32 rt_mfspr(CPU *c, int spr) {
    switch (spr) {
    case 1: return ((u32)c->so << 31) | ((u32)c->ov << 30) | ((u32)c->ca << 29);
    case 8: return c->lr;
    case 9: return c->ctr;
    case 22: return rt_get_dec();
    case 268: case 269: return rt_mftb(c, spr);
    }
    if (spr >= 912 && spr <= 919) return c->gqr[spr - 912];
    if (spr == 920) return c->spr[920] & ~0x0F000000u;   // HID2: DMA queue length always 0 (DMAs are instant)
    return c->spr[spr & 1023];
}
// Locked-cache DMA (DMAU = SPR 922, DMAL = SPR 923). Writing DMAL with the trigger bit moves
// (LEN_U << 2 | LEN_L) 32-byte lines (0 = 128) between main memory and the 16 KB locked cache;
// bit 4 (LD) = load into the cache, else store to memory. NW4R G3D computes its matrices in the
// locked cache and stores them this way - without it, every model is drawn with zero matrices.
static void lc_dma(CPU *c, u32 dmal) {
    u32 dmau = c->spr[922];
    u32 mem = dmau & 0xFFFFFFE0u, lc = dmal & 0xFFFFFFE0u;
    u32 lines = ((dmau & 0x1F) << 2) | ((dmal >> 2) & 3); if (!lines) lines = 128;
    int load = (dmal >> 4) & 1;
    for (u32 k = 0; k < lines * 32; k += 4) {
        u8 *l = g_lc + ((lc + k) & 0x3FFF);
        if (load) { u32 v = R32(mem + k); l[0] = v >> 24; l[1] = v >> 16; l[2] = v >> 8; l[3] = v; }
        else W32(mem + k, (u32)l[0] << 24 | (u32)l[1] << 16 | (u32)l[2] << 8 | l[3]);
    }
}
void rt_mtspr(CPU *c, int spr, u32 v) {
    switch (spr) {
    case 1: c->so = v >> 31; c->ov = (v >> 30) & 1; c->ca = (v >> 29) & 1; return;
    case 8: c->lr = v; return;
    case 9: c->ctr = v; return;
    case 22: rt_set_dec(v); return;
    }
    if (spr >= 912 && spr <= 919) { c->gqr[spr - 912] = v; return; }
    if (spr == 923) { if (v & 2) lc_dma(c, v); c->spr[923] = v & ~2u; return; }   // trigger bit clears when done
    c->spr[spr & 1023] = v;
}
u32 rt_mftb(CPU *c, int which) {
    (void)c;
    u64 ns = host_now_ns();
    u64 tb = ns / 1000000000ull * 60750000ull + ns % 1000000000ull * 60750000ull / 1000000000ull;  // bus/4
    return which == 269 ? (u32)(tb >> 32) : (u32)tb;
}
void rt_mtmsr(CPU *c, u32 v) { c->msr = v; }
void rt_dcbz(CPU *c, u32 ea) { (void)c; ea &= ~31u; for (int k = 0; k < 32; k += 4) W32(ea + k, 0); }

// ---------------------------------------------------------------- paired-single quantization
static double dequant(u32 raw, int type, int scale) {
    double v;
    switch (type) {
    case 4: v = (u8)raw; break; case 5: v = (u16)raw; break;
    case 6: v = (s8)raw; break; case 7: v = (s16)raw; break;
    default: { float f; memcpy(&f, &raw, 4); return f; }
    }
    return v / (double)(1 << 0) * ldexp(1.0, -scale);
}
void rt_psq_l(CPU *c, u32 ea, int fd, int w, int i) {
    u32 g = c->gqr[i]; int type = (g >> 16) & 7; int sc = (g >> 24) & 0x3F; if (sc & 0x20) sc -= 64;
    int sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    u32 a = sz == 1 ? R8(ea) : sz == 2 ? R16(ea) : R32(ea);
    c->f[fd].ps0 = dequant(a, type, sc);
    if (w) { c->f[fd].ps1 = 1.0; return; }
    u32 b = sz == 1 ? R8(ea + 1) : sz == 2 ? R16(ea + 2) : R32(ea + 4);
    c->f[fd].ps1 = dequant(b, type, sc);
}
static u32 quant(double v, int type, int scale) {
    if (type == 0 || type > 7 || type < 4) { float f = (float)v; u32 u; memcpy(&u, &f, 4); return u; }
    v = v * ldexp(1.0, scale);
    double lo = type == 4 ? 0 : type == 5 ? 0 : type == 6 ? -128 : -32768;
    double hi = type == 4 ? 255 : type == 5 ? 65535 : type == 6 ? 127 : 32767;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (u32)(s32)v;
}
void rt_psq_st(CPU *c, u32 ea, int fs, int w, int i) {
    u32 g = c->gqr[i]; int type = g & 7; int sc = (g >> 8) & 0x3F; if (sc & 0x20) sc -= 64;
    int sz = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
    u32 a = quant(c->f[fs].ps0, type, sc), b = quant(c->f[fs].ps1, type, sc);
    if (sz == 1) { W8(ea, a); if (!w) W8(ea + 1, b); }
    else if (sz == 2) { W16(ea, a); if (!w) W16(ea + 2, b); }
    else { W32(ea, a); if (!w) W32(ea + 4, b); }
}

// ---------------------------------------------------------------- hardware registers
// Minimal model: registers read back what was last written. A register polled millions of times
// without being written means the game is spinning on hardware we do not emulate yet: halt and name it.
static u32 g_regs[0x20000];  // 0x0C000000-0x0C00FFFF and 0x0D000000-0x0D00FFFF, 4-byte granularity
static u32 g_poll_reg, g_poll_count;
static u32 *reg_slot(u32 p) { return &g_regs[((p >> 24) & 1) * 0x4000 + ((p & 0xFFFF) >> 2)]; }
static u64 host_ns(void) { return host_now_ns(); }
u32 dsp_read(u32 p, int size); void dsp_write(u32 p, u32 v, int size);
u32 mmio_read(u32 p, int size) {
    if ((p & 0xFFFFFFC0u) == 0x0C005000u) return dsp_read(p, size);
    // Audio interface sample counter (AI_SCNT): advances at 48 kHz in real time.
    if (p == 0x0D006C08) *reg_slot(p) = (u32)(host_ns() * 48000ull / 1000000000ull);
    u32 v = *reg_slot(p);
    if (size == 2) v = (p & 2) ? (v & 0xFFFF) : (v >> 16);
    // PE control: the token/finish status bits are write-1-to-clear and read back as 0 (as in Dolphin's
    // model); otherwise one handler's read-modify-write acknowledge clears the other's pending event.
    if (p == 0x0C00100A && size == 2) v &= ~0xCu;
    else if (size == 1) v = (v >> (8 * (3 - (p & 3)))) & 0xFF;
    if (p == g_poll_reg) {
        if (++g_poll_count == 5000000) {
            char b[96]; snprintf(b, sizeof b, "spinning on hardware register %08X (value %08X)", p, v); halt(b);
        }
    } else { g_poll_reg = p; g_poll_count = 0; }
    if (g_trace_mmio && g_mmio_count++ < 300) fprintf(stderr, "  mmio R%d %08X -> %08X\n", size * 8, p, v);
    return v;
}
void gx_fifo_write(u32 v, int size); void gx_cp_write(u32 p, u32 v, int size);

u32 mmio_peek(u32 p) { return *reg_slot(p); }
void mmio_poke(u32 p, u32 v) { *reg_slot(p) = v; }
void mmio_write(u32 p, u32 v, int size) {
    if ((p & 0xFFFFFFE0u) == 0x0C008000u) { gx_fifo_write(v, size); return; }   
    if ((p & 0xFFFFFFC0u) == 0x0C005000u) { dsp_write(p, v, size); return; }   // GX FIFO (write-gather pipe)
    u32 *r = reg_slot(p);
    if (p == 0x0C00100A && size == 2) {   // PE control: status bits 2/3 are write-1-to-clear
        if (getenv("TTPC_GXLOG")) fprintf(stderr, "[gx] PE ctrl write %04X (was %04X)\n", v & 0xFFFF, *r & 0xFFFF);
        u32 cur = *r & 0xFFFF; u32 nv = (v & ~0xCu) | (cur & 0xC & ~(v & 0xC));
        *r = (*r & 0xFFFF0000u) | nv; g_poll_reg = 0; return;
    }
    if (size == 4) *r = v;
    else if (size == 2) *r = (p & 2) ? ((*r & 0xFFFF0000u) | (v & 0xFFFF)) : ((*r & 0xFFFF) | (v << 16));
    else { int sh = 8 * (3 - (p & 3)); *r = (*r & ~(0xFFu << sh)) | ((v & 0xFF) << sh); }
    g_poll_reg = 0;
    if ((p & 0xFFFFFF00u) == 0x0C000000u) gx_cp_write(p, v, size);
    // EXI (0x0D006800, 3 channels x 0x14): CPR at +0x00, MAR +0x04, LENGTH +0x08, CR +0x0C, DATA +0x10.
    // Transfers complete immediately: clear TSTART and raise "transfer complete" in the channel status.
    if ((p & 0xFFFFFF00u) == 0x0D006800u && size == 4) {
        u32 off = p - 0x0D006800u, ch = off / 0x14, reg = off % 0x14;
        if (ch < 3 && reg == 0x0C && (v & 1)) {
            *r = v & ~1u;
            *reg_slot(0x0D006800u + ch * 0x14) |= 0x8;     // TCINT
            *reg_slot(0x0D006800u + ch * 0x14 + 0x10) = 0;  // immediate read data: no device answers
        }
    }
    if (g_trace_mmio && g_mmio_count++ < 300) fprintf(stderr, "  mmio W%d %08X = %08X\n", size * 8, p, v);
}

// ---------------------------------------------------------------- boot
static void load_dol(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { perror("read"); exit(1); } fclose(f);
#define BE(o) (((u32)d[o] << 24) | ((u32)d[o + 1] << 16) | ((u32)d[o + 2] << 8) | d[o + 3])
    // The header's bss range spans the .sdata/.sdata2 sections too: clear first, then load sections.
    u32 bss = BE(0xD8), bsz = BE(0xDC);
    memset(HOSTPTR(bss, 1), 0, bsz);
    for (int i = 0; i < 18; i++) {
        u32 off = BE(i * 4), addr = BE(0x48 + i * 4), sz = BE(0x90 + i * 4);
        if (sz) memcpy(HOSTPTR(addr, 1), d + off, sz);
    }
    free(d);
}

static void setup_lowmem(void) {
    // Values the IPL/apploader leave in low memory before jumping to the game (see Dolphin's BS2 emu).
    W32(0x80000020, 0x0D15EA5E); W32(0x80000024, 1); W32(0x80000028, 0x01800000);
    W32(0x8000002C, 0x00000023); W32(0x800000F8, 0x0E7BE2C0); W32(0x800000FC, 0x2B73A840);
    W32(0x80000030, 0); W32(0x80000034, 0x817FEC60);
    W32(0x80003100, 0x01800000); W32(0x80003104, 0x01800000); W32(0x80003118, 0x04000000);
    W32(0x8000311C, 0x04000000); W32(0x80003120, 0x93400000); W32(0x80003124, 0x90000800);
    W32(0x80003128, 0x933E0000); W32(0x80003130, 0x933E0000); W32(0x80003134, 0x93400000);
    W32(0x80003138, 0x00000011); W32(0x80003140, 0x00240100); W32(0x80003188, 0x00240100);
    memcpy(HOSTPTR(0x80000000, 1), "SUOE01", 6);  // disc ID
}

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

// Usage: ttpc [main.dol] [timeout-seconds]
// The extracted disc (folder with sys/ and files/) comes from TTPC_DISC, or is looked for at
// disc/DATA, ../disc/DATA, ../../disc/DATA, DATA next to the working folder or the executable.
static const char *find_disc(const char *exe) {
    static char buf[1024];
    if (getenv("TTPC_DISC")) return getenv("TTPC_DISC");
    static const char *rel[] = { "disc/DATA", "../disc/DATA", "../../disc/DATA", "DATA" };
    char dir[1024] = "";
    const char *slash = strrchr(exe, '/'), *bslash = strrchr(exe, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash && slash - exe < (long)sizeof dir) { memcpy(dir, exe, slash - exe + 1); dir[slash - exe + 1] = 0; }
    for (int pass = 0; pass < 2; pass++)
        for (unsigned k = 0; k < sizeof rel / sizeof *rel; k++) {
            snprintf(buf, sizeof buf, "%s%s/sys/main.dol", pass ? dir : "", rel[k]);
            if (file_exists(buf)) { snprintf(buf, sizeof buf, "%s%s", pass ? dir : "", rel[k]); return buf; }
        }
    return "disc/DATA";
}

static void setenv_portable(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* ---------------------------------------------------------------- the launcher (what Play.bat used to do)
   Resuscitated.exe is started by a double click: it works from its own folder, writes its log to ttpc.log there
   (the previous one kept as ttpc.prev.log), records the session's input with a copy of the save it started from
   (recordings/), and picks up the texture pack in ../../textures/upscaled. Anything already set (by a .bat, or
   stderr already redirected) is left alone. Errors that stop it show a message box. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif
void launcher_error(const char *msg) {
    fprintf(stderr, "%s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Trauma Team Resuscitated", MB_OK | MB_ICONERROR);
#endif
}
static void copy_tree(const char *from, const char *to) {   // a mirror of the save folder (small)
    host_mkdir(to); DIR *d = opendir(from); if (!d) return;
    struct dirent *e; char a[1024], b[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name); snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        struct stat st; if (stat(a, &st)) continue;
        if (S_ISDIR(st.st_mode)) { copy_tree(a, b); continue; }
        FILE *fi = fopen(a, "rb"), *fo = fi ? fopen(b, "wb") : NULL; char buf[65536]; size_t n;
        if (fi && fo) while ((n = fread(buf, 1, sizeof buf, fi)) > 0) fwrite(buf, 1, n, fo);
        if (fi) fclose(fi); if (fo) fclose(fo);
    }
    closedir(d);
}
static void launcher_setup(void) {
#ifdef _WIN32
    {   char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);   // work from the exe's folder
        if (n && n < sizeof exe) { char *sl = strrchr(exe, '\\'); if (sl) { *sl = 0; SetCurrentDirectoryA(exe); } } }
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    int redirected = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
#else
    int redirected = 1;
#endif
    if (!redirected) {   // started by a double click: our own log
        remove("ttpc.prev.log"); rename("ttpc.log", "ttpc.prev.log");
        if (!freopen("ttpc.log", "w", stderr)) {}
        setvbuf(stderr, NULL, _IONBF, 0);
    }
#ifdef _OPENMP
    if (!getenv("OMP_NUM_THREADS")) { int n = omp_get_num_procs(); omp_set_num_threads(n > 8 ? 8 : n); }
#endif
    if (!getenv("TTPC_PERFLOG")) setenv_portable("TTPC_PERFLOG", "1");
    if (!getenv("TTPC_RECORD") && !getenv("TTPC_REPLAY")) {
        setenv_portable("TTPC_RECORD", "auto");
        struct stat st; if (!stat("nand", &st)) { host_mkdir("recordings"); copy_tree("nand", "recordings/session_start_nand"); }
    }
    if (!getenv("TTPC_TEXTURES")) { struct stat st; if (!stat("../../textures/upscaled", &st)) setenv_portable("TTPC_TEXTURES", "../../textures/upscaled"); }
}

/* --mission su110 (or TTPC_MISSION): go straight to one mission's gameplay part for testing.
   The game boots as usual, switches to the developer STAGE SELECT (sequence 32) and the
   menu is driven by scripted presses: mode, chapter, story, then the gameplay item.
   Names are mode + chapter + story as on the disc: su (CR-S01), em (Maria), or (Hank),
   en (Tomoe), di (Gabriel), au (Naomi); e.g. su110 = chapter 1, story 0x10. */
static void mission_setup(const char *m) {
    static const char *modes[] = { "su", "em", "or", "en", "di", "au" };
    int mode = -1;
    for (int k = 0; k < 6; k++) if (!strncmp(m, modes[k], 2)) mode = k;
    unsigned ch = 0, st = 0;
    if (mode < 0 || strlen(m) != 5 || sscanf(m + 2, "%1x%2x", &ch, &st) != 2) {
        fprintf(stderr, "[mission] '%s' is not a mission name (su110, em110, or110, en110, di110, au110...)\n", m); return;
    }
    static char script[2048]; char *p = script; long t = getenv("TTPC_MISSION_T0") ? atol(getenv("TTPC_MISSION_T0")) : 90;
    long step = getenv("TTPC_MISSION_STEP") ? atol(getenv("TTPC_MISSION_STEP")) : 8;
    #define PRESS(btn) do { p += sprintf(p, "%ld+%ld:%s;", t, step / 3 > 6 ? 6L : step / 3, btn); t += step; } while (0)
    PRESS("down");                                   /* difficulty -> mode */
    for (int k = 0; k < mode; k++) PRESS("right");
    PRESS("down"); for (unsigned k = 0; k < ch; k++) PRESS("right");
    PRESS("down"); for (unsigned k = 0; k < st / 0x10; k++) PRESS("right");
    for (int k = 0; k < 5; k++) PRESS("down");       /* PLAY ALL, STORY TITLE, EVENT, CONFERENCE -> the gameplay part */
    PRESS("A");
    #undef PRESS
    { char at[32]; snprintf(at, sizeof at, "%s:32", getenv("TTPC_MISSION_SEQ") ? getenv("TTPC_MISSION_SEQ") : "60"); setenv_portable("TTPC_SEQ_AT", at); }
    if (getenv("TTPC_INPUT_MORE")) { strcat(script, getenv("TTPC_INPUT_MORE")); }   /* tests: presses after the menu */
    setenv_portable("TTPC_INPUT", script);
    fprintf(stderr, "[mission] %s: stage select script %s\n", m, script);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[argc - 1], "--prepare-textures")) {   // a console job (PrepareTextures.bat)
#ifdef _WIN32
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {   // the exe is a windowed program: write into the .bat's console
            HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
            if (!ho || GetFileType(ho) == FILE_TYPE_UNKNOWN) { if (!freopen("CONOUT$", "w", stdout)) {} }
            HANDLE he = GetStdHandle(STD_ERROR_HANDLE);
            if (!he || GetFileType(he) == FILE_TYPE_UNKNOWN) { if (!freopen("CONOUT$", "w", stderr)) {} }
        }
#endif
        int texpack_prepare(void); return texpack_prepare();
    }
    setvbuf(stderr, NULL, _IONBF, 0);   // the log survives a crash or a forced exit (Windows buffers redirected stderr)
    launcher_setup();
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--mission") && k + 1 < argc) {
        mission_setup(argv[k + 1]);
        for (int j = k; j + 2 <= argc; j++) argv[j] = argv[j + 2];
        argc -= 2; k--;
    }
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--vr") || !strcmp(argv[k], "--vr-desktop")) {   // VR mode (vr.c)
        setenv_portable(!strcmp(argv[k], "--vr") ? "TTPC_VR" : "TTPC_VR_PREVIEW", "1");
        for (int j = k; j + 1 <= argc; j++) argv[j] = argv[j + 1];
        argc -= 1; k--;
    }
    if (getenv("TTPC_MISSION") && *getenv("TTPC_MISSION")) mission_setup(getenv("TTPC_MISSION"));
    const char *disc = find_disc(argv[0]);
    char dolpath[1200];
    if (argc > 1) snprintf(dolpath, sizeof dolpath, "%s", argv[1]);
    else snprintf(dolpath, sizeof dolpath, "%s/sys/main.dol", disc);
    if (!file_exists(dolpath)) {
        char msg[1600]; snprintf(msg, sizeof msg, "Cannot find the game's main.dol (looked at %s).\n\n"
                        "Put your extracted disc in a folder called disc next to Resuscitated.exe (disc\\DATA\\sys\\main.dol), "
                        "or set TTPC_DISC to the extracted disc folder (the one containing sys and files).", dolpath);
        launcher_error(msg);
        return 1;
    }
    g_mem = host_alloc_guest(0x14000000);
    if (!g_mem) { fprintf(stderr, "cannot reserve guest memory\n"); return 1; }
    load_dol(dolpath);
    setup_lowmem();
    void disc_init(const char *root); void video_init(void); void ios_init(void); int platform_init(void);
    ios_init();
    platform_init();
    video_init();
    fprintf(stderr, "disc: %s\n", disc);
    disc_init(disc);
    CPU *c = &g_cpu;
    int timeout = argc > 2 ? atoi(argv[2]) : getenv("TTPC_TIMEOUT") ? atoi(getenv("TTPC_TIMEOUT")) : 0;
    host_set_timeout(timeout, on_timeout);
    host_install_crash_handler(on_crash);
    c->msr = 0x00002032;
    fprintf(stderr, "booting at %08X (%u recompiled entry points)\n", g_entry, g_nfuncs);
    rt_call(c, g_entry);
    fprintf(stderr, "entry returned\n");
    return 0;
}
