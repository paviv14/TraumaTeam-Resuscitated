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

Settings g_cfg = { .scale = 2, .fullscreen = 0, .stretch = 0, .sharp = 0, .vsync = 0, .textures = 1, .volume = 100, .pointer_speed = 3, .wide_fill = 1, .tex_limit = 1, .tex_compress = 1, .tex_vram_mb = 3072, .endo_speed = 2 };
static const char *CFG_PATH = "settings.ini";

static struct { const char *key; int *v; int lo, hi; } KEYS[] = {
    { "resolution_scale", &g_cfg.scale, 1, 6 }, { "fullscreen", &g_cfg.fullscreen, 0, 1 },
    { "aspect_ratio", &g_cfg.stretch, 0, 3 }, { "ultrawide_sides", &g_cfg.wide_fill, 0, 1 }, { "texture_size_limit", &g_cfg.tex_limit, 0, 2 }, { "texture_compression", &g_cfg.tex_compress, 0, 1 }, { "texture_memory_mb", &g_cfg.tex_vram_mb, 256, 65536 }, { "sharp_pixels", &g_cfg.sharp, 0, 1 },
    { "vsync", &g_cfg.vsync, 0, 1 }, { "texture_pack", &g_cfg.textures, 0, 1 },
    { "volume", &g_cfg.volume, 0, 100 }, { "pointer_speed", &g_cfg.pointer_speed, 1, 5 },
    { "button_prompts", &g_cfg.prompts, 0, 3 }, { "remote_speaker", &g_cfg.speaker, 0, 4 }, { "controller_style", &g_cfg.pad_style, 0, 3 },
    { "saw_automatic", &g_cfg.saw_auto, 0, 1 }, { "bandage_automatic", &g_cfg.bandage_auto, 0, 1 }, { "strike_and_cpr", &g_cfg.swing_a, 0, 2 }, { "endoscope_speed", &g_cfg.endo_speed, 1, 3 },
    { "text_language", &g_cfg.text_lang, 0, 1 }, { "voice_language", &g_cfg.voice_lang, 0, 1 },
};
#define NKEYS (int)(sizeof KEYS / sizeof KEYS[0])

static void binds_defaults_init(void) {
    static int done; if (done) return; done = 1;
    for (int i = 0; i < NBIND; i++) { g_bind[i].dkey = g_bind[i].key; g_bind[i].dkey2 = g_bind[i].key2; g_bind[i].dpad = g_bind[i].pad; }
}
void settings_load(void) {
    binds_defaults_init();
    FILE *f = fopen(CFG_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char k[64]; int v;
            if (line[0] == '#' || line[0] == ';' || sscanf(line, " %63[^= ] = %d", k, &v) != 2) continue;
            if (!strcmp(k, "stretch_to_window")) { if (v) g_cfg.stretch = 2; continue; }   // older settings files
            for (int i = 0; i < NBIND; i++) {
                char n[64];
                snprintf(n, sizeof n, "key_%s", g_bind[i].id); if (!strcmp(k, n)) g_bind[i].key = v;
                snprintf(n, sizeof n, "key2_%s", g_bind[i].id); if (!strcmp(k, n)) g_bind[i].key2 = v;
                snprintf(n, sizeof n, "pad_%s", g_bind[i].id); if (!strcmp(k, n)) g_bind[i].pad = v;
            }
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
    fprintf(f, "# controls: SDL scancodes (0 = none) and controller buttons (30 = left trigger, 31 = right trigger, -1 = none)\n");
    for (int i = 0; i < NBIND; i++) fprintf(f, "key_%s = %d\nkey2_%s = %d\npad_%s = %d\n", g_bind[i].id, g_bind[i].key, g_bind[i].id, g_bind[i].key2, g_bind[i].id, g_bind[i].pad);
    fclose(f);
}

#ifdef TTPC_SDL
#include <SDL2/SDL.h>
void platform_pad_event(const SDL_Event *e);
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
// Scenes ("sequences", see hle.c) the retail game never switches to. Enter on the item switches.
static const struct { int seq; const char *name; } DEV[] = {
    { 32, "Stage select (dev)" }, { 10, "Scene test menu" },
    { 28, "DNA logic puzzle (cut)" }, { 29, "Medicine puzzle (cut)" }, { 30, "Virus action game (cut)" },
    { 6, "Sound test" }, { 7, "Model viewer" }, { 22, "Effect editor" }, { 5, "Effect viewer" },
    { 56, "Comic viewer" }, { 26, "Comic event editor" }, { 24, "Message viewer" },
    { 40, "Forensics test menu" }, { 51, "Diagnosis test menu" }, { 44, "Endoscope stage menu" },
    { 45, "Endoscope path tool" }, { 42, "Endoscope lesion setter" }, { 3, "Model test" }, { 23, "Hit test" },
    { 1, "Back to the title screen" },
};
#define NDEV (int)(sizeof DEV / sizeof DEV[0])
static int g_dev_pick;
extern volatile int g_dev_seq_request;
static void value_text(const Item *it, char *b, size_t n) {
    int v = *it->v;
    if (it->v == &g_cfg.scale) {
        static const char *names[] = { "", "Native 480p", "960p", "1440p", "4K (2112p)", "2640p", "3168p" };
        snprintf(b, n, "%dx  %s", v, names[v]);
    } else if (it->v == &g_cfg.volume) snprintf(b, n, "%d%%", v);
    else if (it->v == &g_cfg.pointer_speed) { static const char *s[] = { "", "Slowest", "Slow", "Normal", "Fast", "Fastest" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.fullscreen) snprintf(b, n, v ? "Fullscreen" : "Window");
    else if (it->v == &g_cfg.stretch) { static const char *s[] = { "4:3", "16:9 widescreen", "Stretch to fill", "Ultrawide" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.sharp) snprintf(b, n, v ? "Sharp" : "Smooth");
    else if (it->v == &g_dev_pick) snprintf(b, n, "%s", DEV[v].name);
    else if (it->v == &g_cfg.wide_fill) snprintf(b, n, v ? "Extend game art" : "Black");
    else if (it->v == &g_cfg.prompts) { static const char *s[] = { "Automatic", "Wii Remote", "Keyboard & mouse", "Controller" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.pad_style) { static const char *s[] = { "Automatic", "Xbox", "PlayStation 4", "PlayStation 5" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.speaker) { static const char *s[] = { "Automatic", "PC speakers", "Wii Remote", "Both", "Off" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.saw_auto || it->v == &g_cfg.bandage_auto) snprintf(b, n, v ? "Automatic" : "Manual");
    else if (it->v == &g_cfg.swing_a) { static const char *s[] = { "Swing key", "Swing key + A", "Click in time" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.endo_speed) { static const char *s[] = { "", "Slow", "Normal", "Fast" }; snprintf(b, n, "%s", s[v]); }
    else if (it->v == &g_cfg.text_lang || it->v == &g_cfg.voice_lang) snprintf(b, n, v ? "Japanese" : "English");
    else if (it->v == &g_cfg.tex_limit) { static const char *s[] = { "Up to 1024", "Up to 2048", "Full (up to 4096)" }; snprintf(b, n, "%s", s[v]); }
    else snprintf(b, n, v ? "On" : "Off");
}

// Tabs and their settings. Item.tab says which tab; the Controls tab also lists the remappable buttons.
enum { T_DISPLAY, T_GRAPHICS, T_AUDIO, T_LANG, T_CONTROLS, T_REMAP, T_DEV, NTAB };
static const char *TABS[NTAB] = { "Display", "Graphics", "Audio", "Language", "Controls", "Remap", "Developer" };
static const int ITEM_TAB[] = { T_DISPLAY, T_DISPLAY, T_DISPLAY, T_DISPLAY, T_GRAPHICS, T_DISPLAY, T_DISPLAY, T_GRAPHICS, T_AUDIO, T_AUDIO,
                                T_CONTROLS, T_CONTROLS, T_CONTROLS, T_CONTROLS, T_CONTROLS, T_CONTROLS, T_CONTROLS, T_LANG, T_LANG, T_DEV };
static Item ITEMS[] = {
    { "Resolution", "Internal rendering resolution. Higher is sharper and needs a faster GPU.", &g_cfg.scale, 1, 6, 1 },
    { "Display", "Window or borderless fullscreen (F11 also switches).", &g_cfg.fullscreen, 0, 1, 1 },
    { "Aspect ratio", "4:3, 16:9, 16:9 stretched to the window, or ultrawide: a wider view of 3D scenes for wider screens, 2D stays centred. Restart after switching from 4:3.", &g_cfg.stretch, 0, 3, 1 },
    { "Ultrawide sides", "On 2D screens: continue the game's own backgrounds and panels into the sides (mirrored), or leave them black.", &g_cfg.wide_fill, 0, 1, 1 },
    { "Texture pack detail", "Largest size texture pack images are used at. 2048 already looks sharp at 1440p and uses a quarter of the memory of 4096. Next launch.", &g_cfg.tex_limit, 0, 2, 1 },
    { "Scaling filter", "How the picture is scaled to the window: smooth, or sharp pixels.", &g_cfg.sharp, 0, 1, 1 },
    { "VSync", "Sync to the monitor's refresh. Off is smoothest on a 60 Hz screen.", &g_cfg.vsync, 0, 1, 1 },
    { "Texture pack", "Use the upscaled textures in the 'textures' folder.", &g_cfg.textures, 0, 1, 1 },
    { "Volume", "Game sound volume.", &g_cfg.volume, 0, 100, 10 },
    { "Wii Remote speaker", "Where the sounds the Wii played from the Remote's speaker go: Automatic (the Remote when one is connected and in use, else the PC), the PC, the Remote, both, or off.", &g_cfg.speaker, 0, 4, 1 },
    { "Pointer speed", "How fast the controller's right stick moves the pointer.", &g_cfg.pointer_speed, 1, 5, 1 },
    { "Button prompts", "Which buttons the game's help shows: Automatic follows what you used last (keyboard & mouse or a controller), or always one of them. Wii Remote keeps the original pictures.", &g_cfg.prompts, 0, 3, 1 },
    { "Controller style", "Button names for a controller: Automatic asks the controller what it is; or always Xbox, PlayStation 4 or PlayStation 5.", &g_cfg.pad_style, 0, 3, 1 },
    { "Saw", "Orthopedics saw: Manual turns it back and forth with the mouse wheel / LB-RB; Automatic saws by itself while A (left click) is held.", &g_cfg.saw_auto, 0, 1, 1 },
    { "Bandage", "Winding a bandage: Manual circles the pointer (or holds the twist keys); Automatic winds it by itself while A is held, during operations and First Response.", &g_cfg.bandage_auto, 0, 1, 1 },
    { "Strike and CPR", "Swing key: middle click / G / Y swings (hold A yourself where the game asks for it). Swing key + A: the swing also holds A. Click in time: while the game shows the moving-Remote hint (chest compressions, strikes), each click or A press is a swing.", &g_cfg.swing_a, 0, 2, 1 },
    { "Endoscope speed", "How fast the scope moves in and out with the wheel, R / V or the right stick.", &g_cfg.endo_speed, 1, 3, 1 },
    { "Text", "Language of all the game's text. The US disc has the Japanese text too. Takes effect when the game is started again.", &g_cfg.text_lang, 0, 1, 1 },
    { "Voices", "Language of the voices. Japanese needs your Japanese disc (HOSPITAL. 6-nin no Ishi) extracted next to the US one, in a folder named like it plus _jp (e.g. disc_jp). Takes effect when the game is started again.", &g_cfg.voice_lang, 0, 1, 1 },
    { "Developer scene", "Hidden developer tools and cut prototypes left in the game. Enter / A goes there now (unsaved progress is lost).", &g_dev_pick, 0, NDEV - 1, 1 },
};
#define NITEMS (int)(sizeof ITEMS / sizeof ITEMS[0])

static void change(int i, int dir) {
    Item *it = &ITEMS[i];
    int v = *it->v + dir * it->step;
    if (it->hi - it->lo == 1) v = *it->v ? 0 : 1;          // toggles wrap
    else if (it->v == &g_cfg.stretch || it->v == &g_cfg.prompts || it->v == &g_cfg.pad_style) v = (v + 4) % 4;
    else if (it->v == &g_cfg.speaker) v = (v + 5) % 5;
    else if (it->v == &g_cfg.swing_a) v = (v + 3) % 3;     // so do these lists
    else if (it->v == &g_dev_pick) v = (v + NDEV) % NDEV;
    if (it->v == &g_dev_pick) { *it->v = v; return; }      // a choice, not a setting
    else if (v < it->lo) v = it->lo; else if (v > it->hi) v = it->hi;
    if (v == *it->v) return;
    int textures_changed = it->v == &g_cfg.textures;
    *it->v = v;
    settings_apply(); settings_save();
    if (textures_changed) video_reload_textures();
    if (it->v == &g_cfg.prompts || it->v == &g_cfg.pad_style) prompts_update(1);
}

// rows of the open tab: >= 0 an item, -1 - n bind n, R_RESET, R_CLOSE
#define R_RESET (-1000)
#define R_CLOSE (-1001)
static int build_rows(int tab, int *rows) {
    int n = 0;
    if (tab == T_REMAP) { for (int b = 0; b < NBIND; b++) rows[n++] = -1 - b; rows[n++] = R_RESET; }
    else for (int i = 0; i < NITEMS; i++) if (ITEM_TAB[i] == tab) rows[n++] = i;
    rows[n++] = R_CLOSE;
    return n;
}
static const char *key_name(int sc) { if (sc <= 0) return "-"; const char *k = SDL_GetScancodeName((SDL_Scancode)sc); return *k ? k : "?"; }
static void bind_set(int b, int col, int v) {
    int *slot = col == 0 ? &g_bind[b].key : col == 1 ? &g_bind[b].key2 : &g_bind[b].pad;
    *slot = v; settings_save(); prompts_update(1);
}

// Runs the menu until it is closed; the game is paused meanwhile.
void menu_run(void) {
    if (!g_gpu) return;
    platform_audio_pause(1);
    SDL_ShowCursor(SDL_ENABLE);
    static int tab = T_DISPLAY;
    int rows[NBIND + NITEMS + 4], nrows = build_rows(tab, rows);
    int sel = 0, col = 0, scroll = 0, done = 0, capture = 0, mx = -1, my = -1, row_h = 1, panel_x = 0, panel_w = 0, list_y = 0, vis = 1;
    int tab_x[NTAB + 1] = {0}, tab_y = 0, tab_h = 1, col_x[4] = {0};
    Uint32 capture_t = 0;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) { platform_pad_event(&e);
            if (e.type == SDL_QUIT) { fprintf(stderr, "[platform] window closed\n"); exit(0); }
            int key = e.type == SDL_KEYDOWN ? e.key.keysym.sym : 0, sc = e.type == SDL_KEYDOWN ? e.key.keysym.scancode : 0;
            int btn = e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.button : -1;
            if (capture) {           // waiting for the new key / button
                int b = -1 - rows[sel];
                if (SDL_GetTicks() - capture_t < 150) continue;
                if (col < 2) {
                    if (key == SDLK_ESCAPE || btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) capture = 0;
                    else if (sc) { bind_set(b, col, sc); capture = 0; }
                } else {
                    if (key == SDLK_ESCAPE) capture = 0;
                    else if (btn >= 0) { bind_set(b, col, btn); capture = 0; }
                    else if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.value > 20000 &&
                             (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
                        bind_set(b, col, e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? PAD_LT : PAD_RT); capture = 0;
                    }
                }
                continue;
            }
            int newtab = tab;
            if (key == SDLK_TAB || key == SDLK_PAGEDOWN || key == SDLK_e || btn == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) newtab = (tab + 1) % NTAB;
            if (key == SDLK_PAGEUP || key == SDLK_q || btn == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) newtab = (tab + NTAB - 1) % NTAB;
            if (e.type == SDL_MOUSEBUTTONDOWN && my >= tab_y && my < tab_y + tab_h)
                for (int t = 0; t < NTAB; t++) if (mx >= tab_x[t] && mx < tab_x[t + 1]) newtab = t;
            if (newtab != tab) { tab = newtab; nrows = build_rows(tab, rows); sel = 0; scroll = 0; col = 0; continue; }
            if (key == SDLK_ESCAPE || key == SDLK_F1 || key == SDLK_BACKSPACE || btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) done = 1;
            if (key == SDLK_UP || key == SDLK_w || btn == SDL_CONTROLLER_BUTTON_DPAD_UP) sel = (sel + nrows - 1) % nrows;
            if (key == SDLK_DOWN || key == SDLK_s || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel = (sel + 1) % nrows;
            int dir = (key == SDLK_LEFT || key == SDLK_a || btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? -1
                    : (key == SDLK_RIGHT || key == SDLK_d || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 1 : 0;
            int r = rows[sel], enter = key == SDLK_RETURN || key == SDLK_SPACE || btn == SDL_CONTROLLER_BUTTON_A;
            int clear = key == SDLK_DELETE || btn == SDL_CONTROLLER_BUTTON_X;
            if (e.type == SDL_MOUSEMOTION) {
                int ww, wh, dw, dh; SDL_GetWindowSize(g_sdl_window, &ww, &wh); SDL_GL_GetDrawableSize(g_sdl_window, &dw, &dh);
                mx = e.motion.x * dw / (ww ? ww : 1); my = e.motion.y * dh / (wh ? wh : 1);
                if (mx >= panel_x && mx < panel_x + panel_w && my >= list_y && my < list_y + vis * row_h) {
                    int i = scroll + (my - list_y) / row_h; if (i < nrows) sel = i;
                    if (rows[sel] < 0 && rows[sel] > R_RESET) for (int c = 0; c < 3; c++) if (mx >= col_x[c] && mx < col_x[c + 1]) col = c;
                }
                r = rows[sel];
            }
            int click = e.type == SDL_MOUSEBUTTONDOWN && mx >= panel_x && mx < panel_x + panel_w && my >= list_y && my < list_y + vis * row_h;
            if (click && e.button.button == SDL_BUTTON_RIGHT && r < 0 && r > R_RESET) { clear = 1; click = 0; }
            if (click) { if (r >= 0 && ITEMS[r].v != &g_dev_pick) dir = 1; else enter = 1; }
            if (r == R_CLOSE) { if (enter) done = 1; }
            else if (r == R_RESET) {
                if (enter) { for (int b = 0; b < NBIND; b++) { g_bind[b].key = g_bind[b].dkey; g_bind[b].key2 = g_bind[b].dkey2; g_bind[b].pad = g_bind[b].dpad; } settings_save(); }
            } else if (r < 0) {           // a button: left / right picks the column, Enter waits for the new one
                int b = -1 - r;
                if (dir) col = (col + 3 + dir) % 3;
                if (enter) { capture = 1; capture_t = SDL_GetTicks(); }
                if (clear) bind_set(b, col, col < 2 ? 0 : -1);
            } else {
                if (enter) { if (ITEMS[r].v == &g_dev_pick) { g_dev_seq_request = DEV[g_dev_pick].seq; done = 1; } else dir = 1; }
                if (dir) change(r, dir);
                if (e.type == SDL_MOUSEWHEEL) change(r, e.wheel.y > 0 ? 1 : -1);
            }
            if (e.type == SDL_MOUSEWHEEL && r < 0) { scroll -= e.wheel.y; }
            if (key == SDLK_F11) { g_cfg.fullscreen ^= 1; settings_apply(); settings_save(); }
        }
        // draw
        int W, H;
        if (!gpu_overlay_begin(&W, &H, 0.55f)) break;
        float u = H / 30.f; if (u * 30 > W) u = W / 30.f;     // layout unit: scales with the window
        panel_w = (int)(u * 28); panel_x = (W - panel_w) / 2; row_h = (int)(u * 1.5f);
        vis = 14; if (vis > nrows) vis = nrows;
        if (sel < scroll) scroll = sel; if (sel >= scroll + vis) scroll = sel - vis + 1;
        if (scroll > nrows - vis) scroll = nrows - vis; if (scroll < 0) scroll = 0;
        int ph = (int)(u * 5.6f) + row_h * vis + (int)(u * 4.2f), py = (H - ph) / 2;
        static const float bg[4] = { 0.06f, 0.07f, 0.09f, 0.92f }, hi[4] = { 0.80f, 0.12f, 0.16f, 0.95f }, line[4] = { 0.80f, 0.12f, 0.16f, 1 };
        static const float white[4] = { 1, 1, 1, 1 }, grey[4] = { 0.72f, 0.74f, 0.78f, 1 }, dimc[4] = { 0.5f, 0.52f, 0.56f, 1 }, tabbg[4] = { 0.14f, 0.15f, 0.18f, 1 };
        gpu_overlay_rect((float)panel_x, (float)py, (float)panel_w, (float)ph, bg);
        gpu_overlay_rect((float)panel_x, (float)py, (float)panel_w, u * 0.25f, line);
        gpu_overlay_text(panel_x + u, py + u * 0.8f, u * 1.4f, "Options", white);
        // tabs
        tab_y = py + (int)(u * 2.6f); tab_h = (int)(u * 1.5f);
        {   float tx = (float)panel_x + u * 0.5f, ts = u * 0.85f;
            for (int t = 0; t < NTAB; t++) {
                float tw = gpu_overlay_text_width(ts, TABS[t]) + u * 1.2f;
                tab_x[t] = (int)tx;
                gpu_overlay_rect(tx, (float)tab_y, tw - u * 0.2f, (float)tab_h, t == tab ? hi : tabbg);
                gpu_overlay_text(tx + u * 0.5f, tab_y + (tab_h - ts) / 2.f, ts, TABS[t], t == tab ? white : grey);
                tx += tw;
            }
            tab_x[NTAB] = (int)tx;
            gpu_overlay_text(tx + u * 0.3f, tab_y + (tab_h - u * 0.6f) / 2.f, u * 0.6f, "Q / E   LB / RB", dimc);
        }
        list_y = tab_y + tab_h + (int)(u * 0.6f);
        int y = list_y;
        col_x[0] = panel_x + (int)(u * 12); col_x[1] = panel_x + (int)(u * 17.5f); col_x[2] = panel_x + (int)(u * 22.5f); col_x[3] = panel_x + panel_w;
        for (int k = scroll; k < scroll + vis; k++) {
            int r = rows[k];
            if (k == sel && !(r < 0 && r > R_RESET)) gpu_overlay_rect((float)panel_x, (float)y, (float)panel_w, (float)row_h, hi);
            float ty = y + (row_h - u * 0.9f) / 2.f, ts = u * 0.9f;
            if (r == R_CLOSE || r == R_RESET) {
                const char *c = r == R_CLOSE ? "Close" : "Reset all controls to default";
                gpu_overlay_text(panel_x + (panel_w - gpu_overlay_text_width(ts, c)) / 2, ty, ts, c, white);
            } else if (r < 0) {
                int b = -1 - r;
                if (k == sel) { gpu_overlay_rect((float)panel_x, (float)y, (float)panel_w, (float)row_h, tabbg);
                                gpu_overlay_rect((float)col_x[col], (float)y, (float)(col_x[col + 1] - col_x[col]), (float)row_h, hi); }
                gpu_overlay_text(panel_x + u, ty, ts, g_bind[b].name, k == sel ? white : grey);
                const char *v[3] = { key_name(g_bind[b].key), key_name(g_bind[b].key2), bind_pad_name(g_bind[b].pad) };
                for (int c = 0; c < 3; c++) {
                    const char *t = (capture && k == sel && c == col) ? "press..." : v[c];
                    gpu_overlay_text(col_x[c] + u * 0.4f, ty, ts * 0.9f, t, white);
                }
            } else {
                gpu_overlay_text(panel_x + u, ty, ts, ITEMS[r].label, k == sel ? white : grey);
                char v[64]; value_text(&ITEMS[r], v, sizeof v);
                char b[80]; snprintf(b, sizeof b, k == sel ? "<  %s  >" : "%s", v);
                gpu_overlay_text(panel_x + panel_w - u - gpu_overlay_text_width(ts, b), ty, ts, b, white);
            }
            y += row_h;
        }
        if (tab == T_REMAP) {   // column titles over the list
            static const char *ct[3] = { "Key", "Alt key", "Controller" };
            for (int c = 0; c < 3; c++) gpu_overlay_text(col_x[c] + u * 0.4f, list_y - u * 0.55f, u * 0.5f, ct[c], dimc);
        }
        if (scroll > 0) gpu_overlay_text(panel_x + panel_w - u * 1.2f, list_y - u * 0.55f, u * 0.6f, "^", grey);
        if (scroll + vis < nrows) gpu_overlay_text(panel_x + panel_w - u * 1.2f, y + u * 0.05f, u * 0.6f, "v", grey);
        int r = rows[sel];
        const char *help = r == R_CLOSE ? "Back to the game." : r == R_RESET ? "Every key and controller button back to the standard layout."
                         : r < 0 ? (capture ? (col < 2 ? "Press the new key (Esc: cancel)." : "Press the new controller button or trigger (Esc: cancel).")
                                            : "Left / right: key, alt key or controller. Enter / A: set a new one. Delete / X or right click: none. The mouse stays fixed (left click A, right click B, middle Swing).")
                         : ITEMS[r].help;
        {   // help text, wrapped to the panel (two lines at most)
            char line1[200] = "", line2[200] = ""; const char *w = help; float maxw = panel_w - 2 * u, hs = u * 0.7f;
            char *cur = line1;
            while (*w) {
                const char *e = strchr(w, ' '); int len = e ? (int)(e - w) : (int)strlen(w);
                char t[200]; snprintf(t, sizeof t, "%s%s%.*s", cur, *cur ? " " : "", len, w);
                if (*cur && gpu_overlay_text_width(hs, t) > maxw && cur == line1) { cur = line2; snprintf(t, sizeof t, "%.*s", len, w); }
                snprintf(cur, 200, "%s", t);
                w += len; while (*w == ' ') w++;
            }
            gpu_overlay_text(panel_x + u, y + u * 0.6f, hs, line1, grey);
            if (*line2) gpu_overlay_text(panel_x + u, y + u * 1.45f, hs, line2, grey);
        }
        gpu_overlay_text(panel_x + u, y + u * 2.6f, u * 0.62f, "Arrows / D-pad: choose and change    Enter / A: change    Q E / LB RB: tabs    Esc / B: close", dimc);
        gpu_overlay_end();
        SDL_Delay(15);
    }
    SDL_ShowCursor(SDL_DISABLE);
    platform_audio_pause(0);
    // drop input that happened while the menu was open
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}
// ---------------------------------------------------------------- the notice at start-up
// Shown once each time the game starts, before the game's own screens: what this is and isn't. Any key, click or
// controller button continues (after a moment, so it isn't skipped by accident).
void disclaimer_run(void) {
    if (!g_gpu || getenv("TTPC_NO_NOTICE")) return;
    static const char *L[] = {
        "TRAUMA TEAM RESUSCITATED", "",
        "An unofficial fan project, made for free.",
        "Not affiliated with, endorsed by or connected to Atlus, Sega or Nintendo.",
        "Trauma Team is (c) Atlus. All names and trademarks belong to their owners.", "",
        "Nothing here is sold: no one is paid and no money is made from it.",
        "It contains no game data. It runs only from your own copy of the game.", "",
        "It is free and always will be.",
        "If someone sold it to you, please ask for a refund.", "",
        "Found a bug? Please tell us: run 'Report a bug' in the game's folder.", 0 };
    Uint32 t0 = SDL_GetTicks();
    for (;;) {
        int go = 0; SDL_Event e;
        while (SDL_PollEvent(&e)) { platform_pad_event(&e);
            if (e.type == SDL_QUIT) exit(0);
            if (SDL_GetTicks() - t0 > 1500 && (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONDOWN)) go = 1;
        }
        if (go) break;
        int W, H;
        if (!gpu_overlay_begin(&W, &H, 1.f)) return;
        const float bg[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 }, grey[4] = { 0.72f, 0.72f, 0.75f, 1 }, red[4] = { 0.85f, 0.12f, 0.12f, 1 };
        gpu_overlay_rect(0, 0, (float)W, (float)H, bg);
        float u = H / 30.f; if (u * 40 > W) u = W / 40.f;
        int n = 0; while (L[n]) n++;
        float y = H / 2.f - n * u * 1.4f / 2.f - u;
        for (int i = 0; i < n; i++) {
            float px = i == 0 ? u * 1.6f : u * 0.95f; const float *c = i == 0 ? white : grey;
            if (L[i][0]) gpu_overlay_text((W - gpu_overlay_text_width(px, L[i])) / 2.f, y, px, L[i], c);
            if (i == 0) gpu_overlay_rect(W / 2.f - u * 6, y + px * 1.15f, u * 12, u * 0.12f, red);
            y += i == 0 ? u * 2.4f : u * 1.4f;
        }
        if (SDL_GetTicks() - t0 > 1500) {
            const char *p = "Press any button to continue";
            gpu_overlay_text((W - gpu_overlay_text_width(u * 0.8f, p)) / 2.f, H - u * 3.f, u * 0.8f, p, white);
        }
        gpu_overlay_end();
        SDL_Delay(16);
    }
}

#else
void disclaimer_run(void) {}
void settings_apply(void) {}
void menu_run(void) {}
#endif

