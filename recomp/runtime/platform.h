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
    int push, pull;           // move the Remote toward / away from the screen (wheel, R / V, right-stick click)
    int swing;                // swing the Remote (and Nunchuk) down hard: CPR compressions (middle mouse, G, left-stick click)
    int roll_l, roll_r;       // twist the Remote left / right about its pointing axis (J / L): bandages, turning tools
    float twist;              // a twist step to add now (radians; the mouse wheel)
} Input;
extern Input g_input;

// VR (vr.c)
int vr_wanted(void);
int vr_active(void);
void vr_init(void);
int vr_frame(unsigned game_tex, int w, int h, float x0f, float x1f);
extern int g_vr_desk_seen, g_vr_pic_y0, g_vr_pic_h;
int vr_key_frame(void);
void vr_set_layer(unsigned tex);   // gl.c: this frame's HUD layer (keyed like the frame), 0 if none
void vr_input(Input *in);

// Settings (settings.ini, options menu in menu.c)
typedef struct { int scale, fullscreen, stretch, sharp, vsync, textures, volume, pointer_speed, wide_fill, tex_limit, tex_compress, tex_vram_mb, prompts, pad_style; } Settings;
void prompts_update(int force);   // platform.c: pick the button-prompt set (settings + the device used last)
extern Settings g_cfg;
void settings_load(void);
void settings_save(void);
void settings_apply(void);
void menu_run(void);
extern int g_window_open;

int platform_init(void);
void platform_present(const u32 *px, int x0, int y0, int w, int h, int stride);
void platform_poll(void);
void platform_set_title(const char *t);
void platform_audio_stats(char *buf, int n);
