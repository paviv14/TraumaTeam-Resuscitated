// Audio DSP stand-in: plays the DSP's side of the mailbox protocol instead of emulating the chip.
//
// Registers (16-bit, 0x0C005000):
//   +0x0 / +0x2  CPU->DSP mailbox hi/lo  (hi bit 15 = not yet read by the DSP; writing lo sends it)
//   +0x4 / +0x6  DSP->CPU mailbox hi/lo  (hi bit 15 = mail waiting; reading lo takes it)
//   +0xA         DSP_CSR: bit0 reset, bit2 halt, bit3 AI DMA int, bit4 its mask, bit5 ARAM int,
//                bit6 its mask, bit7 DSP int, bit8 its mask  (interrupt bits are cleared by writing 1)
// Protocol: after reset the boot ROM sends 0x8071FEED; the CPU then sends 10 mails describing the
// program to load (0x80F3A001, addr, 0x80F3C002, iram addr, 0x80F3A002, len, 0x80F3B002, 0,
// 0x80F3D001, entry); the program starts and reports 0xDCD10000 (task INIT) with a DSP interrupt.
#include "cpu.h"
#include "port.h"
#include <stdio.h>
#include <time.h>

extern u32 g_irq_pending;
u32 mmio_peek(u32 p); void mmio_poke(u32 p, u32 v);

static u32 g_out[32]; static int g_nout;         // DSP -> CPU
static u32 g_in_hi, g_boot_count, g_booted;
static u16 g_csr;
static int g_log = 1;

static void push(u32 m) { if (g_nout < 32) g_out[g_nout++] = m; }
static void raise_dsp_int(void) {
    g_csr |= 0x80;
    mmio_poke(0x0C003000u, mmio_peek(0x0C003000u) | 0x40);   // PI cause: DSP
    g_irq_pending = 1;
}

static void on_mail(u32 m) {
    if (!g_booted) {
        if (++g_boot_count == 10) {
            g_booted = 1;
            if (g_log) fprintf(stderr, "[dsp] program booted (entry %04X) -> task INIT\n", m & 0xFFFF);
            push(0xDCD10000u); raise_dsp_int();
        }
        return;
    }
    // AX: 0xBABExxxx announces a command list (low 16 bits = size), the next mail is its address.
    // We acknowledge each list like the AX program does when it has finished a frame: "yield"
    // (0xDCD10002) + DSP interrupt; the SDK task manager answers 0xCDD1000x (resume/continue).
    // TODO (audio): execute the command list = mix the voices into the output buffers.
    static u32 cmdlist_size; static int nframes;
    if ((m & 0xFFFF0000u) == 0xBABE0000u) { cmdlist_size = m & 0xFFFF; return; }
    if (cmdlist_size) {
        cmdlist_size = 0;
        if (g_log && nframes++ < 3) fprintf(stderr, "[dsp] AX command list at %08X -> frame done\n", m);
        push(0xDCD10002u); raise_dsp_int();
        return;
    }
    if ((m & 0xFFFF0000u) == 0xCDD10000u) return;   // task manager replies
    if (g_log) fprintf(stderr, "[dsp] mail from CPU %08X (unhandled)\n", m);
}

// ---------------------------------------------------------------- audio DMA (AI DMA via the DSP block)
// +0x30/+0x32 DMA source address hi/lo, +0x36 bit15 = enable, bits0-14 = length in 32-byte blocks,
// +0x3A blocks remaining. When a buffer has "played" (length / 48 kHz stereo 16-bit), raise AIDINT
// (CSR bit 3); the AX library then queues the next buffer and mixes the next frame.
static u32 g_dma_hi, g_dma_lo, g_dma_ctl; static u64 g_dma_due; static int g_dma_frames;
u32 g_audio_last_buffer, g_audio_last_len;   // where PC audio output will read from
static u64 dsp_now(void) { return host_now_ns(); }
static u64 dma_period_ns(void) { u32 blocks = g_dma_ctl & 0x7FFF; return blocks ? (u64)blocks * 32 / 4 * 1000000000ull / 48000 : 0; }

void dsp_tick(void) {      // called from rt_poll
    if (!(g_dma_ctl & 0x8000) || !g_dma_due) return;
    u64 t = dsp_now();
    if (t < g_dma_due) return;
    g_audio_last_buffer = ((g_dma_hi << 16) | g_dma_lo); g_audio_last_len = (g_dma_ctl & 0x7FFF) * 32;
    g_dma_due += dma_period_ns(); if (g_dma_due < t) g_dma_due = t + dma_period_ns();
    g_csr |= 0x08;
    if (g_csr & 0x10) { mmio_poke(0x0C003000u, mmio_peek(0x0C003000u) | 0x40); g_irq_pending = 1; }
    if (g_log && ++g_dma_frames <= 3) fprintf(stderr, "[ai] audio buffer %08X len %u played\n", g_audio_last_buffer, g_audio_last_len);
}

u32 dsp_read(u32 p, int size) {
    switch (p & 0x3F) {
    case 0x30: return g_dma_hi;
    case 0x32: return g_dma_lo;
    case 0x36: return g_dma_ctl;
    case 0x3A: return g_dma_ctl & 0x7FFF;
    case 0x0: return size == 4 ? (g_in_hi << 16) : g_in_hi;      // CPU->DSP: always already taken
    case 0x2: return 0;
    case 0x4: {
        // the real boot ROM greets after any reset; our OSInit skipped the reset, so greet on demand
        if (!g_nout && !g_booted && g_boot_count == 0) push(0x8071FEEDu);
        u32 hi = g_nout ? (0x8000 | (g_out[0] >> 16 & 0x7FFF)) : 0;
        if (size == 4) { u32 v = g_nout ? g_out[0] : 0; if (g_nout) { for (int k = 1; k < g_nout; k++) g_out[k - 1] = g_out[k]; g_nout--; } return v | (hi << 16 & 0x80000000u); }
        return hi;
    }
    case 0x6: {
        if (!g_nout) return 0;
        u32 lo = g_out[0] & 0xFFFF;
        for (int k = 1; k < g_nout; k++) g_out[k - 1] = g_out[k];
        g_nout--;
        return lo;
    }
    case 0xA: return g_csr;
    }
    return 0;
}

void dsp_write(u32 p, u32 v, int size) {
    switch (p & 0x3F) {
    case 0x30: g_dma_hi = v & 0xFFFF; if (size == 4) g_dma_lo = 0; return;
    case 0x32: g_dma_lo = v & 0xFFFF; return;
    case 0x36: {
        int was = g_dma_ctl & 0x8000; g_dma_ctl = v & 0xFFFF;
        if ((g_dma_ctl & 0x8000) && !was) g_dma_due = dsp_now() + dma_period_ns(), g_irq_pending = 1;
        return;
    }
    case 0x0:
        if (size == 4) { on_mail(v & 0x7FFFFFFFu | (v & 0x80000000u)); return; }
        g_in_hi = v & 0xFFFF; return;
    case 0x2: on_mail((g_in_hi << 16) | (v & 0xFFFF)); g_in_hi = 0; return;
    case 0xA: {
        u16 w = (u16)v;
        g_csr = (g_csr & 0xA8 & ~(w & 0xA8)) | (w & ~0xA8 & 0xFFFF);   // interrupt bits: write 1 to clear
        if (w & 1) {                                                     // reset: boot ROM says hello
            g_csr &= ~1; g_nout = 0; g_booted = 0; g_boot_count = 0;
            push(0x8071FEEDu);
        }
        if (!(g_csr & 0xA8) || !((g_csr & 0x80 && g_csr & 0x100) || (g_csr & 0x08 && g_csr & 0x10))) mmio_poke(0x0C003000u, mmio_peek(0x0C003000u) & ~0x40u);
        return;
    }
    }
}
