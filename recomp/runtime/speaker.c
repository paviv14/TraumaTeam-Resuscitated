// The Wii Remote's speaker (stethoscope heartbeats and the like).
//
// The game streams its speaker sounds itself: an alarm every 6.67 ms (fn_80314B80, the SDK's speaker helper)
// takes 40 samples (6000 Hz, s16) of the sound playing on each channel (struct at *(0x80580838), 0x68 per
// channel: +0x50 data, +0x54 bytes left, +0x58 id, +0x5D volume), encodes them (WENC, 4-bit ADPCM) and sends
// them with WPADSendStreamData - which goes nowhere on the PC. Here channel 0's samples are taken straight
// from that struct instead and go to:
//   - a real Wii Remote's speaker (wiimote_hid.c), when one is connected;
//   - otherwise the PC's sound, mixed into the game audio (platform.c), with the controller rumbling along
//     to the sound's loudness (a heartbeat thumps) - and a wired DualSense's own speaker when there is one.
// TTPC_SPEAKER=pc forces the PC's sound even with a real Remote; TTPC_SPEAKER=0 turns it all off.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int wiimote_native(void); int whid_speaker(const s16 *pcm, int n);
void platform_speaker_level(float level);   // controller rumble / DualSense (platform.c)
void platform_speaker_pcm(const s16 *pcm, int n);

// ---------------------------------------------------------------- the PC side: a ring of 6000 Hz samples
#define RING 16384
static s16 g_ring[RING]; static volatile unsigned g_w, g_r;
static void ring_push(const s16 *p, int n) { for (int i = 0; i < n; i++) { if (g_w - g_r >= RING) g_r++; g_ring[g_w++ % RING] = p[i]; } }
// mix into the game's stereo output (rate Hz): called from platform_audio_push
void speaker_mix(s16 *lr, int frames, int rate) {
    static double pos;   // in speaker samples
    if (g_w == g_r) { pos = 0; return; }
    double step = 6000.0 / rate;
    for (int i = 0; i < frames; i++) {
        unsigned k = g_r + (unsigned)pos;
        if (k + 1 >= g_w) { g_r = g_w; pos = 0; break; }
        double f = pos - (int)pos;
        int s = (int)(g_ring[k % RING] * (1 - f) + g_ring[(k + 1) % RING] * f);
        for (int c = 0; c < 2; c++) { int v = lr[2 * i + c] + s; lr[2 * i + c] = (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
        pos += step;
    }
    unsigned adv = (unsigned)pos; g_r += adv; pos -= adv;
}

static int mode(void) {   // 0 off, 1 automatic, 2 PC always
    static int m = -1;
    if (m < 0) { const char *e = getenv("TTPC_SPEAKER"); m = !e ? 1 : *e == '0' ? 0 : !strcmp(e, "pc") ? 2 : 1; }
    return m;
}

// fn_80314B80: the speaker alarm (all channels). Channel 0 is ours; the rest stay silent, as there is no Remote.
void f_80314B80(CPU *c) {
    u32 base = R32(0x80580838);
    if (!base || !mode()) return;
    for (int ch = 0; ch < 4; ch++) {
        u32 st = base + 0x68 * ch, data = R32(st + 0x50);
        if (!data) continue;
        int left = (int)R32(st + 0x54), n = left / 2; if (n > 40) n = 40;
        int vol = (s8)R8(st + 0x5D);
        s16 pcm[40]; float e = 0;
        for (int i = 0; i < 40; i++) {
            int s = i < n ? (s16)R16(data + 2 * i) * vol / 10 : 0;
            pcm[i] = (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s); e += (float)pcm[i] * pcm[i];
        }
        {   static u32 last_data[4]; static int lines;
            if (last_data[ch] + 0x50 != data && lines < 200) { lines++;   // a new sound started on this channel
                fprintf(stderr, "[speaker] ch %d sound %d: %d bytes, volume %d -> %s\n", ch, (int)R32(st + 0x58), left, vol,
                        ch ? "dropped (not channel 0)" : !(mode() == 1 && wiimote_native()) ? "PC" : "Wii Remote");
            }
            last_data[ch] = data;
        }
        if (ch == 0) {
            int sent = mode() == 1 && wiimote_native() && whid_speaker(pcm, 40);
            if (!sent) { ring_push(pcm, 40); platform_speaker_pcm(pcm, 40); platform_speaker_level(sqrtf(e / 40) / 8000.f); }
        }
        W32(st + 0x50, data + 0x50);
        int rem = left - 0x50; W32(st + 0x54, (u32)rem);
        if (rem <= 0) { W32(st + 0x58, (u32)-1); W32(st + 0x50, 0); if (ch == 0) platform_speaker_level(0); }
    }
    (void)c;
}

// ---------------------------------------------------------------- the speaker as WPAD sees it
// The console's Bluetooth stack isn't run (the Wii Remote is answered at KPAD level, wiimote.c), so WPAD's own
// speaker calls failed and WPADIsSpeakerEnabled stayed false: the game then skipped every speaker sound - and
// the dialogue lines that wait for them (Naomi's phone calls). Here channel 0's speaker always works.
void rt_call(CPU *c, u32 addr);
static int g_spk_enabled;
// WPADIsSpeakerEnabled(chan) -> bool
void f_80254250(CPU *c) { c->r[3] = c->r[3] == 0 && g_spk_enabled; }
// WPADControlSpeaker(chan, cmd, cb) -> 0; cb(chan, 0) once done. cmd: 0 off, 1 on, 2 mute, 3 unmute, 4 play
void f_802542A0(CPU *c) {
    u32 chan = c->r[3], cmd = c->r[4], cb = c->r[5];
    static int lines; if (lines++ < 40) fprintf(stderr, "[speaker] WPADControlSpeaker(%u, %u)\n", chan, cmd);
    if (chan != 0) { c->r[3] = (u32)-1; return; }   // WPAD_ERR_NO_CONTROLLER
    if (cmd == 0) g_spk_enabled = 0; else if (cmd == 1) g_spk_enabled = 1;
    if (cb) { u32 lr = c->lr; c->r[3] = chan; c->r[4] = 0; rt_call(c, cb); c->lr = lr; }
    c->r[3] = 0;
}
// WPADCanSendStreamData(chan) -> bool; WPADSendStreamData(chan, data, len) -> 0 (the alarm above sends instead)
void f_80255A70(CPU *c) { c->r[3] = c->r[3] == 0 && g_spk_enabled; }
void f_80255B00(CPU *c) { c->r[3] = c->r[3] == 0 ? 0 : (u32)-1; }
