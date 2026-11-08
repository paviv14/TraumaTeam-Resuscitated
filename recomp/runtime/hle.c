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

