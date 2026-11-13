// Forensics (autopsy) examination: the VR layer owns the pointing.
//
// The game's examination task ("autRemainsClothes" work, calc fn_8012E9F0, see docs/FORENSICS_RE.md) reads the
// Wii Remote pointer once per frame (fn_8000B370 / fn_8000B540 -> work+0x38/+0x3C) and uses it three ways:
//   * overview (state 1): A zooms the camera toward the pointed spot (unprojected pointer -> work+0x44);
//   * close-up (state 4): the HIT-file zones are projected to the screen and tested against the pointer
//     (fn_8012F570), and the invisible collision models are ray-tested along the camera ray through the
//     pointer (fn_801301F0 -> nw4r ray test fn_80017BB0); A then runs that zone's / model's script procedure.
// In VR the body is drawn by the VR layer, posed as the game sees it, and the laser finds a spot on it. That spot
// (in the model's own frame, together with the model->view matrix the game drew it with) is handed in here:
//   * the pointer the task reads becomes that spot projected through the game's own camera (fn_80011940), so the
//     zoom target and the zone test land exactly on it;
//   * the collision models are ray-tested along the laser itself (in the game's world space), so a spot on a side
//     the game camera can't see still finds its zone. If the laser finds none of them, the camera ray through the
//     projected spot is used (the game's own test).
// Only while the task's calc runs; everything else in the game sees the normal pointer. TTPC_AUTVR=0 turns it off.
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

void orig_8012E9F0(CPU *c); void orig_801301F0(CPU *c); void orig_8000B370(CPU *c); void orig_8000B540(CPU *c);
void f_80010200(CPU *c); void f_80011940(CPU *c); void f_80034470(CPU *c); void f_80017BB0(CPU *c); void f_800142C0(CPU *c);
u64 wiimote_reads(void);

// ---------------------------------------------------------------- what the VR layer hands in (model space)
static struct { int valid, k; float ref[12], o[3], p[3]; } g_pick;
void aut_vr_pick(int valid, int k, const float ref[12], const float o[3], const float p[3]) {
    g_pick.valid = valid; if (!valid) return;
    g_pick.k = k; memcpy(g_pick.ref, ref, 48); memcpy(g_pick.o, o, 12); memcpy(g_pick.p, p, 12);
}

static u32 g_obj; static u64 g_last_read; static int g_in, g_ptr_ok, g_vr_any; static float g_ptr[2]; static u8 g_vr_hit[16];
// the task's state (work+0; 8 = paused, the state underneath in work+1), -1 when the examination isn't running
int aut_state(void) {
    if (!g_obj || wiimote_reads() - g_last_read > 20) return -1;
    int s = (int)R8(g_obj); return s == 8 ? (int)R8(g_obj + 1) : s;
}
static int enabled(void) { static int on = -1; if (on < 0) { const char *e = getenv("TTPC_AUTVR"); on = !(e && *e == '0'); } return on; }

// ---------------------------------------------------------------- calling the game from here
// A scratch frame below the guest stack: callees store their LR at our sp+4 and build their frames below sp.
#define SCR 0x200
static u32 frame_push(CPU *c) { u32 sp = c->r[1] - SCR; W32(sp, c->r[1]); c->r[1] = sp; return sp; }
static void frame_pop(CPU *c) { c->r[1] += SCR; }
static void wvec(u32 a, const float v[3]) { WF32(a, v[0]); WF32(a + 4, v[1]); WF32(a + 8, v[2]); }
static void rvec(u32 a, float v[3]) { v[0] = RF32(a); v[1] = RF32(a + 4); v[2] = RF32(a + 8); }
static u32 camera(CPU *c, u32 id) { c->r[3] = id; f_80010200(c); return c->r[3]; }

static void aff(const float m[12], const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = m[i * 4] * p[0] + m[i * 4 + 1] * p[1] + m[i * 4 + 2] * p[2] + m[i * 4 + 3]; }
static void rigid_inv(const float v[12], float o[12]) {   // [R|t]^-1 = [R^T | -R^T t] (the camera's lookAt matrix)
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) o[i * 4 + j] = v[j * 4 + i];
        o[i * 4 + 3] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]); }
}
static void mul34(const float a[12], const float b[12], float o[12]) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++)
        o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (j == 3 ? a[i * 4 + 3] : 0);
}

// the nw4r ray test of one of the task's collision models, as fn_801301F0 does it but along the given ray
static int ray_model(CPU *c, u32 sp, u32 obj, int i, const float o[3], const float d[3]) {
    u32 mdl = R32(obj + 0x84 + 4 * i); if (!mdl) return 0;
    u32 inst = R32(mdl + 0xC); if (!inst) return 0;
    wvec(sp + 0x10, o); wvec(sp + 0x1C, d);
    c->r[3] = sp + 0x40; f_80034470(c);   // hit info (as the original clears it)
    c->r[3] = inst; c->r[4] = sp + 0x10; c->r[5] = sp + 0x1C; c->r[6] = sp + 0x40; c->r[7] = 0;
    c->f[1].ps0 = c->f[1].ps1 = 10000.0;
    f_80017BB0(c);
    return c->r[3] != 0;
}

// the VR spot -> game world, the pointer that shows it, and which collision models the laser goes through
static int prepare(CPU *c, u32 obj) {
    u32 cam = camera(c, 0xF); if (!cam) return 0;
    float V[12], Vi[12], W[12]; for (int i = 0; i < 12; i++) V[i] = RF32(cam + 4 * i);
    rigid_inv(V, Vi); mul34(Vi, g_pick.ref, W);   // model -> world = view^-1 * (model -> view)
    float P[3], O[3], D[3]; aff(W, g_pick.p, P); aff(W, g_pick.o, O);
    for (int i = 0; i < 3; i++) D[i] = P[i] - O[i];
    float dl = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]); if (!(dl > 1e-6f)) return 0;
    for (int i = 0; i < 3; i++) D[i] /= dl;
    u32 sp = frame_push(c);
    // the pointer: the spot projected by the game's own camera (the same call the zone test projects with)
    u32 cam1 = camera(c, 1); wvec(sp + 0x10, P); c->r[3] = sp + 0x28; c->r[4] = sp + 0x10; c->r[5] = cam1; f_80011940(c);
    float s[3]; rvec(sp + 0x28, s);
    g_ptr_ok = isfinite(s[0]) && isfinite(s[1]) && fabsf(s[0]) < 1e5f && fabsf(s[1]) < 1e5f;
    g_ptr[0] = s[0]; g_ptr[1] = s[1];
    // the laser through the collision models (only the close-up tests them)
    g_vr_any = 0; memset(g_vr_hit, 0, sizeof g_vr_hit);
    int st = R8(obj), n = R8(obj + 9); if (n > 16) n = 16;
    if (st == 4) for (int i = 0; i < n; i++) { g_vr_hit[i] = (u8)ray_model(c, sp, obj, i, O, D); g_vr_any |= g_vr_hit[i]; }
    {   // log each new spot once, with a check of the frame conversion: the body model's own world matrix vs ours
        static float lp[3]; static int lst = -1;
        if (fabsf(lp[0] - P[0]) + fabsf(lp[1] - P[1]) + fabsf(lp[2] - P[2]) > 0.5f || st != lst) {
            memcpy(lp, P, 12); lst = st;
            char hits[40] = ""; for (int i = 0; i < n && i < 16; i++) hits[i] = g_vr_hit[i] ? '1' : '0'; hits[n < 16 ? n : 16] = 0;
            fprintf(stderr, "[aut] vr spot %s: world %.1f %.1f %.1f -> pointer %.1f %.1f | state %d, laser through models [%s]\n",
                    g_pick.k ? "effects" : "body", P[0], P[1], P[2], s[0], s[1], st, hits);
            u32 bm = R32(obj + 0x80), bi = bm ? R32(bm + 0xC) : 0;
            if (bi) { c->r[3] = bi; c->r[4] = sp + 0x80; f_800142C0(c);
                fprintf(stderr, "[aut]   body world origin (game) %.1f %.1f %.1f | VR frame origin %.1f %.1f %.1f\n",
                        RF32(sp + 0x8C), RF32(sp + 0x9C), RF32(sp + 0xAC), W[3], W[7], W[11]); }
        }
    }
    frame_pop(c);
    return 1;
}

// the examination task's calc
void f_8012E9F0(CPU *c) {
    u32 obj = R32(c->r[3] + 0x38), task = c->r[3], lr = c->lr;
    g_obj = obj; g_last_read = wiimote_reads();
    int s0 = R8(obj);
    g_in = 0;
    if (enabled() && g_pick.valid && (s0 == 1 || s0 == 4)) { g_in = prepare(c, obj); c->r[3] = task; c->lr = lr; }
    orig_8012E9F0(c);
    g_in = 0;
    int s1 = R8(obj);
    if (s1 != s0) {
        if (s0 == 4 && s1 == 5) fprintf(stderr, "[aut] A: zone %d (its script procedure %d)\n", R8(obj + 6), R8(obj + 6) + 1);
        else if (s0 == 4 && s1 == 6) fprintf(stderr, "[aut] A: collision model %d (its script procedure %d)\n", R8(obj + 0xA), R8(obj + 0xA));
        else if (s0 == 4 && s1 == 7) fprintf(stderr, "[aut] A: nothing there\n");
        else fprintf(stderr, "[aut] state %d -> %d\n", s0, s1);
    }
}
// the pointer, while the task's calc reads it
void f_8000B370(CPU *c) { if (g_in && g_ptr_ok) { c->f[1].ps0 = c->f[1].ps1 = g_ptr[0]; return; } orig_8000B370(c); }
void f_8000B540(CPU *c) { if (g_in && g_ptr_ok) { c->f[1].ps0 = c->f[1].ps1 = g_ptr[1]; return; } orig_8000B540(c); }
// a collision model under the pointer: the laser's answer when it went through any of them
void f_801301F0(CPU *c) {
    u32 i = c->r[4] & 0xFF;
    if (g_in && g_vr_any) { c->r[3] = i < 16 && g_vr_hit[i]; return; }
    orig_801301F0(c);
}
