// GPU front end: the graphics FIFO and command processor (CP), with a headless "GPU" that consumes
// commands instantly. The renderer will read the same FIFO data.
//
// Writes to the write-gather pipe (0x0C008000) are stored in guest memory at the PI FIFO write pointer
// (PI 0x0C00300C base, 0x0C003010 end, 0x0C003014 write pointer, bit 27 = wrapped), as on hardware.
// CP registers (16-bit, 0x0C000000): +0 status (bit4 breakpoint hit), +2 control (bit1 CP interrupts,
// bit5 breakpoint enable), +0x3C/+0x3E breakpoint address lo/hi.
// Pixel engine: BP register 0x45 bit 1 = draw done ("finish" interrupt), 0x48 = draw sync token.
//
// GPU read position: the GPU consumes everything written, but stops exactly at an armed breakpoint
// (which may be at data already written, e.g. the end of a frame queued earlier). PE events take effect
// when the read position passes the place they were written.
#include "cpu.h"
#include <stdio.h>

extern u32 g_irq_pending;
u32 mmio_peek(u32 p); void mmio_poke(u32 p, u32 v);
static u16 r16(u32 p) { u32 w = mmio_peek(p & ~3u); return (p & 2) ? (w & 0xFFFF) : (w >> 16); }
static void w16(u32 p, u16 v) { u32 w = mmio_peek(p & ~3u); mmio_poke(p & ~3u, (p & 2) ? ((w & 0xFFFF0000u) | v) : ((w & 0xFFFF) | ((u32)v << 16))); }

static int g_bp_opcode;            // last byte was BP opcode 0x61
static u64 g_written;              // linear count of bytes written to the FIFO
static u64 g_read;                 // linear position the GPU has consumed up to
static int g_bp_armed, g_bp_hit;
u64 g_gx_frames;
static int g_log = 1;
static int g_token_budget = 1;
void gx_on_retrace(void) { g_token_budget = 1; }   // called at every VI retrace

typedef struct { u64 pos; u32 v; } Event;
static Event g_ev[256]; static int g_nev;

static u32 fifo_base(void) { return mmio_peek(0x0C00300C) & 0x1FFFFFE0u; }
static u32 fifo_end(void)  { return mmio_peek(0x0C003010) & 0x1FFFFFE0u; }
static u32 fifo_wp(void)   { return mmio_peek(0x0C003014) & 0x03FFFFFFu; }

static void pe_apply(u32 v) {
    u32 reg = v >> 24;
    if (reg == 0x45 && (v & 2)) {              // draw done: PE_CTRL bit 3 -> "finish" interrupt
        mmio_poke(0x0C001008, mmio_peek(0x0C001008) | 0x8); g_irq_pending = 1;
    } else if (reg == 0x48) {                  // draw sync token -> token register + "token" interrupt
        mmio_poke(0x0C00100C, (mmio_peek(0x0C00100C) & 0xFFFF0000u) | (v & 0xFFFF));
        mmio_poke(0x0C001008, mmio_peek(0x0C001008) | 0x4); g_irq_pending = 1;
    }
}

static void store(u32 v, int size) {
    u32 base = fifo_base(), end = fifo_end();
    u32 wp = mmio_peek(0x0C003014), a = wp & 0x03FFFFFFu;
    if (!base || !end) return;
    for (int k = size - 1; k >= 0; k--) {
        W8(0x80000000u | a, (u8)(v >> (8 * k)));
        if (++a >= end) { a = base; wp ^= 0x08000000u; }   // wrap flag
    }
    g_written += size;
    mmio_poke(0x0C003014, (wp & 0x08000000u) | a);
}

void gx_fifo_write(u32 v, int size) {
    store(v, size);
    if (size == 1) { g_bp_opcode = (v == 0x61); return; }
    if (size == 4 && g_bp_opcode) {
        u32 reg = v >> 24;
        if ((reg == 0x45 && (v & 2)) || reg == 0x48) {
            if (g_nev < 256) g_ev[g_nev++] = (Event){ g_written, v };
            g_irq_pending = 1;
        }
    }
    g_bp_opcode = 0;
}

// Linear FIFO position of the breakpoint address, or -1 if it has not been written yet.
static s64 bp_position(void) {
    u32 bp = (((u32)r16(0x0C00003E) << 16) | r16(0x0C00003C)) & 0x03FFFFFFu;
    u32 base = fifo_base(), end = fifo_end(), wp = fifo_wp();
    if (!base || end <= base) return -1;
    u32 size = end - base;
    u32 behind = (wp >= bp) ? wp - bp : wp + size - bp;      // bytes from bp forward to wp
    if ((u64)behind <= g_written - g_read) return (s64)(g_written - behind);   // in the unconsumed span
    // Already consumed by our instant GPU: a real (slower) GPU would be stopped right there, so the
    // breakpoint counts as reached at the current read position.
    if (behind < size / 2) return (s64)g_read;
    return -1;                                                 // not written yet: still ahead
}

// Advance the GPU read position; called from rt_poll and when the breakpoint changes.
void gx_tick(void) {
    if (g_bp_hit) return;                                      // GPU halted at the breakpoint
    u64 target = g_written;
    int stop = 0;
    if (g_bp_armed) { s64 p = bp_position(); if (p >= 0) { target = (u64)p; stop = 1; } }
    // Frame pacing: the GPU completes at most one frame (draw-sync token) per display refresh, like a
    // game running at 60 Hz on hardware. The game's frame flip relies on this (token == last + 1 at
    // each retrace). A token beyond the budget stops the GPU just before it until the next refresh.
    int k = 0;
    while (k < g_nev && g_ev[k].pos <= target) {
        if ((g_ev[k].v >> 24) == 0x48) {
            if (!g_token_budget) { target = g_ev[k].pos - 1; stop = 0; break; }
            g_token_budget--;
        }
        pe_apply(g_ev[k++].v);
    }
    if (k) { for (int j = k; j < g_nev; j++) g_ev[j - k] = g_ev[j]; g_nev -= k; }
    g_read = target;
    if (stop) {
        g_bp_hit = 1;
        w16(0x0C000000, r16(0x0C000000) | 0x10);
        g_gx_frames++;
        if (g_log && g_gx_frames <= 5) fprintf(stderr, "[gx] GPU reached breakpoint (frame %llu)\n", (unsigned long long)g_gx_frames);
        g_irq_pending = 1;
    }
}

// CP control writes: arming / releasing the breakpoint.
void gx_cp_write(u32 p, u32 v, int size) {
    if (p == 0x0C000002 && size == 2) {
        int armed = (v & 0x20) != 0;
        if (armed != g_bp_armed || !armed) {
            g_bp_armed = armed;
            if (!armed) { g_bp_hit = 0; w16(0x0C000000, r16(0x0C000000) & ~0x10); }
            gx_tick();
        }
    }
}

// PI cause bits for the GPU follow the device status (re-evaluated around each dispatch).
u32 gx_cause(void) {
    u16 pe = r16(0x0C00100A), sr = r16(0x0C000000), cr = r16(0x0C000002);
    u32 c = 0;
    if ((pe & 0x4) && (pe & 0x1)) c |= 0x200;   // token, if its interrupt is enabled
    if ((pe & 0x8) && (pe & 0x2)) c |= 0x400;   // finish, if enabled
    if ((sr & 0x10) && (cr & 0x22) == 0x22) c |= 0x800;
    return c;
}
