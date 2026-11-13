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
// Push the Remote toward the screen (endoscope etc.): mouse wheel up / R / Tab, or the right-stick click;
// pull it back: mouse wheel down / V.
// Swing both down hard (CPR compressions): middle mouse button / G, or the left-stick click.
// Twist the Remote (bandages, turning tools): hold A and move the pointer in circles (or hold J / L).
//
// TTPC_INPUT scripts presses for headless tests: "600+10:A;900+5:A,B;1200+60:right"
//   = from the game's 600th controller read (~ game frame) hold A for 10 reads, etc. Names: A B 1 2 plus minus home up down left right C Z.
//   Also: su sd sl sr (Nunchuk stick up/down/left/right) and @x/y (pointer, -1..1) for the event's span.
#include "cpu.h"
#include "platform.h"
static float g_twist_step;   // mouse-wheel twist waiting to be applied (roll_motion)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "wiimote_hid.h"

enum {
    BTN_LEFT = 0x0001, BTN_RIGHT = 0x0002, BTN_DOWN = 0x0004, BTN_UP = 0x0008, BTN_PLUS = 0x0010,
    BTN_2 = 0x0100, BTN_1 = 0x0200, BTN_B = 0x0400, BTN_A = 0x0800, BTN_MINUS = 0x1000,
    BTN_Z = 0x2000, BTN_C = 0x4000, BTN_HOME = 0x8000,
    PUSH_FLAG = 0x40000000, PULL_FLAG = 0x20000000, SWING_FLAG = 0x10000000,   // internal: push / pull gestures (not KPAD button bits)
    ROLL_L_FLAG = 0x08000000, ROLL_R_FLAG = 0x04000000,                        // internal: twist the Remote left / right
};

static u32 g_hold, g_prev, g_trig, g_rel, g_host_hold;
static u64 g_reads;   // controller reads by the game (~ game frames): the clock for TTPC_INPUT
u64 wiimote_reads(void) { return g_reads; }
static float g_px, g_py, g_ppx, g_ppy, g_sx, g_sy;
static int g_point_valid = 1;
static u64 g_vi;

typedef struct { u64 from, len; u32 mask; int stick, has_ptr; float px, py; float sx, sy; } ScriptEv;
static ScriptEv g_script[4096]; static int g_nscript;
static u64 script_end;
int wiimote_script_active(void) { return g_nscript && g_reads < script_end + 30; }   // VR hides the scripted menu

static u32 name_mask(const char *s, int n) {
    static const struct { const char *n; u32 m; } t[] = {
        {"A",BTN_A},{"B",BTN_B},{"1",BTN_1},{"2",BTN_2},{"plus",BTN_PLUS},{"minus",BTN_MINUS},{"home",BTN_HOME},
        {"up",BTN_UP},{"down",BTN_DOWN},{"left",BTN_LEFT},{"right",BTN_RIGHT},{"C",BTN_C},{"Z",BTN_Z},{"push",PUSH_FLAG},{"pull",PULL_FLAG},{"swing",SWING_FLAG},{"rolll",ROLL_L_FLAG},{"rollr",ROLL_R_FLAG} };
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
        while (*s && *s != ';') {
            const char *b = s; while (*s && *s != ',' && *s != ';') s++;
            int n = (int)(s - b);
            if (b[0] == '@') { ev.has_ptr = 1; sscanf(b + 1, "%f/%f", &ev.px, &ev.py); }
            else if (n == 2 && b[0] == 's' && strchr("udlr", b[1])) { ev.stick = 1; ev.sx += b[1] == 'r' ? 1 : b[1] == 'l' ? -1 : 0; ev.sy += b[1] == 'u' ? 1 : b[1] == 'd' ? -1 : 0; }
            else ev.mask |= name_mask(b, n);
            if (*s == ',') s++;
        }
        if (*s == ';') s++;
        g_script[g_nscript++] = ev;
    }
}

volatile int g_replay_reached;   // set by the VR layer when a TTPC_REPLAY_UNTIL milestone is reached
// Input recording / replay. A recording lists, per controller read where anything changed:
//   <read> <buttons hex> <pointer x> <pointer y> <pointer valid> <stick x> <stick y>
// TTPC_RECORD=file writes one (Play.bat records every session into recordings/);
// TTPC_REPLAY=file plays one back instead of the live mouse/keyboard/pad.
typedef struct { u64 read; u32 hold; float px, py, sx, sy; int valid; } RecEv;
static FILE *g_rec; static RecEv g_rep_state; static RecEv *g_rep; static int g_nrep, g_irep;
static void rec_init(void) {
    const char *r = getenv("TTPC_RECORD");
    char auto_name[128];
    if (r && !strcmp(r, "auto")) {   // recordings/session_YYYYMMDD_HHMMSS.txt
        void host_mkdir(const char *path); host_mkdir("recordings");
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        strftime(auto_name, sizeof auto_name, "recordings/session_%Y%m%d_%H%M%S.txt", tm); r = auto_name;
    }
    if (r && *r) { g_rec = fopen(r, "w"); if (g_rec) { fprintf(g_rec, "# Trauma Team input recording v1\n"); fprintf(stderr, "[input] recording to %s\n", r); } }
    const char *p = getenv("TTPC_REPLAY");
    if (p && *p) {
        FILE *f = fopen(p, "r"); char line[256]; int cap = 0;
        while (f && fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            RecEv e; unsigned long long rd; unsigned h;
            if (sscanf(line, "%llu %x %f %f %d %f %f", &rd, &h, &e.px, &e.py, &e.valid, &e.sx, &e.sy) != 7) continue;
            e.read = rd; e.hold = h;
            if (g_nrep == cap) { cap = cap ? cap * 2 : 4096; g_rep = realloc(g_rep, sizeof *g_rep * cap); }
            g_rep[g_nrep++] = e;
        }
        if (f) fclose(f);
        fprintf(stderr, "[input] replaying %s (%d events)\n", p, g_nrep);
    }
}

// Called once per retrace (after platform_poll): latch this frame's buttons and pointer.
static WiiState g_wii; static int g_wii_on, g_wii_ptr;   // a real Wii Remote (wiimote_hid.c), and whether its pointer sees the sensor bar
static float g_wii_px, g_wii_py, g_wii_hx = 1, g_wii_hy, g_wii_dist = 1.5f;
int wiimote_native(void) { return g_wii_on; }
void wiimote_frame(void) {
    static int init; if (!init) { init = 1; parse_script(); rec_init(); }
    {   static int was; static unsigned short lastb;
        g_wii_on = whid_poll(&g_wii);
        if (g_wii_on != was) { fprintf(stderr, "[wiimote] %s\n", g_wii_on ? "real Wii Remote in use" : "real Wii Remote gone"); was = g_wii_on; }
        if (g_wii_on) {
            if (g_wii.buttons != lastb) { void platform_wii_used(void); platform_wii_used(); lastb = g_wii.buttons; }
            g_wii_ptr = whid_pointer(&g_wii, &g_wii_px, &g_wii_py, &g_wii_hx, &g_wii_hy, &g_wii_dist);
        } else g_wii_ptr = 0;
    }
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
    if (in->push) h |= PUSH_FLAG;
    if (in->pull) h |= PULL_FLAG;
    if (in->swing) h |= SWING_FLAG;
    if (in->roll_l) h |= ROLL_L_FLAG;   if (in->roll_r) h |= ROLL_R_FLAG;
    g_twist_step += in->twist;
    {   // the endoscope drives with A + B held on the Wii; here the wheel / right stick does it without them
        extern char g_op_mode[3];
        if (g_op_mode[0] == 'e' && g_op_mode[1] == 'n' && (in->push || in->pull)) h |= BTN_A | BTN_B;
    }
    { static int got;   // while a TTPC_INPUT script is still driving a menu (e.g. --mission), ignore the real controls
      if (!got) { got = 1; for (int k = 0; k < g_nscript; k++) if (g_script[k].from + g_script[k].len > script_end) script_end = g_script[k].from + g_script[k].len; }
      if (g_reads < script_end + 30) h = 0; }
    if (g_wii_on) h |= g_wii.buttons;
    g_host_hold = h;
    g_ppx = g_px; g_ppy = g_py;
    if (g_wii_ptr) { g_px = g_wii_px; g_py = g_wii_py; g_point_valid = 1; }
    else if (g_window_open && !getenv("TTPC_POINTER")) {
        g_px = in->mouse_x * 2.f - 1.f; g_py = in->mouse_y * 2.f - 1.f;
        g_point_valid = in->mouse_in && fabsf(g_px) <= 1.05f && fabsf(g_py) <= 1.05f;
    } else {   // headless: aim at TTPC_POINTER="x,y" (-1..1), default the centre
        static float hx, hy; static int got;
        if (!got) { got = 1; const char *e = getenv("TTPC_POINTER"); if (e) sscanf(e, "%f,%f", &hx, &hy); }
        g_px = hx; g_py = hy; g_point_valid = 1;
    }
    if (g_wii_on && g_wii.nunchuk && (g_wii.stick[0] || g_wii.stick[1])) { g_sx = g_wii.stick[0]; g_sy = g_wii.stick[1]; }
    else if (in->has_analog) { g_sx = in->nx; g_sy = in->ny; }
    else {
        g_sx = (float)(in->stick_right - in->stick_left);
        g_sy = (float)(in->stick_up - in->stick_down);
        if (g_sx && g_sy) { g_sx *= 0.7071f; g_sy *= 0.7071f; }
    }
}

// "Push" / "pull" (move the Remote toward / away from the screen, e.g. the endoscope while holding A+B):
// a steady motion while the input is held - the sensor-bar distance changes smoothly and the
// accelerometer shows a gentle thrust along z (the Remote's pointing axis). No braking spike and no
// drifting back afterwards: both read as moving the other way. The distance resets only once A and B
// are both released (out of the gesture).
static float g_dist = 1.5f, g_dist_prev = 1.5f;
static void push_motion(int dir, u32 hold, float *acc_z) {
    static float v;   // current speed in m per read, eased toward the target
    static float speed = -1; if (speed < 0) { const char *e = getenv("TTPC_ENDO_SPEED"); speed = e ? (float)atof(e) : 0.5f; if (speed <= 0 || speed > 4) speed = 0.5f; }
    float target = dir * 0.008f * speed;   // gentler than before (13 Nov: "too aggressive"); TTPC_ENDO_SPEED scales it
    v += (target - v) * 0.35f;
    if (fabsf(v) < 1e-5f) v = 0;
    g_dist_prev = g_dist;
    g_dist -= v;
    if (g_dist < 0.5f) g_dist = 0.5f; if (g_dist > 2.5f) g_dist = 2.5f;
    *acc_z = -(target - v) * 60.f - v * 20.f;   // KPAD acc reads opposite to the motion; small and smooth
    if (*acc_z > 0.6f) *acc_z = 0.6f; if (*acc_z < -0.6f) *acc_z = -0.6f;
    if (!dir && !(hold & (BTN_A | BTN_B)) && v == 0) g_dist = g_dist_prev = 1.5f;
}

// "Swing": bring the Remote (and Nunchuk) down hard and stop - a chest compression in CPR. The
// accelerometers (in g, KPAD reads the opposite of the motion) see a short drop toward free fall,
// then the hard stop: a spike of several g, then settle. One press = one swing; held, it repeats.
static int g_swing_t = -1;
static float swing_motion(int held) {
    static const float Y[] = { -0.6f, -0.2f, -0.1f, -3.6f, -3.0f, -1.8f, -1.2f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    static int was;
    if (g_swing_t < 0 && held && !was) g_swing_t = 0;
    was = held;
    if (g_swing_t < 0) return -1.f;
    float y = Y[g_swing_t++];
    if (g_swing_t >= (int)(sizeof Y / sizeof Y[0])) { g_swing_t = held ? 0 : -1; }
    return y;
}

// "Twist": turn the Remote about its pointing axis (bandages are wound by turning it round and round; some tools
// turn with it). Held, it keeps turning (about one turn a second); the angle stays where it is while A or B is
// held and goes back to level once both are let go.
// Circling the pointer while A is held twists the Remote too: the direction the pointer moves in turns by a full
// turn per circle, and the Remote turns with it (clockwise on screen = twist right). Only a steady circle counts,
// so straight strokes and zig-zags (incisions, sutures) leave the Remote level.
static float circle_motion(float px, float py, int valid, u32 hold) {
    static float lx, ly, head, rate; static int have;
    float vx = px - lx, vy = py - ly, sp = sqrtf(vx * vx + vy * vy), turn = 0;
    if (valid && (hold & BTN_A) && sp > 0.004f) {
        float hd = atan2f(vy, vx);
        if (have) { float d = hd - head; while (d > (float)M_PI) d -= 2 * (float)M_PI; while (d < -(float)M_PI) d += 2 * (float)M_PI;
            rate += (d - rate) * 0.3f;
            if (fabsf(rate) > 0.035f && fabsf(d) < 1.2f && d * rate > 0) turn = d; }
        head = hd; have = 1;
    } else if (!(hold & BTN_A)) { have = 0; rate = 0; }
    else if (sp <= 0.004f) rate *= 0.8f;   // paused: keep the heading, let the rhythm fade
    lx = px; ly = py;
    return turn;
}
static float g_twist;   // the Remote's (and Nunchuk's) current twist, radians
float wiimote_twist(void) { return g_twist; }
static float roll_motion(int dir, u32 hold, float turn) {
    static float a; static int idle;
    turn += g_twist_step; g_twist_step = 0;
    a += turn;
    if (dir || turn) idle = 0; else idle++;
    if (dir) a += dir * 0.105f;   // radians per read
    else if (idle > 150 && !(hold & (BTN_A | BTN_B))) a *= 0.7f;   // back to level once it's been left alone ~2.5 s
    if (fabsf(a) < 1e-3f) a = 0;
    while (a > (float)M_PI) a -= 2 * (float)M_PI;
    while (a < -(float)M_PI) a += 2 * (float)M_PI;
    g_twist = a;
    return a;
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
    {   // VR asked to leave Little Guy's computer: run the game's own close for it (the routine its
        // module table lists next to "is the computer open", 0x8007DF40: flags the autCompCalc task to close)
        extern int g_vr_close_comp; void f_8007DF40(CPU *c);
        if (g_vr_close_comp) { g_vr_close_comp = 0; CPU save = *c; f_8007DF40(c); *c = save; fprintf(stderr, "[vr] computer: closed by the game's own exit routine\n"); }
    }
    // buttons are latched per read: host input from the last retrace plus any scripted presses
    g_reads++;
    u32 h = g_host_hold;
    for (int k = 0; k < g_nscript; k++) if (g_reads >= g_script[k].from && g_reads < g_script[k].from + g_script[k].len) {
        const ScriptEv *ev = &g_script[k];
        h |= ev->mask;
        if (ev->stick) { g_sx = ev->sx; g_sy = ev->sy; }
        if (ev->has_ptr) { g_ppx = g_px; g_ppy = g_py; g_px = ev->px; g_py = ev->py; g_point_valid = 1; }
    }
    {   // TTPC_REPLAY_UNTIL=morgue: the replay only brings the game to that point (fast, with TTPC_SPEED), then you take over
        static const char *until = (const char *)1;
        if (until == (const char *)1) until = getenv("TTPC_REPLAY_UNTIL");
        if (g_nrep && until && *until && (g_replay_reached || g_irep >= g_nrep)) {
            void host_set_speed(double s); host_set_speed(1.0);
            fprintf(stderr, "[input] replay handed over at read %llu (%s): your controls now\n", (unsigned long long)g_reads, g_replay_reached ? until : "end of recording");
            g_nrep = 0; until = NULL;
        }
    }
    if (g_nrep) {   // replay: the recorded state at or before this read
        while (g_irep < g_nrep && g_rep[g_irep].read <= g_reads) {
            const RecEv *e = &g_rep[g_irep++];
            g_rep_state = *e;
        }
        h = g_rep_state.hold; g_ppx = g_px; g_ppy = g_py;
        g_px = g_rep_state.px; g_py = g_rep_state.py; g_point_valid = g_rep_state.valid; g_sx = g_rep_state.sx; g_sy = g_rep_state.sy;
    }
    if (g_rec) {
        static u32 lh = 0xFFFFFFFF; static float lx, ly, lsx, lsy; static int lv;
        if (h != lh || g_px != lx || g_py != ly || g_sx != lsx || g_sy != lsy || g_point_valid != lv) {
            fprintf(g_rec, "%llu %X %.4f %.4f %d %.3f %.3f\n", (unsigned long long)g_reads, h, g_px, g_py, g_point_valid, g_sx, g_sy);
            lh = h; lx = g_px; ly = g_py; lsx = g_sx; lsy = g_sy; lv = g_point_valid;
            fflush(g_rec);
        }
    }
    float acc_z; push_motion((h & PUSH_FLAG) ? 1 : (h & PULL_FLAG) ? -1 : 0, h, &acc_z);
    float acc_y = swing_motion((h & SWING_FLAG) != 0);
    float roll = roll_motion((h & ROLL_L_FLAG) ? -1 : (h & ROLL_R_FLAG) ? 1 : 0, h, circle_motion(g_px, g_py, g_point_valid, h));
    h &= ~(PUSH_FLAG | PULL_FLAG | SWING_FLAG | ROLL_L_FLAG | ROLL_R_FLAG);
    g_prev = g_hold; g_hold = h; g_trig = g_hold & ~g_prev; g_rel = g_prev & ~g_hold;
    if (dbg && (g_trig || g_reads % 600 == 0)) fprintf(stderr, "[pad] read %llu vi %llu hold %04X trig %04X\n", (unsigned long long)g_reads, (unsigned long long)g_vi, g_hold, g_trig);
    for (u32 k = 0; k < 0xB0; k += 4) W32(buf + k, 0);
    W32(buf + 0x00, g_hold); W32(buf + 0x04, g_trig); W32(buf + 0x08, g_rel);
    {   // held level, pointing at the TV (plus any push motion along z)
        // twisted by roll about the pointing axis: gravity (and the swing) turn between y and x
        static float last_x, last_z, last_y = -1.f;
        float cr = cosf(roll), sr = sinf(roll), ax = -acc_y * sr, ay = acc_y * cr;
        wf(buf + 0x0C, ax); wf(buf + 0x10, ay); wf(buf + 0x14, acc_z);
        wf(buf + 0x18, sqrtf(ax * ax + ay * ay + acc_z * acc_z)); wf(buf + 0x1C, fabsf(ax - last_x) + fabsf(acc_z - last_z) + fabsf(ay - last_y)); last_x = ax; last_z = acc_z; last_y = ay;
    }
    wf(buf + 0x20, g_px); wf(buf + 0x24, g_py);
    float vx = g_px - g_ppx, vy = g_py - g_ppy;
    wf(buf + 0x28, vx); wf(buf + 0x2C, vy); wf(buf + 0x30, sqrtf(vx * vx + vy * vy));
    wf(buf + 0x34, cosf(roll)); wf(buf + 0x38, sinf(roll));   // horizon: the twist
    wf(buf + 0x48, g_dist);                          // distance to the sensor bar (m)
    wf(buf + 0x4C, g_dist - g_dist_prev); wf(buf + 0x50, fabsf(g_dist - g_dist_prev));
    wf(buf + 0x54, cosf(roll)); wf(buf + 0x58, sinf(roll));   // acc_vertical
    W8(buf + 0x5C, 1);                               // dev_type: Wii Remote + Nunchuk
    W8(buf + 0x5D, 0);                               // wpad_err: OK
    W8(buf + 0x5E, g_point_valid ? 2 : 0);           // pointer seen (2 = both sensor bar dots)
    W8(buf + 0x5F, 5);                               // data format: Nunchuk + accelerometer + pointer
    wf(buf + 0x60, g_sx); wf(buf + 0x64, g_sy);     // Nunchuk stick
    {   // Nunchuk accelerometer: follows the Remote's swing (both hands come down together in CPR)
        static float last_ny = -1.f;
        wf(buf + 0x68, 0.f); wf(buf + 0x6C, acc_y); wf(buf + 0x70, 0.f); wf(buf + 0x74, fabsf(acc_y)); wf(buf + 0x78, fabsf(acc_y - last_ny)); last_ny = acc_y;
    }
    if (g_wii_on && !g_nrep) {   // a real Remote: its own motion, pointer distance and roll, and the Nunchuk's
        static float lx, ly, lz, lnx, lny, lnz, lhx = 1, lhy, ld = 1.5f;
        const float *a = g_wii.acc;
        wf(buf + 0x0C, a[0]); wf(buf + 0x10, a[1]); wf(buf + 0x14, a[2]);
        wf(buf + 0x18, sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])); wf(buf + 0x1C, fabsf(a[0] - lx) + fabsf(a[1] - ly) + fabsf(a[2] - lz));
        lx = a[0]; ly = a[1]; lz = a[2];
        float an = sqrtf(a[0] * a[0] + a[1] * a[1]);   // acc_vertical: the roll from gravity
        if (an > 0.2f) { wf(buf + 0x54, -a[1] / an); wf(buf + 0x58, a[0] / an); }
        if (g_wii_ptr) {
            wf(buf + 0x34, g_wii_hx); wf(buf + 0x38, g_wii_hy);
            wf(buf + 0x3C, g_wii_hx - lhx); wf(buf + 0x40, g_wii_hy - lhy); wf(buf + 0x44, fabsf(g_wii_hx - lhx) + fabsf(g_wii_hy - lhy));
            wf(buf + 0x48, g_wii_dist); wf(buf + 0x4C, g_wii_dist - ld); wf(buf + 0x50, fabsf(g_wii_dist - ld));
            lhx = g_wii_hx; lhy = g_wii_hy; ld = g_wii_dist;
            W8(buf + 0x5E, (u8)g_wii_ptr);
        } else W8(buf + 0x5E, 0);
        if (g_wii.nunchuk) {
            const float *n = g_wii.nacc;
            wf(buf + 0x68, n[0]); wf(buf + 0x6C, n[1]); wf(buf + 0x70, n[2]);
            wf(buf + 0x74, sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])); wf(buf + 0x78, fabsf(n[0] - lnx) + fabsf(n[1] - lny) + fabsf(n[2] - lnz));
            lnx = n[0]; lny = n[1]; lnz = n[2];
        }
    }
    if (err) W32(err, 0);
    c->r[3] = 1;
}

