// High-level replacements for SDK functions. Each overrides the weak recompiled function at that
// address. Keep every replacement small and document what the original did.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#define HLE(addr, name) void f_##addr(CPU *c)

// __OSInitAudioSystem: uploads a tiny bootstrap program to the audio DSP and spins on DSP_CSR until
// the DSP acknowledges. Audio is handled natively on PC, so there is no DSP to wake up.
HLE(8021A9A0, __OSInitAudioSystem) { (void)c; fprintf(stderr, "[hle] __OSInitAudioSystem skipped\n"); }

// Debug hook for functions listed in RECOMP_LOG at lift time. Prints arguments and the game's
// frame-pacing state (queue at 0x804860B0, flags in small data) - adjust freely while investigating.
void rt_logfn(CPU *c, u32 addr) {
    {   // forensics investigation (TTPC_AUTTRACE=1): the body-examination module (0x8012E490..0x80132350) and the script-VM entry
        // points it calls. Each function is logged when its arguments change, so a click shows up as a short burst.
        static int on = -1; if (on < 0) on = getenv("TTPC_AUTTRACE") && *getenv("TTPC_AUTTRACE") != '0';
        if ((addr >= 0x8012E000u && addr < 0x80132800u) || (addr >= 0x80028000u && addr < 0x8002F000u) || addr == 0x80076CF0u) {
            if (!on) return;
            if (addr == 0x8002E600u || addr == 0x8002A500u) return;   // per-instruction script VM steps: far too frequent
            static u32 seen[512][4]; static int ns; int k;
            for (k = 0; k < ns; k++) if (seen[k][0] == addr) break;
            if (k == ns && ns < 512) { seen[ns][0] = addr; ns++; seen[k][1] = ~0u; }
            if (k < 512 && seen[k][1] == c->r[3] && seen[k][2] == c->r[4] && seen[k][3] == c->r[5]) return;
            if (k < 512) { seen[k][1] = c->r[3]; seen[k][2] = c->r[4]; seen[k][3] = c->r[5]; }
            char sx[3][40]; for (int i = 0; i < 3; i++) { u32 p = c->r[3 + i]; sx[i][0] = 0;
                if (p >= 0x80000000u && p < 0x81800000u) { int n = 0; for (; n < 32; n++) { u8 ch = R8(p + n); if (!ch) break; if (ch < 32 || ch > 126) { n = -1; break; } sx[i][n] = (char)ch; } if (n > 2) sx[i][n] = 0; else sx[i][0] = 0; } }
            fprintf(stderr, "[aut] %08X lr %08X r3 %08X%s%s%s r4 %08X%s%s%s r5 %08X%s%s%s r6 %08X f1 %.3f\n", addr, c->lr,
                    c->r[3], sx[0][0] ? " \"" : "", sx[0], sx[0][0] ? "\"" : "", c->r[4], sx[1][0] ? " \"" : "", sx[1], sx[1][0] ? "\"" : "",
                    c->r[5], sx[2][0] ? " \"" : "", sx[2], sx[2][0] ? "\"" : "", c->r[6], c->f[1].ps0);
            return;
        }
    }
    if (addr == 0x8000C660u || addr == 0x8000C6D0u) return;   // per-retrace callbacks: too chatty
    if (addr == 0x80248120u) {   // nandConvertErrorCode(ios_result): log every non-success
        if (c->r[3]) fprintf(stderr, "[nand] IOS result %d converted (caller lr %08X)\n", (s32)c->r[3], c->lr);
        return;
    }
    static u32 req;
    if (addr == 0x80022E40u) { req = c->r[4]; fprintf(stderr, "[save] NandProc request %08X op %u (lr %08X)\n", req, R32(req), c->lr); return; }
    if (addr == 0x80040200u) {
        static u32 last = ~0u; u32 b = req - 0xC;
        if (c->r[3] != last) fprintf(stderr, "[save] message %u | state %u res %d 0x12c %08X 0x128 %08X\n", c->r[3], R32(b), (s32)R32(b + 0x130), R32(b + 0x12C), R32(b + 0x128));
        last = c->r[3]; return;
    }
    static int n; if (n++ > 20000) return;
    u32 r13 = c->r[13];
    if (addr == 0x802320B0u || addr == 0x80232410u) {
        fprintf(stderr, "[fn %08X] drawdone flag %u | msr EE %u\n", addr, R8(r13 - 0x4720), (c->msr >> 15) & 1);
        return;
    }
    if (getenv("TTPC_LOGSHORT")) { fprintf(stderr, "[fn %08X] r3=%08X r4=%08X r5=%08X r6=%08X lr %08X\n", addr, c->r[3], c->r[4], c->r[5], c->r[6], c->lr); return; }
    fprintf(stderr, "[fn %08X] r3=%08X r4=%08X | q head %u tail %u | flags %u %u | last tok %u gpu tok %u | fb %08X\n",
            addr, c->r[3], c->r[4], R16(0x804860EE), R16(0x804860EC), R8(r13 - 0x51FB), R8(r13 - 0x51FA),
            R16(r13 - 0x51F8), R16(0xCC00100E), R32(r13 - 0x5200));
}


// ---------------------------------------------------------------- developer menus
// The game runs as numbered "sequences" (table at 0x803413B0, switched by f_80007760(seq, a, b)).
// Retail boots sequence 1 (title). The developers' own start menu (sequence 0) and test scenes are
// still in the executable, just never reached. TTPC_SEQ=N boots one directly (tests); the options
// menu switches to one from the running game (below), which keeps the console's normal start-up.
#include "platform.h"
void f_80009900(CPU *c); void f_80007760(CPU *c);
HLE(80007700, cadSequenceInit) {
    c->r[3] = 0x803413A0u;   // "cadSequence"
    c->r[4] = 0x1003; c->r[5] = 0; c->r[6] = 0; c->r[7] = 0x80007690u; c->r[8] = 0; c->r[9] = 0;
    f_80009900(c);
    int seq = 1;
    const char *e = getenv("TTPC_SEQ");
    if (e && *e) seq = atoi(e);
    if (seq != 1) fprintf(stderr, "[hle] booting sequence %d\n", seq);
    c->r[3] = (u32)seq; c->r[4] = 0; c->r[5] = 0;
    f_80007760(c);
}
// Switching later, from the running game, keeps the boot path (title, system setup, Wii Remote and
// Bluetooth start-up) exactly as on the console. The request is taken at the top of the sequence
// manager's own per-frame task, where the game itself switches.
volatile int g_dev_seq_request = -1;
void orig_80007690(CPU *c);
u64 wiimote_reads(void);
HLE(80007690, cadSequenceTask) {
    static long at = -2; static int at_seq;
    if (at == -2) { const char *e = getenv("TTPC_SEQ_AT"); at = -1; if (e) sscanf(e, "%ld:%d", &at, &at_seq); }   // read:seq (tests)
    if (at >= 0 && wiimote_reads() >= (u64)at) { g_dev_seq_request = at_seq; at = -1; }
    {   // tests: log sequence changes; TTPC_MEMDUMP="read,read" writes RAM snapshots (mem_<read>.bin)
        static int last = -99; int cur = (int)R32(c->r[13] - 32760u);
        if (cur != last) { fprintf(stderr, "[seq] now %d (read %llu)\n", cur, (unsigned long long)wiimote_reads()); last = cur; }
        static const char *md; static int mdi; if (!mdi) { mdi = 1; md = getenv("TTPC_MEMDUMP"); }
        static unsigned done; int k = 0;
        if (md) { const char *q = md; while (q && *q) { long r = atol(q); if ((long)wiimote_reads() >= r && !(done & (1u << k))) { done |= 1u << k;
            char fn[64]; snprintf(fn, sizeof fn, "mem_%ld.bin", r); FILE *f = fopen(fn, "wb");
            if (f) { extern u8 *g_mem; fwrite(g_mem, 1, 0x01800000, f); fclose(f); fprintf(stderr, "[seq] dumped %s\n", fn); } }
            q = strchr(q, ','); if (q) q++; k++; } }
    }
    int r = g_dev_seq_request;
    if (r >= 0) {
        g_dev_seq_request = -1;
        fprintf(stderr, "[hle] switching to sequence %d\n", r);
        u32 sp = c->r[1], lr = c->lr;
        c->r[3] = (u32)r; c->r[4] = 0; c->r[5] = 0; f_80007760(c);
        c->r[1] = sp; c->lr = lr; c->r[3] = 0;
        return;
    }
    orig_80007690(c);
}
