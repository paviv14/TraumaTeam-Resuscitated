// AX audio mixer: does the work of the DSP's AX microcode (Wii, "new" command set) natively.
//
// Every 3 ms frame the AX library sends a command list (0xBABExxxx + address). Commands (u16 + args):
//   0x00 SETUP addr          reset the mix buffers (initial volume ramps at addr)
//   0x01 ADD_TO_LR addr      add 96 s32 samples from addr to main L and R (02 subtract, 03 add/sub)
//   0x04 PROCESS addr        run the linked list of voice parameter blocks (PBs) starting at addr
//   0x05-0x07 MIX_AUXA/B/C   upload an aux bus to RAM for CPU effects and mix back the processed one
//   0x08-0x09 UPL_AUX*_MIX_LRSC
//   0x0A COMPRESSOR (4 words)  0x0B OUTPUT vol, surround addr, LR addr  0x0C OUTPUT_DPL2
//   0x0D WM_OUTPUT 4 addrs (Wii Remote speakers)  0x0E END
// Each running voice: fetch samples (ADPCM / PCM16 / PCM8) from memory with looping, resample by
// ratio (16.16) to 32 kHz, apply the volume envelope and per-bus volume ramps, mix; then write the
// voice state (position, ADPCM history, volumes) back into its PB like the DSP does.
// OUTPUT writes 96 interleaved s16 stereo frames (right, left) for the audio DMA to play.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NS 96                          // samples per AX frame (3 ms at 32 kHz)

static s32 g_main[3][NS];              // L, R, S
static s32 g_aux[3][3][NS];            // aux A/B/C x L, R, S
#define NWM 18                         // Wii Remote speaker samples per frame (6 kHz)
static s32 g_wm[4][NWM];               // the four Remotes' speaker mixes

// PB word offsets (u16 units) - AXPBWii
enum {
    PB_NEXT = 0, PB_THIS = 2, PB_SRC_TYPE = 4, PB_COEF_SEL = 5, PB_MIXCTL = 6, PB_RUNNING = 8, PB_IS_STREAM = 9,
    PB_MIXER = 10,                     // 24 words: L, Ld, R, Rd, AL, ALd, AR, ARd, BL, BLd, BR, BRd, CL, CLd, CR, CRd, S, Sd, AS, ASd, BS, BSd, CS, CSd
    PB_ITD = 34,                       // initial time delay: on, addr (2), offset L/R, target L/R
    PB_DPOP = 41,                      // 12 words
    PB_VOL = 53, PB_VOL_DELTA = 54,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_ADDR = 57, PB_END_ADDR = 59, PB_CUR_ADDR = 61,
    PB_COEFS = 63, PB_GAIN = 79, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO = 83, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
    PB_LPF = 93,                       // enabled, yn1, a0, b0
    PB_REMOTE = 107, PB_REMOTE_MIXCTL = 108, PB_REMOTE_MIXER = 109,   // after the biquad (10 words): Wii Remote speaker mix, 4 x (main, delta, aux, delta)
    PB_WORDS = 160                     // 0x140 bytes (checked against live blocks: stereo stream, 22.05 kHz ADPCM)
};

static u32 phys(u32 a) { return a & 0x1FFFFFFF; }
static u16 rd16(u32 a) { u32 p = phys(a); if (p + 2 > 0x01800000u && (p < 0x10000000u || p + 2 > 0x14000000u)) return 0; return (u16)(g_mem[p] << 8 | g_mem[p + 1]); }
static void wr16(u32 a, u16 v) { u32 p = phys(a); if (p + 2 > 0x01800000u && (p < 0x10000000u || p + 2 > 0x14000000u)) return; g_mem[p] = v >> 8; g_mem[p + 1] = (u8)v; }
static u8 rd8(u32 p) { p = phys(p); if (p >= 0x01800000u && (p < 0x10000000u || p >= 0x14000000u)) return 0; return g_mem[p]; }
static void wr32(u32 a, u32 v) { wr16(a, v >> 16); wr16(a + 2, (u16)v); }
static u32 rd32(u32 a) { return (u32)rd16(a) << 16 | rd16(a + 2); }
static s16 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

typedef struct { u16 w[PB_WORDS]; u32 addr; } PB;

#include "sinc.h"
typedef struct { u32 addr, cur; int ratio_seen; float h[SINC_TAPS]; } VoiceHist;
#define NHIST 1024
static VoiceHist g_hist[NHIST];
unsigned g_hist_restarts;   // for the perf log   // per voice (by PB address): its last 16 input samples
static u32 pb32(const PB *p, int o) { return (u32)p->w[o] << 16 | p->w[o + 1]; }
static void pbset32(PB *p, int o, u32 v) { p->w[o] = v >> 16; p->w[o + 1] = (u16)v; }

// ---------------------------------------------------------------- sample fetch (the "accelerator")
// Addresses are in sample units of the format: nibbles (ADPCM), 16-bit words (PCM16), bytes (PCM8),
// relative to physical memory.
static s16 fetch(PB *p, int *ended) {
    int fmt = p->w[PB_FORMAT];
    u32 cur = pb32(p, PB_CUR_ADDR), end = pb32(p, PB_END_ADDR);
    s16 out = 0;
    if (fmt == 0) {   // ADPCM: 8-byte frames = header byte (predictor << 4 | scale) + 14 samples
        if ((cur & 15) == 0) { p->w[PB_PRED_SCALE] = rd8(cur >> 1); cur += 2; }
        int ps = p->w[PB_PRED_SCALE], scale = 1 << (ps & 15), pred = (ps >> 4) & 7;
        s16 c1 = (s16)p->w[PB_COEFS + 2 * pred], c2 = (s16)p->w[PB_COEFS + 2 * pred + 1];
        u8 b = rd8(cur >> 1);
        int nib = (cur & 1) ? (b & 15) : (b >> 4); if (nib >= 8) nib -= 16;
        s16 yn1 = (s16)p->w[PB_YN1], yn2 = (s16)p->w[PB_YN2];
        s32 v = ((nib * scale) << 11) + c1 * yn1 + c2 * yn2;
        v = (v + 1024) >> 11;
        out = clamp16(v);
        p->w[PB_YN2] = (u16)yn1; p->w[PB_YN1] = (u16)out;
        cur++;
    } else if (fmt == 0x0A) {        // PCM16
        out = (s16)rd16(cur * 2); cur++;
    } else if (fmt == 0x19) {        // PCM8
        out = (s16)((s8)rd8(cur) << 8); cur++;
    } else { cur++; }
    // end of sample data (the end address is inclusive)
    if (cur > end) {   // the end address is the last sample
        // Like the hardware accelerator: the address always wraps to the loop start, then the
        // microcode either continues (looping) or stops the voice. NW4R reads the position back to
        // decide a sound has finished, so a stopped voice must not be left past its end.
        cur = pb32(p, PB_LOOP_ADDR);
        if (p->w[PB_LOOPING]) {
            if (fmt == 0 && !p->w[PB_IS_STREAM]) {
                p->w[PB_PRED_SCALE] = p->w[PB_LOOP_PS]; p->w[PB_YN1] = p->w[PB_LOOP_YN1]; p->w[PB_YN2] = p->w[PB_LOOP_YN2];
            }
        } else { *ended = 1; }
    }
    pbset32(p, PB_CUR_ADDR, cur);
    return out;
}

// ---------------------------------------------------------------- one voice
static void process_voice(PB *p) {
    s32 in[NS];
    int ended = 0;
    // resampling: ratio is 16.16 input samples per output sample. A 16-tap band-limited filter (sinc.h)
    // over the voice's recent input; that history lives here, by PB address (the PB only holds 4 of
    // them), and starts over when the game has moved the voice (its position isn't where we left it).
    u32 ratio = pb32(p, PB_RATIO), frac = p->w[PB_FRAC];
    if (!ratio) ratio = 0x10000;
    VoiceHist *vh = &g_hist[(p->addr >> 5) & (NHIST - 1)];
    if (vh->addr != p->addr || vh->cur != pb32(p, PB_CUR_ADDR) || vh->ratio_seen == 0) {
        {   static int lines; g_hist_restarts++;
            if (vh->addr == p->addr && lines < 150) { lines++;   // the same voice, moved by the game: worth seeing
                fprintf(stderr, "[ax] voice %08X%s restarted: left at %08X, now %08X (loop %08X end %08X, ratio %08X, last %04X %04X %04X %04X)\n", p->addr,
                        p->w[PB_IS_STREAM] ? " stream" : "", vh->cur, pb32(p, PB_CUR_ADDR), pb32(p, PB_LOOP_ADDR), pb32(p, PB_END_ADDR), ratio,
                        p->w[PB_LAST], p->w[PB_LAST + 1], p->w[PB_LAST + 2], p->w[PB_LAST + 3]); } }
        memset(vh, 0, sizeof *vh); vh->addr = p->addr;
        vh->h[SINC_TAPS / 2 - 1] = (s16)p->w[PB_LAST + 2]; vh->h[SINC_TAPS / 2] = (s16)p->w[PB_LAST + 3];
        // the linear scheme's "next" sample (LAST+3) was already fetched: keep the inputs ahead of the
        // point filled by fetching the rest now
        for (int k = SINC_TAPS / 2 + 1; k < SINC_TAPS; k++) vh->h[k] = ended ? 0 : fetch(p, &ended);
    }
    vh->ratio_seen = 1;
    const SincTable *tab = sinc_table(ratio / 65536.0);
    for (int i = 0; i < NS; i++) {
        float v = sinc_apply(tab, vh->h, frac / 65536.0);
        in[i] = (s32)lrintf(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
        frac += ratio;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            memmove(vh->h, vh->h + 1, sizeof(float) * (SINC_TAPS - 1));
            vh->h[SINC_TAPS - 1] = ended ? 0 : fetch(p, &ended);
        }
    }
    p->w[PB_FRAC] = (u16)frac;
    p->w[PB_LAST + 2] = (u16)(s16)vh->h[SINC_TAPS / 2 - 1]; p->w[PB_LAST + 3] = (u16)(s16)vh->h[SINC_TAPS / 2];
    vh->cur = pb32(p, PB_CUR_ADDR);
    if (ended) vh->addr = 0;
    if (ended) p->w[PB_RUNNING] = 0;

    // low-pass filter: y = a0 * x + b0 * y[-1]  (a0, b0 in .15)
    if (p->w[PB_LPF]) {
        s32 y = (s16)p->w[PB_LPF + 1], a0 = p->w[PB_LPF + 2], b0 = p->w[PB_LPF + 3];
        for (int i = 0; i < NS; i++) { y = (a0 * in[i] + b0 * y) >> 15; in[i] = y; }
        p->w[PB_LPF + 1] = (u16)clamp16(y);
    }
    // volume envelope (.15, linear ramp per sample)
    s32 vol = p->w[PB_VOL], vd = (s16)p->w[PB_VOL_DELTA];
    for (int i = 0; i < NS; i++) { in[i] = (in[i] * vol) >> 15; vol += vd; if (vol < 0) vol = 0; if (vol > 0xFFFF) vol = 0xFFFF; }
    p->w[PB_VOL] = (u16)vol;

    // bus mixing (Wii mixer_control): main L 0x1, R 0x2, S 0x4, main ramp 0x8 | aux A L 0x10, R 0x20,
    // LR ramp 0x40, S 0x80, S ramp 0x100 | aux B 0x200, 0x400, 0x800, 0x1000, 0x2000 | aux C 0x4000..
    u32 mc = pb32(p, PB_MIXCTL);
    static const struct { int vol_word, bit, ramp, bus, ch; } dst[] = {
        { 0, 0x1, 0x8, -1, 0 }, { 2, 0x2, 0x8, -1, 1 }, { 16, 0x4, 0x8, -1, 2 },
        { 4, 0x10, 0x40, 0, 0 }, { 6, 0x20, 0x40, 0, 1 }, { 18, 0x80, 0x100, 0, 2 },
        { 8, 0x200, 0x800, 1, 0 }, { 10, 0x400, 0x800, 1, 1 }, { 20, 0x1000, 0x2000, 1, 2 },
        { 12, 0x4000, 0x10000, 2, 0 }, { 14, 0x8000, 0x10000, 2, 1 }, { 22, 0x20000, 0x40000, 2, 2 },
    };
    for (unsigned d = 0; d < sizeof dst / sizeof *dst; d++) {
        if (!(mc & dst[d].bit)) continue;
        int vw = PB_MIXER + dst[d].vol_word, ramp = (mc & dst[d].ramp) != 0;
        s32 cv = p->w[vw], delta = (s16)p->w[vw + 1];
        s32 *bus = dst[d].bus < 0 ? g_main[dst[d].ch] : g_aux[dst[d].bus][dst[d].ch];
        for (int i = 0; i < NS; i++) {
            bus[i] += (in[i] * cv) >> 15;
            if (ramp) { cv += delta; if (cv < 0) cv = 0; if (cv > 0xFFFF) cv = 0xFFFF; }
        }
        if (ramp) p->w[vw] = (u16)cv;
    }
    // the Wii Remote speakers (6 kHz): the voice taken down to 18 samples, at each Remote's volume
    if (p->w[PB_REMOTE]) {
        { static int lines; if (lines < 10) { lines++; fprintf(stderr, "[ax] voice %08X plays on the Wii Remote speaker (volumes %04X %04X %04X %04X, ctl %04X)\n", p->addr,
              p->w[PB_REMOTE_MIXER], p->w[PB_REMOTE_MIXER + 4], p->w[PB_REMOTE_MIXER + 8], p->w[PB_REMOTE_MIXER + 12], p->w[PB_REMOTE_MIXCTL]); } }
        for (int r = 0; r < 4; r++) {
            s32 cv = p->w[PB_REMOTE_MIXER + 4 * r]; if (!cv) continue;
            for (int k = 0; k < NWM; k++) {
                int a = k * NS / NWM, b = (k + 1) * NS / NWM; s32 sum = 0;
                for (int i = a; i < b; i++) sum += in[i];
                g_wm[r][k] += (sum / (b - a) * cv) >> 15;
            }
        }
    }
}

// Words the DSP writes back into a PB after processing.
static const int g_writeback[] = { PB_RUNNING, PB_VOL, PB_CUR_ADDR, PB_CUR_ADDR + 1, PB_PRED_SCALE, PB_YN1, PB_YN2,
                                   PB_FRAC, PB_LAST, PB_LAST + 1, PB_LAST + 2, PB_LAST + 3, PB_LPF + 1 };

static int g_voices_run;
static void process_list(u32 addr) {
    int guard = 0;
    while (addr && guard++ < 512) {
        PB pb; pb.addr = addr;
        for (int k = 0; k < PB_WORDS; k++) pb.w[k] = rd16(addr + 2 * k);
        u32 next = pb32(&pb, PB_NEXT);
        if (pb.w[PB_RUNNING] == 1) {
            u16 before[PB_WORDS]; memcpy(before, pb.w, sizeof before);
            static int axlog = -1; if (axlog < 0) { const char *e = getenv("TTPC_AXLOG"); axlog = e ? (atoi(e) == 2 ? 2 : 1) : 0; }
            if (axlog) {   // TTPC_AXLOG=1: every voice when it starts or changes (TTPC_AXLOG=2: streams only)
                if (axlog == 2 && !pb.w[PB_IS_STREAM]) goto nolog;
                static u32 last_loop[256], last_end[256], last_flags[256]; int slot = (addr >> 6) & 255;
                u32 flags = pb.w[PB_LOOPING] | pb.w[PB_FORMAT] << 4;
                if (pb32(&pb, PB_LOOP_ADDR) != last_loop[slot] || pb32(&pb, PB_END_ADDR) != last_end[slot] || flags != last_flags[slot])
                    fprintf(stderr, "[axv] %08X%s loop %08X end %08X cur %08X looping %u fmt %u ratio %08X vol %04X mix %08X gain %04X\n", addr, pb.w[PB_IS_STREAM] ? " stream" : "",
                            pb32(&pb, PB_LOOP_ADDR), pb32(&pb, PB_END_ADDR), pb32(&pb, PB_CUR_ADDR), pb.w[PB_LOOPING], pb.w[PB_FORMAT], pb32(&pb, PB_RATIO), pb.w[PB_VOL], pb32(&pb, PB_MIXCTL), pb.w[PB_GAIN]);
                last_loop[slot] = pb32(&pb, PB_LOOP_ADDR); last_end[slot] = pb32(&pb, PB_END_ADDR); last_flags[slot] = flags;
            }
            nolog:
            {   // a voice with anything set past the low-pass filter (biquad, Remote speaker fields): show it once
                static u32 seen[64]; static int nseen;
                int any = 0; for (int k = 97; k < 140; k++) if (pb.w[k]) any = 1;
                int known = 0; for (int k = 0; k < nseen; k++) if (seen[k] == addr) known = 1;
                if (any && !known && nseen < 64) { seen[nseen++] = addr;
                    char line[400]; int n = 0; for (int k = 97; k < 140; k++) n += snprintf(line + n, sizeof line - n, "%04X ", pb.w[k]);
                    fprintf(stderr, "[ax] voice %08X words 97-139: %s\n", addr, line); }
            }
            process_voice(&pb);
            if (axlog && pb.w[PB_RUNNING] == 0) fprintf(stderr, "[axv] %08X ended at %08X\n", addr, pb32(&pb, PB_CUR_ADDR));
            g_voices_run++;
            for (unsigned k = 0; k < sizeof g_writeback / sizeof *g_writeback; k++) wr16(addr + 2 * g_writeback[k], pb.w[g_writeback[k]]);
            for (int k = 0; k < 24; k++) if (pb.w[PB_MIXER + k] != before[PB_MIXER + k]) wr16(addr + 2 * (PB_MIXER + k), pb.w[PB_MIXER + k]);
        }
        addr = next;
    }
}

// Aux buses go to the CPU for effects (reverb etc.): upload L/R/S (96 s32 each) to `up`, and mix the
// processed buffers the CPU left at `down` (from the previous frame) into main.
static void mix_aux(int bus, u16 vol, u32 up, u32 down) {
    for (int ch = 0; ch < 3; ch++) for (int i = 0; i < NS; i++) wr32(up + 4 * (ch * NS + i), (u32)g_aux[bus][ch][i]);
    for (int ch = 0; ch < 3; ch++) for (int i = 0; i < NS; i++) {
        s32 v = (s32)rd32(down + 4 * (ch * NS + i));
        g_main[ch][i] += (s32)(((s64)v * vol) >> 15);
    }
}

static void output(u16 vol, u32 surround, u32 lr) {
    for (int i = 0; i < NS; i++) {
        s32 l = (s32)(((s64)g_main[0][i] * vol) >> 15), r = (s32)(((s64)g_main[1][i] * vol) >> 15);
        wr16(lr + 4 * i, (u16)clamp16(r));
        wr16(lr + 4 * i + 2, (u16)clamp16(l));
        wr32(surround + 4 * i, (u32)g_main[2][i]);
    }
}

void ax_run_list(u32 addr, u32 size) {
    u32 a = addr | 0x80000000u, end = a + (size ? size : 0x400);
    static int log_once;
    int guard = 0;
    while (a < end && guard++ < 64) {
        u16 cmd = rd16(a); a += 2;
        switch (cmd) {
        case 0x00: memset(g_main, 0, sizeof g_main); memset(g_aux, 0, sizeof g_aux); memset(g_wm, 0, sizeof g_wm); a += 4; break;
        case 0x01: case 0x02: case 0x03: {
            u32 src = rd32(a); a += 4;
            for (int i = 0; i < NS; i++) {
                s32 v = (s32)rd32(src + 4 * i);
                if (cmd == 0x01) { g_main[0][i] += v; g_main[1][i] += v; }
                else if (cmd == 0x02) { g_main[0][i] -= v; g_main[1][i] -= v; }
                else { g_main[0][i] += v; g_main[1][i] -= v; }
            }
            break;
        }
        case 0x04: g_voices_run = 0; process_list(rd32(a)); a += 4; break;
        case 0x05: case 0x06: case 0x07: {
            u16 vol = rd16(a); u32 up = rd32(a + 2), down = rd32(a + 6); a += 10;
            mix_aux(cmd - 0x05, vol, up, down);
            break;
        }
        case 0x08: case 0x09: a += 10; break;      // aux upload + LRS mix variants: rarely used
        case 0x0A: a += 8; break;                  // compressor: not modelled
        case 0x0B: { u16 vol = rd16(a); u32 s = rd32(a + 2), lr = rd32(a + 6); a += 10; output(vol, s, lr); break; }
        case 0x0C: { u16 vol = rd16(a); u32 s = rd32(a + 2), lr = rd32(a + 6); a += 10; output(vol, s, lr); break; }
        case 0x0D: {                               // Wii Remote speaker output: 18 s16 per Remote, for AXRmt
            { static int once; if (!once) { once = 1; fprintf(stderr, "[ax] Remote speaker output to %08X %08X %08X %08X\n", rd32(a), rd32(a + 4), rd32(a + 8), rd32(a + 12)); } }
            for (int r = 0; r < 4; r++) { u32 o = rd32(a + 4 * r);
                if (o) for (int k = 0; k < NWM; k++) wr16(o + 2 * k, (u16)clamp16(g_wm[r][k])); }
            a += 16; break;
        }
        case 0x0E: return;
        default:
            if (log_once++ < 5) fprintf(stderr, "[ax] unknown command %04X in list %08X\n", cmd, addr);
            return;
        }
    }
}

// Debug (TTPC_AXPB=1): dump the first few frames' running voice blocks.
int ax_dump_pbs(u32 list) {
    u32 a = list | 0x80000000u;
    static const int len[16] = { 4, 4, 4, 4, 4, 10, 10, 10, 10, 10, 8, 10, 10, 16, 0, 0 };
    for (int g = 0; g < 16; g++) {
        u16 cmd = rd16(a);
        if (cmd == 0x04) {
            u32 pb = rd32(a + 2); int n = 0, shown = 0;
            while (pb && n < 512) {
                if (rd16(pb + 2 * PB_RUNNING) != 0 && shown < 3) {
                    shown++;
                    fprintf(stderr, "[axpb] %08X:", pb);
                    for (int k = 0; k < 0xA0; k++) fprintf(stderr, "%s%04X", k % 16 ? " " : "\n   ", rd16(pb + 2 * k));
                    fprintf(stderr, "\n");
                }
                pb = rd32(pb); n++;
            }
            return shown;
        }
        if (cmd > 0x0D) return 0;
        a += 2 + len[cmd];
    }
    return 0;
}
