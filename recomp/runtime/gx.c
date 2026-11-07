// GPU front end: the graphics FIFO and command processor (CP), currently with a headless "GPU" that
// consumes commands instantly. The renderer will read the same FIFO data.
//
// Writes to the write-gather pipe (0x0C008000) are stored in guest memory at the PI FIFO write pointer
// (PI 0x0C00300C base, 0x0C003010 end, 0x0C003014 write pointer, bit 27 = wrapped), as on hardware.
// CP registers (16-bit, 0x0C000000): +0 status (bit4 breakpoint hit), +2 control (bit1 CP interrupts,
// bit5 breakpoint enable), +0x3C/+0x3E breakpoint address lo/hi.
// Pixel engine: BP register 0x45 bit 1 = draw done ("finish" interrupt), 0x48 = draw sync token.
#include "cpu.h"
#include <stdio.h>

extern u32 g_irq_pending;
u32 mmio_peek(u32 p); void mmio_poke(u32 p, u32 v);
static u16 r16(u32 p) { u32 w = mmio_peek(p & ~3u); return (p & 2) ? (w & 0xFFFF) : (w >> 16); }
static void w16(u32 p, u16 v) { u32 w = mmio_peek(p & ~3u); mmio_poke(p & ~3u, (p & 2) ? ((w & 0xFFFF0000u) | v) : ((w & 0xFFFF) | ((u32)v << 16))); }

static int g_bp_pending;           // last byte was BP opcode 0x61
static u64 g_written;              // total bytes ever written to the FIFO
static u64 g_bp_target;            // g_written value at which the armed breakpoint is reached
static u32 g_bp_addr; static int g_bp_armed;
u64 g_gx_frames;

// The GPU stops reading at an armed breakpoint: PE events written after it are held until it is
// released, so tokens/draw-done arrive in the order and at the pace the game expects.
static u32 g_held[64]; static int g_nheld;
static void pe_apply(u32 v);
static void pe_events(u32 v) {
    if (g_bp_armed) { if (g_nheld < 64) g_held[g_nheld++] = v; return; }
    pe_apply(v);
}
static void pe_apply(u32 v) {
    u32 reg = v >> 24;
    if (reg == 0x45 && (v & 2)) {              // draw done: PE_CTRL bit 3, PI cause 0x400
        mmio_poke(0x0C001008, mmio_peek(0x0C001008) | 0x8);
        mmio_poke(0x0C003000, mmio_peek(0x0C003000) | 0x400); g_irq_pending = 1;
    } else if (reg == 0x48 || reg == 0x47) {
        static int n; if (n++ < 6) fprintf(stderr, "[gx] draw sync BP %02X token %04X\n", reg, v & 0xFFFF);
    }
    if (reg == 0x48) {                  // draw sync token: token value, PE_CTRL bit 2, PI cause 0x200
        mmio_poke(0x0C00100C, (mmio_peek(0x0C00100C) & 0xFFFF0000u) | (v & 0xFFFF));
        mmio_poke(0x0C001008, mmio_peek(0x0C001008) | 0x4);
        mmio_poke(0x0C003000, mmio_peek(0x0C003000) | 0x200); g_irq_pending = 1;
    }
}

static void store(u32 v, int size) {
    u32 base = mmio_peek(0x0C00300C) & 0x1FFFFFE0u, end = mmio_peek(0x0C003010) & 0x1FFFFFE0u;
    u32 wp = mmio_peek(0x0C003014), a = wp & 0x1FFFFFFFu & ~0x04000000u;
    if (!base || !end) return;
    for (int k = size - 1; k >= 0; k--) {
        W8(0x80000000u | a, (u8)(v >> (8 * k)));
        if (++a >= end) { a = base; wp ^= 0x08000000u; }   // wrap flag
    }
    g_written += size;
    mmio_poke(0x0C003014, (wp & 0x08000000u) | a);
}

static u64 g_sizes[5];
void gx_fifo_write(u32 v, int size) {
    store(v, size);
    g_sizes[size]++;
    if ((g_sizes[1] + g_sizes[2] + g_sizes[4]) == 4000) fprintf(stderr, "[gx] fifo writes: %llu x8 %llu x16 %llu x32\n", (unsigned long long)g_sizes[1], (unsigned long long)g_sizes[2], (unsigned long long)g_sizes[4]);
    if (size == 1) { g_bp_pending = (v == 0x61); return; }
    if (size == 4 && g_bp_pending) pe_events(v);
    g_bp_pending = 0;
}

// CP register writes we need to observe: arming the breakpoint.
void gx_cp_write(u32 p, u32 v, int size) {
    if (p == 0x0C000002 && size == 2) {
        int was = g_bp_armed;
        g_bp_armed = (v & 0x20) != 0;
        if (g_bp_armed && !was) {
            g_bp_addr = ((u32)r16(0x0C00003E) << 16 | r16(0x0C00003C)) & 0x1FFFFFFFu;
            // The headless GPU has already consumed everything written so far, so the breakpoint is
            // reached as soon as the write pointer has passed its address.
            g_bp_target = g_written;
            static int n; if (n++ < 5) fprintf(stderr, "[gx] breakpoint armed at %08X (fifo wp %08X, %llu bytes written)\n", g_bp_addr, mmio_peek(0x0C003014), (unsigned long long)g_written);
        }
        if (!g_bp_armed) {
            w16(0x0C000000, r16(0x0C000000) & ~0x10);
            for (int k = 0; k < g_nheld; k++) pe_apply(g_held[k]);   // GPU continues past the breakpoint
            g_nheld = 0;
        }
    }
}

void gx_tick(void) {   // called from rt_poll: the "GPU" reaches an armed breakpoint
    if (!g_bp_armed) return;
    u32 wp = mmio_peek(0x0C003014) & 0x1FFFFFFFu & ~0x0C000000u;
    if (wp != g_bp_addr && g_written == g_bp_target) return;   // nothing new submitted yet
    if (r16(0x0C000000) & 0x10) return;                         // already signalled
    w16(0x0C000000, r16(0x0C000000) | 0x10);
    g_gx_frames++;
    if (g_gx_frames <= 3) fprintf(stderr, "[gx] breakpoint reached (frame %llu), CP control %04X\n", (unsigned long long)g_gx_frames, r16(0x0C000002));
    if (r16(0x0C000002) & 0x2) { mmio_poke(0x0C003000, mmio_peek(0x0C003000) | 0x800); g_irq_pending = 1; }
}

// Pixel engine control (0x0C00100A): bits 0/1 enable token/finish interrupts, bits 2/3 are their
// status flags, cleared by writing 1 (as on hardware).
void gx_pe_write(u32 p, u32 v, int size) {
    if (p != 0x0C00100A || size != 2) return;
    u16 old = r16(0x0C00100A);           // value before this write was stored by the caller
    (void)old;
}
// PI cause bits for the GPU follow the device status (called before/after each dispatch).
u32 gx_cause(void) {
    u16 pe = r16(0x0C00100A), sr = r16(0x0C000000), cr = r16(0x0C000002);
    u32 c = 0;
    if (pe & 0x4) c |= 0x200;
    if (pe & 0x8) c |= 0x400;
    if ((sr & 0x10) && (cr & 0x22) == 0x22) c |= 0x800;
    return c;
}
