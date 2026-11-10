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
void platform_audio_push(const s16 *lr, int frames, int rate) {
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
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[platform] no display (%s): running headless\n", SDL_GetError());
        return 0;
    }
    int zoom = getenv("TTPC_ZOOM") ? atoi(getenv("TTPC_ZOOM")) : 2;   // window size multiplier
    if (zoom < 1) zoom = 1;
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
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);   // never the Windows generic (GDI) OpenGL 1.1
            g_win = SDL_CreateWindow("Trauma Team (native)", SDL_WINDOWPOS_CENTERED_DISPLAY(disp), SDL_WINDOWPOS_CENTERED_DISPLAY(disp),
                                     640 * zoom, 480 * zoom, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!g_win) fprintf(stderr, "[platform] window: %s\n", SDL_GetError());
            else if (!(g_glctx = SDL_GL_CreateContext(g_win))) fprintf(stderr, "[platform] OpenGL context: %s\n", SDL_GetError());
            else if (SDL_GL_MakeCurrent(g_win, g_glctx) != 0) fprintf(stderr, "[platform] make current: %s\n", SDL_GetError());
            else {
                typedef const unsigned char *(SDLCALL *GetStr)(unsigned);
                GetStr gs = (GetStr)SDL_GL_GetProcAddress("glGetString");
                if (gs) fprintf(stderr, "[platform] display %d (%s): OpenGL %s | %s | %s\n", disp, SDL_GetDisplayName(disp),
                                (const char *)gs(0x1F02), (const char *)gs(0x1F00), (const char *)gs(0x1F01));   // version, vendor, renderer
            }
            if (g_win && g_glctx && SDL_GL_GetCurrentContext() == g_glctx) {
                SDL_GL_SetSwapInterval(0);   // the game paces itself to the 60 Hz retrace
                settings_load();
                if (gpu_init(g_cfg.scale)) {
                    settings_apply();
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
    g_win = SDL_CreateWindow("Trauma Team (native, software)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
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
static float g_pad_px = 0.5f, g_pad_py = 0.5f;
static void pad_open(int index) {
    if (g_pad || !SDL_IsGameController(index)) return;
    g_pad = SDL_GameControllerOpen(index);
    if (g_pad) fprintf(stderr, "[input] controller: %s\n", SDL_GameControllerName(g_pad));
}
static float axis(SDL_GameControllerAxis a) {
    float v = g_pad ? SDL_GameControllerGetAxis(g_pad, a) / 32767.f : 0.f;
    const float dz = 0.18f;                                    // dead zone, rescaled to 0..1 beyond it
    if (v > dz) return (v - dz) / (1 - dz); if (v < -dz) return (v + dz) / (1 - dz); return 0;
}

void platform_set_title(const char *t) { if (g_win) SDL_SetWindowTitle(g_win, t); }
static int g_wheel_push, g_wheel_pull;
void platform_poll(void) {
    if (!g_window_open) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { fprintf(stderr, "[platform] window closed\n"); exit(0); }
        if (e.type == SDL_CONTROLLERDEVICEADDED) pad_open(e.cdevice.which);
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
            SDL_GameControllerClose(g_pad); g_pad = NULL; g_pointer_pad = 0;
            for (int i = 0; i < SDL_NumJoysticks(); i++) pad_open(i);
        }
        if (e.type == SDL_MOUSEMOTION) g_pointer_pad = 0;
        if (e.type == SDL_MOUSEWHEEL && e.wheel.y > 0) { g_wheel_push = 8; g_wheel_pull = 0; }   // each notch keeps the motion going ~0.13 s
        if (e.type == SDL_MOUSEWHEEL && e.wheel.y < 0) { g_wheel_pull = 8; g_wheel_push = 0; }
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
    g_input.up = KEY(UP);       g_input.down = KEY(DOWN);
    g_input.left = KEY(LEFT);   g_input.right = KEY(RIGHT);
    g_input.stick_up = KEY(W);  g_input.stick_down = KEY(S);
    g_input.stick_left = KEY(A); g_input.stick_right = KEY(D);
    g_input.a = KEY(SPACE) || KEY(RETURN) || KEY(Z);
    g_input.b = KEY(LSHIFT) || KEY(X) || KEY(BACKSPACE);
    g_input.plus = KEY(EQUALS) || KEY(KP_PLUS) || KEY(P);
    g_input.minus = KEY(MINUS) || KEY(KP_MINUS) || KEY(M);
    g_input.home = KEY(ESCAPE) || KEY(H);
    g_input.one = KEY(1);  g_input.two = KEY(2);
    g_input.c = KEY(C) || KEY(Q);  g_input.z = KEY(E) || KEY(F);   // Nunchuk buttons
    g_input.push = KEY(R) || KEY(TAB) || g_wheel_push > 0; if (g_wheel_push > 0) g_wheel_push--;
    g_input.pull = KEY(V) || g_wheel_pull > 0; if (g_wheel_pull > 0) g_wheel_pull--;
    g_input.swing = KEY(G) || g_input.mmb;
#undef KEY
    g_input.nx = g_input.ny = 0; g_input.has_analog = 0;
    if (g_pad) {
#define PB(b) SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_##b)
        g_input.a |= PB(A);                     g_input.b |= PB(B) || axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.3f;
        g_input.z |= axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.3f;   g_input.c |= PB(LEFTSHOULDER);
        g_input.up |= PB(DPAD_UP); g_input.down |= PB(DPAD_DOWN); g_input.left |= PB(DPAD_LEFT); g_input.right |= PB(DPAD_RIGHT);
        g_input.one |= PB(X); g_input.two |= PB(Y); g_input.push |= PB(RIGHTSTICK); g_input.swing |= PB(LEFTSTICK);
        g_input.plus |= PB(START); g_input.minus |= PB(BACK); g_input.home |= PB(GUIDE);
        // left stick = Nunchuk stick (y up)
        g_input.nx = axis(SDL_CONTROLLER_AXIS_LEFTX); g_input.ny = -axis(SDL_CONTROLLER_AXIS_LEFTY);
        g_input.has_analog = g_input.nx != 0 || g_input.ny != 0;
        // right stick = pointer velocity (full tilt crosses the screen in ~0.6 s; squared for precision)
        float rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
        if (rx || ry) {
            if (!g_pointer_pad) { g_pad_px = g_input.mouse_x; g_pad_py = g_input.mouse_y; g_pointer_pad = 1; }
            static const float speeds[6] = { 0, 0.7f, 1.1f, 1.6f, 2.3f, 3.2f };   // screen widths per second, by setting
            const float speed = speeds[g_cfg.pointer_speed >= 1 && g_cfg.pointer_speed <= 5 ? g_cfg.pointer_speed : 3] / 60.f;
            g_pad_px += rx * (rx < 0 ? -rx : rx) * speed; g_pad_py += ry * (ry < 0 ? -ry : ry) * speed;
        }
        if (PB(RIGHTSHOULDER)) { g_pad_px = 0.5f; g_pad_py = 0.5f; g_pointer_pad = 1; }
        if (g_pad_px < 0) g_pad_px = 0; if (g_pad_px > 1) g_pad_px = 1; if (g_pad_py < 0) g_pad_py = 0; if (g_pad_py > 1) g_pad_py = 1;
#undef PB
    }
    if (g_pointer_pad) { g_input.mouse_x = g_pad_px; g_input.mouse_y = g_pad_py; g_input.mouse_in = 1; }
}

#else   // headless build
int platform_init(void) { return 0; }
void platform_present(const u32 *px, int x0, int y0, int w, int h, int stride) { (void)px; (void)x0; (void)y0; (void)w; (void)h; (void)stride; }
void platform_poll(void) {}
void platform_set_title(const char *t) { (void)t; }
void platform_audio_stats(char *buf, int n) { if (n) buf[0] = 0; }
#endif
