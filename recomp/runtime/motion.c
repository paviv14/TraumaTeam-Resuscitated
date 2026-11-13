// Motion the game asks for by name: instead of faking sensor data for the whole Wii Remote, the game's own motion
// getters are answered from what the player does on the PC.
//
// Twist (bandages, see docs/EMERGENCY_RE.md): the bandage wrap (fn_80173870, First Response's vein-securing and
// fracture bandages) reads the *Nunchuk's* accelerometer through fn_8000B7C0 and turns its gravity direction into a
// roll angle (x * 1.081 -> -1..1 of a half turn; y >= 0 means past 90 degrees). The bandage turns with that angle and
// progresses as it sweeps over the guide marks. Here fn_8000B7C0 answers with gravity turned by the twist the player
// makes (hold A and circle the mouse: one circle = one turn; or J / L), so the wrap follows directly.
#include "cpu.h"
#include <stdio.h>
#include <math.h>

void orig_8000B7C0(CPU *c); void orig_80173870(CPU *c); void orig_80253960(CPU *c);
void platform_rumble(int on);

// WPADControlMotor(chan, on): the game's rumble (it calls this for every channel, and only while the
// Vibration option is on). Channel 0 goes to a real Wii Remote if there is one, and to the controller.
void f_80253960(CPU *c) {
    u32 chan = c->r[3], on = c->r[4];
    if (chan == 0) {
        static int last = -1;
        if ((int)(on != 0) != last) { last = on != 0; platform_rumble(last); }
    }
    orig_80253960(c);
}
float wiimote_twist(void);

// Nunchuk acceleration (chan r3, out vec3 r4)
void f_8000B7C0(CPU *c) {
    float t = wiimote_twist();
    if (t == 0.f) { orig_8000B7C0(c); return; }
    u32 out = c->r[4];
    WF32(out, sinf(t)); WF32(out + 4, -cosf(t)); WF32(out + 8, 0.f);   // gravity (0,-1,0) turned about the pointing axis
}

// the bandage wrap's calc: log what it sees (angle, progress) while it runs
void f_80173870(CPU *c) {
    u32 w = R32(c->r[3]);
    orig_80173870(c);
    if (!w) return;
    static int n; static float lp = -1, la = 1e9f;
    float roll = RF32(w + 0x3BC), ang = RF32(w + 0x3C4), prog = RF32(w + 0x3B8);
    if ((++n % 15) == 0 && (fabsf(prog - lp) > 1e-4f || fabsf(ang - la) > 0.5f)) {
        fprintf(stderr, "[bandage] state %u twist %.0f deg -> game roll %.2f, bandage angle %.0f, progress %.3f\n",
                R32(w), wiimote_twist() * 57.2958f, roll, ang, prog);
        lp = prog; la = ang;
    }
}
