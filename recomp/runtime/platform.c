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

Input g_input;
int g_window_open;

#ifdef TTPC_SDL
#include <SDL2/SDL.h>

#include "gx_state.h"
SDL_Window *g_sdl_window;
#define g_win g_sdl_window
static SDL_Renderer *g_ren;
static SDL_GLContext g_glctx;
extern int g_view_x, g_view_y, g_view_w, g_view_h;
static SDL_Texture *g_tex;
static int g_tw, g_th;

int platform_init(void) {
    if (getenv("TTPC_HEADLESS")) return 0;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[platform] no display (%s): running headless\n", SDL_GetError());
        return 0;
    }
    int zoom = getenv("TTPC_ZOOM") ? atoi(getenv("TTPC_ZOOM")) : 2;   // window size multiplier
    if (zoom < 1) zoom = 1;
    // OpenGL (GPU rendering) unless TTPC_SOFTWARE=1 or it is unavailable.
    if (!getenv("TTPC_SOFTWARE")) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        g_win = SDL_CreateWindow("Trauma Team (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 640 * zoom, 480 * zoom, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
        if (g_win && (g_glctx = SDL_GL_CreateContext(g_win))) {
            SDL_GL_SetSwapInterval(0);   // the game paces itself to the 60 Hz retrace
            int scale = getenv("TTPC_SCALE") ? atoi(getenv("TTPC_SCALE")) : 1;
            if (gpu_init(scale)) {
                SDL_ShowCursor(SDL_DISABLE);
                g_window_open = 1;
                return 1;
            }
            SDL_GL_DeleteContext(g_glctx); g_glctx = NULL;
        }
        fprintf(stderr, "[platform] OpenGL 3.3 unavailable (%s): using the software renderer\n", SDL_GetError());
        if (g_win) SDL_DestroyWindow(g_win);
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

void platform_poll(void) {
    if (!g_window_open) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { fprintf(stderr, "[platform] window closed\n"); exit(0); }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
            u32 fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
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
#undef KEY
}

#else   // headless build
int platform_init(void) { return 0; }
void platform_present(const u32 *px, int x0, int y0, int w, int h, int stride) { (void)px; (void)x0; (void)y0; (void)w; (void)h; (void)stride; }
void platform_poll(void) {}
#endif
