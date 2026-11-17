// Band-limited resampling filter shared by the AX voice mixer (ax.c) and the PC output (platform.c).
//
// Straight-line interpolation between two samples lets through strong mirror images of the high notes
// (a 22 kHz sample played at 32 kHz gets a copy of an 8 kHz tone at 14 kHz only ~10 dB down) - the
// constant "crunchy" sound of GitHub issue #1. Here every output sample is a 16-tap Kaiser-windowed sinc
// over the input around it: images end up ~50 dB down.
//
// SINC_TAPS input samples h[0..15] around the point, which lies between h[7] and h[8] at fraction f.
#pragma once
#include <math.h>

#define SINC_TAPS 16
#define SINC_PHASES 256

typedef struct { float c[SINC_PHASES + 1][SINC_TAPS]; int fc_q; } SincTable;

static double sinc_i0(double x) {   // modified Bessel function of the first kind, order 0
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

// cutoff fc in units of the input's Nyquist frequency (1.0 = keep everything the input can hold)
static void sinc_build(SincTable *t, int fc_q) {
    double fc = fc_q / 64.0, beta = 7.0;
    for (int p = 0; p <= SINC_PHASES; p++) {
        double f = (double)p / SINC_PHASES, sum = 0, h[SINC_TAPS];
        for (int k = 0; k < SINC_TAPS; k++) {
            double d = k - (SINC_TAPS / 2 - 1) - f, x = d / (SINC_TAPS / 2.0);
            double win = fabs(x) >= 1 ? 0 : sinc_i0(beta * sqrt(1 - x * x)) / sinc_i0(beta);
            double s = d == 0 ? 1 : sin(M_PI * fc * d) / (M_PI * fc * d);
            h[k] = fc * s * win; sum += h[k];
        }
        for (int k = 0; k < SINC_TAPS; k++) t->c[p][k] = (float)(h[k] / sum);
    }
    t->fc_q = fc_q;
}

// the table for a resampling step (input samples per output sample): full band when the step is 1 or
// below (upsampling), narrowed to the output's band when it is above (downsampling)
static const SincTable *sinc_table(double step) {
    static SincTable tabs[65]; static int built[65];
    int q = step <= 1.0 ? 58 : (int)(58.0 / step); if (q < 4) q = 4; if (q > 64) q = 64;
    if (step == 1.0) q = 64;
    if (!built[q]) { sinc_build(&tabs[q], q); built[q] = 1; }
    return &tabs[q];
}

// one output sample: h = the 16 inputs, f in [0,1)
static inline float sinc_apply(const SincTable *t, const float *h, double f) {
    double pf = f * SINC_PHASES; int p = (int)pf; float a = (float)(pf - p);
    const float *c0 = t->c[p], *c1 = t->c[p + 1];
    float s = 0;
    for (int k = 0; k < SINC_TAPS; k++) s += h[k] * (c0[k] + a * (c1[k] - c0[k]));
    return s;
}
