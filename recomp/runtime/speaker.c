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
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int wiimote_native(void); int whid_speaker(const s16 *pcm, int n);
void platform_speaker_level(float level);   // controller rumble / DualSense (platform.c)
int platform_last_dev(void);
// the real Remote's speaker - while the Remote is what's being played with (a controller picked up last gets the
// sound on its own speaker instead, a DualSense's)
static int remote_out(void) { return wiimote_native() && (platform_last_dev() == 3 || platform_last_dev() == 1); }
int platform_speaker_pcm(const s16 *pcm, int n);   // 1: a DualSense played it (then not the PC speakers)

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

// Some PC Bluetooth adapters never play a Remote's speaker data: TTPC_SPEAKER=both plays it on the PC as well.
static int mode(void) {   // 0 off, 1 the Remote if there is one (else the PC), 2 PC only, 3 Remote only, 4 both
    const char *e = getenv("TTPC_SPEAKER");
    if (e) return *e == '0' ? 0 : !strcmp(e, "pc") ? 2 : !strcmp(e, "remote") ? 3 : !strcmp(e, "both") ? 4 : 1;
    static const int map[5] = { 1, 2, 3, 4, 0 };   // settings: Automatic, PC, Remote, Both, Off
    return map[g_cfg.speaker >= 0 && g_cfg.speaker <= 4 ? g_cfg.speaker : 0];
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
                        ch ? "dropped (not channel 0)" : !(mode() != 2 && wiimote_native()) ? "PC" : mode() == 4 ? "Wii Remote + PC" : "Wii Remote");
            }
            last_data[ch] = data;
        }
        if (ch == 0) {
            int sent = mode() != 2 && remote_out() && whid_speaker(pcm, 40);
            if ((!sent && mode() != 3) || mode() == 4) { if (!platform_speaker_pcm(pcm, 40)) ring_push(pcm, 40); platform_speaker_level(sqrtf(e / 40) / 8000.f); }
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
static void spk_volume_check(CPU *c);
// WPADIsSpeakerEnabled(chan) -> bool
void f_80254250(CPU *c) { c->r[3] = c->r[3] == 0 && g_spk_enabled; }
// WPADControlSpeaker(chan, cmd, cb) -> 0; cb(chan, 0) once done. cmd: 0 off, 1 on, 2 mute, 3 unmute, 4 play
void f_802542A0(CPU *c) {
    u32 chan = c->r[3], cmd = c->r[4], cb = c->r[5];
    static int lines; if (lines++ < 40) fprintf(stderr, "[speaker] WPADControlSpeaker(%u, %u)\n", chan, cmd);
    spk_volume_check(c);
    if (chan != 0) { c->r[3] = (u32)-1; return; }   // WPAD_ERR_NO_CONTROLLER
    if (cmd == 0) g_spk_enabled = 0; else if (cmd == 1) g_spk_enabled = 1;
    if (cb) { u32 lr = c->lr; c->r[3] = chan; c->r[4] = 0; rt_call(c, cb); c->lr = lr; }
    c->r[3] = 0;
}
// WPADCanSendStreamData(chan) -> bool; WPADSendStreamData(chan, data, len) -> 0 (the alarm above sends instead)
void f_80255A70(CPU *c) { c->r[3] = c->r[3] == 0 && g_spk_enabled; }
// The game's sound library (NW4R, voices mixed for the Remote by AX) encodes 6 kHz 4-bit Yamaha ADPCM itself and
// sends it here 20 bytes (40 samples) at a time: forward the packet as it is to a real Remote, or decode it for
// the PC (speakers + rumble + DualSense) - the same destinations as the alarm above.
int whid_speaker_raw(const u8 *adpcm, int n);
void f_80255B00(CPU *c) {
    u32 chan = c->r[3], data = c->r[4]; int len = (int)c->r[5];
    if (chan != 0 || !mode()) { c->r[3] = chan ? (u32)-1 : 0; return; }
    if (len > 20) len = 20; if (len < 0) len = 0;
    u8 b[20]; for (int i = 0; i < len; i++) b[i] = R8(data + i);
    static int lines; if (lines < 3) { lines++; fprintf(stderr, "[speaker] stream data from the sound library (%d bytes) -> %s\n", len, mode() != 2 && wiimote_native() ? (mode() == 4 ? "Wii Remote + PC" : "Wii Remote") : "PC"); }
    int sent = mode() != 2 && remote_out() && whid_speaker_raw(b, len);
    if ((!sent && mode() != 3) || mode() == 4) {
        static const int DIFF[16] = { 1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15 };
        static const int SCALE[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
        static int pred, step = 127;
        s16 pcm[40]; float e = 0; int n = 0;
        for (int i = 0; i < len; i++) for (int h = 0; h < 2; h++) {
            int code = h ? b[i] & 15 : b[i] >> 4;
            pred += step * DIFF[code] / 8; if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768;
            step = step * SCALE[code & 7] >> 8; if (step < 127) step = 127; if (step > 24576) step = 24576;
            pcm[n++] = (s16)pred; e += (float)pred * pred;
        }
        if (!platform_speaker_pcm(pcm, n)) ring_push(pcm, n); platform_speaker_level(n ? sqrtf(e / n) / 8000.f : 0);
    }
    c->r[3] = 0;
}

// WPADGetSpeakerVolume() -> 0..127 (the console's setting, BT.SPKV in SYSCONF, read by WPADInit). A save without
// that setting reads as 0, and at 0 the game sends nothing to the Remote's speaker and skips the lines that need
// it: then use the console's default.
static void spk_volume_check(CPU *c) {
    static int checked; if (checked) return; checked = 1;
    u32 addr = c->r[13] + 4294949991u;   // WPAD's copy of the setting (SDA)
    int v = R8(addr);
    fprintf(stderr, "[speaker] Remote speaker volume setting: %d%s\n", v, v ? "" : " - using 88 (the Wii's default)");
    if (!v) W8(addr, 88);
}
void f_802558A0(CPU *c) { spk_volume_check(c); c->r[3] = R8(c->r[13] + 4294949991u); }

// Diagnostics: which output lines the game's sounds ask for (bit 0 = TV; any higher bit = a Wii Remote's speaker)
void orig_802EF410(CPU *c); void orig_80302EA0(CPU *c);
static void log_line(const char *what, u32 line, u32 lr) {
    static u32 seen[32][2]; static int n;
    for (int i = 0; i < n; i++) if (seen[i][0] == line && seen[i][1] == lr) return;
    if (n < 32) { seen[n][0] = line; seen[n][1] = lr; n++; fprintf(stderr, "[speaker] %s output line %X (from %08X)\n", what, line, lr); }
}
void f_802EF410(CPU *c) { log_line("new channel", c->r[7], c->lr); orig_802EF410(c); }
void f_80302EA0(CPU *c) { log_line("voice", c->r[4], c->lr); orig_80302EA0(c); }
void orig_80024690(CPU *c); void orig_802EE990(CPU *c);
void f_80024690(CPU *c) {   // the game: drop the speakers of Remotes that aren't there from an output mask
    u32 in = c->r[3], lr = c->lr; orig_80024690(c);
    static int n; if (n < 60 && (in & ~1u)) { n++; fprintf(stderr, "[speaker] sound for the Remote: lines %X -> %X (from %08X)\n", in, c->r[3], lr); }
}
void f_802EE990(CPU *c) {   // BasicSound output lines
    if (c->r[4] & ~1u) { static int n; if (n++ < 60) fprintf(stderr, "[speaker] sound %08X output lines %X (from %08X)\n", c->r[3], c->r[4], c->lr); }
    orig_802EE990(c);
}
