// Virtual Wii Remote + Nunchuk on channel 0, fed from host input (platform.c).
//
// The game reads controllers through KPAD: WPADProbe(chan, &type) to see what is connected and
// KPADRead(chan, samples, max) -> KPADReadEx (0xB0-byte KPADStatus samples). Both are replaced here,
// so the Bluetooth stack underneath can stay idle.
//
// KPADStatus layout (this SDK, 0xB0 bytes):
//   0x00 hold  0x04 trig  0x08 release              (u32 button masks)
//   0x0C acc (x,y,z)  0x18 acc_value  0x1C acc_speed
//   0x20 pos (x,y: -1..1, y down)  0x28 vec  0x30 speed
//   0x34 horizon (x,y)  0x3C hori_vec  0x44 hori_speed
//   0x48 dist  0x4C dist_vec  0x50 dist_speed  0x54 acc_vertical (x,y)
//   0x5C dev_type  0x5D wpad_err  0x5E dpd_valid_fg  0x5F data_format
//   0x60 Nunchuk: stick (x,y)  0x68 acc (x,y,z)  0x74 acc_value  0x78 acc_speed
//
// Default mapping (mouse + keyboard):
//   mouse = pointer, left button = A, right button = B
//   arrows = D-pad, WASD = Nunchuk stick, Space/Enter = A, Shift = B, Q/C = C, E/F = Z
//   1, 2 = 1/2 buttons, +/- (or P/M) = Plus/Minus, Esc/H = Home
// Game controller (platform.c): left stick = Nunchuk stick, right stick = pointer (RB recentres),
//   A = A, B / RT = B, LT = Z, LB = C, D-pad, X/Y = 1/2, Start/Back = +/-, Guide = Home
//
// TTPC_INPUT scripts presses for headless tests: "600+10:A;900+5:A,B;1200+60:right"
//   = from the game's 600th controller read (~ game frame) hold A for 10 reads, etc. Names: A B 1 2 plus minus home up down left right C Z.
#include "cpu.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    BTN_LEFT = 0x0001, BTN_RIGHT = 0x0002, BTN_DOWN = 0x0004, BTN_UP = 0x0008, BTN_PLUS = 0x0010,
    BTN_2 = 0x0100, BTN_1 = 0x0200, BTN_B = 0x0400, BTN_A = 0x0800, BTN_MINUS = 0x1000,
    BTN_Z = 0x2000, BTN_C = 0x4000, BTN_HOME = 0x8000,
};

static u32 g_hold, g_prev, g_trig, g_rel, g_host_hold;
static u64 g_reads;   // controller reads by the game (~ game frames): the clock for TTPC_INPUT
static float g_px, g_py, g_ppx, g_ppy, g_sx, g_sy;
static int g_point_valid = 1;
static u64 g_vi;

typedef struct { u64 from, len; u32 mask; } ScriptEv;
static ScriptEv g_script[4096]; static int g_nscript;

static u32 name_mask(const char *s, int n) {
    static const struct { const char *n; u32 m; } t[] = {
        {"A",BTN_A},{"B",BTN_B},{"1",BTN_1},{"2",BTN_2},{"plus",BTN_PLUS},{"minus",BTN_MINUS},{"home",BTN_HOME},
        {"up",BTN_UP},{"down",BTN_DOWN},{"left",BTN_LEFT},{"right",BTN_RIGHT},{"C",BTN_C},{"Z",BTN_Z} };
    for (unsigned k = 0; k < sizeof t / sizeof *t; k++) if ((int)strlen(t[k].n) == n && !strncmp(s, t[k].n, n)) return t[k].m;
    fprintf(stderr, "[wiimote] unknown button '%.*s' in TTPC_INPUT\n", n, s);
    return 0;
}
static void parse_script(void) {
    const char *s = getenv("TTPC_INPUT");
    while (s && *s && g_nscript < 4096) {
        char *e; ScriptEv ev = {0};
        ev.from = strtoull(s, &e, 10); s = e;
        ev.len = 1; if (*s == '+') { ev.len = strtoull(s + 1, &e, 10); s = e; }
        if (*s == ':') s++;
        while (*s && *s != ';') { const char *b = s; while (*s && *s != ',' && *s != ';') s++; ev.mask |= name_mask(b, (int)(s - b)); if (*s == ',') s++; }
        if (*s == ';') s++;
        g_script[g_nscript++] = ev;
    }
}

// Called once per retrace (after platform_poll): latch this frame's buttons and pointer.
void wiimote_frame(void) {
    static int init; if (!init) { init = 1; parse_script(); }
    g_vi++;
    const Input *in = &g_input;
    u32 h = 0;
    if (in->up) h |= BTN_UP;       if (in->down) h |= BTN_DOWN;
    if (in->left) h |= BTN_LEFT;   if (in->right) h |= BTN_RIGHT;
    if (in->a || in->lmb) h |= BTN_A;
    if (in->b || in->rmb) h |= BTN_B;
    if (in->plus) h |= BTN_PLUS;   if (in->minus) h |= BTN_MINUS;
    if (in->home) h |= BTN_HOME;   if (in->one) h |= BTN_1;   if (in->two) h |= BTN_2;
    if (in->c) h |= BTN_C;         if (in->z) h |= BTN_Z;
    g_host_hold = h;
    g_ppx = g_px; g_ppy = g_py;
    if (g_window_open && !getenv("TTPC_POINTER")) {
        g_px = in->mouse_x * 2.f - 1.f; g_py = in->mouse_y * 2.f - 1.f;
        g_point_valid = in->mouse_in && fabsf(g_px) <= 1.05f && fabsf(g_py) <= 1.05f;
    } else {   // headless: aim at TTPC_POINTER="x,y" (-1..1), default the centre
        static float hx, hy; static int got;
        if (!got) { got = 1; const char *e = getenv("TTPC_POINTER"); if (e) sscanf(e, "%f,%f", &hx, &hy); }
        g_px = hx; g_py = hy; g_point_valid = 1;
    }
    if (in->has_analog) { g_sx = in->nx; g_sy = in->ny; }
    else {
        g_sx = (float)(in->stick_right - in->stick_left);
        g_sy = (float)(in->stick_up - in->stick_down);
        if (g_sx && g_sy) { g_sx *= 0.7071f; g_sy *= 0.7071f; }
    }
}

static void wf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); W32(a, v); }

// WPADProbe(chan, u32 *type) -> 0 connected (type 1 = Wii Remote + Nunchuk), -1 nothing there.
void f_802533C0(CPU *c) {
    u32 chan = c->r[3], type = c->r[4];
    if (chan == 0) { if (type) W32(type, 1); c->r[3] = 0; }
    else { if (type) W32(type, 0xFD); c->r[3] = (u32)-1; }
}

// KPADReadEx(chan, KPADStatus *samples, u32 max, void *uwpad, s32 *err) -> number of samples.
void f_8025F830(CPU *c) {
    u32 chan = c->r[3], buf = c->r[4], max = c->r[5], err = c->r[7];
    static int dbg = -1; if (dbg < 0) dbg = getenv("TTPC_PADLOG") != NULL;
    if (0 && dbg && chan == 0) { static u64 n; if (n++ % 600 == 0 || g_trig) fprintf(stderr, "[pad] read #%llu vi %llu hold %04X trig %04X lr %08X\n", (unsigned long long)n, (unsigned long long)g_vi, g_hold, g_trig, c->lr); }
    if (chan != 0 || !max) { if (err) W32(err, (u32)-1); c->r[3] = 0; return; }   // no controller
    // buttons are latched per read: host input from the last retrace plus any scripted presses
    g_reads++;
    u32 h = g_host_hold;
    for (int k = 0; k < g_nscript; k++) if (g_reads >= g_script[k].from && g_reads < g_script[k].from + g_script[k].len) h |= g_script[k].mask;
    g_prev = g_hold; g_hold = h; g_trig = g_hold & ~g_prev; g_rel = g_prev & ~g_hold;
    if (dbg && (g_trig || g_reads % 600 == 0)) fprintf(stderr, "[pad] read %llu vi %llu hold %04X trig %04X\n", (unsigned long long)g_reads, (unsigned long long)g_vi, g_hold, g_trig);
    for (u32 k = 0; k < 0xB0; k += 4) W32(buf + k, 0);
    W32(buf + 0x00, g_hold); W32(buf + 0x04, g_trig); W32(buf + 0x08, g_rel);
    wf(buf + 0x0C, 0.f); wf(buf + 0x10, -1.f); wf(buf + 0x14, 0.f); wf(buf + 0x18, 1.f);   // held level, pointing at the TV
    wf(buf + 0x20, g_px); wf(buf + 0x24, g_py);
    float vx = g_px - g_ppx, vy = g_py - g_ppy;
    wf(buf + 0x28, vx); wf(buf + 0x2C, vy); wf(buf + 0x30, sqrtf(vx * vx + vy * vy));
    wf(buf + 0x34, 1.f); wf(buf + 0x38, 0.f);       // horizon: no roll
    wf(buf + 0x48, 1.5f);                            // distance to the sensor bar (m)
    wf(buf + 0x54, 1.f); wf(buf + 0x58, 0.f);       // acc_vertical
    W8(buf + 0x5C, 1);                               // dev_type: Wii Remote + Nunchuk
    W8(buf + 0x5D, 0);                               // wpad_err: OK
    W8(buf + 0x5E, g_point_valid ? 2 : 0);           // pointer seen (2 = both sensor bar dots)
    W8(buf + 0x5F, 5);                               // data format: Nunchuk + accelerometer + pointer
    wf(buf + 0x60, g_sx); wf(buf + 0x64, g_sy);     // Nunchuk stick
    wf(buf + 0x68, 0.f); wf(buf + 0x6C, -1.f); wf(buf + 0x70, 0.f); wf(buf + 0x74, 1.f);
    if (err) W32(err, 0);
    c->r[3] = 1;
}
