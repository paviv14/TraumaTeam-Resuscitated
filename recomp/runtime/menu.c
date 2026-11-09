// Options menu and settings.ini.
//
// F1 (or Back + Start on a controller) opens the menu over the paused game. Settings are saved to
// settings.ini next to the game and applied immediately; environment variables (TTPC_SCALE...) still
// override the file for one run.
#include "cpu.h"
#include "platform.h"
#include "gx_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_cfg = { .scale = 2, .fullscreen = 0, .stretch = 0, .sharp = 0, .vsync = 0, .textures = 1, .volume = 100, .pointer_speed = 3 };
static const char *CFG_PATH = "settings.ini";

static struct { const char *key; int *v; int lo, hi; } KEYS[] = {
    { "resolution_scale", &g_cfg.scale, 1, 6 }, { "fullscreen", &g_cfg.fullscreen, 0, 1 },
    { "stretch_to_window", &g_cfg.stretch, 0, 1 }, { "sharp_pixels", &g_cfg.sharp, 0, 1 },
    { "vsync", &g_cfg.vsync, 0, 1 }, { "texture_pack", &g_cfg.textures, 0, 1 },
    { "volume", &g_cfg.volume, 0, 100 }, { "pointer_speed", &g_cfg.pointer_speed, 1, 5 },
};
#define NKEYS (int)(sizeof KEYS / sizeof KEYS[0])

void settings_load(void) {
    FILE *f = fopen(CFG_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char k[64]; int v;
            if (line[0] == '#' || line[0] == ';' || sscanf(line, " %63[^= ] = %d", k, &v) != 2) continue;
            for (int i = 0; i < NKEYS; i++) if (!strcmp(k, KEYS[i].key))
                *KEYS[i].v = v < KEYS[i].lo ? KEYS[i].lo : v > KEYS[i].hi ? KEYS[i].hi : v;
        }
        fclose(f);
    }
    if (getenv("TTPC_SCALE")) g_cfg.scale = atoi(getenv("TTPC_SCALE"));
    if (g_cfg.scale < 1) g_cfg.scale = 1;
}
void settings_save(void) {
    FILE *f = fopen(CFG_PATH, "w");
    if (!f) { fprintf(stderr, "[settings] cannot write %s\n", CFG_PATH); return; }
    fprintf(f, "# Trauma Team settings (also changed in game: F1 or Back + Start)\n");
    for (int i = 0; i < NKEYS; i++) fprintf(f, "%s = %d\n", KEYS[i].key, *KEYS[i].v);
    fclose(f);
}

#ifdef TTPC_SDL
#include <SDL2/SDL.h>
extern SDL_Window *g_sdl_window;
void video_reload_textures(void);
void platform_audio_pause(int pause);

// Apply everything that can change at run time (called at startup and from the menu).
void settings_apply(void) {
    if (!g_gpu) return;
    gpu_set_scale(g_cfg.scale);
    g_disp_stretch = g_cfg.stretch; g_disp_sharp = g_cfg.sharp;
    u32 fs = SDL_GetWindowFlags(g_sdl_window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (!!fs != !!g_cfg.fullscreen) SDL_SetWindowFullscreen(g_sdl_window, g_cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_GL_SetSwapInterval(g_cfg.vsync ? 1 : 0);
}

typedef struct { const char *label, *help; int *v, lo, hi, step; } Item;
static void value_text(const Item *it, char *b, size_t n) {
    int v = *it->v;
    if (it->v == &g_cfg.scale) {
        static const char *names[] = { "", "Native 480p", "960p", "1440p", "4K (2112p)", "2640p", "3168p" };
        snprintf(b, n, "%dx  %s", v, names[v]);
    } else if (it->v == &g_cfg.volume) snprintf(b, n, "%d%%", v);
    else if (it->v == &g_cfg.pointer_speed) { static const char *s[] = { "", "Slowest", "Slow", "Normal", "Fast", "Fastest" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.fullscreen) snprintf(b, n, v ? "Fullscreen" : "Window");
    else if (it->v == &g_cfg.stretch) snprintf(b, n, v ? "Stretch to fill" : "Original 4:3");
    else if (it->v == &g_cfg.sharp) snprintf(b, n, v ? "Sharp" : "Smooth");
    else snprintf(b, n, v ? "On" : "Off");
}

static Item ITEMS[] = {
    { "Resolution", "Internal rendering resolution. Higher is sharper and needs a faster GPU.", &g_cfg.scale, 1, 6, 1 },
    { "Display", "Window or borderless fullscreen (F11 also switches).", &g_cfg.fullscreen, 0, 1, 1 },
    { "Aspect ratio", "Keep the game's 4:3 picture, or stretch it to the window.", &g_cfg.stretch, 0, 1, 1 },
    { "Scaling filter", "How the picture is scaled to the window: smooth, or sharp pixels.", &g_cfg.sharp, 0, 1, 1 },
    { "VSync", "Sync to the monitor's refresh. Off is smoothest on a 60 Hz screen.", &g_cfg.vsync, 0, 1, 1 },
    { "Texture pack", "Use the upscaled textures in the 'textures' folder.", &g_cfg.textures, 0, 1, 1 },
    { "Volume", "Game sound volume.", &g_cfg.volume, 0, 100, 10 },
    { "Pointer speed", "How fast the controller's right stick moves the pointer.", &g_cfg.pointer_speed, 1, 5, 1 },
};
#define NITEMS (int)(sizeof ITEMS / sizeof ITEMS[0])

static void change(int i, int dir) {
    Item *it = &ITEMS[i];
    int v = *it->v + dir * it->step;
    if (it->hi - it->lo == 1) v = *it->v ? 0 : 1;          // toggles wrap
    else if (v < it->lo) v = it->lo; else if (v > it->hi) v = it->hi;
    if (v == *it->v) return;
    int textures_changed = it->v == &g_cfg.textures;
    *it->v = v;
    settings_apply(); settings_save();
    if (textures_changed) video_reload_textures();
}

// Runs the menu until it is closed; the game is paused meanwhile.
void menu_run(void) {
    if (!g_gpu) return;
    platform_audio_pause(1);
    SDL_ShowCursor(SDL_ENABLE);
    int sel = 0, done = 0, mx = -1, my = -1, rows_y[NITEMS + 1], row_h = 1, panel_x = 0, panel_w = 0;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { fprintf(stderr, "[platform] window closed\n"); exit(0); }
            int key = e.type == SDL_KEYDOWN ? e.key.keysym.sym : 0;
            int btn = e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.button : -1;
            if (key == SDLK_ESCAPE || key == SDLK_F1 || key == SDLK_BACKSPACE || btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) done = 1;
            if (key == SDLK_UP || key == SDLK_w || btn == SDL_CONTROLLER_BUTTON_DPAD_UP) sel = (sel + NITEMS) % (NITEMS + 1);
            if (key == SDLK_DOWN || key == SDLK_s || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel = (sel + 1) % (NITEMS + 1);
            int dir = (key == SDLK_LEFT || key == SDLK_a || btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? -1
                    : (key == SDLK_RIGHT || key == SDLK_d || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 1 : 0;
            if (key == SDLK_RETURN || key == SDLK_SPACE || btn == SDL_CONTROLLER_BUTTON_A) { if (sel == NITEMS) done = 1; else dir = 1; }
            if (dir && sel < NITEMS) change(sel, dir);
            if (key == SDLK_F11) { g_cfg.fullscreen ^= 1; settings_apply(); settings_save(); }
            if (e.type == SDL_MOUSEMOTION) {
                int ww, wh, dw, dh; SDL_GetWindowSize(g_sdl_window, &ww, &wh); SDL_GL_GetDrawableSize(g_sdl_window, &dw, &dh);
                mx = e.motion.x * dw / (ww ? ww : 1); my = e.motion.y * dh / (wh ? wh : 1);
                for (int i = 0; i <= NITEMS; i++) if (mx >= panel_x && mx < panel_x + panel_w && my >= rows_y[i] && my < rows_y[i] + row_h) sel = i;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && mx >= panel_x && mx < panel_x + panel_w) {
                for (int i = 0; i <= NITEMS; i++) if (my >= rows_y[i] && my < rows_y[i] + row_h) {
                    sel = i;
                    if (i == NITEMS) done = 1; else change(i, e.button.button == SDL_BUTTON_RIGHT ? -1 : 1);
                }
            }
            if (e.type == SDL_MOUSEWHEEL && sel < NITEMS) change(sel, e.wheel.y > 0 ? 1 : -1);
        }
        // draw
        int W, H;
        if (!gpu_overlay_begin(&W, &H, 0.55f)) break;
        float u = H / 30.f; if (u * 30 > W) u = W / 30.f;     // layout unit: scales with the window
        panel_w = (int)(u * 28); panel_x = (W - panel_w) / 2; row_h = (int)(u * 1.6f);
        int ph = (int)(u * 4.2f) + row_h * (NITEMS + 1) + (int)(u * 3.6f), py = (H - ph) / 2;
        static const float bg[4] = { 0.06f, 0.07f, 0.09f, 0.92f }, hi[4] = { 0.80f, 0.12f, 0.16f, 0.95f }, line[4] = { 0.80f, 0.12f, 0.16f, 1 };
        static const float white[4] = { 1, 1, 1, 1 }, grey[4] = { 0.72f, 0.74f, 0.78f, 1 }, dimc[4] = { 0.5f, 0.52f, 0.56f, 1 };
        gpu_overlay_rect((float)panel_x, (float)py, (float)panel_w, (float)ph, bg);
        gpu_overlay_rect((float)panel_x, (float)py, (float)panel_w, u * 0.25f, line);
        gpu_overlay_text(panel_x + u, py + u * 0.9f, u * 1.6f, "Options", white);
        int y = py + (int)(u * 3.4f);
        for (int i = 0; i <= NITEMS; i++) {
            rows_y[i] = y;
            if (i == sel) gpu_overlay_rect((float)panel_x, (float)y, (float)panel_w, (float)row_h, hi);
            float ty = y + (row_h - u) / 2.f;
            if (i == NITEMS) {
                const char *c = "Close"; gpu_overlay_text(panel_x + (panel_w - gpu_overlay_text_width(u, c)) / 2, ty, u, c, white);
            } else {
                gpu_overlay_text(panel_x + u, ty, u, ITEMS[i].label, i == sel ? white : grey);
                char v[64]; value_text(&ITEMS[i], v, sizeof v);
                char b[80]; snprintf(b, sizeof b, i == sel ? "<  %s  >" : "%s", v);
                gpu_overlay_text(panel_x + panel_w - u - gpu_overlay_text_width(u, b), ty, u, b, white);
            }
            y += row_h;
        }
        const char *help = sel < NITEMS ? ITEMS[sel].help : "Back to the game.";
        gpu_overlay_text(panel_x + u, y + u * 0.6f, u * 0.75f, help, grey);
        gpu_overlay_text(panel_x + u, y + u * 1.9f, u * 0.7f, "Arrows / D-pad: choose and change    Enter / A: change    Esc / B: close", dimc);
        gpu_overlay_end();
        SDL_Delay(15);
    }
    SDL_ShowCursor(SDL_DISABLE);
    platform_audio_pause(0);
    // drop input that happened while the menu was open
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}
#else
void settings_apply(void) {}
void menu_run(void) {}
#endif
