#pragma once
#include "cpu.h"

// Host input snapshot, refreshed at every retrace by platform_poll().
typedef struct {
    float mouse_x, mouse_y;   // 0..1 across the game picture
    int mouse_in;             // pointer is over the window
    int lmb, rmb, mmb;
    int up, down, left, right, a, b, plus, minus, home, one, two, c, z;   // Wii Remote / Nunchuk buttons
    int stick_up, stick_down, stick_left, stick_right;                    // Nunchuk stick (digital, keys)
    float nx, ny; int has_analog;                                         // Nunchuk stick (analog, pad)
    int push;                 // thrust the Remote toward the screen (wheel up / R / right-stick click)
} Input;
extern Input g_input;
extern int g_window_open;

int platform_init(void);
void platform_present(const u32 *px, int x0, int y0, int w, int h, int stride);
void platform_poll(void);
void platform_set_title(const char *t);
