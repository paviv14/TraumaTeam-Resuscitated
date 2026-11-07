// High-level replacements for SDK functions. Each overrides the weak recompiled function at that
// address. Keep every replacement small and document what the original did.
#include "cpu.h"
#include <stdio.h>

#define HLE(addr, name) void f_##addr(CPU *c)

// __OSInitAudioSystem: uploads a tiny bootstrap program to the audio DSP and spins on DSP_CSR until
// the DSP acknowledges. Audio is handled natively on PC, so there is no DSP to wake up.
HLE(8021A9A0, __OSInitAudioSystem) { (void)c; fprintf(stderr, "[hle] __OSInitAudioSystem skipped\n"); }
