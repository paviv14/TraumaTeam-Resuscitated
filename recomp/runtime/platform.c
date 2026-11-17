// Host platform layer: window, frame presentation and input state (SDL2).
// Built without TTPC_SDL it is a headless stub (servers, CI, the frame-dump workflow).
//
// Frames: the renderer calls platform_present() for every EFB -> XFB copy (what the game displays).
// Input:  platform_poll() runs at each 60 Hz retrace; it pumps window events and snapshots the
//         keyboard and mouse into g_input, which the virtual Wii Remote (wiimote.c) reads.
#include "cpu.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

Input g_input;
int g_window_open;

// ---------------------------------------------------------------- audio
// The audio DMA hands over each buffer as it "plays" (paced by the host clock). Optional WAV capture
// (TTPC_WAV=file) for testing; SDL output keeps a small queue and drops data if it falls behind.
static FILE *g_wav; static u32 g_wav_frames; static int g_wav_rate;
static void wav_header(void) {
    u32 data = g_wav_frames * 4, rate = (u32)g_wav_rate;
    u8 h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    u32 riff = 36 + data, br = rate * 4;
    memcpy(h + 4, &riff, 4); memcpy(h + 24, &rate, 4); memcpy(h + 28, &br, 4); memcpy(h + 40, &data, 4);
    fseek(g_wav, 0, SEEK_SET); fwrite(h, 1, 44, g_wav); fseek(g_wav, 0, SEEK_END); fflush(g_wav);
}
static void wav_push(const s16 *lr, int frames, int rate) {
    static int tried;
    if (!g_wav && !tried) { tried = 1; const char *f = getenv("TTPC_WAV"); if (f && (g_wav = fopen(f, "wb"))) { g_wav_rate = rate; wav_header(); } }
    if (!g_wav) return;
    fwrite(lr, 4, (size_t)frames, g_wav); g_wav_frames += (u32)frames;
    if ((g_wav_frames & 0x3FFF) < (u32)frames) wav_header();
}
#ifdef TTPC_SDL
static void sdl_audio_push(const s16 *lr, int frames, int rate);
#endif
void speaker_mix(s16 *lr, int frames, int rate);   // speaker.c: the Wii Remote speaker's sounds, on the PC
void platform_audio_push(const s16 *in, int frames, int rate) {
    static s16 *mix; static int cap;
    if (frames * 2 > cap) { free(mix); cap = frames * 2 + 1024; mix = malloc(sizeof *mix * (size_t)cap); }
    const s16 *lr = in;
    if (mix) { memcpy(mix, in, sizeof *mix * (size_t)frames * 2); speaker_mix(mix, frames, rate); lr = mix; }
    wav_push(lr, frames, rate);
#ifdef TTPC_SDL
    sdl_audio_push(lr, frames, rate);
#endif
}

#ifdef TTPC_SDL
#include <SDL2/SDL.h>
#ifdef _WIN32
// Ask hybrid-graphics drivers for the discrete GPU (NVIDIA Optimus / AMD switchable graphics).
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#include "gx_state.h"
SDL_Window *g_sdl_window;
#define g_win g_sdl_window
static SDL_Renderer *g_ren;
static SDL_GLContext g_glctx;
extern int g_view_x, g_view_y, g_view_w, g_view_h;
static SDL_Texture *g_tex;
static int g_tw, g_th;

static SDL_AudioDeviceID g_audio; static int g_audio_rate, g_audio_paused, g_audio_playing;
static u32 g_au_underruns, g_au_drops; static u64 g_au_level, g_au_samples;
static void sdl_audio_push(const s16 *lr, int frames, int rate) {
    static int failed;
    if (failed || getenv("TTPC_MUTE")) return;
    if (!g_audio || rate != g_audio_rate) {
        if (g_audio) SDL_CloseAudioDevice(g_audio);
        if (!(SDL_WasInit(SDL_INIT_AUDIO)) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[audio] unavailable: %s\n", SDL_GetError()); failed = 1; return; }
        SDL_AudioSpec want = { 0 }, have;
        want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        g_audio_rate = rate; g_audio_playing = 0;
        if (!g_audio) { fprintf(stderr, "[audio] no output device (%s): continuing without sound\n", SDL_GetError()); failed = 1; return; }
        fprintf(stderr, "[audio] output %d Hz stereo\n", rate);
    }
    // A cushion of queued sound (~60 ms) absorbs frames where the game thread is busy. The game makes
    // sound by the PC's clock and the sound card plays it by its own, so the queue would slowly drain
    // or overfill (a click each time it hits a limit). Instead the sound is resampled by up to ±1%
    // (inaudible) to hold the queue at its target. Playback (re)starts once the cushion is there.
    u32 ms = (u32)rate * 4 / 1000, queued = SDL_GetQueuedAudioSize(g_audio), target = 60 * ms;
    if (g_audio_playing && queued == 0) { SDL_PauseAudioDevice(g_audio, 1); g_audio_playing = 0; g_au_underruns++; }
    if (queued > 250 * ms) { g_au_drops++; return; }   // far over (e.g. after a hitch): drop
    double err = ((double)queued - target) / target;    // >0: too much queued
    if (err > 1) err = 1; if (err < -1) err = -1;
    double step = 1.0 + 0.01 * err;                     // input frames consumed per output frame (±1% at most)
    static s16 out[2 * 8192]; static double pos; static s16 last[2];
    int no = 0;
    // linear interpolation between the previous buffer's last frame (index -1) and this buffer
    while (no < 8192) {
        double p = pos; int i = (int)p - 1;   // pos is relative to "last" at index 0
        if (i + 1 >= frames) break;
        double f = p - (int)p;
        for (int c = 0; c < 2; c++) {
            double s0 = i < 0 ? last[c] : lr[2 * i + c], s1 = lr[2 * (i + 1) + c];
            int v = (int)(s0 + (s1 - s0) * f);
            if (g_cfg.volume < 100) v = v * g_cfg.volume / 100;
            out[2 * no + c] = (s16)v;
        }
        no++; pos += step;
    }
    pos -= frames; last[0] = lr[2 * (frames - 1)]; last[1] = lr[2 * (frames - 1) + 1];
    g_au_level += queued / ms; g_au_samples++;
    SDL_QueueAudio(g_audio, out, (u32)no * 4);
    if (!g_audio_playing && queued + (u32)no * 4 >= target && !g_audio_paused) { SDL_PauseAudioDevice(g_audio, 0); g_audio_playing = 1; }
}
void platform_audio_stats(char *buf, int n) {   // for the perf log
    snprintf(buf, (size_t)n, "audio %.0f ms queued, %u underruns, %u drops", g_au_samples ? (double)g_au_level / g_au_samples : 0.0, g_au_underruns, g_au_drops);
    g_au_level = g_au_samples = 0;
}

void platform_audio_pause(int pause) { g_audio_paused = pause; g_audio_playing = 0; if (g_audio) { SDL_PauseAudioDevice(g_audio, 1); SDL_ClearQueuedAudio(g_audio); } }   // resumes via the cushion

int platform_init(void) {
    if (getenv("TTPC_HEADLESS")) return 0;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");   // full-resolution window with Windows display scaling
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1"); SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");   // the exe's icon (resuscitated.rc)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[platform] no display (%s): running headless\n", SDL_GetError());
        return 0;
    }
    int zoom = getenv("TTPC_ZOOM") ? atoi(getenv("TTPC_ZOOM")) : 2;   // window size multiplier
    if (zoom < 1) zoom = 1;
    int win_w = 640 * zoom, win_h = 480 * zoom;   // TTPC_WINSIZE=WxH (tests) overrides
    if (getenv("TTPC_WINSIZE")) sscanf(getenv("TTPC_WINSIZE"), "%dx%d", &win_w, &win_h);
    // OpenGL (GPU rendering) unless TTPC_SOFTWARE=1 or it is unavailable.
    if (!getenv("TTPC_SOFTWARE")) {
        // Creating the GPU context can fail transiently on Windows (driver still starting, the window
        // landing on a display without the GPU, a remote/streaming session): retry a few times before
        // falling back to the much slower software renderer.
        int ndisp = SDL_GetNumVideoDisplays(); if (ndisp < 1) ndisp = 1;
        for (int attempt = 0; attempt < 5 + ndisp; attempt++) {
            if (attempt) { fprintf(stderr, "[platform] OpenGL context attempt %d failed, retrying\n", attempt); SDL_Delay(300); }
            // after a few tries on the main display, try each display in turn (a virtual display, e.g. one
            // made for game streaming, may have no OpenGL driver)
            int disp = attempt < 4 ? 0 : (attempt - 4) % ndisp;
            SDL_GL_ResetAttributes();
            { int vr45 = vr_wanted() && attempt < 2;   // VR: SteamVR's OpenXR wants a newer OpenGL; fall back to 3.3
              SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, vr45 ? 4 : 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, vr45 ? 5 : 3); }
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);   // never the Windows generic (GDI) OpenGL 1.1
            g_win = SDL_CreateWindow("Trauma Team Resuscitated", SDL_WINDOWPOS_CENTERED_DISPLAY(disp), SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                                     win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!g_win) fprintf(stderr, "[platform] window: %s\n", SDL_GetError());
            else if (!(g_glctx = SDL_GL_CreateContext(g_win))) fprintf(stderr, "[platform] OpenGL context: %s\n", SDL_GetError());
            else if (SDL_GL_MakeCurrent(g_win, g_glctx) != 0) fprintf(stderr, "[platform] make current: %s\n", SDL_GetError());
            else {
                typedef const unsigned char *(SDLCALL *GetStr)(unsigned);
                GetStr gs = (GetStr)SDL_GL_GetProcAddress("glGetString");
                if (gs) fprintf(stderr, "[platform] display %d (%s): OpenGL %s | %s | %s\n", disp, SDL_GetDisplayName(disp),
                                (const char *)gs(0x1F02), (const char *)gs(0x1F00), (const char *)gs(0x1F01));   // version, vendor, renderer
                SDL_DisplayMode dm;
                if (SDL_GetCurrentDisplayMode(disp, &dm) == 0) fprintf(stderr, "[platform] display mode %dx%d @ %d Hz\n", dm.w, dm.h, dm.refresh_rate);
            }
            if (g_win && g_glctx && SDL_GL_GetCurrentContext() == g_glctx) {
                SDL_GL_SetSwapInterval(0);   // the game paces itself to the 60 Hz retrace
                settings_load();
                if (gpu_init(g_cfg.scale)) {
                    settings_apply();
                    vr_init();
                    SDL_ShowCursor(SDL_DISABLE);
                    g_window_open = 1;
                    return 1;
                }
            }
            if (g_glctx) { SDL_GL_DeleteContext(g_glctx); g_glctx = NULL; }
            if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
        }
        fprintf(stderr, "[platform] OpenGL 3.3 unavailable (%s): using the software renderer\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Trauma Team",
            "Could not start GPU rendering (OpenGL 3.3), so the game will run with the slow software renderer.\n\n"
            "Restarting the game usually fixes this. If it keeps happening, update the graphics driver.", NULL);
    }
    g_win = SDL_CreateWindow("Trauma Team Resuscitated (software renderer)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             640 * zoom, 480 * zoom, SDL_WINDOW_RESIZABLE);
    if (!g_win) { fprintf(stderr, "[platform] window: %s\n", SDL_GetError()); return 0; }
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    SDL_RenderSetLogicalSize(g_ren, 640, 480);        // keep 4:3, letterbox when resized
    SDL_ShowCursor(SDL_DISABLE);                      // the game draws its own pointer
    g_window_open = 1;
    return 1;
}

void platform_present(const u32 *px, int x0, int y0, int w, int h, int stride) {
    if (!g_window_open || w <= 0 || h <= 0) return;
    if (!g_tex || g_tw != w || g_th != h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        g_tw = w; g_th = h;
        SDL_RenderSetLogicalSize(g_ren, w, h);
    }
    SDL_UpdateTexture(g_tex, NULL, px + (long)y0 * stride + x0, stride * 4);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

// ---------------------------------------------------------------- game controllers
// Any SDL game controller (Xbox, PlayStation, Switch Pro...). The right stick drives the pointer
// like a mouse would; whichever of mouse or pad moved last owns the pointer.
static SDL_GameController *g_pad;
static int g_pointer_pad;                 // 1 = pad owns the pointer
static int g_last_dev = 1;                // 1 = keyboard & mouse, 2 = controller, 3 = a real Wii Remote (used last)
void platform_wii_used(void) { g_last_dev = 3; }
int platform_last_dev(void) { return g_last_dev; }
// rumble from the game (WPADControlMotor, motion.c): a real Wii Remote and the controller both
static int g_rumble_game; static float g_rumble_spk;
static void pad_rumble_apply(void) {
    if (!g_pad) return;
    int lo = g_rumble_game ? 0x5000 : 0, hi = g_rumble_game ? 0xA000 : 0;
    int s = (int)(g_rumble_spk * 0xC000); if (s > 0xC000) s = 0xC000;   // the speaker's loudness on the big motor
    if (s > lo) lo = s;
    SDL_GameControllerRumble(g_pad, (Uint16)lo, (Uint16)hi, lo || hi ? 5000 : 0);
}
void platform_rumble(int on) {
    void whid_rumble(int on); whid_rumble(on);
    g_rumble_game = on; pad_rumble_apply();
}
// the speaker's sound on the PC (speaker.c): the controller rumbles with its loudness ...
void platform_speaker_level(float level) {
    static float last; if (fabsf(level - last) < 0.05f && level) return; last = level;
    g_rumble_spk = level < 0.08f ? 0 : level; pad_rumble_apply();
}
// ... and a wired DualSense (it is a USB sound card too) plays it from its own speaker
static SDL_AudioDeviceID g_ds_audio; static int g_ds_tried;
int platform_speaker_pcm(const s16 *pcm, int n) {
    if (!g_pad || SDL_GameControllerGetType(g_pad) != SDL_CONTROLLER_TYPE_PS5) return 0;
    if (!g_ds_tried) {
        g_ds_tried = 1;
        for (int i = 0; i < SDL_GetNumAudioDevices(0); i++) {
            const char *nm = SDL_GetAudioDeviceName(i, 0);
            if (nm && (strstr(nm, "DualSense") || strstr(nm, "Wireless Controller"))) {
                // its four channels: 1-2 the speaker (or a headset), 3-4 the haptic motors - the sound goes to the
                // speaker alone (sent to all four, it came out as vibration), at the device's own 48 kHz
                SDL_AudioSpec want = { 0 }, have; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 4; want.samples = 512;
                g_ds_audio = SDL_OpenAudioDevice(nm, 0, &want, &have, 0);
                if (g_ds_audio) {
                    SDL_PauseAudioDevice(g_ds_audio, 0);
                    // the controller sends its sound to the headphone jack until told otherwise: route it to the
                    // built-in speaker (output path 3: the right channel to the speaker) at a good volume
                    u8 fx[47] = { 0 };
                    fx[0] = 0x20 | 0x80;   // valid: speaker volume, audio control
                    fx[5] = 0x64;          // speaker volume
                    fx[7] = 3 << 4;        // audio control: output path = speaker
                    int r = SDL_GameControllerSendEffect(g_pad, fx, sizeof fx);
                    fprintf(stderr, "[speaker] playing on the DualSense's speaker (%s)%s\n", nm, r ? " - couldn't switch it to the speaker" : "");
                }
                break;
            }
        }
    }
    if (g_ds_audio && SDL_GetQueuedAudioSize(g_ds_audio) < 48000 * 8 / 4) {
        static s16 out[40 * 8 * 4 * 4]; static s16 prev; int m = 0;
        for (int i = 0; i < n && i < 160; i++) for (int k = 0; k < 8; k++) {
            int v = (prev + (pcm[i] - prev) * (k + 1) / 8) * 4;   // the controller's speaker is quiet if (v > 32767) v = 32767; if (v < -32768) v = -32768;   // 6 -> 48 kHz
            out[m++] = (s16)v; out[m++] = (s16)v; out[m++] = 0; out[m++] = 0;
            if (k == 7) prev = pcm[i];
        }
        SDL_QueueAudio(g_ds_audio, out, (Uint32)m * 2);
    }
    return g_ds_audio != 0;
}
static float g_pad_px = 0.5f, g_pad_py = 0.5f;
// Every connected controller is opened; the one used last is g_pad (a DualSense paired over Bluetooth and an
// 8BitDo on USB: only the first was read, the other did nothing).
static SDL_GameController *g_pads[8];
static int pad_id(SDL_GameController *c) { return c ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) : -1; }
static void pad_open(int index) {
    if (!SDL_IsGameController(index)) return;
    int id = SDL_JoystickGetDeviceInstanceID(index);
    for (int k = 0; k < 8; k++) if (g_pads[k] && pad_id(g_pads[k]) == id) return;
    for (int k = 0; k < 8; k++) if (!g_pads[k]) {
        g_pads[k] = SDL_GameControllerOpen(index);
        if (g_pads[k]) { fprintf(stderr, "[input] controller: %s\n", SDL_GameControllerName(g_pads[k])); if (!g_pad) g_pad = g_pads[k]; }
        return;
    }
}
static void pad_use(int id) {   // a press or a real move on this controller: it becomes the one read
    if (pad_id(g_pad) == id) return;
    for (int k = 0; k < 8; k++) if (g_pads[k] && pad_id(g_pads[k]) == id) {
        g_pad = g_pads[k]; fprintf(stderr, "[input] using %s\n", SDL_GameControllerName(g_pad)); void prompts_update(int); prompts_update(1); return; }
}
static float axis(SDL_GameControllerAxis a) {
    float v = g_pad ? SDL_GameControllerGetAxis(g_pad, a) / 32767.f : 0.f;
    const float dz = 0.18f;                                    // dead zone, rescaled to 0..1 beyond it
    if (v > dz) return (v - dz) / (1 - dz); if (v < -dz) return (v + dz) / (1 - dz); return 0;
}

#define SC(n) SDL_SCANCODE_##n
#define CB(n) SDL_CONTROLLER_BUTTON_##n
// The settled layout (docs/CONTROLS.md); the mouse buttons stay fixed (left = A, right = B, middle = swing).
Bind g_bind[NBIND] = {
    [B_A] = { "a", "A", SC(SPACE), SC(RETURN), CB(A) },       [B_B] = { "b", "B", SC(LSHIFT), SC(X), CB(B) },
    [B_Z] = { "z", "Z (Nunchuk)", SC(E), SC(F), PAD_LT },     [B_C] = { "c", "C (Nunchuk)", SC(Q), SC(C), CB(LEFTSHOULDER) },
    [B_ONE] = { "one", "1", SC(1), 0, CB(X) },                [B_TWO] = { "two", "2", SC(2), 0, -1 },
    [B_PLUS] = { "plus", "+", SC(EQUALS), SC(P), CB(START) }, [B_MINUS] = { "minus", "-", SC(MINUS), SC(M), CB(BACK) },
    [B_HOME] = { "home", "HOME", SC(ESCAPE), SC(H), CB(GUIDE) },
    [B_UP] = { "up", "D-pad up", SC(UP), 0, CB(DPAD_UP) },    [B_DOWN] = { "down", "D-pad down", SC(DOWN), 0, CB(DPAD_DOWN) },
    [B_LEFT] = { "left", "D-pad left", SC(LEFT), 0, CB(DPAD_LEFT) }, [B_RIGHT] = { "right", "D-pad right", SC(RIGHT), 0, CB(DPAD_RIGHT) },
    [B_ST_UP] = { "stick_up", "Stick up", SC(W), 0, -1 },     [B_ST_DOWN] = { "stick_down", "Stick down", SC(S), 0, -1 },
    [B_ST_LEFT] = { "stick_left", "Stick left", SC(A), 0, -1 }, [B_ST_RIGHT] = { "stick_right", "Stick right", SC(D), 0, -1 },
    [B_SWING] = { "swing", "Swing / CPR", SC(G), 0, CB(Y) },
    [B_PUSH] = { "push", "Push (paddles, scope)", SC(R), SC(TAB), -1 }, [B_PULL] = { "pull", "Pull (scope)", SC(V), 0, -1 },
    [B_TW_L] = { "twist_l", "Twist left", SC(J), 0, CB(LEFTSHOULDER) }, [B_TW_R] = { "twist_r", "Twist right", SC(L), 0, CB(RIGHTSHOULDER) },
    [B_GRAB] = { "grab", "Grab / shock (B half)", 0, 0, PAD_RT }, [B_CENTER] = { "center", "Centre pointer", 0, 0, CB(RIGHTSHOULDER) },
};
#undef SC
#undef CB
static int pad_down(int p) {
    if (!g_pad || p < 0) return 0;
    if (p == PAD_LT) return axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.3f;
    if (p == PAD_RT) return axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.3f;
    return SDL_GameControllerGetButton(g_pad, (SDL_GameControllerButton)p);
}
const char *bind_pad_name(int p) {
    static const char *x[] = { "A", "B", "X", "Y", "View", "Guide", "Menu", "L-stick", "R-stick", "LB", "RB", "D-up", "D-down", "D-left", "D-right",
                               "Share", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad" };
    static const char *ps[] = { "Cross", "Circle", "Square", "Triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "D-up", "D-down", "D-left", "D-right",
                                "Mute", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad" };
    int sty = 0;   // 0 Xbox, 1 PS4, 2 PS5
    { int st = g_cfg.pad_style; if (!st && g_pad) { int t = SDL_GameControllerGetType(g_pad); st = t == SDL_CONTROLLER_TYPE_PS4 ? 2 : t == SDL_CONTROLLER_TYPE_PS5 ? 3 : 1; } sty = st >= 2 ? st - 1 : 0; }
    if (p < 0) return "-";
    if (p == PAD_LT) return sty ? "L2" : "LT";
    if (p == PAD_RT) return sty ? "R2" : "RT";
    if (p < 21) return sty ? (sty == 2 && p == 4 ? "Create" : ps[p]) : x[p];
    return "?";
}

void platform_set_title(const char *t) { if (g_win) SDL_SetWindowTitle(g_win, t); }

// Button prompts: which set of help pictures the game shows (texpack.c prompt sets). Settings: Automatic (the
// device used last), Wii Remote (the original pictures), Keyboard & mouse, Controller; the controller's style is
// asked from SDL (PS4 / PS5 / everything else Xbox) unless the settings fix it.

// ---------------------------------------------------------------- live button prompts: Wii action -> glyphs
static void tok(PromptTok *o, int *n, int max, int kind, const char *name) { if (*n < max) { o[*n].kind = kind; snprintf(o[*n].name, sizeof o[*n].name, "%s", name); (*n)++; } }
static const char *short_key(int sc) {
    static char b[24]; if (sc <= 0) return "?";
    const char *k = SDL_GetScancodeName((SDL_Scancode)sc); if (!k || !*k) return "?";
    static const char *map[][2] = { { "Left Shift", "Shift" }, { "Right Shift", "Shift" }, { "Left Ctrl", "Ctrl" }, { "Right Ctrl", "Ctrl" },
        { "Left Alt", "Alt" }, { "Right Alt", "Alt" }, { "Return", "Enter" }, { "Escape", "Esc" }, { "Backspace", "Bksp" },
        { "CapsLock", "Caps" }, { "Delete", "Del" }, { "Insert", "Ins" }, { "PageUp", "PgUp" }, { "PageDown", "PgDn" },
        { "Up", "\x5e" }, { "Down", "v" }, { "Left", "<" }, { "Right", ">" }, { "Left GUI", "Win" }, { "Right GUI", "Win" } };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(k, map[i][0])) return map[i][1];
    if (!strncmp(k, "Keypad ", 7)) { snprintf(b, sizeof b, "Num%s", k + 7); return b; }
    snprintf(b, sizeof b, "%s", k); return b;
}
const char *bind_key_label(int b) { return short_key(g_bind[b].key > 0 ? g_bind[b].key : g_bind[b].key2); }
static void pad_tok(PromptTok *o, int *n, int max, int p) {
    char nm[8];
    if (p < 0) { tok(o, n, max, 1, "-"); return; }
    if (p == PAD_LT) snprintf(nm, sizeof nm, "lt"); else if (p == PAD_RT) snprintf(nm, sizeof nm, "rt"); else snprintf(nm, sizeof nm, "b%d", p);
    tok(o, n, max, 0, nm);
}
static void key_tok(PromptTok *o, int *n, int max, int b) { tok(o, n, max, 1, short_key(g_bind[b].key > 0 ? g_bind[b].key : g_bind[b].key2)); }
// four direction keys: the WASD / arrow-key pictures when they are those keys, else the key names
static void dir_keys(PromptTok *o, int *n, int max, int up, int lf, int dn, int rt, const char *which) {
    int k[4] = { g_bind[up].key, g_bind[lf].key, g_bind[dn].key, g_bind[rt].key }, b[4] = { up, lf, dn, rt };
    if (k[0] == SDL_SCANCODE_W && k[1] == SDL_SCANCODE_A && k[2] == SDL_SCANCODE_S && k[3] == SDL_SCANCODE_D && !*which) { tok(o, n, max, 0, "wasd"); return; }
    if (k[0] == SDL_SCANCODE_UP && k[1] == SDL_SCANCODE_LEFT && k[2] == SDL_SCANCODE_DOWN && k[3] == SDL_SCANCODE_RIGHT) {
        char nm[16]; snprintf(nm, sizeof nm, "arrows%s", which); tok(o, n, max, 0, nm); return; }
    int lo = 0, hi = 4; if (!strcmp(which, "_lr")) { b[0] = lf; b[1] = rt; hi = 2; } else if (!strcmp(which, "_ud")) { b[1] = dn; hi = 2; }
    char join[24] = ""; int single = 1;
    for (int i = lo; i < hi; i++) { const char *s = short_key(g_bind[b[i]].key); if (strlen(s) != 1) single = 0; else strncat(join, s, 1); }
    if (single) { tok(o, n, max, 1, join); return; }
    for (int i = lo; i < hi; i++) key_tok(o, n, max, b[i]);
}
int prompt_tokens(int set, const char *a, PromptTok *o, int max) {
    int n = 0, kb = set == 1;
#define IS(x) (!strcmp(a, x))
    if (IS("A")) { if (kb) tok(o, &n, max, 0, "mouse_l"); else pad_tok(o, &n, max, g_bind[B_A].pad); }
    else if (IS("B")) { if (kb) tok(o, &n, max, 0, "mouse_r"); else pad_tok(o, &n, max, g_bind[B_B].pad); }
    else if (IS("AB")) {   // grab: left + right click; on a pad A + the B half (RT)
        if (kb) { tok(o, &n, max, 0, "mouse_l"); tok(o, &n, max, 2, "+"); tok(o, &n, max, 0, "mouse_r"); }
        else { pad_tok(o, &n, max, g_bind[B_A].pad); tok(o, &n, max, 2, "+"); pad_tok(o, &n, max, g_bind[B_GRAB].pad); } }
    else if (IS("ZB")) {   // defibrillator: Z + B
        if (kb) { key_tok(o, &n, max, B_Z); tok(o, &n, max, 2, "+"); tok(o, &n, max, 0, "mouse_r"); }
        else { pad_tok(o, &n, max, g_bind[B_Z].pad); tok(o, &n, max, 2, "+"); pad_tok(o, &n, max, g_bind[B_GRAB].pad); } }
    else if (IS("Z")) { if (kb) key_tok(o, &n, max, B_Z); else pad_tok(o, &n, max, g_bind[B_Z].pad); }
    else if (IS("C")) { if (kb) key_tok(o, &n, max, B_C); else pad_tok(o, &n, max, g_bind[B_C].pad); }
    else if (IS("plus")) { if (kb) key_tok(o, &n, max, B_PLUS); else pad_tok(o, &n, max, g_bind[B_PLUS].pad); }
    else if (IS("minus")) { if (kb) key_tok(o, &n, max, B_MINUS); else pad_tok(o, &n, max, g_bind[B_MINUS].pad); }
    else if (IS("home")) { if (kb) key_tok(o, &n, max, B_HOME); else pad_tok(o, &n, max, g_bind[B_HOME].pad); }
    else if (IS("one")) { if (kb) key_tok(o, &n, max, B_ONE); else pad_tok(o, &n, max, g_bind[B_ONE].pad); }
    else if (IS("two")) { if (kb) key_tok(o, &n, max, B_TWO); else pad_tok(o, &n, max, g_bind[B_TWO].pad); }
    else if (IS("stick")) {   // the Nunchuk stick: the four keys, or the left stick
        if (kb) dir_keys(o, &n, max, B_ST_UP, B_ST_LEFT, B_ST_DOWN, B_ST_RIGHT, "");
        else tok(o, &n, max, 0, "ls"); }
    else if (IS("pointer")) tok(o, &n, max, 0, kb ? "mouse_move" : "rs");
    else if (IS("pointA")) {   // point, then press A
        tok(o, &n, max, 0, kb ? "mouse_move" : "rs"); tok(o, &n, max, 2, "+");
        if (kb) tok(o, &n, max, 0, "mouse_l"); else pad_tok(o, &n, max, g_bind[B_A].pad); }
    else if (IS("dpad") || IS("dpad_lr") || IS("dpad_ud")) {
        const char *w = a + 4;   // "", "_lr", "_ud"
        if (kb) dir_keys(o, &n, max, B_UP, B_LEFT, B_DOWN, B_RIGHT, w); else tok(o, &n, max, 0, a); }
    else if (IS("swing")) { if (kb) tok(o, &n, max, 0, "mouse_m"); else pad_tok(o, &n, max, g_bind[B_SWING].pad); }
    else if (IS("twist") || IS("twistn") || IS("circle")) {   // twist the Remote / the Nunchuk: the wheel, or the twist buttons
        if (kb) { if (IS("twistn")) { tok(o, &n, max, 1, "Ctrl"); tok(o, &n, max, 2, "+"); } tok(o, &n, max, 0, "wheel"); }
        else { if (IS("twistn")) { pad_tok(o, &n, max, g_bind[B_Z].pad); tok(o, &n, max, 2, "+"); }
               pad_tok(o, &n, max, g_bind[B_TW_L].pad); tok(o, &n, max, 2, "/"); pad_tok(o, &n, max, g_bind[B_TW_R].pad); } }
    else if (IS("paddles")) { if (kb) { key_tok(o, &n, max, B_ST_UP); tok(o, &n, max, 2, "+"); tok(o, &n, max, 0, "wheel"); } else { pad_tok(o, &n, max, 7); tok(o, &n, max, 2, "+"); pad_tok(o, &n, max, 8); } }
    else if (IS("fwd") || IS("back")) { if (kb) tok(o, &n, max, 0, "wheel"); else tok(o, &n, max, 0, "rs"); }
    else if (IS("circle")) {   // bandage: hold A and circle the pointer
        if (kb) { tok(o, &n, max, 0, "mouse_l"); tok(o, &n, max, 2, "+"); tok(o, &n, max, 0, "mouse_move"); }
        else { pad_tok(o, &n, max, g_bind[B_A].pad); tok(o, &n, max, 2, "+"); tok(o, &n, max, 0, "rs"); } }
#undef IS
    return n;
}
static unsigned bind_signature(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < NBIND; i++) { int v[3] = { g_bind[i].key, g_bind[i].key2, g_bind[i].pad };
        for (int k = 0; k < 3; k++) { h ^= (unsigned)v[k]; h *= 16777619u; } }
    return h;
}
int g_prompt_set;   // 0 Wii Remote pictures, else another controller's (gl.c hides the 3D Remote then)
void prompts_update(int force) {
    static int cur = -1;
    int wiimote_native(void);
    int dev = g_cfg.prompts == 0 ? (g_last_dev == 3 && wiimote_native() ? 0 : g_last_dev == 2 && g_pad ? 2 : 1) : g_cfg.prompts == 1 ? 0 : g_cfg.prompts == 2 ? 1 : 2;
    int set = 0;
    if (dev == 1) set = 1;
    else if (dev == 2) {
        int style = g_cfg.pad_style;
        if (style == 0) {
            style = 1;
            if (g_pad) switch (SDL_GameControllerGetType(g_pad)) {
                case SDL_CONTROLLER_TYPE_PS4: style = 2; break;
                case SDL_CONTROLLER_TYPE_PS5: style = 3; break;
                default: break;
            }
        }
        set = 1 + style;   // 2 xbox, 3 ps4, 4 ps5
    }
    if (set && !texpack_prompt_count(set)) set = set == 4 && texpack_prompt_count(3) ? 3 : 0;   // a missing set: fall back
    unsigned sig = bind_signature(); static unsigned cur_sig;
    if (sig != cur_sig) { cur_sig = sig; texpack_prompts_changed(sig); void gpu_prompts_flush(void); gpu_prompts_flush(); force = 1; }
    if (set == cur && !force) return;
    cur = set; g_prompt_set = set;
    texpack_set_prompts(set);
    video_retarget_textures();
    static const char *nm[] = { "Wii Remote (original)", "keyboard & mouse", "Xbox", "PlayStation 4", "PlayStation 5" };
    fprintf(stderr, "[prompts] showing %s buttons\n", nm[set]);
}
// Controllers plugged in or out. Every event loop calls this (the game's, the options menu's, the notice at
// start): a loop that dropped these events left the controllers that were connected at start unopened.
void platform_pad_event(const SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED) pad_open(e->cdevice.which);
    if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (int k = 0; k < 8; k++) if (g_pads[k] && pad_id(g_pads[k]) == e->cdevice.which) {
            if (g_pad == g_pads[k]) { g_pad = NULL; g_pointer_pad = 0; }
            SDL_GameControllerClose(g_pads[k]); g_pads[k] = NULL; }
        if (!g_pad) for (int k = 0; k < 8; k++) if (g_pads[k]) { g_pad = g_pads[k]; break; }
    }
}
static int g_wheel_push, g_wheel_pull, g_wheel_notch;
void platform_poll(void) {
    if (!g_window_open) return;
    { static int shown; if (!shown && g_glctx) { shown = 1; void disclaimer_run(void); disclaimer_run(); for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open(i); } }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { fprintf(stderr, "[platform] window closed\n"); exit(0); }
        platform_pad_event(&e);
        if (e.type == SDL_CONTROLLERBUTTONDOWN) pad_use(e.cbutton.which);
        if (e.type == SDL_CONTROLLERAXISMOTION && (e.caxis.value > 20000 || e.caxis.value < -20000)) pad_use(e.caxis.which);
        if (e.type == SDL_MOUSEMOTION) g_pointer_pad = 0;
        // the device used last (for the button prompts): a press, or a real move, not a twitch
        if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEWHEEL) g_last_dev = 1;
        if (e.type == SDL_MOUSEMOTION && (e.motion.xrel * e.motion.xrel + e.motion.yrel * e.motion.yrel) > 64) g_last_dev = 1;
        if (e.type == SDL_CONTROLLERBUTTONDOWN) g_last_dev = 2;
        if (e.type == SDL_CONTROLLERAXISMOTION && (e.caxis.value > 20000 || e.caxis.value < -20000)) g_last_dev = 2;
        if (e.type == SDL_MOUSEWHEEL && e.wheel.y) g_wheel_notch += e.wheel.y > 0 ? 1 : -1;   // used in platform_poll, by what is being played
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9 && !e.key.repeat) { void video_capture_request(void); video_capture_request(); }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F1 && !e.key.repeat && g_glctx) { menu_run(); return; }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && g_pad && g_glctx &&
            ((e.cbutton.button == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK)) ||
             (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START)))) { menu_run(); return; }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
            u32 fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            g_cfg.fullscreen = !fs; if (g_glctx) settings_save();
            SDL_SetWindowFullscreen(g_win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
    }
    const u8 *k = SDL_GetKeyboardState(NULL);
    int mx, my; u32 mb = SDL_GetMouseState(&mx, &my);
    // Mouse position -> game screen coordinates (0..1), honouring the letterboxed logical size.
    if (g_ren) {
        float lx, ly; SDL_RenderWindowToLogical(g_ren, mx, my, &lx, &ly);
        int lw, lh; SDL_RenderGetLogicalSize(g_ren, &lw, &lh);
        if (lw <= 0) { lw = 640; lh = 480; }
        g_input.mouse_x = lx / lw; g_input.mouse_y = ly / lh;
    } else if (g_view_w > 0) {   // GL: picture rectangle in drawable pixels; mouse is in window points
        int ww, wh, dw, dh; SDL_GetWindowSize(g_win, &ww, &wh); SDL_GL_GetDrawableSize(g_win, &dw, &dh);
        float fx = mx * (float)dw / ww, fy = my * (float)dh / wh;
        g_input.mouse_x = (fx - g_view_x) / g_view_w; g_input.mouse_y = (fy - g_view_y) / g_view_h;
    }
    g_input.mouse_in = SDL_GetMouseFocus() == g_win;
    g_input.lmb = (mb & SDL_BUTTON_LMASK) != 0;
    g_input.rmb = (mb & SDL_BUTTON_RMASK) != 0;
    g_input.mmb = (mb & SDL_BUTTON_MMASK) != 0;
#define KEY(n) (k[SDL_SCANCODE_##n] != 0)
#define BK(b) ((g_bind[b].key > 0 && k[g_bind[b].key]) || (g_bind[b].key2 > 0 && k[g_bind[b].key2]))
    g_input.up = BK(B_UP);       g_input.down = BK(B_DOWN);
    g_input.left = BK(B_LEFT);   g_input.right = BK(B_RIGHT);
    g_input.stick_up = BK(B_ST_UP);  g_input.stick_down = BK(B_ST_DOWN);
    g_input.stick_left = BK(B_ST_LEFT); g_input.stick_right = BK(B_ST_RIGHT);
    g_input.a = BK(B_A);
    g_input.b = BK(B_B);
    g_input.plus = BK(B_PLUS);
    g_input.minus = BK(B_MINUS);
    g_input.home = BK(B_HOME);
    g_input.one = BK(B_ONE);  g_input.two = BK(B_TWO);
    g_input.c = BK(B_C);  g_input.z = BK(B_Z);       // Nunchuk buttons
    g_input.roll_l = BK(B_TW_L);  g_input.roll_r = BK(B_TW_R);     // twist the Remote
    // The mouse wheel, by what is being played (CONTROLS.md): the endoscope drives with it (Ctrl + wheel twists the
    // snare); elsewhere it twists the Remote one step a notch, and W + wheel up is the paddles' push.
    extern char g_op_mode[3]; int endo = g_op_mode[0] == 'e' && g_op_mode[1] == 'n';
    int ctrl = KEY(LCTRL) || KEY(RCTRL);
    static int w_push;   // W is being used for the paddles' push (not the stick) until it is let go
    g_input.twist = 0;
    if (g_wheel_notch) {
        int up = g_wheel_notch > 0;
        // each notch drives a little longer and notches add up (5 frames a notch moved in jerks)
        if (endo && !ctrl) { if (up) { g_wheel_push = g_wheel_push + 12 > 36 ? 36 : g_wheel_push + 12; g_wheel_pull = 0; }
                             else { g_wheel_pull = g_wheel_pull + 12 > 36 ? 36 : g_wheel_pull + 12; g_wheel_push = 0; } }
        else if (!endo && up && BK(B_ST_UP)) { g_wheel_push = 8; w_push = 1; }
        else g_input.twist = g_wheel_notch * 0.2618f;   // 15 degrees a notch (up = clockwise)
        g_wheel_notch = 0;
    }
    if (!BK(B_ST_UP)) w_push = 0;
    if (w_push) g_input.stick_up = 0;
    g_input.push = BK(B_PUSH) || g_wheel_push > 0; if (g_wheel_push > 0) g_wheel_push--;
    g_input.pull = BK(B_PULL) || g_wheel_pull > 0; if (g_wheel_pull > 0) g_wheel_pull--;
    g_input.swing = BK(B_SWING) || g_input.mmb;
    {   // a grab (left + right click) holds on while left click does: right click may be let go to turn with the wheel
        static int grab; if (g_input.lmb && g_input.rmb) grab = 1; if (!g_input.lmb) grab = 0;
        if (grab) g_input.rmb = 1;
    }
#undef KEY
#undef BK
    g_input.nx = g_input.ny = 0; g_input.has_analog = 0;
    if (g_pad) {
#define PB(b) SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_##b)
#define PP(b) pad_down(g_bind[b].pad)
        // the settled layout (docs/CONTROLS.md)
        int pa = PP(B_A), lt = PP(B_Z), rt = PP(B_GRAB);
        int op = g_op_mode[0] && !(g_op_mode[0] == 'd' && g_op_mode[1] == 'i');   // in play where the Remote is twisted
        g_input.a |= pa;
        g_input.b |= PP(B_B) || (rt && (pa || lt));        // Grab (RT) is only the B half of A+RT (grab) and LT+RT (defibrillator)
        g_input.z |= lt;
        int lb = PP(B_TW_L), rb = PP(B_TW_R);
        // chapter select (its episode grid drawn just now, video.c): LB / RB switch the character, like the D-pad
        extern u64 g_chapter_ns; u64 host_now_ns(void);
        int chapter = g_chapter_ns && host_now_ns() - g_chapter_ns < 250000000ull;
        if (chapter) { g_input.left |= lb; g_input.right |= rb; }
        int twist_pad = op && !chapter && (!endo || lt);                // twist buttons; in the endoscope only while Z is held (else C)
        if (twist_pad) { g_input.roll_l |= lb; g_input.roll_r |= rb; }
        g_input.c |= PP(B_C) && !((twist_pad || chapter) && g_bind[B_C].pad == g_bind[B_TW_L].pad);
        g_input.up |= PP(B_UP); g_input.down |= PP(B_DOWN); g_input.left |= PP(B_LEFT); g_input.right |= PP(B_RIGHT);
        g_input.one |= PP(B_ONE); g_input.two |= PP(B_TWO);
        g_input.swing |= PP(B_SWING);                       // strike / CPR
        g_input.push |= PP(B_PUSH) || (PB(LEFTSTICK) && PB(RIGHTSTICK));   // the paddles: both sticks pressed
        g_input.pull |= PP(B_PULL);
        g_input.plus |= PP(B_PLUS); g_input.minus |= PP(B_MINUS); g_input.home |= PP(B_HOME);
        int center = PP(B_CENTER) && !((twist_pad || chapter) && g_bind[B_CENTER].pad == g_bind[B_TW_R].pad);
        float lx = axis(SDL_CONTROLLER_AXIS_LEFTX), ly = -axis(SDL_CONTROLLER_AXIS_LEFTY);
        float rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
        if (pa && (lx || ly)) { rx += lx; ry -= ly; lx = ly = 0; }   // dragging: both sticks move the pointer, the Nunchuk stick rests
        g_input.nx = lx; g_input.ny = ly; g_input.has_analog = lx != 0 || ly != 0;
        if (endo && !PB(RIGHTSTICK)) {                     // the endoscope: the right stick drives it; R3 held, it aims the light
            if (ry < -0.5f) g_input.push = 1; else if (ry > 0.5f) g_input.pull = 1;
            rx = ry = 0;
        }
        // right stick = pointer velocity (full tilt crosses the screen in ~0.6 s; squared for precision)
        if (rx || ry) {
            if (!g_pointer_pad) { g_pad_px = g_input.mouse_x; g_pad_py = g_input.mouse_y; g_pointer_pad = 1; }
            static const float speeds[6] = { 0, 0.7f, 1.1f, 1.6f, 2.3f, 3.2f };   // screen widths per second, by setting
            const float speed = speeds[g_cfg.pointer_speed >= 1 && g_cfg.pointer_speed <= 5 ? g_cfg.pointer_speed : 3] / 60.f;
            g_pad_px += rx * (rx < 0 ? -rx : rx) * speed; g_pad_py += ry * (ry < 0 ? -ry : ry) * speed;
        }
        if (center) { g_pad_px = 0.5f; g_pad_py = 0.5f; g_pointer_pad = 1; }
        if (g_pad_px < 0) g_pad_px = 0; if (g_pad_px > 1) g_pad_px = 1; if (g_pad_py < 0) g_pad_py = 0; if (g_pad_py > 1) g_pad_py = 1;
#undef PB
#undef PP
    }
    if (g_pointer_pad) { g_input.mouse_x = g_pad_px; g_input.mouse_y = g_pad_py; g_input.mouse_in = 1; }
    prompts_update(0);
    vr_input(&g_input);
}

#else   // headless build
Bind g_bind[NBIND];
int g_prompt_set;
int platform_init(void) { return 0; }
void platform_present(const u32 *px, int x0, int y0, int w, int h, int stride) { (void)px; (void)x0; (void)y0; (void)w; (void)h; (void)stride; }
void platform_poll(void) {}
void platform_set_title(const char *t) { (void)t; }
void prompts_update(int force) { (void)force; }
void platform_wii_used(void) {}
void platform_rumble(int on) { void whid_rumble(int on); whid_rumble(on); }
void platform_speaker_level(float level) { (void)level; }
int platform_speaker_pcm(const s16 *pcm, int n) { (void)pcm; (void)n; return 0; }
void platform_audio_stats(char *buf, int n) { if (n) buf[0] = 0; }
int platform_last_dev(void) { return 0; }
const char *bind_key_label(int b) { (void)b; return "W"; }
int prompt_tokens(int set, const char *act, PromptTok *out, int max) { (void)set; (void)act; (void)out; (void)max; return 0; }
#endif
