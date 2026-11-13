/* VR mode (first test): the game plays on Naomi's office computer.
 *
 * With --vr (or TTPC_VR=1) the port opens an OpenXR session (SteamVR, Valve Index) next to its normal window.
 * Each game frame it draws a 3D room (a glTF file, default vr/naomi_office.glb next to the exe) into both eyes
 * and shows the game's picture on the mesh whose material is called "screen". The right controller's laser is
 * the Wii Remote pointer when it hits that screen: trigger = A, grip or B = B. Left stick walks, right stick
 * turns in 30 degree steps, right stick click puts you back in the chair. Left A / B = - / +.
 * The desktop window keeps showing the normal flat picture.
 *
 * --vr-desktop (TTPC_VR_PREVIEW=1, no headset): the same room in the desktop window, played like a first-person game:
 * mouse looks (F2 frees the mouse), WASD walks, hold Ctrl to sit, the crosshair aims the laser, left mouse = trigger (A),
 * right mouse = B; the game's other keys work as usual.
 *
 * The room is lit already (light baked into vertex colours), so everything is drawn unlit:
 * colour = base colour x texture x vertex colour (+ emissive), in linear space into sRGB targets. */
#include "platform.h"
#include "cpu.h"
#include "gx_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef TTPC_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf.h"
#include "third_party/stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#define TTPC_OPENXR 1
#endif

#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif

// ---------------------------------------------------------------- GL functions (own copies; gl.c keeps its own)
#define VGL \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) X(void, Clear, (GLbitfield)) X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) X(GLboolean, IsEnabled, (GLenum)) X(void, DepthFunc, (GLenum)) X(void, DepthMask, (GLboolean)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) X(void, BlendFunc, (GLenum, GLenum)) X(void, GetIntegerv, (GLenum, GLint *)) \
    X(void, GenTextures, (GLsizei, GLuint *)) X(void, BindTexture, (GLenum, GLuint)) X(void, ActiveTexture, (GLenum)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, GenerateMipmap, (GLenum)) X(void, PixelStorei, (GLenum, GLint)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) X(void, BindFramebuffer, (GLenum, GLuint)) X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint *)) X(void, BindRenderbuffer, (GLenum, GLuint)) X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) X(void, UseProgram, (GLuint)) X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform1f, (GLint, GLfloat)) X(void, Uniform2f, (GLint, GLfloat, GLfloat)) X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, Uniform3fv, (GLint, GLsizei, const GLfloat *)) X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) X(void, BindVertexArray, (GLuint)) X(void, GenBuffers, (GLsizei, GLuint *)) X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) X(GLenum, GetError, (void)) X(void, DrawElements, (GLenum, GLsizei, GLenum, const void *)) X(void, DrawArrays, (GLenum, GLint, GLsizei))
#define X(ret, name, args) static ret (APIENTRY *v##name) args;
VGL
#undef X
static int vgl_load(void) {
    int ok = 1;
#define X(ret, name, args) v##name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); if (!v##name) { fprintf(stderr, "[vr] missing gl%s\n", #name); ok = 0; }
    VGL
#undef X
    return ok;
}

// ---------------------------------------------------------------- small maths (column-major 4x4, like GL)
typedef struct { float m[16]; } M4;
static M4 m4_id(void) { M4 r = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}}; return r; }
static M4 m4_mul(M4 a, M4 b) { M4 r; for (int c = 0; c < 4; c++) for (int i = 0; i < 4; i++) { float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + i] * b.m[c * 4 + k]; r.m[c * 4 + i] = s; } return r; }
static M4 m4_pose(const float q[4], const float p[3]) {   // quaternion (x,y,z,w) + position
    float x = q[0], y = q[1], z = q[2], w = q[3]; M4 r = m4_id();
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + z * w); r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w); r.m[5] = 1 - 2 * (x * x + z * z); r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w); r.m[9] = 2 * (y * z - x * w); r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = p[0]; r.m[13] = p[1]; r.m[14] = p[2]; return r;
}
static M4 m4_rigid_inv(M4 a) {   // inverse of rotation + translation
    M4 r = m4_id();
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[j * 4 + i] = a.m[i * 4 + j];
    for (int i = 0; i < 3; i++) r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    return r;
}
static M4 m4_yaw(float a, float x, float y, float z) { M4 r = m4_id(); r.m[0] = cosf(a); r.m[2] = -sinf(a); r.m[8] = sinf(a); r.m[10] = cosf(a); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static M4 m4_frustum(float l, float r_, float d, float u, float n, float f) {   // tangents of the four half-angles
    M4 r = {{0}}; float w = r_ - l, h = u - d;
    r.m[0] = 2 / w; r.m[5] = 2 / h; r.m[8] = (r_ + l) / w; r.m[9] = (u + d) / h;
    r.m[10] = -(f + n) / (f - n); r.m[11] = -1; r.m[14] = -2 * f * n / (f - n); return r;
}
static void m4_point(M4 a, const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a.m[i] * p[0] + a.m[4 + i] * p[1] + a.m[8 + i] * p[2] + a.m[12 + i]; }
static void m4_dir(M4 a, const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a.m[i] * p[0] + a.m[4 + i] * p[1] + a.m[8 + i] * p[2]; }

// ---------------------------------------------------------------- the room (glTF)
typedef struct { float base[4], emis[3]; GLuint tex, etex; int blend, mask; float cutoff; int screen, name_paper; } Mat;
typedef struct { GLuint vao; int n; int mat; int grp; int room; } Prim;   // grp: 1 door, 3 mouse (moved at draw time); room: 0 office, 1 morgue
static int g_room;                         // which room is shown
static int g_exam_now, g_morgue_now, g_click_t, g_pending_bed = -1, g_exit_t; static float g_click_px[2]; static float g_exam_fade;
typedef struct Ray Ray;
static int mdl_hit(int k, const Ray *r, float *sx, float *sy, float *dist); static int bed_hit(int k, const Ray *r, float *dist);
static void morgue_marker(int which, float *x, float *y); static void click_at(float sx, float sy, int frames);
static void mdl_draw(int k); static void mdl_place(int k); static void scn_draw(void); static void creature_draw(void); static void creature_update(void);
static int keyed_scene(void);
static float g_spot[2][3]; static int g_spot_ok[2];   // morgue: where the body [0] and the personal effects [1] lie (bed tops)
static Mat *g_mat; static int g_nmat;
static Prim *g_prim; static int g_nprim;
static GLuint g_white, g_prog, g_lineprog, g_linevao, g_linevbo;
static GLint u_lit, u_mvp, u_base, u_emis, u_tex, u_etex, u_cut, u_screen, u_scr_k, u_crop, u_line_mvp, u_line_col;
static float g_scr_o[3], g_scr_u[3], g_scr_v[3];   // screen: top-left corner, right edge, down edge (world)
static int g_have_screen;
// What shows where. The monitor always shows Little Guy's desktop: the last frame in which the game drew the
// desktop wallpaper (a 648x440 texture) is kept and shown there. Everything else the game draws (the office
// scene, dialogue, the morgue...) goes on a panel on the wall above the desk, for now.
int g_vr_desk_seen;                       // set by video.c when the desktop wallpaper is drawn this frame
static int g_desk_now, g_desk_have, g_desk_hold;
static GLuint g_desk_tex, g_desk_fbo; static int g_desk_w, g_desk_h;
static float g_x0f = 0, g_x1f = 1;         // the game's own 640 columns within the picture (ultrawide sides excluded)
#define DESK_Y0 (22.f / 480) 
#define DESK_Y1 (462.f / 480)
// The pane: a sheet of glass that floats in front of you and shows what the game puts on screen that has no
// place in the room yet. In the office it shows only the game's interface (speech bubbles, the LIMIT
// hearts, help pages...), with the painted office removed; elsewhere (map, morgue...) the whole picture.
static float g_pan_o[3], g_pan_u[3], g_pan_v[3]; static int g_pan_placed;
static GLuint g_pan_vao, g_pan_vbo;
static float desk_aspect(void); static int panel_on(void);
// scenes, from what the game draws each frame
int g_vr_office_seen;                     // the painted office (a red 640x480 picture) was drawn (and skipped)
static int g_office_now, g_office_hold;
int g_vr_pic_y0 = 24, g_vr_pic_h = 480;  // the presented picture's rows in the game's 528-row frame (gl.c)
// office hotspots, in the painted office's own pixels (advmap/aut/aut_room1_stage01.h2d polygons)
// (the centres of the markers the game puts on them, in its 640x528 frame)
static const float HS_PC[2] = { 166, 173 }, HS_RECORDER[2] = { 111, 320 }, HS_DOOR[2] = { 576, 152 };
#define OFFICE_BG_Y 0
static float g_obj_min[7][3], g_obj_max[7][3]; static int g_obj_ok[7];   // 0 recorder, 1 door, 2 monitor body, 3 mouse, 4 the secret book, 5/6 the sliding panels
static float g_dish[7][3]; static int g_dish_ok[7];   // the hidden compartment's seven culture dishes (left to right)
static float g_niche_t; static int g_niche_open;  // the hidden compartment: 0 closed .. 1 open
static float g_dish_t[7]; static int g_dish_tog[7];   // per dish: seconds of agitation left; the pair's fused/apart toggle
static void aq_poke(int d); static int g_aim_dish;
// the door: swings open (into the alcove) when you leave through it; beyond it, a black void that shows the
// game's other screens (the map...) on a pane in the doorway until those places are built
static float g_door_t; static int g_door_req, g_last_kind;   // g_last_kind: what the laser was on (4 = door)
static const float HINGE[3] = { -0.8f, 0, -7.25f };
// the desk mouse: hold it (grip near it, or E on the keyboard) and it moves the computer's pointer
static float g_mouse_off[2]; static int g_mouse_held = -1; static float g_mouse_grab[3], g_mouse_grab_off[2];
static GLint u_model, u_hole;
// the LIMIT hearts: their spot in the game's picture (pixels) and their plaque on the wall
static unsigned g_lay_tex;   // gl.c's HUD layer this frame (the LIMIT hearts, keyed like the frame)
static float HEARTS_PX[4] = { 44, 68, 256, 116 };   // measured each frame from the HUD draws in the top-left corner
static float g_hr[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
static GLuint g_saver_tex;
int g_vr_case;                       // disc.c: the forensics case being loaded (110, 120...)
static int g_case_shown = -1; static GLuint g_case_tex;
// stepping away from the desk while the computer is on presses its EXIT button for you
static int g_auto_exit; static const float EXIT_BTN[2] = { 0.823f, 0.875f };
int g_vr_close_comp;   // wiimote.c runs the game's close-computer routine on the next controller read   // vr/screensaver.png: the monitor when the computer is off
static M4 g_head; static int g_head_ok;
// where the game's interface is on screen (office): rectangles of the drawn pieces, this frame and last
typedef struct { float x0, y0, x1, y1; } Rect;
static Rect g_ui[2][96]; static int g_nui[2], g_uicur;
static int g_active, g_preview;
static M4 g_world_from_stage;   // where the play area sits in the room
static float g_seat[3] = { -1.55f, 0.0f, -1.62f };   // Naomi's chair (glTF frame), facing -x (the desk)
static float g_turn = (float)(M_PI / 2);   // play area yaw (stage -z -> room -x)
static int g_arrive;   // a room change: put the player at that room's arrival spot

static GLuint upload_image(const cgltf_image *im, int srgb) {
    if (!im || !im->buffer_view) return 0;
    const u8 *p = (const u8 *)im->buffer_view->buffer->data + im->buffer_view->offset;
    int w, h, c; u8 *px = stbi_load_from_memory(p, (int)im->buffer_view->size, &w, &h, &c, 4);
    if (!px) { fprintf(stderr, "[vr] image %s: %s\n", im->name ? im->name : "?", stbi_failure_reason()); return 0; }
    GLuint t; vGenTextures(1, &t); vBindTexture(GL_TEXTURE_2D, t);
    vPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    vTexImage2D(GL_TEXTURE_2D, 0, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    vGenerateMipmap(GL_TEXTURE_2D);
    vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    vTexParameteri(GL_TEXTURE_2D, 0x84FE /* MAX_ANISOTROPY */, 8);
    free(px); return t;
}

static int load_room(const char *path, int room) {
    cgltf_options opt = {0}; cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success || cgltf_load_buffers(&opt, d, path) != cgltf_result_success) {
        fprintf(stderr, "[vr] can't load %s\n", path); if (d) cgltf_free(d); return 0;
    }
    GLuint *img_lin = calloc(d->images_count + 1, sizeof *img_lin);
    int mbase = g_nmat; g_nmat += (int)d->materials_count; g_mat = realloc(g_mat, (g_nmat + 1) * sizeof *g_mat);
    memset(g_mat + mbase, 0, (g_nmat + 1 - mbase) * sizeof *g_mat);
    for (int i = mbase; i < g_nmat; i++) {
        cgltf_material *m = &d->materials[i - mbase]; Mat *o = &g_mat[i];
        memcpy(o->base, m->pbr_metallic_roughness.base_color_factor, 16); memcpy(o->emis, m->emissive_factor, 12);
        if (m->has_emissive_strength) for (int k = 0; k < 3; k++) o->emis[k] *= m->emissive_strength.emissive_strength;
        const cgltf_texture *bt = m->pbr_metallic_roughness.base_color_texture.texture, *et = m->emissive_texture.texture;
        o->tex = bt ? upload_image(bt->image, 1) : 0; o->etex = et ? upload_image(et->image, 1) : 0;
        o->blend = m->alpha_mode == cgltf_alpha_mode_blend; o->mask = m->alpha_mode == cgltf_alpha_mode_mask; o->cutoff = m->alpha_cutoff;
        if (o->blend) { o->mask = 1; o->cutoff = 0.5f; o->blend = 0; }   // leaves: cut-out reads better than sorting in VR
        o->screen = m->name && !strcmp(m->name, "screen"); o->name_paper = m->name && !strcmp(m->name, "paper");
    }
    static int cap;   // shared by every room loaded
    for (cgltf_size ni = 0; ni < d->nodes_count; ni++) {
        cgltf_node *nd = &d->nodes[ni];
        float wm[16]; cgltf_node_transform_world(nd, wm); M4 W; memcpy(W.m, wm, 64);
        if (!nd->mesh) {   // the morgue's bed markers: where the body and the personal effects are laid
            int k = !nd->name ? -1 : !strcmp(nd->name, "bed_body_spot") ? 0 : !strcmp(nd->name, "bed_effects_spot") ? 1 : -1;
            if (k >= 0) { memcpy(g_spot[k], W.m + 12, 12); g_spot_ok[k] = 1; }
            if (room == 0 && nd->name && !strncmp(nd->name, "cdish_", 6)) { int d = atoi(nd->name + 6); if (d >= 0 && d < 7) { memcpy(g_dish[d], W.m + 12, 12); g_dish_ok[d] = 1; } }
            continue;
        }
        int obj = room != 0 || !nd->name ? -1 : !strncmp(nd->name, "recorder", 8) ? 0 : !strncmp(nd->name, "door_exit", 9) ? 1 : !strncmp(nd->name, "monitor", 7) ? 2 : !strncmp(nd->name, "mouse", 5) ? 3 : !strncmp(nd->name, "secret_book", 11) ? 4 : !strncmp(nd->name, "secret_panel_r", 14) ? 6 : !strncmp(nd->name, "secret_panel", 12) ? 5 : -1;
        for (cgltf_size pi = 0; pi < nd->mesh->primitives_count; pi++) {
            cgltf_primitive *pr = &nd->mesh->primitives[pi];
            const cgltf_accessor *ap = NULL, *at = NULL, *ac = NULL;
            for (cgltf_size a = 0; a < pr->attributes_count; a++) {
                cgltf_attribute *A = &pr->attributes[a];
                if (A->type == cgltf_attribute_type_position) ap = A->data;
                if (A->type == cgltf_attribute_type_texcoord && A->index == 0) at = A->data;
                if (A->type == cgltf_attribute_type_color && A->index == 0) ac = A->data;
            }
            if (!ap) continue;
            int nv = (int)ap->count; float *v = malloc(sizeof(float) * 9 * nv);
            for (int k = 0; k < nv; k++) {
                float p[3], q[4] = { 1, 1, 1, 1 }, t[2] = { 0, 0 };
                cgltf_accessor_read_float(ap, k, p, 3); m4_point(W, p, v + k * 9);
                if (at) cgltf_accessor_read_float(at, k, t, 2);
                if (ac) cgltf_accessor_read_float(ac, k, q, ac->type == cgltf_type_vec3 ? 3 : 4);
                v[k * 9 + 3] = t[0]; v[k * 9 + 4] = t[1]; memcpy(v + k * 9 + 5, q, 16);
                if (obj >= 0) for (int c = 0; c < 3; c++) {
                    float x = v[k * 9 + c];
                    if (!g_obj_ok[obj] || x < g_obj_min[obj][c]) g_obj_min[obj][c] = x;
                    if (!g_obj_ok[obj] || x > g_obj_max[obj][c]) g_obj_max[obj][c] = x;
                    if (c == 2) g_obj_ok[obj] = 1;
                }
            }
            int ni_ = pr->indices ? (int)pr->indices->count : nv; u32 *ix = malloc(4 * ni_);
            for (int k = 0; k < ni_; k++) ix[k] = pr->indices ? (u32)cgltf_accessor_read_index(pr->indices, k) : (u32)k;
            int mi = pr->material ? mbase + (int)(pr->material - d->materials) : -1;
            if (mi >= 0 && g_mat[mi].screen && !g_have_screen) {   // corners from the UVs: (0,0) top-left, u right, v down
                float best[3] = { 1e9f, 1e9f, 1e9f }; float *P00 = NULL, *P10 = NULL, *P01 = NULL;
                for (int k = 0; k < nv; k++) {
                    float u = v[k * 9 + 3], w = v[k * 9 + 4];
                    float d0 = u * u + w * w, d1 = (u - 1) * (u - 1) + w * w, d2 = u * u + (w - 1) * (w - 1);
                    if (d0 < best[0]) { best[0] = d0; P00 = v + k * 9; } if (d1 < best[1]) { best[1] = d1; P10 = v + k * 9; } if (d2 < best[2]) { best[2] = d2; P01 = v + k * 9; }
                }
                for (int c = 0; c < 3; c++) { g_scr_o[c] = P00[c]; g_scr_u[c] = P10[c] - P00[c]; g_scr_v[c] = P01[c] - P00[c]; }
                g_have_screen = 1;
                fprintf(stderr, "[vr] screen at %.2f %.2f %.2f, %.2f x %.2f m\n", g_scr_o[0], g_scr_o[1], g_scr_o[2],
                        sqrtf(g_scr_u[0] * g_scr_u[0] + g_scr_u[1] * g_scr_u[1] + g_scr_u[2] * g_scr_u[2]), sqrtf(g_scr_v[0] * g_scr_v[0] + g_scr_v[1] * g_scr_v[1] + g_scr_v[2] * g_scr_v[2]));
            }
            if (g_nprim == cap) { cap = cap ? cap * 2 : 256; g_prim = realloc(g_prim, cap * sizeof *g_prim); }
            Prim *o = &g_prim[g_nprim++]; o->n = ni_; o->mat = mi; o->grp = obj == 1 || obj == 3 ? obj : obj == 4 ? 6 : obj == 5 ? 5 : obj == 6 ? 7 : 0; o->room = room;
            GLuint vb, ib; vGenVertexArrays(1, &o->vao); vBindVertexArray(o->vao);
            vGenBuffers(1, &vb); vBindBuffer(GL_ARRAY_BUFFER, vb); vBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * nv, v, GL_STATIC_DRAW);
            vGenBuffers(1, &ib); vBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib); vBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * ni_, ix, GL_STATIC_DRAW);
            vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
            vEnableVertexAttribArray(1); vVertexAttribPointer(1, 2, GL_FLOAT, 0, 36, (void *)12);
            vEnableVertexAttribArray(2); vVertexAttribPointer(2, 4, GL_FLOAT, 0, 36, (void *)20);
            vBindVertexArray(0); free(v); free(ix);
        }
    }
    free(img_lin); cgltf_free(d);
    fprintf(stderr, "[vr] room %s: %d materials, %d draws%s\n", path, g_nmat, g_nprim, room == 0 && !g_have_screen ? " (no 'screen' material!)" : "");
    return g_nprim > 0;
}

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aP; layout(location=1) in vec2 aT; layout(location=2) in vec4 aC;\n"
    "uniform mat4 uMVP; uniform mat4 uModel; out vec2 vT; out vec4 vC;\n"
    "void main() { gl_Position = uMVP * uModel * vec4(aP, 1.0); vT = aT; vC = aC; }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 vT; in vec4 vC; uniform vec4 uBase; uniform vec3 uEmis; uniform sampler2D uTex, uETex; uniform vec3 uLit; uniform float uCut;\n"
    "uniform int uScreen; uniform vec2 uScrK; uniform vec4 uCrop; uniform vec4 uHole; out vec4 o;\n"
    "void main() {\n"
    "  if (uScreen == 2) {   // the game's interface over a magenta key (the office painting was left out)\n"
    "    if (vT.x > uHole.x && vT.x < uHole.z && vT.y > uHole.y && vT.y < uHole.w) discard;   // (the hearts live on the wall)\n"
    "    vec2 s = vec2(mix(uCrop.x, uCrop.z, vT.x), 1.0 - mix(uCrop.y, uCrop.w, vT.y));\n"
    "    vec3 c = texture(uTex, s).rgb;\n"
    "    float a = 1.0 - clamp((min(c.r, c.b) - c.g) * 1.1, 0.0, 1.0);   // how much of it is NOT the key\n"
    "    if (a < 0.04) discard;\n"
    "    c = clamp((c - (1.0 - a) * vec3(1.0, 0.0, 1.0)) / a, 0.0, 1.0);   // take the key colour back out of the edges\n"
    "    o = vec4(pow(c, vec3(2.2)), a); return; }\n"
    "  if (uScreen == 1) {   // the game picture, letterboxed to keep its shape; it holds sRGB values -> linear\n"
    "    vec2 t = (vT - 0.5) * uScrK + 0.5;\n"
    "    if (t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    vec2 s = vec2(mix(uCrop.x, uCrop.z, t.x), 1.0 - mix(uCrop.y, uCrop.w, t.y));\n"
    "    vec3 c = texture(uTex, s).rgb; o = vec4(pow(c, vec3(2.2)), 1.0); return; }\n"
    "  if (uScreen == 3) { vec4 t = texture(uTex, vT) * vC * uBase; o = t; return; }   // the examination tunnel: glowing, see-through\n"
    "  if (uScreen == 4) { vec4 t = texture(uTex, vT); o = vec4(mix(vC.rgb * 2.0, t.rgb, t.a * 0.9), 1.0); return; }   // the body: its marks over the glow\n"
    "  vec4 b = uBase * texture(uTex, vT); if (b.a < uCut) discard;\n"
    "  vec3 c = b.rgb * (uLit.x > 0.0 ? uLit : vC.rgb * 2.0) + uEmis * texture(uETex, vT).rgb;   // baked light was stored at half range\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";
static const char *LVS = "#version 330 core\nlayout(location=0) in vec3 aP; uniform mat4 uMVP; void main() { gl_Position = uMVP * vec4(aP, 1.0); }\n";
static const char *LFS = "#version 330 core\nuniform vec4 uCol; out vec4 o; void main() { o = uCol; }\n";

static GLuint build(const char *vs, const char *fs) {
    GLuint p = vCreateProgram(); const char *src[2] = { vs, fs }; GLenum ty[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    for (int i = 0; i < 2; i++) {
        GLuint s = vCreateShader(ty[i]); vShaderSource(s, 1, &src[i], NULL); vCompileShader(s);
        GLint ok; vGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; vGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[vr] shader: %s\n", log); }
        vAttachShader(p, s);
    }
    vLinkProgram(p); return p;
}

// ---------------------------------------------------------------- drawing
typedef struct Ray { float o[3], d[3]; int valid; } Ray;
static Ray g_ray;                 // the pointing hand's laser (world)
static float g_ray_len = 2.0f;
static int g_ray_on_screen;
static M4 g_hand[2]; static int g_hand_ok[2];

static void draw_box(M4 vp, M4 at, float sx, float sy, float sz, const float col[4]) {   // a small solid box (controllers)
    static const float c[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
    static const int f[36] = { 0,1,2, 0,2,3, 4,6,5, 4,7,6, 0,4,5, 0,5,1, 3,2,6, 3,6,7, 0,3,7, 0,7,4, 1,5,6, 1,6,2 };
    float v[36 * 3];
    for (int i = 0; i < 36; i++) { float p[3] = { c[f[i]][0] * sx, c[f[i]][1] * sy, c[f[i]][2] * sz }; m4_point(at, p, v + i * 3); }
    M4 mvp = vp; vUniformMatrix4fv(u_line_mvp, 1, 0, mvp.m); vUniform4fv(u_line_col, 1, col);
    vBindBuffer(GL_ARRAY_BUFFER, g_linevbo); vBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW); vDrawArrays(GL_TRIANGLES, 0, 36);
}

char g_vr_shot[256];   // video.c (F9): save what VR shows to this file
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);
static void vr_shot_save(int w, int h) {   // the framebuffer just drawn -> PNG (bottom-up rows flipped)
    if (!g_vr_shot[0] || w <= 0 || h <= 0) return;
    u8 *px = malloc((size_t)w * h * 4); if (!px) return;
    vReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h / 2; y++) for (int x = 0; x < w * 4; x++) { u8 t = px[y * w * 4 + x]; px[y * w * 4 + x] = px[(h - 1 - y) * w * 4 + x]; px[(h - 1 - y) * w * 4 + x] = t; }
    for (int i = 0; i < w * h; i++) px[i * 4 + 3] = 255;
    if (stbi_write_png(g_vr_shot, w, h, 4, px, w * 4)) fprintf(stderr, "[vr] saved the headset view: %s\n", g_vr_shot);
    free(px); g_vr_shot[0] = 0;
}
static void draw_scene(M4 proj, M4 view, GLuint game_tex, int gw, int gh) {
    M4 vp = m4_mul(proj, view);
    vUseProgram(g_prog); vUniformMatrix4fv(u_mvp, 1, 0, vp.m);
    vUniform1i(u_tex, 0); vUniform1i(u_etex, 1);
    // keep the game's shape on the screen: shrink the longer side of the screen's UV range
    float sw = sqrtf(g_scr_u[0] * g_scr_u[0] + g_scr_u[1] * g_scr_u[1] + g_scr_u[2] * g_scr_u[2]);
    float sh = sqrtf(g_scr_v[0] * g_scr_v[0] + g_scr_v[1] * g_scr_v[1] + g_scr_v[2] * g_scr_v[2]);
    float qa = sh > 0 ? sw / sh : 1, ga = desk_aspect();
    (void)qa; (void)ga;
    vUniform2f(u_scr_k, 1, 1); vUniform4f(u_crop, g_x0f, DESK_Y0, g_x1f, DESK_Y1);   // the desktop fills the whole monitor
    M4 ident = m4_id(), door_m, mouse_m = m4_id();
    {   float a = -1.65f * g_door_t; M4 r = m4_yaw(a, 0, 0, 0), t = m4_yaw(0, HINGE[0], 0, HINGE[2]), ti = m4_yaw(0, -HINGE[0], 0, -HINGE[2]);
        door_m = m4_mul(t, m4_mul(r, ti)); }
    mouse_m.m[12] = g_mouse_off[0]; mouse_m.m[14] = g_mouse_off[1];
    M4 panel_m = m4_id(), panel_r_m = m4_id(), book_m = m4_id();
    {   // the compartment: the wall panel steps back into the wall, then slides away along it; the book tips out
        float t = g_niche_t, in_ = t < 0.25f ? t / 0.25f : 1.f, sl = t > 0.25f ? (t - 0.25f) / 0.75f : 0.f;
        sl = sl * sl * (3 - 2 * sl);
        panel_m.m[12] = panel_r_m.m[12] = -0.025f * in_; panel_m.m[14] = -0.56f * sl; panel_r_m.m[14] = 0.56f * sl;   // (glTF z runs opposite to the room's)   // the two halves part
        if (g_obj_ok[4]) {
            float a = 0.42f * (t < 0.3f ? t / 0.3f : 1.f), c = cosf(a), sn = sinf(a);
            float py = g_obj_min[4][1], pz = g_obj_max[4][2];   // pivot: the book's bottom front edge
            book_m.m[5] = c; book_m.m[6] = sn; book_m.m[9] = -sn; book_m.m[10] = c;
            book_m.m[13] = py - (c * py - sn * pz); book_m.m[14] = pz - (sn * py + c * pz);
        }
    }
    int cur_grp = -1;
    for (int i = 0; i < g_nprim; i++) {
        Prim *p = &g_prim[i]; if (p->room != g_room) continue;
        Mat *m = p->mat >= 0 && p->mat < g_nmat ? &g_mat[p->mat] : NULL;
        if (p->grp != cur_grp) { cur_grp = p->grp; vUniformMatrix4fv(u_model, 1, 0, (p->grp == 1 ? door_m : p->grp == 3 ? mouse_m : p->grp == 5 ? panel_m : p->grp == 7 ? panel_r_m : p->grp == 6 ? book_m : ident).m); }
        static const float one[4] = { 1, 1, 1, 1 }, zero[3] = { 0, 0, 0 };
        vActiveTexture(GL_TEXTURE1); vBindTexture(GL_TEXTURE_2D, m && m->etex ? m->etex : g_white);
        vActiveTexture(GL_TEXTURE0);
        if (m && m->screen && !g_desk_now && g_saver_tex) {   // computer off: Naomi's screensaver (the CIFM seal)
            static const float blk[4] = { 0, 0, 0, 1 }, glow[3] = { 0.8f, 0.8f, 0.8f };
            vActiveTexture(GL_TEXTURE1); vBindTexture(GL_TEXTURE_2D, g_saver_tex); vActiveTexture(GL_TEXTURE0); vBindTexture(GL_TEXTURE_2D, g_white);
            vUniform1i(u_screen, 0); vUniform4fv(u_base, 1, blk); vUniform3fv(u_emis, 1, glow); vUniform1f(u_cut, -1.f);
            vBindVertexArray(p->vao); vDrawElements(GL_TRIANGLES, p->n, GL_UNSIGNED_INT, 0); continue;
        }
        if (m && m->screen) { vBindTexture(GL_TEXTURE_2D, g_desk_now ? game_tex : g_desk_have ? g_desk_tex : g_white); vUniform1i(u_screen, g_desk_have || g_desk_now ? 1 : 0);
                              if (!g_desk_have && !g_desk_now) { static const float off[4] = { 0.01f, 0.012f, 0.015f, 1 }; vUniform4fv(u_base, 1, off); vUniform3fv(u_emis, 1, zero); vUniform1f(u_cut, -1.f); vBindVertexArray(p->vao); vDrawElements(GL_TRIANGLES, p->n, GL_UNSIGNED_INT, 0); continue; } }
        else { vBindTexture(GL_TEXTURE_2D, m && m->tex ? m->tex : g_white); vUniform1i(u_screen, 0); }
        vUniform4fv(u_base, 1, m ? m->base : one); vUniform3fv(u_emis, 1, m ? m->emis : zero);
        vUniform1f(u_cut, m && m->mask ? m->cutoff : -1.f);
        { static const float lamp[3] = { 1.08f, 1.02f, 0.92f }, no[3] = { 0, 0, 0 }; vUniform3fv(u_lit, 1, m && m->name_paper ? lamp : no); }   // the report sits in the desk lamp's light
        vBindVertexArray(p->vao); vDrawElements(GL_TRIANGLES, p->n, GL_UNSIGNED_INT, 0);
    }
    vUniformMatrix4fv(u_model, 1, 0, ident.m); vUniform4f(u_hole, 2, 2, 2, 2);
    { static const float no[3] = { 0, 0, 0 }; vUniform3fv(u_lit, 1, no); }
    if (g_room == 0) creature_draw();   // the little swimmer in the hidden compartment
    if (g_room == 1 && g_exam_fade > 0.01f && g_head_ok) {   // examining: the room fades into a dark void
        float col[4] = { 0.0f, 0.01f, 0.015f, g_exam_fade };
        vUseProgram(g_lineprog); vBindVertexArray(g_linevao); vEnableVertexAttribArray(0);
        vBindBuffer(GL_ARRAY_BUFFER, g_linevbo); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (void *)0);
        vEnable(GL_BLEND); vBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); vDisable(GL_DEPTH_TEST); vDisable(GL_CULL_FACE);
        M4 at = m4_id(); at.m[12] = g_head.m[12]; at.m[13] = g_head.m[13]; at.m[14] = g_head.m[14];
        draw_box(vp, at, 0.3f, 0.3f, 0.3f, col);
        vDisable(GL_BLEND); vEnable(GL_DEPTH_TEST); vClear(GL_DEPTH_BUFFER_BIT);
        vUseProgram(g_prog); vUniformMatrix4fv(u_mvp, 1, 0, vp.m);
        scn_draw();   // the game's own examination background, all around
    }
    if (g_room == 1) {
        if (g_exam_fade > 0.5f) mdl_draw(g_exam_now ? g_exam_now - 1 : 0);   // the model you're examining, in front of you
        else { mdl_draw(0); mdl_draw(1); }                                     // the body and the personal effects on their beds
    }
    if (g_door_t > 0.02f) {   // the void beyond the open door
        static const float black[4] = { 0, 0, 0, 1 }, zero3[3] = { 0, 0, 0 };
        float q[4][9] = { { -0.8f, 2.2f, -7.272f, 0, 0, 1, 1, 1, 1 }, { 0.2f, 2.2f, -7.272f, 1, 0, 1, 1, 1, 1 },
                          { 0.2f, 0.0f, -7.272f, 1, 1, 1, 1, 1, 1 }, { -0.8f, 0.0f, -7.272f, 0, 1, 1, 1, 1, 1 } };
        vBindBuffer(GL_ARRAY_BUFFER, g_pan_vbo); vBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        vActiveTexture(GL_TEXTURE0); vBindTexture(GL_TEXTURE_2D, g_white); vUniform1i(u_screen, 0);
        vUniform4fv(u_base, 1, black); vUniform3fv(u_emis, 1, zero3); vUniform1f(u_cut, -1.f);
        vBindVertexArray(g_pan_vao); vDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    int wiimote_script_active(void);
    if (panel_on() && game_tex && g_pan_placed && !wiimote_script_active()) {   // the pane (hidden while --mission drives the developer menu)
        static const float one[4] = { 1, 1, 1, 1 }, zero[3] = { 0, 0, 0 };
        float q[4][9]; for (int i = 0; i < 4; i++) {
            float su = (i == 1 || i == 2), sv = (i >= 2);
            for (int c = 0; c < 3; c++) q[i][c] = g_pan_o[c] + g_pan_u[c] * su + g_pan_v[c] * sv;
            q[i][3] = su; q[i][4] = sv; q[i][5] = q[i][6] = q[i][7] = q[i][8] = 1; }
        vBindBuffer(GL_ARRAY_BUFFER, g_pan_vbo); vBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        vActiveTexture(GL_TEXTURE0); vBindTexture(GL_TEXTURE_2D, game_tex); vUniform1i(u_screen, keyed_scene() ? 2 : 1);
        vUniform4f(u_hole, 2, 2, 2, 2);
        if (keyed_scene()) { vEnable(GL_BLEND); vBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); vDepthMask(0); }
        vUniform2f(u_scr_k, 1, 1); vUniform4f(u_crop, g_x0f, 0, g_x1f, 1); vUniform4fv(u_base, 1, one); vUniform3fv(u_emis, 1, zero);
        vBindVertexArray(g_pan_vao); vDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        vUniform4f(u_hole, 2, 2, 2, 2);
        if (g_office_now && g_lay_tex) {   // the hearts plaque on the wall, left of the desk (from the HUD layer)
            vBindTexture(GL_TEXTURE_2D, g_lay_tex); vUniform1i(u_screen, 2);
            const float w = 0.75f, h = w * (HEARTS_PX[3] - HEARTS_PX[1]) / (HEARTS_PX[2] - HEARTS_PX[0]);
            float q[4][9] = { { -2.97f, 2.1f, -0.95f, 0, 0, 1, 1, 1, 1 }, { -2.97f, 2.1f, -0.95f - w, 1, 0, 1, 1, 1, 1 },
                              { -2.97f, 2.1f - h, -0.95f - w, 1, 1, 1, 1, 1, 1 }, { -2.97f, 2.1f - h, -0.95f, 0, 1, 1, 1, 1, 1 } };
            float hy = (float)g_vr_pic_h, cx0 = g_x0f + (g_x1f - g_x0f) * HEARTS_PX[0] / 640.f, cx1 = g_x0f + (g_x1f - g_x0f) * HEARTS_PX[2] / 640.f;
            vUniform4f(u_crop, cx0, (HEARTS_PX[1] - g_vr_pic_y0) / hy, cx1, (HEARTS_PX[3] - g_vr_pic_y0) / hy);
            vBindBuffer(GL_ARRAY_BUFFER, g_pan_vbo); vBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
            vDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        }
        vDisable(GL_BLEND); vDepthMask(1);
    }
    vBindVertexArray(g_linevao); vUseProgram(g_lineprog);
    vEnableVertexAttribArray(0);
    for (int h = 0; h < 2; h++) if (g_hand_ok[h]) {   // controllers: a dark grip with a light ring
        static const float dark[4] = { 0.05f, 0.05f, 0.06f, 1 }, ring[4] = { 0.5f, 0.55f, 0.6f, 1 };
        float off[3] = { 0, -0.01f, 0.05f }; float pos[3]; m4_point(g_hand[h], off, pos);
        M4 at = g_hand[h]; at.m[12] = pos[0]; at.m[13] = pos[1]; at.m[14] = pos[2];
        vBindBuffer(GL_ARRAY_BUFFER, g_linevbo); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (void *)0);
        draw_box(vp, at, 0.017f, 0.02f, 0.06f, dark);
        draw_box(vp, g_hand[h], 0.03f, 0.008f, 0.012f, ring);
    }
    if (g_ray.valid) {   // the laser: a thin bright beam, cyan on the screen, white elsewhere
        float col[4] = { g_ray_on_screen ? 0.35f : 0.9f, g_ray_on_screen ? 0.9f : 0.9f, 1.0f, 1 };
        M4 at = m4_id(); float z[3] = { -g_ray.d[0], -g_ray.d[1], -g_ray.d[2] };
        float up[3] = { 0, 1, 0 }; if (fabsf(z[1]) > 0.95f) { up[0] = 1; up[1] = 0; }
        float x[3] = { up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0] };
        float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); for (int i = 0; i < 3; i++) x[i] /= xl;
        float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
        for (int i = 0; i < 3; i++) { at.m[i] = x[i]; at.m[4 + i] = y[i]; at.m[8 + i] = z[i]; at.m[12 + i] = g_ray.o[i] + g_ray.d[i] * g_ray_len / 2; }
        vBindBuffer(GL_ARRAY_BUFFER, g_linevbo); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (void *)0);
        draw_box(vp, at, 0.0012f, 0.0012f, g_ray_len / 2, col);
        if (g_ray_on_screen) {   // a dot where it lands
            M4 dot = at; for (int i = 0; i < 3; i++) dot.m[12 + i] = g_ray.o[i] + g_ray.d[i] * (g_ray_len - 0.002f);
            static const float w[4] = { 1, 1, 1, 1 }; draw_box(vp, dot, 0.004f, 0.004f, 0.002f, w);
        }
    }
    vBindVertexArray(0);
}

static float len3(const float *a) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
// where a ray meets a quad (o + u*s + v*t), as 0..1 across content of aspect `ca` letterboxed into it
static int ray_quad(const float *O, const float *U, const float *V, float ca, const Ray *r, float *px, float *py, float *dist) {
    if (!r->valid) return 0;
    float n[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
    float dn = n[0] * r->d[0] + n[1] * r->d[1] + n[2] * r->d[2]; if (fabsf(dn) < 1e-6f) return 0;
    float w[3] = { O[0] - r->o[0], O[1] - r->o[1], O[2] - r->o[2] };
    float t = (n[0] * w[0] + n[1] * w[1] + n[2] * w[2]) / dn; if (t <= 0) return 0;
    float h[3] = { r->o[0] + r->d[0] * t - O[0], r->o[1] + r->d[1] * t - O[1], r->o[2] + r->d[2] * t - O[2] };
    float uu = (h[0] * U[0] + h[1] * U[1] + h[2] * U[2]) / (U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
    float vv = (h[0] * V[0] + h[1] * V[1] + h[2] * V[2]) / (V[0] * V[0] + V[1] * V[1] + V[2] * V[2]);
    if (uu < -0.02f || uu > 1.02f || vv < -0.02f || vv > 1.02f) return 0;
    float qa = len3(U) / len3(V), kx = qa > ca ? qa / ca : 1, ky = qa > ca ? 1 : ca / qa;
    *px = (uu - 0.5f) * kx + 0.5f; *py = (vv - 0.5f) * ky + 0.5f; *dist = t; return 1;
}
static float desk_aspect(void) { return (4.f / 3) * (DESK_Y1 - DESK_Y0 > 0 ? 1.f / (DESK_Y1 - DESK_Y0) : 1); }
static int panel_on(void) { return !g_desk_now; }
static int keyed_scene(void) { return g_office_now || g_morgue_now || g_exam_now; }
static int ray_box(const Ray *r, const float *mn, const float *mx, float pad, float *dist) {
    float t0 = 0, t1 = 1e9f;
    for (int c = 0; c < 3; c++) {
        float lo = mn[c] - pad, hi = mx[c] + pad;
        if (fabsf(r->d[c]) < 1e-7f) { if (r->o[c] < lo || r->o[c] > hi) return 0; continue; }
        float a = (lo - r->o[c]) / r->d[c], b = (hi - r->o[c]) / r->d[c]; if (a > b) { float t = a; a = b; b = t; }
        if (a > t0) t0 = a; if (b < t1) t1 = b; if (t0 > t1) return 0;
    }
    *dist = t0; return 1;
}
// a spot in the painted office -> the pointer (0..1 across the presented picture)
static void office_spot(const float *hs, float *gx, float *gy) {
    *gx = hs[0] / 640.f; *gy = (hs[1] + OFFICE_BG_Y - g_vr_pic_y0) / (float)g_vr_pic_h;
}
// which culture dish the laser is on (-1 none): anywhere in the open compartment counts, and picks the dish nearest where the
// laser enters it, so aiming at a creature, its dish or the wall just behind it all work
static int aim_dish(const Ray *r, float *dist) {
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f }; int any = 0;
    for (int d = 0; d < 7; d++) if (g_dish_ok[d]) { any = 1; for (int c = 0; c < 3; c++) { if (g_dish[d][c] < mn[c]) mn[c] = g_dish[d][c]; if (g_dish[d][c] > mx[c]) mx[c] = g_dish[d][c]; } }
    if (!any) return -1;
    for (int c = 0; c < 3; c += 2) { mn[c] -= 0.17f; mx[c] += 0.17f; }   // the compartment around the row of dishes (and a little of the wall's edge)
    mn[1] -= 0.04f; mx[1] += 0.6f;
    float t; if (!ray_box(r, mn, mx, 0.f, &t)) return -1;
    float p[3]; for (int c = 0; c < 3; c++) p[c] = r->o[c] + r->d[c] * (t + 0.05f);   // a little past the opening
    int bd = -1; float bb = 1e9f;
    for (int d = 0; d < 7; d++) if (g_dish_ok[d]) { float dx = p[0] - g_dish[d][0], dz = p[2] - g_dish[d][2], q = dx * dx + dz * dz; if (q < bb) { bb = q; bd = d; } }
    *dist = t; return bd;
}
// what the laser is on: 0 nothing, 1 monitor / room object, 2 pane; gx/gy = 0..1 across the game's picture (the pointer)
static int laser_target(const Ray *r, float *gx, float *gy, float *dist) {
    float d1 = 1e9f, d2 = 1e9f, ax = 0, ay = 0, bx = 0, by = 0; int hit1 = 0, hit2 = 0;
    if (g_have_screen) hit1 = ray_quad(g_scr_o, g_scr_u, g_scr_v, desk_aspect(), r, &ax, &ay, &d1);
    if (!hit1 && g_obj_ok[2] && ray_box(r, g_obj_min[2], g_obj_max[2], 0.01f, &d1)) { hit1 = 1; ax = ay = 0.5f; }
    if (panel_on() && g_pan_placed) hit2 = ray_quad(g_pan_o, g_pan_u, g_pan_v, 4.f / 3, r, &bx, &by, &d2);
    if (keyed_scene() && hit2) {   // the pane only takes the laser where an interface piece was drawn last frame
        float x = bx * 640.f, y = by * g_vr_pic_h + g_vr_pic_y0; int in = 0; const int last = g_uicur ^ 1;
        for (int k = 0; k < g_nui[last] && !in; k++) { Rect R = g_ui[last][k]; in = x >= R.x0 - 4 && x <= R.x1 + 4 && y >= R.y0 - 4 && y <= R.y1 + 4; }
        if (!in) hit2 = 0;
    }
    float dr = 1e9f, dd = 1e9f, dm = 1e9f; int hm = 0;
    if (g_office_now && g_obj_ok[3]) {   // the desk mouse stands for the computer: pointing at it = pointing at the PC
        float mn[3], mx[3]; for (int c = 0; c < 3; c++) { float o = c == 0 ? g_mouse_off[0] : c == 2 ? g_mouse_off[1] : 0; mn[c] = g_obj_min[3][c] + o; mx[c] = g_obj_max[3][c] + o; }
        hm = ray_box(r, mn, mx, 0.035f, &dm);
    }
    if (hm && (!hit1 || dm < d1)) { office_spot(HS_PC, gx, gy); *dist = dm; g_last_kind = 5; return 1; }
    if (g_office_now && g_obj_ok[4]) {   // the dark green book on the shelf
        float db; if (ray_box(r, g_obj_min[4], g_obj_max[4], 0.02f, &db) && (!hit1 || db < d1) && (!hit2 || db < d2)) { *gx = *gy = -1; *dist = db; g_last_kind = 9; return 0; }
    }
    if (g_office_now && g_niche_t > 0.5f) {   // the culture dishes (the nearest one the laser crosses)
        float best; int bd = aim_dish(r, &best);
        if (bd >= 0 && (!hit1 || best < d1) && (!hit2 || best < d2)) { *gx = *gy = -1; *dist = best; g_last_kind = 10; g_aim_dish = bd; return 0; }
    }
    int hr = g_office_now && g_obj_ok[0] && ray_box(r, g_obj_min[0], g_obj_max[0], 0.03f, &dr);
    int hd = g_office_now && g_obj_ok[1] && ray_box(r, g_obj_min[1], g_obj_max[1], 0.02f, &dd);
    g_last_kind = 0;
    if (g_room == 1) {   // the morgue: 6 the model under examination, 7/8 the body / personal-effects bed
        float sx, sy, d;
        if (g_exam_now) {
            int k = g_exam_now - 1, o = 1 - k;
            if (mdl_hit(k, r, &sx, &sy, &d) && (!hit2 || d < d2)) { *gx = sx / 640.f; *gy = (sy - g_vr_pic_y0) / (float)g_vr_pic_h; *dist = d; g_last_kind = 6; return 1; }
            (void)o;
        } else if (!hit2) for (int k = 0; k < 2; k++) if (bed_hit(k, r, &d)) {
            float mx_, my_; morgue_marker(k, &mx_, &my_); *gx = mx_ / 640.f; *gy = (my_ - g_vr_pic_y0) / (float)g_vr_pic_h; *dist = d; g_last_kind = 7 + k; return 1;
        }
    }
    if (hit2 && (!hit1 || d2 < d1) && (!hr || d2 < dr) && (!hd || d2 < dd)) { *gx = bx; *gy = by; *dist = d2; g_last_kind = 2; return 2; }
    if (hr && (!hit1 || dr < d1) && (!hd || dr < dd)) { office_spot(HS_RECORDER, gx, gy); *dist = dr; g_last_kind = 3; return 1; }
    if (hd && (!hit1 || dd < d1)) { office_spot(HS_DOOR, gx, gy); *dist = dd; g_last_kind = 4; return 1; }
    if (hit1) { *dist = d1; return 0; }   // the computer is used with the desk mouse only (the laser just stops on it)
    return 0;
}
// the pane follows lazily: it stays put while you look at it, and comes back in front of you when you look away
static void place_pane(void) {
    if (!keyed_scene() && !g_desk_now && g_door_t > 0.5f) {   // the other screens show in the doorway
        const float w = 0.9f, h = 0.675f;
        g_pan_o[0] = -0.3f - w / 2; g_pan_o[1] = 1.3f + h / 2; g_pan_o[2] = -7.268f;
        g_pan_u[0] = w; g_pan_u[1] = 0; g_pan_u[2] = 0; g_pan_v[0] = 0; g_pan_v[1] = -h; g_pan_v[2] = 0;
        g_pan_placed = 2; return;
    }
    if (g_pan_placed == 2) g_pan_placed = 0;
    if (!g_head_ok) return;
    if (keyed_scene()) {   // the comic bubbles ride in front of your face (smoothed a little so they don't jitter)
        const float dist = 0.95f, w = 0.95f, h = w * 0.75f;
        float eye[3] = { g_head.m[12], g_head.m[13], g_head.m[14] };
        float fwd[3] = { -g_head.m[8], -g_head.m[9], -g_head.m[10] }, up[3] = { g_head.m[4], g_head.m[5], g_head.m[6] }, rt[3] = { g_head.m[0], g_head.m[1], g_head.m[2] };
        float o[3], u[3], v[3];
        for (int k = 0; k < 3; k++) { u[k] = rt[k] * w; v[k] = -up[k] * h; o[k] = eye[k] + fwd[k] * dist - u[k] / 2 - v[k] / 2 - up[k] * 0.06f; }
        float a = g_pan_placed == 1 ? 0.35f : 1.f;
        for (int k = 0; k < 3; k++) { g_pan_o[k] += (o[k] - g_pan_o[k]) * a; g_pan_u[k] += (u[k] - g_pan_u[k]) * a; g_pan_v[k] += (v[k] - g_pan_v[k]) * a; }
        g_pan_placed = 1; return;
    }
    float eye[3] = { g_head.m[12], g_head.m[13], g_head.m[14] }, fw[3] = { -g_head.m[8], 0, -g_head.m[10] };
    float fl = sqrtf(fw[0] * fw[0] + fw[2] * fw[2]); if (fl < 1e-3f) return; fw[0] /= fl; fw[2] /= fl;
    float dist = g_office_now ? 0.95f : 1.8f, w = g_office_now ? 0.75f : 1.6f, h = w * 0.75f;
    float c[3] = { eye[0] + fw[0] * dist, eye[1] - (g_office_now ? 0.32f : 0.0f), eye[2] + fw[2] * dist };
    if (g_pan_placed) {   // still roughly in view and at the right size? leave it
        float pc[3] = { g_pan_o[0] + g_pan_u[0] / 2 + g_pan_v[0] / 2, g_pan_o[1] + g_pan_u[1] / 2 + g_pan_v[1] / 2, g_pan_o[2] + g_pan_u[2] / 2 + g_pan_v[2] / 2 };
        float to[3] = { pc[0] - eye[0], 0, pc[2] - eye[2] }; float tl = sqrtf(to[0] * to[0] + to[2] * to[2]);
        float cosang = tl > 0 ? (to[0] * fw[0] + to[2] * fw[2]) / tl : 1;
        if (cosang > 0.8f && fabsf(len3(g_pan_u) - w) < 0.01f && fabsf(tl - dist) < 0.5f) return;
    }
    float right[3] = { -fw[2], 0, fw[0] };   // facing the eye: right = forward x up
    float tilt = g_office_now ? 0.32f : 0.0f;   // lean the pane back so it faces the eye
    float down[3] = { fw[0] * sinf(tilt), -cosf(tilt), fw[2] * sinf(tilt) };
    for (int k = 0; k < 3; k++) { g_pan_u[k] = right[k] * w; g_pan_v[k] = down[k] * h; g_pan_o[k] = c[k] - g_pan_u[k] / 2 - g_pan_v[k] / 2; }
    g_pan_placed = 1;
}
// pointer: where the laser meets the screen (0..1 across the game picture), honouring the letterbox
static int ray_screen(const Ray *r, int gw, int gh, float *px, float *py, float *dist) {
    (void)gw; (void)gh; return laser_target(r, px, py, dist) != 0;
}
static int ray_screen_old(const Ray *r, int gw, int gh, float *px, float *py, float *dist) {
    if (!g_have_screen || !r->valid) return 0;
    float n[3] = { g_scr_u[1] * g_scr_v[2] - g_scr_u[2] * g_scr_v[1], g_scr_u[2] * g_scr_v[0] - g_scr_u[0] * g_scr_v[2], g_scr_u[0] * g_scr_v[1] - g_scr_u[1] * g_scr_v[0] };
    float dn = n[0] * r->d[0] + n[1] * r->d[1] + n[2] * r->d[2]; if (fabsf(dn) < 1e-6f) return 0;
    float w[3] = { g_scr_o[0] - r->o[0], g_scr_o[1] - r->o[1], g_scr_o[2] - r->o[2] };
    float t = (n[0] * w[0] + n[1] * w[1] + n[2] * w[2]) / dn; if (t <= 0) return 0;
    float h[3] = { r->o[0] + r->d[0] * t - g_scr_o[0], r->o[1] + r->d[1] * t - g_scr_o[1], r->o[2] + r->d[2] * t - g_scr_o[2] };
    float uu = (h[0] * g_scr_u[0] + h[1] * g_scr_u[1] + h[2] * g_scr_u[2]) / (g_scr_u[0] * g_scr_u[0] + g_scr_u[1] * g_scr_u[1] + g_scr_u[2] * g_scr_u[2]);
    float vv = (h[0] * g_scr_v[0] + h[1] * g_scr_v[1] + h[2] * g_scr_v[2]) / (g_scr_v[0] * g_scr_v[0] + g_scr_v[1] * g_scr_v[1] + g_scr_v[2] * g_scr_v[2]);
    if (uu < -0.05f || uu > 1.05f || vv < -0.05f || vv > 1.05f) return 0;
    float sw = sqrtf(g_scr_u[0] * g_scr_u[0] + g_scr_u[1] * g_scr_u[1] + g_scr_u[2] * g_scr_u[2]), sh = sqrtf(g_scr_v[0] * g_scr_v[0] + g_scr_v[1] * g_scr_v[1] + g_scr_v[2] * g_scr_v[2]);
    float qa = sw / sh, ga = gh > 0 ? (float)gw / gh : 4.f / 3, kx = qa > ga ? qa / ga : 1, ky = qa > ga ? 1 : ga / qa;
    *px = (uu - 0.5f) * kx + 0.5f; *py = (vv - 0.5f) * ky + 0.5f; *dist = t; return 1;
}


// ---------------------------------------------------------------- the aquarium: the creatures in the hidden compartment's seven dishes
// Each creature type is a rigged glTF model (vr/creature*.glb) with an 'idle' and an 'agitated' loop (the first swimmer has a single
// 'swim' loop, played faster when stirred). A type can appear more than once (the pair, the trio) and in recoloured variants. Every
// instance is posed from its type's animation and skinned on the CPU each frame; where it is and how it moves is decided per dish in
// creature_update(). Touch a dish (or point at it and pull the trigger) to stir its occupants.
typedef struct {
    cgltf_data *d; cgltf_primitive *pr; cgltf_skin *skin; cgltf_animation *idle, *agit;
    int nv, ni; float *p, *n, *uv, *w; u16 *j; GLuint ibo;
    float lo[3], hi[3], dur_i, dur_a;
    float mf[3], mu[3];            // the model's own forward and up axes
    const cgltf_image *img;
} CrType;
typedef struct { int type; GLuint tex, vao, vbo; float *cpu; float t; float glow; } CrInst;
enum { CT_SWIM, CT_PAIR, CT_PLATES, CT_TRIO, CT_EYE, CT_WORM, CT_N };
static CrType g_ct[CT_N]; static CrInst g_ci[16]; static int g_nci;
static int g_ci_of[7][3], g_nci_of[7];   // per dish: its occupants
static float g_aq_clock;
static GLuint g_bub_vao, g_bub_vbo; static float g_bub_cpu[17 * 9 * 9 * 6]; static int g_bub_nv;   // the eye creature's bubble

static float aq_dur(const cgltf_animation *a) {
    float m = 1; if (!a) return m;
    for (cgltf_size c = 0; c < a->samplers_count; c++) { const cgltf_accessor *in = a->samplers[c].input; float x; cgltf_accessor_read_float(in, in->count - 1, &x, 1); if (x > m) m = x; }
    return m;
}
static int ct_load(CrType *T, const char *path, const float mf[3], const float mu[3]) {
    cgltf_options opt = {0}; cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success || cgltf_load_buffers(&opt, d, path) != cgltf_result_success) { if (d) cgltf_free(d); fprintf(stderr, "[vr] no %s\n", path); return 0; }
    cgltf_node *mn = NULL; for (cgltf_size i = 0; i < d->nodes_count; i++) if (d->nodes[i].mesh) { mn = &d->nodes[i]; if (mn->skin) break; }
    if (!mn) { cgltf_free(d); return 0; }
    T->d = d; T->pr = &mn->mesh->primitives[0]; T->skin = mn->skin; memcpy(T->mf, mf, 12); memcpy(T->mu, mu, 12);
    for (cgltf_size i = 0; i < d->animations_count; i++) {
        const char *nm = d->animations[i].name ? d->animations[i].name : "";
        if (!strcmp(nm, "idle") || !strcmp(nm, "swim")) T->idle = &d->animations[i]; else if (!strcmp(nm, "agitated")) T->agit = &d->animations[i];
    }
    if (!T->idle && d->animations_count) T->idle = &d->animations[0];
    if (!T->agit) T->agit = T->idle;
    T->dur_i = aq_dur(T->idle); T->dur_a = aq_dur(T->agit);
    const cgltf_accessor *ap = NULL, *an = NULL, *at = NULL, *aj = NULL, *aw = NULL;
    for (cgltf_size a = 0; a < T->pr->attributes_count; a++) { cgltf_attribute *A = &T->pr->attributes[a];
        if (A->type == cgltf_attribute_type_position) ap = A->data; else if (A->type == cgltf_attribute_type_normal) an = A->data;
        else if (A->type == cgltf_attribute_type_texcoord && A->index == 0) at = A->data;
        else if (A->type == cgltf_attribute_type_joints && A->index == 0) aj = A->data; else if (A->type == cgltf_attribute_type_weights && A->index == 0) aw = A->data; }
    if (!ap) { cgltf_free(d); T->d = NULL; return 0; }
    int n = T->nv = (int)ap->count;
    T->p = malloc(12 * (size_t)n); T->n = malloc(12 * (size_t)n); T->uv = calloc((size_t)n, 8);
    if (T->skin && aj && aw) { T->w = malloc(16 * (size_t)n); T->j = malloc(8 * (size_t)n); } else T->skin = NULL;
    for (int i = 0; i < 3; i++) { T->lo[i] = 1e9f; T->hi[i] = -1e9f; }
    for (int k = 0; k < n; k++) {
        cgltf_accessor_read_float(ap, k, T->p + k * 3, 3);
        if (an) cgltf_accessor_read_float(an, k, T->n + k * 3, 3); else { T->n[k * 3] = 0; T->n[k * 3 + 1] = 1; T->n[k * 3 + 2] = 0; }
        if (at) cgltf_accessor_read_float(at, k, T->uv + k * 2, 2);
        if (T->skin) { cgltf_accessor_read_float(aw, k, T->w + k * 4, 4); cgltf_uint jj[4] = { 0 }; cgltf_accessor_read_uint(aj, k, jj, 4); for (int i = 0; i < 4; i++) T->j[k * 4 + i] = (u16)jj[i]; }
        else if (mn) { float wm[16]; cgltf_node_transform_world(mn, wm); float q[3] = { T->p[k * 3], T->p[k * 3 + 1], T->p[k * 3 + 2] };   // static: bake the node's own placement
            for (int r = 0; r < 3; r++) T->p[k * 3 + r] = wm[r] * q[0] + wm[4 + r] * q[1] + wm[8 + r] * q[2] + wm[12 + r]; }
        for (int i = 0; i < 3; i++) { float x = T->p[k * 3 + i]; if (x < T->lo[i]) T->lo[i] = x; if (x > T->hi[i]) T->hi[i] = x; }
    }
    T->ni = T->pr->indices ? (int)T->pr->indices->count : n; u32 *ix = malloc(4 * (size_t)T->ni);
    for (int k = 0; k < T->ni; k++) ix[k] = T->pr->indices ? (u32)cgltf_accessor_read_index(T->pr->indices, k) : (u32)k;
    vGenBuffers(1, &T->ibo); vBindBuffer(GL_ELEMENT_ARRAY_BUFFER, T->ibo); vBufferData(GL_ELEMENT_ARRAY_BUFFER, 4 * (size_t)T->ni, ix, GL_STATIC_DRAW); free(ix);
    const cgltf_material *m = T->pr->material; T->img = m && m->pbr_metallic_roughness.base_color_texture.texture ? m->pbr_metallic_roughness.base_color_texture.texture->image : NULL;
    fprintf(stderr, "[vr] creature %s: %d vertices%s%s\n", path, n, T->skin ? ", rigged" : ", static", T->agit && T->agit != T->idle ? ", idle + agitated loops" : "");
    return 1;
}
// the model's colour map, optionally recoloured: pixels whose hue is in [h0, h1] (degrees, saturated enough) are turned so the
// middle of that range lands on hue hto (shading and the spread of tones are kept)
static GLuint ct_texture(const CrType *T, float h0, float h1, float hto) {
    if (!T->img || !T->img->buffer_view) return g_white;
    const u8 *src = (const u8 *)T->img->buffer_view->buffer->data + T->img->buffer_view->offset;
    int w, h, c; u8 *px = stbi_load_from_memory(src, (int)T->img->buffer_view->size, &w, &h, &c, 4);
    if (!px) { fprintf(stderr, "[vr] creature colour map: %s\n", stbi_failure_reason()); return g_white; }
    if (h0 >= 0) for (int i = 0; i < w * h; i++) {
        float r = px[i * 4] / 255.f, g = px[i * 4 + 1] / 255.f, b = px[i * 4 + 2] / 255.f, mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), dl = mx - mn;
        if (dl < 0.08f || mx < 0.12f || dl / mx < 0.22f) continue;
        float hue = mx == r ? 60 * fmodf((g - b) / dl, 6) : mx == g ? 60 * ((b - r) / dl + 2) : 60 * ((r - g) / dl + 4); if (hue < 0) hue += 360;
        int in = h0 <= h1 ? (hue >= h0 && hue <= h1) : (hue >= h0 || hue <= h1); if (!in) continue;
        float nh = fmodf(hue + hto - (h0 <= h1 ? (h0 + h1) / 2 : fmodf((h0 + h1 + 360) / 2, 360)) + 720, 360), H = nh / 60, X = dl * (1 - fabsf(fmodf(H, 2) - 1)), rr, gg, bb; int seg = (int)H % 6;
        if (seg == 0) { rr = dl; gg = X; bb = 0; } else if (seg == 1) { rr = X; gg = dl; bb = 0; } else if (seg == 2) { rr = 0; gg = dl; bb = X; }
        else if (seg == 3) { rr = 0; gg = X; bb = dl; } else if (seg == 4) { rr = X; gg = 0; bb = dl; } else { rr = dl; gg = 0; bb = X; }
        px[i * 4] = (u8)((rr + mn) * 255); px[i * 4 + 1] = (u8)((gg + mn) * 255); px[i * 4 + 2] = (u8)((bb + mn) * 255);
    }
    GLuint t; vGenTextures(1, &t); vBindTexture(GL_TEXTURE_2D, t); vPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); vGenerateMipmap(GL_TEXTURE_2D);
    vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(px); return t;
}
static int ci_new(int type, GLuint tex, int dish) {
    if (g_nci >= 16 || !g_ct[type].d) return -1;
    CrInst *I = &g_ci[g_nci]; CrType *T = &g_ct[type]; I->type = type; I->tex = tex; I->cpu = malloc(36 * (size_t)T->nv); I->t = (float)g_nci * 0.37f;
    vGenVertexArrays(1, &I->vao); vBindVertexArray(I->vao);
    vGenBuffers(1, &I->vbo); vBindBuffer(GL_ARRAY_BUFFER, I->vbo); vBufferData(GL_ARRAY_BUFFER, 36 * (size_t)T->nv, NULL, GL_DYNAMIC_DRAW);
    vBindBuffer(GL_ELEMENT_ARRAY_BUFFER, T->ibo);
    vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
    vEnableVertexAttribArray(1); vVertexAttribPointer(1, 2, GL_FLOAT, 0, 36, (void *)12);
    vEnableVertexAttribArray(2); vVertexAttribPointer(2, 4, GL_FLOAT, 0, 36, (void *)20);
    vBindVertexArray(0);
    g_ci_of[dish][g_nci_of[dish]++] = g_nci;
    return g_nci++;
}
static void anim_sample(const cgltf_animation_sampler *sm, float t, float *out, int nc) {   // linear (rotations: normalised lerp)
    const cgltf_accessor *in = sm->input, *ou = sm->output; int n = (int)in->count, k = 0; float t0, t1;
    cgltf_accessor_read_float(in, 0, &t0, 1); if (t <= t0 || n == 1) { cgltf_accessor_read_float(ou, 0, out, nc); return; }
    for (k = 0; k < n - 1; k++) { cgltf_accessor_read_float(in, k + 1, &t1, 1); if (t1 >= t) break; }
    if (k >= n - 1) { cgltf_accessor_read_float(ou, n - 1, out, nc); return; }
    cgltf_accessor_read_float(in, k, &t0, 1); cgltf_accessor_read_float(in, k + 1, &t1, 1);
    float a[4], b[4], u = t1 > t0 ? (t - t0) / (t1 - t0) : 0; cgltf_accessor_read_float(ou, k, a, nc); cgltf_accessor_read_float(ou, k + 1, b, nc);
    if (nc == 4 && a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0) for (int i = 0; i < 4; i++) b[i] = -b[i];
    float l = 0; for (int i = 0; i < nc; i++) { out[i] = a[i] + (b[i] - a[i]) * u; l += out[i] * out[i]; }
    if (nc == 4 && l > 0) { l = sqrtf(l); for (int i = 0; i < 4; i++) out[i] /= l; }
}
static void norm3(float *v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-9f) for (int i = 0; i < 3; i++) v[i] /= l; }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
// pose an instance (advance its own clock, agitated loop or idle) and skin it into the room: centred on C, its forward along F,
// its up along U, its longest side `len` metres
static void ci_update(CrInst *I, int agit, float rate, const float C[3], const float F_[3], const float U_[3], float len) {
    CrType *T = &g_ct[I->type]; const float dt = 1.f / 60;
    cgltf_animation *an = agit ? T->agit : T->idle; float dur = agit ? T->dur_a : T->dur_i;
    I->t = fmodf(I->t + dt * rate, dur > 0 ? dur : 1);
    static float J[128][16]; int nj = 0;
    if (T->skin && an) {
        for (cgltf_size c = 0; c < an->channels_count; c++) {
            cgltf_animation_channel *ch = &an->channels[c]; cgltf_node *nd = ch->target_node; if (!nd) continue;
            if (ch->target_path == cgltf_animation_path_type_rotation) { anim_sample(ch->sampler, I->t, nd->rotation, 4); nd->has_rotation = 1; }
            else if (ch->target_path == cgltf_animation_path_type_translation) { anim_sample(ch->sampler, I->t, nd->translation, 3); nd->has_translation = 1; }
            else if (ch->target_path == cgltf_animation_path_type_scale) { anim_sample(ch->sampler, I->t, nd->scale, 3); nd->has_scale = 1; }
        }
        nj = (int)T->skin->joints_count; if (nj > 128) nj = 128;
        for (int j = 0; j < nj; j++) {
            float w[16], ib[16]; cgltf_node_transform_world(T->skin->joints[j], w);
            if (T->skin->inverse_bind_matrices) cgltf_accessor_read_float(T->skin->inverse_bind_matrices, j, ib, 16); else { memset(ib, 0, 64); ib[0] = ib[5] = ib[10] = ib[15] = 1; }
            M4 a, b; memcpy(a.m, w, 64); memcpy(b.m, ib, 64); M4 r = m4_mul(a, b); memcpy(J[j], r.m, 64);
        }
    }
    float F[3] = { F_[0], F_[1], F_[2] }, U[3] = { U_[0], U_[1], U_[2] }; norm3(F);
    float d = U[0] * F[0] + U[1] * F[1] + U[2] * F[2]; for (int i = 0; i < 3; i++) U[i] -= d * F[i]; norm3(U);
    float S[3], ms[3]; cross3(U, F, S); cross3(T->mu, T->mf, ms);
    float ext = 0; for (int i = 0; i < 3; i++) if (T->hi[i] - T->lo[i] > ext) ext = T->hi[i] - T->lo[i];
    float sc = ext > 0 ? len / ext : 1, ctr[3] = { (T->lo[0] + T->hi[0]) / 2, (T->lo[1] + T->hi[1]) / 2, (T->lo[2] + T->hi[2]) / 2 };
    const float L[3] = { 0.2f, 0.9f, 0.3f };
    for (int k = 0; k < T->nv; k++) {
        float p[3], n[3]; const float *P = T->p + k * 3, *N = T->n + k * 3;
        if (nj) { p[0] = p[1] = p[2] = n[0] = n[1] = n[2] = 0;
            for (int i = 0; i < 4; i++) { float wt = T->w[k * 4 + i]; if (wt <= 0) continue; int j = T->j[k * 4 + i]; if (j >= nj) continue; const float *m = J[j];
                for (int r = 0; r < 3; r++) { p[r] += wt * (m[r] * P[0] + m[4 + r] * P[1] + m[8 + r] * P[2] + m[12 + r]); n[r] += wt * (m[r] * N[0] + m[4 + r] * N[1] + m[8 + r] * N[2]); } } }
        else { memcpy(p, P, 12); memcpy(n, N, 12); }
        float q[3] = { p[0] - ctr[0], p[1] - ctr[1], p[2] - ctr[2] };
        float a_s = q[0] * ms[0] + q[1] * ms[1] + q[2] * ms[2], a_u = q[0] * T->mu[0] + q[1] * T->mu[1] + q[2] * T->mu[2], a_f = q[0] * T->mf[0] + q[1] * T->mf[1] + q[2] * T->mf[2];
        float b_s = n[0] * ms[0] + n[1] * ms[1] + n[2] * ms[2], b_u = n[0] * T->mu[0] + n[1] * T->mu[1] + n[2] * T->mu[2], b_f = n[0] * T->mf[0] + n[1] * T->mf[1] + n[2] * T->mf[2];
        float *o = I->cpu + k * 9, nw[3];
        for (int i = 0; i < 3; i++) { o[i] = C[i] + sc * (S[i] * a_s + U[i] * a_u + F[i] * a_f); nw[i] = S[i] * b_s + U[i] * b_u + F[i] * b_f; }
        float nl = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]), dd = nl > 0 ? fabsf(nw[0] * L[0] + nw[1] * L[1] + nw[2] * L[2]) / nl : 1;
        o[3] = T->uv[k * 2]; o[4] = T->uv[k * 2 + 1]; o[5] = o[6] = o[7] = 0.5f * (0.55f + 0.5f * dd); o[8] = 1;
    }
}
static void creature_load(const char *unused) {
    (void)unused;
    const float px[3] = { 1, 0, 0 }, nx[3] = { -1, 0, 0 }, py[3] = { 0, 1, 0 }, pz[3] = { 0, 0, 1 };
    ct_load(&g_ct[CT_SWIM], "vr/creature.glb", py, pz);          // head first, face up
    ct_load(&g_ct[CT_PAIR], "vr/creature_pair.glb", pz, py);     // face forward, crest up
    ct_load(&g_ct[CT_PLATES], "vr/creature_plates.glb", pz, py); // flat on the agar, crystals up
    ct_load(&g_ct[CT_TRIO], "vr/creature_trio.glb", nx, py);     // the eye leads
    ct_load(&g_ct[CT_EYE], "vr/creature_eye.glb", pz, py);       // the eye looks forward (at you)
    ct_load(&g_ct[CT_WORM], "vr/creature_worm.glb", py, pz);     // head first, lying on the agar
    (void)px;
    ci_new(CT_SWIM, ct_texture(&g_ct[CT_SWIM], -1, 0, 0), 0);
    ci_new(CT_PAIR, ct_texture(&g_ct[CT_PAIR], -1, 0, 0), 1);               // purple, as made
    ci_new(CT_PAIR, ct_texture(&g_ct[CT_PAIR], 175, 330, 357), 1);          // red: the purple turned red
    ci_new(CT_PLATES, ct_texture(&g_ct[CT_PLATES], -1, 0, 0), 2);
    ci_new(CT_TRIO, ct_texture(&g_ct[CT_TRIO], -1, 0, 0), 3);               // yellow, as made
    ci_new(CT_TRIO, ct_texture(&g_ct[CT_TRIO], 25, 70, 118), 3);            // green: only the yellow changes
    ci_new(CT_TRIO, ct_texture(&g_ct[CT_TRIO], 25, 70, 285), 3);            // purple
    ci_new(CT_EYE, ct_texture(&g_ct[CT_EYE], -1, 0, 0), 4);
    ci_new(CT_WORM, ct_texture(&g_ct[CT_WORM], -1, 0, 0), 5);
    vGenVertexArrays(1, &g_bub_vao); vBindVertexArray(g_bub_vao); vGenBuffers(1, &g_bub_vbo); vBindBuffer(GL_ARRAY_BUFFER, g_bub_vbo);
    vBufferData(GL_ARRAY_BUFFER, sizeof g_bub_cpu, NULL, GL_DYNAMIC_DRAW);
    vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
    vEnableVertexAttribArray(1); vVertexAttribPointer(1, 2, GL_FLOAT, 0, 36, (void *)12);
    vEnableVertexAttribArray(2); vVertexAttribPointer(2, 4, GL_FLOAT, 0, 36, (void *)20);
    vBindVertexArray(0);
}
static void aq_poke(int d) {   // you touched (or clicked) dish d
    if (d < 0 || d > 5 || !g_nci_of[d]) return;
    if (d == 1) { g_dish_tog[1] = !g_dish_tog[1]; fprintf(stderr, "[vr] dish 2: the pair %s\n", g_dish_tog[1] ? "fuse" : "split apart"); }
    else { g_dish_t[d] = 4.f; fprintf(stderr, "[vr] dish %d stirred\n", d + 1); }
}
static float smooth01(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }
static void bubble_build(const float C[3], float r, float agit) {   // a translucent dome over the eye creature
    int nv = 0; const int RU = 16, RV = 8;
    for (int a = 0; a < RU; a++) for (int b = 0; b < RV; b++) {
        float u0 = 2 * (float)M_PI * a / RU, u1 = 2 * (float)M_PI * (a + 1) / RU, v0 = 1.75f * b / RV, v1 = 1.75f * (b + 1) / RV;   // a cap: from the top down past the middle
        float uv[6][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v0 }, { u1, v1 }, { u0, v1 } };
        for (int k = 0; k < 6; k++) {
            float u = uv[k][0], v = uv[k][1], wob = 1 + 0.06f * agit * sinf(u * 3 + g_aq_clock * 11) * sinf(v * 2 + g_aq_clock * 7);
            float nx = sinf(v) * cosf(u), ny = cosf(v), nz = sinf(v) * sinf(u), *o = g_bub_cpu + nv * 9;
            o[0] = C[0] + r * wob * nx; o[1] = C[1] + r * wob * ny; o[2] = C[2] + r * wob * nz; o[3] = 0; o[4] = 0;
            float rim = 0.25f + 0.75f * powf(sinf(v) * 0.8f + 0.2f, 2);
            o[5] = 0.55f + 0.2f * agit; o[6] = 0.5f; o[7] = 1.0f; o[8] = (0.12f + 0.2f * rim) * (1 + 0.6f * agit);
            nv++;
        }
    }
    g_bub_nv = nv;
}
static void creature_update(void) {   // once per frame: every dish's occupants
    if (g_niche_t < 0.02f) return;
    const float dt = 1.f / 60; g_aq_clock += dt; float t = g_aq_clock;
    for (int d = 0; d < 7; d++) if (g_dish_t[d] > 0) g_dish_t[d] -= dt;
    const float up[3] = { 0, 1, 0 };
    #define PAN(d) (g_dish_t[d] > 1.5f ? 1.f : g_dish_t[d] > 0 ? g_dish_t[d] / 1.5f : 0.f)
    // 1: the little swimmer: circles above the agar, head first, banking; stirred, it panics
    if (g_dish_ok[0] && g_nci_of[0]) { static float a; float p = PAN(0); a += dt * (1.0f + 2.6f * p);
        float rad = 0.022f + 0.006f * p * sinf(t * 9), C[3] = { g_dish[0][0] + rad * cosf(a), g_dish[0][1] + 0.01f + (0.002f + 0.002f * p) * sinf(t * 2.7f), g_dish[0][2] + rad * sinf(a) };
        float F[3] = { -sinf(a), 0, cosf(a) }, bank = 0.35f + 0.35f * p, S[3]; cross3(up, F, S);
        float U[3] = { up[0] * cosf(bank) + S[0] * sinf(bank), up[1] * cosf(bank) + S[1] * sinf(bank), up[2] * cosf(bank) + S[2] * sinf(bank) };
        ci_update(&g_ci[g_ci_of[0][0]], p > 0, 1 + 1.8f * p, C, F, U, 0.03f); }
    // 2: the pair: drift around the dish keeping apart; touched, they spiral into one yin-yang (and back apart next time)
    if (g_dish_ok[1] && g_nci_of[1] == 2) { static float a, fz; float tg = g_dish_tog[1] ? 1.f : 0.f; int moving = fabsf(fz - tg) > 0.01f;
        fz += (tg > fz ? 1 : -1) * 0.012f; if (fabsf(fz - tg) < 0.012f) fz = tg; a += dt * 0.55f; float f = smooth01(fz);
        float cx = g_dish[1][0] + 0.009f * cosf(t * 0.45f), cz = g_dish[1][2] + 0.009f * sinf(t * 0.45f), sp = t * 1.7f;
        for (int k = 0; k < 2; k++) {
            float ang = a + k * ((float)M_PI + 0.5f * sinf(t * 0.6f)), r0 = 0.026f + 0.003f * sinf(t * 0.9f + k);
            float ux = g_dish[1][0] + r0 * cosf(ang), uz = g_dish[1][2] + r0 * sinf(ang), Fu[3] = { -sinf(ang), 0, cosf(ang) };   // apart
            float s2 = sp + k * (float)M_PI, fx = cx + 0.0065f * cosf(s2), fz2 = cz + 0.0065f * sinf(s2), Ff[3] = { -sinf(s2), 0, cosf(s2) };   // fused: chasing tails
            float C[3] = { ux + (fx - ux) * f, g_dish[1][1] + 0.018f + 0.002f * sinf(t * 1.3f + k * 2), uz + (fz2 - uz) * f };
            float F[3] = { Fu[0] + (Ff[0] - Fu[0]) * f, 0, Fu[2] + (Ff[2] - Fu[2]) * f };
            if (F[0] * F[0] + F[2] * F[2] < 1e-6f) { F[0] = Ff[0]; F[2] = Ff[2]; }
            ci_update(&g_ci[g_ci_of[1][k]], moving, moving ? 1.6f : 1.f, C, F, up, 0.026f);
        } }
    // 3: the crystal plates: still at the bottom; touched, they glow
    if (g_dish_ok[2] && g_nci_of[2]) { CrInst *I = &g_ci[g_ci_of[2][0]]; CrType *T = &g_ct[I->type];
        float ext = 0; for (int i = 0; i < 3; i++) if (T->hi[i] - T->lo[i] > ext) ext = T->hi[i] - T->lo[i];
        float hgt = ext > 0 ? (T->hi[1] - T->lo[1]) / ext * 0.07f : 0.01f, C[3] = { g_dish[2][0], g_dish[2][1] + 0.0005f + hgt / 2, g_dish[2][2] }, F[3] = { 0, 0, 1 };
        float p = PAN(2); I->glow = p * (0.75f + 0.25f * sinf(t * 6)); ci_update(I, 0, 0, C, F, up, 0.07f); }
    // 4: the trio: three colours dancing around each other; stirred, the dance turns frantic
    if (g_dish_ok[3] && g_nci_of[3]) { static float a; float p = PAN(3); a += dt * (1.2f + 2.8f * p);
        float cx = g_dish[3][0] + 0.008f * cosf(t * 0.5f), cz = g_dish[3][2] + 0.008f * sinf(t * 0.5f);
        for (int k = 0; k < g_nci_of[3]; k++) { float ang = a + 2 * (float)M_PI * k / 3, r = 0.017f + 0.004f * p * sinf(t * 7 + k);
            float C[3] = { cx + r * cosf(ang), g_dish[3][1] + 0.016f + (0.003f + 0.003f * p) * sinf(t * 1.3f + k * 2.1f), cz + r * sinf(ang) }, F[3] = { -sinf(ang), 0.15f * sinf(t * 2 + k), cosf(ang) };
            ci_update(&g_ci[g_ci_of[3][k]], p > 0, 1 + 1.5f * p, C, F, up, 0.022f); } }
    // 5: the eye: hovers, always looking at you, wings circling it under its bubble; stirred, the bubble swells and the eye twitches
    if (g_dish_ok[4] && g_nci_of[4]) { float p = PAN(4);
        float C[3] = { g_dish[4][0], g_dish[4][1] + 0.024f + 0.003f * sinf(t * 1.1f), g_dish[4][2] };
        float F[3] = { g_head.m[12] - C[0], g_head.m[13] - C[1], g_head.m[14] - C[2] }; norm3(F);
        if (p > 0) { F[0] += 0.18f * p * sinf(t * 23); F[1] += 0.14f * p * sinf(t * 31 + 1); F[2] += 0.18f * p * sinf(t * 19 + 2); }
        ci_update(&g_ci[g_ci_of[4][0]], p > 0, 1 + 1.2f * p, C, F, up, 0.03f);
        float B[3] = { C[0], C[1] + 0.008f, C[2] }; bubble_build(B, 0.017f * (1 + 0.38f * p + 0.06f * p * sinf(t * 9)), p); }
    // 6: the worm: wanders over the agar wiggling; stirred, it races about and bumps into the dish walls
    if (g_dish_ok[5] && g_nci_of[5]) { static float x, z, h; float p = PAN(5), sp = 0.006f + 0.03f * p;
        h += dt * (0.6f * sinf(t * 0.37f) + 0.3f * sinf(t * 1.1f) + 4.0f * p * sinf(t * 5.3f));
        float nx = x + cosf(h) * sp * dt, nz = z + sinf(h) * sp * dt, r = sqrtf(nx * nx + nz * nz), lim = 0.032f;
        if (r > lim) {   // the wall: turn back (a hard bounce when frantic)
            float rx = nx / r, rz = nz / r, vx = cosf(h), vz = sinf(h), dn = vx * rx + vz * rz;
            if (p > 0.05f) { vx -= 2 * dn * rx; vz -= 2 * dn * rz; h = atan2f(vz, vx); } else h += 0.08f;
            nx = rx * lim; nz = rz * lim;
        }
        x = nx; z = nz;
        float C[3] = { g_dish[5][0] + x, g_dish[5][1] + 0.0035f, g_dish[5][2] + z }, F[3] = { cosf(h), 0, sinf(h) };
        ci_update(&g_ci[g_ci_of[5][0]], p > 0, 1 + 2.0f * p, C, F, up, 0.045f); }
    #undef PAN
}
static void creature_draw(void) {
    if (g_niche_t < 0.02f || !g_nci) return;
    static const float one[4] = { 1, 1, 1, 1 }, zero[3] = { 0, 0, 0 }; M4 id = m4_id();
    vUniformMatrix4fv(u_model, 1, 0, id.m); vUniform1i(u_screen, 0); vUniform4fv(u_base, 1, one); vUniform1f(u_cut, 0.3f);
    { static const float no[3] = { 0, 0, 0 }; vUniform3fv(u_lit, 1, no); }
    vActiveTexture(GL_TEXTURE1); vBindTexture(GL_TEXTURE_2D, g_white); vActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < g_nci; i++) {
        CrInst *I = &g_ci[i]; CrType *T = &g_ct[I->type];
        float glow[3] = { 0.25f * I->glow, 0.55f * I->glow, 0.95f * I->glow }; vUniform3fv(u_emis, 1, I->glow > 0 ? glow : zero);
        vBindTexture(GL_TEXTURE_2D, I->tex ? I->tex : g_white);
        vBindVertexArray(I->vao); vBindBuffer(GL_ARRAY_BUFFER, I->vbo); vBufferData(GL_ARRAY_BUFFER, 36 * (size_t)T->nv, I->cpu, GL_DYNAMIC_DRAW);
        vDrawElements(GL_TRIANGLES, T->ni, GL_UNSIGNED_INT, 0);
    }
    vUniform3fv(u_emis, 1, zero);
    if (g_bub_nv) {   // the bubble: see-through, drawn last
        vBindTexture(GL_TEXTURE_2D, g_white); vUniform1i(u_screen, 3); vUniform4fv(u_base, 1, one);
        vEnable(GL_BLEND); vBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); vDepthMask(0);
        vBindVertexArray(g_bub_vao); vBindBuffer(GL_ARRAY_BUFFER, g_bub_vbo); vBufferData(GL_ARRAY_BUFFER, 36 * (size_t)g_bub_nv, g_bub_cpu, GL_DYNAMIC_DRAW);
        vDrawArrays(GL_TRIANGLES, 0, g_bub_nv);
        vDisable(GL_BLEND); vDepthMask(1); vUniform1i(u_screen, 0);
    }
    vBindVertexArray(0);
}

typedef struct GLSave GLSave; static void gl_save(GLSave *s); static void gl_restore(const GLSave *s);
// ---------------------------------------------------------------- shared setup
static GLuint g_game_tex; static int g_gw = 640, g_gh = 480;
static int common_init(void) {
    g_prog = build(VS, FS); g_lineprog = build(LVS, LFS);
    u_mvp = vGetUniformLocation(g_prog, "uMVP"); u_model = vGetUniformLocation(g_prog, "uModel"); u_hole = vGetUniformLocation(g_prog, "uHole"); u_base = vGetUniformLocation(g_prog, "uBase"); u_emis = vGetUniformLocation(g_prog, "uEmis");
    u_tex = vGetUniformLocation(g_prog, "uTex"); u_etex = vGetUniformLocation(g_prog, "uETex"); u_cut = vGetUniformLocation(g_prog, "uCut"); u_lit = vGetUniformLocation(g_prog, "uLit");
    u_screen = vGetUniformLocation(g_prog, "uScreen"); u_scr_k = vGetUniformLocation(g_prog, "uScrK"); u_crop = vGetUniformLocation(g_prog, "uCrop");
    u_line_mvp = vGetUniformLocation(g_lineprog, "uMVP"); u_line_col = vGetUniformLocation(g_lineprog, "uCol");
    vGenVertexArrays(1, &g_linevao); vGenBuffers(1, &g_linevbo);
    static const u8 white[4] = { 255, 255, 255, 255 };
    vGenTextures(1, &g_white); vBindTexture(GL_TEXTURE_2D, g_white); vTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    {   // wall panel quad: pos, uv, colour
        float q[4][9]; for (int i = 0; i < 4; i++) {
            float su = (i == 1 || i == 2), sv = (i >= 2);
            for (int c = 0; c < 3; c++) q[i][c] = g_pan_o[c] + g_pan_u[c] * su + g_pan_v[c] * sv;
            q[i][3] = su; q[i][4] = sv; q[i][5] = q[i][6] = q[i][7] = q[i][8] = 1; }
        vGenVertexArrays(1, &g_pan_vao); vBindVertexArray(g_pan_vao); vGenBuffers(1, &g_pan_vbo); vBindBuffer(GL_ARRAY_BUFFER, g_pan_vbo);
        vBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
        vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
        vEnableVertexAttribArray(1); vVertexAttribPointer(1, 2, GL_FLOAT, 0, 36, (void *)12);
        vEnableVertexAttribArray(2); vVertexAttribPointer(2, 4, GL_FLOAT, 0, 36, (void *)20);
        vBindVertexArray(0);
    }
    const char *room = getenv("TTPC_VR_SCENE"); if (!room || !*room) room = "vr/naomi_office.glb";
    if (!load_room(room, 0)) return 0;
    if (!load_room("vr/morgue.glb", 1)) fprintf(stderr, "[vr] no morgue room (vr/morgue.glb)\n");
    creature_load("vr/creature.glb");
    {   // extra pictures next to the room (made from the game's own files)
        int w, h, c; u8 *px = stbi_load("vr/screensaver.png", &w, &h, &c, 4);
        if (px) { vGenTextures(1, &g_saver_tex); vBindTexture(GL_TEXTURE_2D, g_saver_tex); vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                  vGenerateMipmap(GL_TEXTURE_2D); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); free(px); }
        px = stbi_load("vr/case_report.png", &w, &h, &c, 4);
        if (px) { GLuint t; vGenTextures(1, &t); vBindTexture(GL_TEXTURE_2D, t); vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                  vGenerateMipmap(GL_TEXTURE_2D); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                  vTexParameteri(GL_TEXTURE_2D, 0x84FE, 8); free(px);
                  for (int i = 0; i < g_nmat; i++) if (g_mat[i].name_paper) g_mat[i].tex = t; g_case_tex = t; }
    }
    g_world_from_stage = m4_yaw(g_turn, g_seat[0], g_seat[1], g_seat[2]);
    return 1;
}

struct GLSave { GLint prog, vao, abuf, fb_draw, fb_read, vp[4], tex0, tex1, active; GLboolean depth, blend, cull, scissor, srgb, dmask; GLboolean cmask[4]; };
static void gl_save(GLSave *s) {
    vGetIntegerv(GL_CURRENT_PROGRAM, &s->prog); vGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &s->vao); vGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->abuf);
    vGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &s->fb_draw); vGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &s->fb_read);
    vGetIntegerv(GL_VIEWPORT, s->vp); vGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    vActiveTexture(GL_TEXTURE0); vGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex0); vActiveTexture(GL_TEXTURE1); vGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex1);
    s->depth = vIsEnabled(GL_DEPTH_TEST); s->blend = vIsEnabled(GL_BLEND); s->cull = vIsEnabled(GL_CULL_FACE); s->scissor = vIsEnabled(GL_SCISSOR_TEST); s->srgb = vIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLint dm; vGetIntegerv(GL_DEPTH_WRITEMASK, &dm); s->dmask = (GLboolean)dm;
    GLint cm[4]; vGetIntegerv(GL_COLOR_WRITEMASK, cm); for (int i = 0; i < 4; i++) s->cmask[i] = (GLboolean)cm[i];
}
static void gl_restore(const GLSave *s) {
    vUseProgram(s->prog); vBindVertexArray(s->vao); vBindBuffer(GL_ARRAY_BUFFER, s->abuf);
    vBindFramebuffer(0x8CA9, s->fb_draw); vBindFramebuffer(0x8CA8, s->fb_read);
    vViewport(s->vp[0], s->vp[1], s->vp[2], s->vp[3]);
    vActiveTexture(GL_TEXTURE1); vBindTexture(GL_TEXTURE_2D, s->tex1); vActiveTexture(GL_TEXTURE0); vBindTexture(GL_TEXTURE_2D, s->tex0); vActiveTexture(s->active);
    (s->depth ? vEnable : vDisable)(GL_DEPTH_TEST); (s->blend ? vEnable : vDisable)(GL_BLEND); (s->cull ? vEnable : vDisable)(GL_CULL_FACE);
    (s->scissor ? vEnable : vDisable)(GL_SCISSOR_TEST); (s->srgb ? vEnable : vDisable)(GL_FRAMEBUFFER_SRGB);
    vDepthMask(s->dmask); vColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
}
static void gl_chk(const char *w) { static int n; GLenum e; while ((e = vGetError()) && n < 40) { n++; fprintf(stderr, "[vr] gl error 0x%04X at %s\n", e, w); } }
static void draw_state(void) {
    vEnable(GL_DEPTH_TEST); vDepthFunc(GL_LESS); vDepthMask(1); vDisable(GL_BLEND); vDisable(GL_CULL_FACE); vDisable(GL_SCISSOR_TEST);
    vColorMask(1, 1, 1, 1); vEnable(GL_FRAMEBUFFER_SRGB);
}

// ---------------------------------------------------------------- preview (no headset): the left eye in the desktop window
static GLuint g_pv_fbo, g_pv_col, g_pv_dep; static int g_pv_w, g_pv_h;
static int preview_frame(void) {
    SDL_Window *win = SDL_GL_GetCurrentWindow(); int ww, wh; SDL_GL_GetDrawableSize(win, &ww, &wh);
    if (ww != g_pv_w || wh != g_pv_h) {
        if (!g_pv_fbo) { vGenFramebuffers(1, &g_pv_fbo); vGenTextures(1, &g_pv_col); vGenRenderbuffers(1, &g_pv_dep); }
        vBindTexture(GL_TEXTURE_2D, g_pv_col); vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, ww, wh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        vBindRenderbuffer(GL_RENDERBUFFER, g_pv_dep); vRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ww, wh);
        vBindFramebuffer(GL_FRAMEBUFFER, g_pv_fbo); vFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_pv_col, 0);
        vFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_pv_dep);
        g_pv_w = ww; g_pv_h = wh;
    }
    // desktop controls: mouse looks (captured; F2 frees it), WASD walks, left mouse = trigger
    static int init; static float px_, pz_, yaw, pitch = -0.15f; static int captured = 1, f2_prev;
    if (!init) { init = 1; px_ = g_seat[0] + 0.1f; pz_ = g_seat[2]; yaw = g_turn; SDL_SetRelativeMouseMode(SDL_TRUE); SDL_GetRelativeMouseState(NULL, NULL); }
    if (g_arrive) { g_arrive = 0; px_ = g_seat[0]; pz_ = g_seat[2]; yaw = g_turn; }
    {   static int once; if (!once) { once = 1; const char *e = getenv("TTPC_VR_START");   // tests: where the preview camera starts (x,z,yaw)
            if (e) sscanf(e, "%f,%f,%f", &px_, &pz_, &yaw);
            if (getenv("TTPC_VR_NICHE")) g_niche_open = 1; } }
    const u8 *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_F2] && !f2_prev) { captured = !captured; SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE); }
    f2_prev = k[SDL_SCANCODE_F2];
    int dx = 0, dy = 0; u32 mb = SDL_GetRelativeMouseState(&dx, &dy);
    static int lmb_prev;
    if (g_mouse_held == 2) {   // holding the desk mouse: your mouse slides it; right-drag looks around; walk away (or T) to let go
        if (!(mb & SDL_BUTTON_RMASK)) { g_mouse_off[1] -= dx * 0.0004f; g_mouse_off[0] += dy * 0.0004f; dx = dy = 0; }
        float mc[3]; for (int c = 0; c < 3; c++) mc[c] = (g_obj_min[3][c] + g_obj_max[3][c]) / 2;
        float ddx = px_ - (mc[0] + g_mouse_off[0]), ddz = pz_ - (mc[2] + g_mouse_off[1]);
        (void)ddx; (void)ddz;
        if (k[SDL_SCANCODE_T] && g_desk_now && g_auto_exit == 0) { g_vr_close_comp = 1; g_auto_exit = -90; }
    } else if (0) {
        Ray er; float el[3] = { 0, 0, 0 }, fl[3] = { 0, 0, -1 }; m4_point(g_head, el, er.o); m4_dir(g_head, fl, er.d); er.valid = 1;
        float mn[3], mx[3], d; for (int c = 0; c < 3; c++) { mn[c] = g_obj_min[3][c] + (c == 0 ? g_mouse_off[0] : c == 2 ? g_mouse_off[1] : 0); mx[c] = g_obj_max[3][c] + (c == 0 ? g_mouse_off[0] : c == 2 ? g_mouse_off[1] : 0); }
        if (ray_box(&er, mn, mx, 0.03f, &d) && d < 1.5f) { g_mouse_held = 2; fprintf(stderr, "[vr] took the mouse (T lets go)\n"); }
    }
    lmb_prev = (mb & SDL_BUTTON_LMASK) != 0;
    if (g_mouse_off[0] < -0.12f) g_mouse_off[0] = -0.12f; if (g_mouse_off[0] > 0.12f) g_mouse_off[0] = 0.12f;
    if (g_mouse_off[1] < -0.16f) g_mouse_off[1] = -0.16f; if (g_mouse_off[1] > 0.16f) g_mouse_off[1] = 0.16f;
    if (captured) { yaw -= dx * 0.0025f; pitch -= dy * 0.0025f; if (pitch > 1.4f) pitch = 1.4f; if (pitch < -1.4f) pitch = -1.4f; }
    float fx = -sinf(yaw), fz = -cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw), mv = 1.4f / 60;
    float wx = (k[SDL_SCANCODE_W] ? 1.f : 0.f) - (k[SDL_SCANCODE_S] ? 1.f : 0.f), wr = (k[SDL_SCANCODE_D] ? 1.f : 0.f) - (k[SDL_SCANCODE_A] ? 1.f : 0.f);
    px_ += (fx * wx + rx * wr) * mv; pz_ += (fz * wx + rz * wr) * mv;
    // stay inside the office (and its door alcove)
    if (px_ < -2.85f) px_ = -2.85f; if (px_ > 1.05f) px_ = 1.05f; if (pz_ > 0.75f) pz_ = 0.75f; if (pz_ < -7.1f) pz_ = -7.1f;
    if (pz_ < -4.45f) { if (px_ < -1.3f) px_ = -1.3f; if (px_ > 0.25f) px_ = 0.25f; }
    float eyeh = k[SDL_SCANCODE_LCTRL] ? 1.15f : 1.55f;   // hold Ctrl: sit down (eye height)
    M4 head = m4_yaw(yaw, px_, eyeh, pz_);
    M4 pit = m4_id(); pit.m[5] = cosf(pitch); pit.m[6] = sinf(pitch); pit.m[9] = -sinf(pitch); pit.m[10] = cosf(pitch);
    head = m4_mul(head, pit); g_head = head; g_head_ok = 1; place_pane();
    float t = tanf(32 * (float)M_PI / 180), a = (float)ww / wh;
    M4 proj = m4_frustum(-t * a, t * a, -t, t, 0.03f, 50.f), view = m4_rigid_inv(head);
    // the laser comes from a hand below-right of the eye and goes to whatever the crosshair (screen centre) is on
    float el[3] = { 0, 0, 0 }, ew[3], fwl[3] = { 0, 0, -1 }, fw[3]; m4_point(head, el, ew); m4_dir(head, fwl, fw);
    float ol[3] = { 0.16f, -0.22f, -0.2f }, o[3]; m4_point(head, ol, o);
    Ray er; memcpy(er.o, ew, 12); memcpy(er.d, fw, 12); er.valid = 1;
    float hx, hy, dist, target[3];
    int on = ray_screen(&er, g_gw, g_gh, &hx, &hy, &dist);
    for (int i = 0; i < 3; i++) target[i] = ew[i] + fw[i] * (on ? dist : 2.0f);
    float d[3] = { target[0] - o[0], target[1] - o[1], target[2] - o[2] }; float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) { g_ray.o[i] = o[i]; g_ray.d[i] = d[i] / l; }
    g_ray.valid = 1;
    g_hand[1] = head; for (int i = 0; i < 3; i++) g_hand[1].m[12 + i] = o[i];
    for (int i = 0; i < 3; i++) { g_hand[1].m[8 + i] = -g_ray.d[i]; }
    g_hand_ok[1] = 1;
    vBindFramebuffer(GL_FRAMEBUFFER, g_pv_fbo); vViewport(0, 0, ww, wh); draw_state();
    vClearColor(0.02f, 0.01f, 0.01f, 1); vClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    float rpx, rpy, rd; g_ray_on_screen = ray_screen(&g_ray, g_gw, g_gh, &rpx, &rpy, &rd); g_ray_len = g_ray_on_screen ? rd : 1.5f;
    gl_chk("pv setup"); draw_scene(proj, view, g_game_tex, g_gw, g_gh); gl_chk("pv scene");
    { GLint vpv[4]; vGetIntegerv(GL_VIEWPORT, vpv); vr_shot_save(vpv[2], vpv[3]); }
    if (!g_ray_on_screen) {   // crosshair when the laser isn't on the monitor
        M4 vp = m4_mul(proj, view), at = head; for (int i = 0; i < 3; i++) at.m[12 + i] = ew[i] + fw[i] * 0.5f;
        static const float wc[4] = { 0.9f, 0.9f, 0.9f, 1 };
        vDisable(GL_DEPTH_TEST); vUseProgram(g_lineprog); vBindVertexArray(g_linevao); vBindBuffer(GL_ARRAY_BUFFER, g_linevbo);
        vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 12, (void *)0); draw_box(vp, at, 0.0018f, 0.0018f, 0.0005f, wc); vBindVertexArray(0);
    }
    vDisable(GL_FRAMEBUFFER_SRGB);
    vBindFramebuffer(0x8CA8, g_pv_fbo); vBindFramebuffer(0x8CA9, 0);
    vBlitFramebuffer(0, 0, ww, wh, 0, 0, ww, wh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return 1;
}

#ifdef TTPC_OPENXR
// ---------------------------------------------------------------- OpenXR
static XrInstance g_xi; static XrSystemId g_sys; static XrSession g_xs; static XrSpace g_stage, g_aim[2];
static XrSessionState g_state; static int g_running;
static XrSwapchain g_sc[2]; static int g_scw[2], g_sch[2]; static XrSwapchainImageOpenGLKHR *g_scimg[2]; static uint32_t g_sc_count[2];
static GLuint g_fbo, g_depth[2];
static XrActionSet g_aset; static XrAction a_aim, a_trig, a_grip, a_a, a_b, a_stick, a_click; static XrPath g_hp[2];
static XrViewConfigurationView g_vcv[2];
static int g_trig[2], g_gripb[2], g_ab[2], g_bb[2], g_click_prev, g_turn_ready = 1;
static float g_stick[2][2];
static int g_point_hand = 1;   // right by default; pressing the other trigger switches

#define XRC(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { char s_[XR_MAX_RESULT_STRING_SIZE] = "?"; if (g_xi) xrResultToString(g_xi, r_, s_); fprintf(stderr, "[vr] %s failed: %s\n", #x, s_); return 0; } } while (0)

static XrPath path_(const char *s) { XrPath p = 0; xrStringToPath(g_xi, s, &p); return p; }
static XrAction action(const char *name, XrActionType t) {
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO }; ci.actionType = t; ci.countSubactionPaths = 2; ci.subactionPaths = g_hp;
    strcpy(ci.actionName, name); strcpy(ci.localizedActionName, name); XrAction a = XR_NULL_HANDLE; xrCreateAction(g_aset, &ci, &a); return a;
}
static void bind(const char *profile, const char **pairs, int n, XrAction *acts) {
    XrActionSuggestedBinding b[32]; for (int i = 0; i < n; i++) { b[i].action = acts[i]; b[i].binding = path_(pairs[i]); }
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING }; sb.interactionProfile = path_(profile);
    sb.suggestedBindings = b; sb.countSuggestedBindings = n;
    XrResult r = xrSuggestInteractionProfileBindings(g_xi, &sb); if (XR_FAILED(r)) fprintf(stderr, "[vr] bindings for %s refused (%d)\n", profile, r);
}

static int xr_init(void) {
    const char *ext[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO }; strcpy(ici.applicationInfo.applicationName, "Trauma Team PC");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0); ici.enabledExtensionCount = 1; ici.enabledExtensionNames = ext;
    XRC(xrCreateInstance(&ici, &g_xi));
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRC(xrGetSystem(g_xi, &sgi, &g_sys));
    PFN_xrGetOpenGLGraphicsRequirementsKHR req_fn = NULL;
    XRC(xrGetInstanceProcAddr(g_xi, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req_fn));
    XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR }; XRC(req_fn(g_xi, g_sys, &req));
    fprintf(stderr, "[vr] runtime wants OpenGL %d.%d or newer\n", XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported));
    XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR }; gb.hDC = wglGetCurrentDC(); gb.hGLRC = wglGetCurrentContext();
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = g_sys;
    XRC(xrCreateSession(g_xi, &sci, &g_xs));
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE; rs.poseInReferenceSpace.orientation.w = 1;
    if (XR_FAILED(xrCreateReferenceSpace(g_xs, &rs, &g_stage))) {   // no play area: standing height from the floor guess
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.position.y = -1.2f; XRC(xrCreateReferenceSpace(g_xs, &rs, &g_stage));
    }
    // swapchains, one per eye, sRGB
    uint32_t n = 2; XRC(xrEnumerateViewConfigurationViews(g_xi, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, g_vcv));
    int64_t fmts[64]; uint32_t nf = 0; XRC(xrEnumerateSwapchainFormats(g_xs, 64, &nf, fmts));
    int64_t fmt = GL_SRGB8_ALPHA8; int found = 0; for (uint32_t i = 0; i < nf; i++) if (fmts[i] == GL_SRGB8_ALPHA8) found = 1;
    if (!found) { fmt = fmts[0]; fprintf(stderr, "[vr] sRGB swapchain not offered, using 0x%llx\n", (long long)fmt); }
    vGenFramebuffers(1, &g_fbo);
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO }; ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; ci.format = fmt;
        ci.sampleCount = 1; ci.width = g_vcv[e].recommendedImageRectWidth; ci.height = g_vcv[e].recommendedImageRectHeight; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        XRC(xrCreateSwapchain(g_xs, &ci, &g_sc[e])); g_scw[e] = ci.width; g_sch[e] = ci.height;
        XRC(xrEnumerateSwapchainImages(g_sc[e], 0, &g_sc_count[e], NULL));
        g_scimg[e] = calloc(g_sc_count[e], sizeof **g_scimg); for (uint32_t i = 0; i < g_sc_count[e]; i++) g_scimg[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        XRC(xrEnumerateSwapchainImages(g_sc[e], g_sc_count[e], &g_sc_count[e], (XrSwapchainImageBaseHeader *)g_scimg[e]));
        vGenRenderbuffers(1, &g_depth[e]); vBindRenderbuffer(GL_RENDERBUFFER, g_depth[e]); vRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ci.width, ci.height);
        fprintf(stderr, "[vr] eye %d: %dx%d\n", e, ci.width, ci.height);
    }
    // input
    g_hp[0] = path_("/user/hand/left"); g_hp[1] = path_("/user/hand/right");
    XrActionSetCreateInfo as = { XR_TYPE_ACTION_SET_CREATE_INFO }; strcpy(as.actionSetName, "game"); strcpy(as.localizedActionSetName, "Game");
    XRC(xrCreateActionSet(g_xi, &as, &g_aset));
    a_aim = action("aim", XR_ACTION_TYPE_POSE_INPUT); a_trig = action("trigger", XR_ACTION_TYPE_FLOAT_INPUT); a_grip = action("grip", XR_ACTION_TYPE_FLOAT_INPUT);
    a_a = action("a", XR_ACTION_TYPE_BOOLEAN_INPUT); a_b = action("b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a_stick = action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT); a_click = action("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    {   const char *p[] = { "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose", "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value",
                            "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value", "/user/hand/left/input/a/click", "/user/hand/right/input/a/click",
                            "/user/hand/left/input/b/click", "/user/hand/right/input/b/click", "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick",
                            "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click" };
        XrAction a[] = { a_aim, a_aim, a_trig, a_trig, a_grip, a_grip, a_a, a_a, a_b, a_b, a_stick, a_stick, a_click, a_click };
        bind("/interaction_profiles/valve/index_controller", p, 14, a); }
    {   const char *p[] = { "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose", "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value",
                            "/user/hand/left/input/squeeze/click", "/user/hand/right/input/squeeze/click", "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick",
                            "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/x/click", "/user/hand/right/input/a/click",
                            "/user/hand/left/input/y/click", "/user/hand/right/input/b/click" };
        XrAction a[] = { a_aim, a_aim, a_trig, a_trig, a_grip, a_grip, a_stick, a_stick, a_click, a_click, a_a, a_a, a_b, a_b };
        bind("/interaction_profiles/oculus/touch_controller", p, 14, a); }
    {   const char *p[] = { "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose", "/user/hand/left/input/select/click", "/user/hand/right/input/select/click",
                            "/user/hand/left/input/menu/click", "/user/hand/right/input/menu/click" };
        XrAction a[] = { a_aim, a_aim, a_trig, a_trig, a_b, a_b };
        bind("/interaction_profiles/khr/simple_controller", p, 6, a); }
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO }; at.countActionSets = 1; at.actionSets = &g_aset;
    XRC(xrAttachSessionActionSets(g_xs, &at));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc = { XR_TYPE_ACTION_SPACE_CREATE_INFO }; sc.action = a_aim; sc.subactionPath = g_hp[h]; sc.poseInActionSpace.orientation.w = 1;
        XRC(xrCreateActionSpace(g_xs, &sc, &g_aim[h]));
    }
    fprintf(stderr, "[vr] OpenXR ready (put the headset on; SteamVR shows the room)\n");
    return 1;
}

static void xr_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(g_xi, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state = ((XrEventDataSessionStateChanged *)&ev)->state;
            if (g_state == XR_SESSION_STATE_READY) { XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; if (XR_SUCCEEDED(xrBeginSession(g_xs, &bi))) g_running = 1; }
            if (g_state == XR_SESSION_STATE_STOPPING) { xrEndSession(g_xs); g_running = 0; }
            if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) { g_running = 0; g_active = 0; fprintf(stderr, "[vr] session ended\n"); }
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static void xr_input(XrTime t) {
    XrActiveActionSet aas = { g_aset, XR_NULL_PATH }; XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO }; si.countActiveActionSets = 1; si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g_xs, &si))) return;
    for (int h = 0; h < 2; h++) {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO }; gi.subactionPath = g_hp[h];
        XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT }; XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN }; XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
        gi.action = a_trig; xrGetActionStateFloat(g_xs, &gi, &f); int trig = f.isActive && f.currentState > (g_trig[h] ? 0.45f : 0.6f);
        if (trig && !g_trig[h]) g_point_hand = h;   // pull a trigger: that hand points
        g_trig[h] = trig;
        gi.action = a_grip; xrGetActionStateFloat(g_xs, &gi, &f); g_gripb[h] = f.isActive && f.currentState > 0.7f;
        gi.action = a_a; xrGetActionStateBoolean(g_xs, &gi, &b); g_ab[h] = b.isActive && b.currentState;
        gi.action = a_b; xrGetActionStateBoolean(g_xs, &gi, &b); g_bb[h] = b.isActive && b.currentState;
        gi.action = a_stick; xrGetActionStateVector2f(g_xs, &gi, &v); g_stick[h][0] = v.isActive ? v.currentState.x : 0; g_stick[h][1] = v.isActive ? v.currentState.y : 0;
        if (h == 1) { gi.action = a_click; xrGetActionStateBoolean(g_xs, &gi, &b); int c = b.isActive && b.currentState;
            if (c && !g_click_prev) { g_turn = (float)(M_PI / 2); g_world_from_stage = m4_yaw(g_turn, g_seat[0], g_seat[1], g_seat[2]); }   // back to the chair
            g_click_prev = c; }
        XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
        g_hand_ok[h] = XR_SUCCEEDED(xrLocateSpace(g_aim[h], g_stage, t, &loc)) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (g_hand_ok[h]) g_hand[h] = m4_mul(g_world_from_stage, m4_pose(&loc.pose.orientation.x, &loc.pose.position.x));
        if (g_hand_ok[h] && g_obj_ok[3]) {   // grip near the desk mouse: hold it; it slides on the tray with your hand
            float hp[3] = { g_hand[h].m[12], g_hand[h].m[13], g_hand[h].m[14] };
            float mc[3]; for (int c = 0; c < 3; c++) mc[c] = (g_obj_min[3][c] + g_obj_max[3][c]) / 2;
            mc[0] += g_mouse_off[0]; mc[2] += g_mouse_off[1];
            float d2 = (hp[0] - mc[0]) * (hp[0] - mc[0]) + (hp[1] - mc[1]) * (hp[1] - mc[1]) + (hp[2] - mc[2]) * (hp[2] - mc[2]);
            if (g_mouse_held < 0 && g_gripb[h] && d2 < 0.12f * 0.12f) { g_mouse_held = h; memcpy(g_mouse_grab, hp, 12); memcpy(g_mouse_grab_off, g_mouse_off, 8); }
            if (g_mouse_held == h) {
                { g_mouse_off[0] = g_mouse_grab_off[0] + hp[0] - g_mouse_grab[0]; g_mouse_off[1] = g_mouse_grab_off[1] + hp[2] - g_mouse_grab[2]; }
                if (g_mouse_off[0] < -0.12f) g_mouse_off[0] = -0.12f; if (g_mouse_off[0] > 0.12f) g_mouse_off[0] = 0.12f;
                if (g_mouse_off[1] < -0.16f) g_mouse_off[1] = -0.16f; if (g_mouse_off[1] > 0.16f) g_mouse_off[1] = 0.16f;
            }
        }
    }
    // right stick: 30 degree snap turns about the head; left stick: walk where the head looks
    float sx = g_stick[1][0];
    if (fabsf(sx) < 0.3f) g_turn_ready = 1;
    else if (g_turn_ready && fabsf(sx) > 0.7f) {
        g_turn_ready = 0; float da = sx > 0 ? -(float)M_PI / 6 : (float)M_PI / 6;
        XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION }; XrReferenceSpaceCreateInfo vs = { 0 }; (void)vs;
        float pivot[3] = { g_world_from_stage.m[12], 0, g_world_from_stage.m[14] };
        (void)hl;
        M4 r = m4_yaw(da, 0, 0, 0), tp = m4_yaw(0, pivot[0], 0, pivot[2]), tn = m4_yaw(0, -pivot[0], 0, -pivot[2]);
        g_world_from_stage = m4_mul(tp, m4_mul(r, m4_mul(tn, g_world_from_stage)));
    }
    float lx = g_stick[0][0], ly = g_stick[0][1];
    if (lx * lx + ly * ly > 0.04f && g_hand_ok[0]) {
        float fwd[3] = { -g_world_from_stage.m[8], 0, -g_world_from_stage.m[10] }, right[3] = { g_world_from_stage.m[0], 0, g_world_from_stage.m[2] };
        const float speed = 1.4f / 60;
        g_world_from_stage.m[12] += (fwd[0] * ly + right[0] * lx) * speed; g_world_from_stage.m[14] += (fwd[2] * ly + right[2] * lx) * speed;
    }
    int h = g_point_hand;
    if (g_hand_ok[h]) {
        float o[3] = { 0, 0, 0 }, d[3] = { 0, 0, -1 };
        m4_point(g_hand[h], o, g_ray.o); m4_dir(g_hand[h], d, g_ray.d); g_ray.valid = 1;
    } else g_ray.valid = 0;
}

static int xr_frame(void) {
    xr_events();
    if (!g_running) return 0;
    XrFrameState fs = { XR_TYPE_FRAME_STATE }; XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    if (XR_FAILED(xrWaitFrame(g_xs, &wi, &fs))) return 0;
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO }; if (XR_FAILED(xrBeginFrame(g_xs, &bi))) return 0;
    xr_input(fs.predictedDisplayTime);
    float px, py, dist; g_ray_on_screen = ray_screen(&g_ray, g_gw, g_gh, &px, &py, &dist); g_ray_len = g_ray_on_screen ? dist : 1.5f;
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    int submit = 0;
    if (fs.shouldRender) {
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } }; XrViewState vst = { XR_TYPE_VIEW_STATE }; uint32_t nv = 2;
        XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO }; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = fs.predictedDisplayTime; li.space = g_stage;
        if (XR_SUCCEEDED(xrLocateViews(g_xs, &li, &vst, 2, &nv, views)) && (vst.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            draw_state();
            for (int e = 0; e < 2; e++) {
                uint32_t idx; XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO }; xrAcquireSwapchainImage(g_sc[e], &ai, &idx);
                XrSwapchainImageWaitInfo wi2 = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi2.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(g_sc[e], &wi2);
                vBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
                vFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_scimg[e][idx].image, 0);
                vFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth[e]);
                vViewport(0, 0, g_scw[e], g_sch[e]); vClearColor(0.02f, 0.01f, 0.01f, 1); vClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                M4 eye = m4_mul(g_world_from_stage, m4_pose(&views[e].pose.orientation.x, &views[e].pose.position.x));
                if (e == 0) { g_head = eye; g_head_ok = 1; place_pane(); }
                XrFovf f = views[e].fov;
                M4 proj = m4_frustum(tanf(f.angleLeft), tanf(f.angleRight), tanf(f.angleDown), tanf(f.angleUp), 0.03f, 50.f);
                draw_scene(proj, m4_rigid_inv(eye), g_game_tex, g_gw, g_gh);
                if (e == 0) vr_shot_save(g_scw[e], g_sch[e]);
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO }; xrReleaseSwapchainImage(g_sc[e], &ri);
                pv[e].pose = views[e].pose; pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = g_sc[e]; pv[e].subImage.imageRect.extent.width = g_scw[e]; pv[e].subImage.imageRect.extent.height = g_sch[e];
            }
            layer.space = g_stage; layer.viewCount = 2; layer.views = pv; submit = 1;
        }
    }
    const XrCompositionLayerBaseHeader *layers[1] = { (const XrCompositionLayerBaseHeader *)&layer };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO }; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = submit ? 1 : 0; ei.layers = submit ? layers : NULL;
    xrEndFrame(g_xs, &ei);
    return 1;
}
#endif

// ---------------------------------------------------------------- per draw (gl.c): leave out what the room replaces
// Naomi's office painting (sprite/bg/mira_jishitu.tpx, 640x480 CI8): recognised by its texel data
#define OFFICE_HASH 0x2E0252893EFEE5CCull   // as the runtime hashes it (video.c tex_data_hash)
static int office_picture(TexEntry *e) {
    static u64 seen[16]; static int ns;
    int known = 0; for (int k = 0; k < ns; k++) if (seen[k] == e->data_hash) known = 1;
    if (!known && ns < 16) { seen[ns++] = e->data_hash; fprintf(stderr, "[vr] 640x480 picture %08X data %016llX%s\n", e->addr, (unsigned long long)e->data_hash, e->data_hash == OFFICE_HASH ? " = Naomi's office" : ""); }
    return e->data_hash == OFFICE_HASH;
}

// ---------------------------------------------------------------- the morgue: the body and the personal effects
// The game draws them only while you examine them (a floating model in a dark tunnel). VR takes them from those
// draws: model-space triangles (kept once) and, every frame, where each vertex lands on the game's screen. They
// lie on their beds in the morgue, turned the way the game is looking at them (the side the game sees faces up),
// and touching a spot clicks exactly that spot in the game's view. disc.c says where the two model files sit in
// memory, so their draws are told apart from everything else by their textures.
extern u32 g_vr_mdl_lo[2], g_vr_mdl_hi[2];
typedef struct {
    int ntri; float *pos, *nrm, *uv;   // per triangle corner, in the model's reference frame
    float *scr; u8 *vis;               // per corner: game screen x/y this frame, w > 0
    GLuint tex, vao, vbo; float *cpu; int frame;
} MPart;
typedef struct {
    MPart part[48]; int nparts, cur, frame;
    float ref[12], ref_inv[12];        // the first part's model->view matrix this frame (and its inverse)
    float lo[3], hi[3]; int axis;      // bounds in the reference frame, longest axis
    float R[9], scale, T[3]; int placed; float lit_R[9];
    float camd;                        // the game's camera distance this frame (it zooms in on a clicked spot)
} MModel;
static MModel g_mdl[2];
static int g_vr_frame_no, g_exam_seen, g_exam_now, g_exam_hold;   // examining: 1 body, 2 personal effects
// examining: the room fades into a dark void and the model floats in front of you, posed exactly as the game sees it
// (the game's view space, anchored where your head was when it opened, scaled so the model is ~0.9 m away)
static float g_exam_fade; static M4 g_exam_A; static float g_exam_k; static int g_exam_anchored, g_exam_exit_cd, g_scripted;
static int g_vr_morgue_seen, g_morgue_now, g_morgue_hold;
static int g_hit_part, g_hit_tri; static float g_hit_b[2];   // the last spot the laser found on the model
static int g_bubble_seen, g_bubble_now;                      // a speech bubble is up (its frame was drawn)
static int g_stick_cmd;                                      // examining: the Nunchuk stick left/right the grab is holding (-1/0/1)
static float g_chk_ref[12]; static int g_chk_frozen;          // the model's pose when you tapped (the game's zoom swings its camera)
// a tap checks the spot directly: the game zooms in on a click, so VR clicks again on the same spot once it has
// zoomed (CHECK), and when the remark is over backs out of the close-up (B)
static int g_chk_st, g_chk_t, g_chk_part, g_chk_tri, g_chk_quiet, g_press_b; static float g_chk_b[2], g_chk_d0, g_chk_prevd;
static float g_mk[8][2]; static int g_nmk, g_nmk_last; static float g_mk_last[8][2];   // the morgue's hotspot markers (centres)


static int in_mdl(u32 addr, int k) { addr &= 0x1FFFFFFF; return g_vr_mdl_hi[k] && addr >= g_vr_mdl_lo[k] && addr < g_vr_mdl_hi[k]; }
int vr_track(const Setup *s) {
    if (!g_active) return -1;
    for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (!e) continue; for (int k = 0; k < 2; k++) if (in_mdl(e->addr, k)) return k; }
    return g_exam_now ? 2 : -1;   // while examining, everything else in 3D is the game's examination background
}
static void affine_inv(const float a[12], float o[12]) {   // inverse of [R|t] (3x4 rows)
    float r[9] = { a[0], a[1], a[2], a[4], a[5], a[6], a[8], a[9], a[10] };
    float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
    if (fabsf(det) < 1e-12f) det = 1e-12f; float id = 1.f / det, inv[9];
    inv[0] = (r[4] * r[8] - r[5] * r[7]) * id; inv[1] = (r[2] * r[7] - r[1] * r[8]) * id; inv[2] = (r[1] * r[5] - r[2] * r[4]) * id;
    inv[3] = (r[5] * r[6] - r[3] * r[8]) * id; inv[4] = (r[0] * r[8] - r[2] * r[6]) * id; inv[5] = (r[2] * r[3] - r[0] * r[5]) * id;
    inv[6] = (r[3] * r[7] - r[4] * r[6]) * id; inv[7] = (r[1] * r[6] - r[0] * r[7]) * id; inv[8] = (r[0] * r[4] - r[1] * r[3]) * id;
    for (int i = 0; i < 3; i++) { o[i * 4] = inv[i * 3]; o[i * 4 + 1] = inv[i * 3 + 1]; o[i * 4 + 2] = inv[i * 3 + 2];
        o[i * 4 + 3] = -(inv[i * 3] * a[3] + inv[i * 3 + 1] * a[7] + inv[i * 3 + 2] * a[11]); }
}
static void aff_pt(const float a[12], const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a[i * 4] * p[0] + a[i * 4 + 1] * p[1] + a[i * 4 + 2] * p[2] + a[i * 4 + 3]; }
static void aff_dir(const float a[12], const float p[3], float o[3]) { for (int i = 0; i < 3; i++) o[i] = a[i * 4] * p[0] + a[i * 4 + 1] * p[1] + a[i * 4 + 2] * p[2]; }
static void aff_mul(const float a[12], const float b[12], float o[12]) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++)
        o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (j == 3 ? a[i * 4 + 3] : 0);
}
// the examination's own background (the game's swirling tunnel): taken fresh every frame, in the game's view space
typedef struct { float *v; int nv, cap; TexEntry *e; GLuint vao, vbo; } Scn;
static Scn g_scenery[160]; static int g_nscenery, g_scn_frame;
static struct { TexEntry *e; u32 ver; GLuint tex; } g_scn_tex[24];
static GLuint scn_texture(TexEntry *e) {
    if (!e || !e->px) return g_white;
    int k = -1; for (int i = 0; i < 24; i++) if (g_scn_tex[i].e == e) { k = i; break; }
    if (k < 0) { for (int i = 0; i < 24; i++) if (!g_scn_tex[i].e) { k = i; break; } if (k < 0) k = 0; g_scn_tex[k].e = e; g_scn_tex[k].ver = ~0u; if (!g_scn_tex[k].tex) vGenTextures(1, &g_scn_tex[k].tex); }
    if (g_scn_tex[k].ver != e->version) {
        g_scn_tex[k].ver = e->version; size_t np = (size_t)e->w * e->h; u8 *px = malloc(np * 4);
        for (size_t i = 0; i < np; i++) { u32 c = e->px[i]; px[i * 4] = c >> 24; px[i * 4 + 1] = c >> 16; px[i * 4 + 2] = c >> 8; px[i * 4 + 3] = c; }
        vBindTexture(GL_TEXTURE_2D, g_scn_tex[k].tex); vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, e->w, e->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT); free(px);
    }
    return g_scn_tex[k].tex;
}
static void scn_capture(const Setup *s, int prim, int n, const VrVert *v, const float M[12]) {
    if (n < 3 || v[0].w == 1.f || !s->tex[0].e) return;   // flat interface pieces stay on the pane; untextured cards (the black backdrop) are left out
    if (g_scn_frame != g_vr_frame_no) { g_scn_frame = g_vr_frame_no; g_nscenery = 0; }
    if (g_nscenery >= 160) return;
    Scn *sc = &g_scenery[g_nscenery++]; int need = n * 6 * 9;
    if (need > sc->cap) { free(sc->v); sc->cap = need + 512; sc->v = malloc(sizeof(float) * (size_t)sc->cap); }
    int nv = 0;
    #define CORNER(i) do { const VrVert *q = &v[i]; float *o = sc->v + nv * 9; aff_pt(M, q->pos, o); o[3] = q->uv[0]; o[4] = q->uv[1]; memcpy(o + 5, q->col, 16); nv++; } while (0)
    switch (prim) {
    case 0: case 1: for (int k = 0; k + 3 < n; k += 4) { CORNER(k); CORNER(k + 1); CORNER(k + 2); CORNER(k); CORNER(k + 2); CORNER(k + 3); } break;
    case 2: for (int k = 0; k + 2 < n; k += 3) { CORNER(k); CORNER(k + 1); CORNER(k + 2); } break;
    case 3: for (int k = 0; k + 2 < n; k++) { CORNER(k); CORNER(k + 1); CORNER(k + 2); } break;
    case 4: for (int k = 1; k + 1 < n; k++) { CORNER(0); CORNER(k); CORNER(k + 1); } break;
    default: break;
    }
    #undef CORNER
    sc->nv = nv; sc->e = s->tex[0].e;
}
static void scn_draw(void) {   // around you, in the game's view space scaled to the room (called with the room program bound)
    if (g_scn_frame < g_vr_frame_no - 2 || !g_nscenery) return;
    M4 sc = m4_id(); sc.m[0] = sc.m[5] = sc.m[10] = g_exam_k; M4 W = m4_mul(g_exam_A, sc);
    static const float one[4] = { 1, 1, 1, 1 }, zero[3] = { 0, 0, 0 };
    float base[4] = { 1, 1, 1, g_exam_fade };
    vUniformMatrix4fv(u_model, 1, 0, W.m); vUniform1i(u_screen, 3); vUniform4fv(u_base, 1, base); vUniform3fv(u_emis, 1, zero); (void)one;
    vEnable(GL_BLEND); vBlendFunc(GL_SRC_ALPHA, GL_ONE); vDisable(GL_DEPTH_TEST); vDisable(GL_CULL_FACE); vDepthMask(0);
    vActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < g_nscenery; i++) {
        Scn *q = &g_scenery[i]; if (!q->nv) continue;
        if (!q->vao) { vGenVertexArrays(1, &q->vao); vGenBuffers(1, &q->vbo); vBindVertexArray(q->vao); vBindBuffer(GL_ARRAY_BUFFER, q->vbo);
            vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
            vEnableVertexAttribArray(1); vVertexAttribPointer(1, 2, GL_FLOAT, 0, 36, (void *)12);
            vEnableVertexAttribArray(2); vVertexAttribPointer(2, 4, GL_FLOAT, 0, 36, (void *)20); }
        vBindVertexArray(q->vao); vBindBuffer(GL_ARRAY_BUFFER, q->vbo); vBufferData(GL_ARRAY_BUFFER, 36 * (size_t)q->nv, q->v, GL_STREAM_DRAW);
        vBindTexture(GL_TEXTURE_2D, scn_texture(q->e)); vDrawArrays(GL_TRIANGLES, 0, q->nv);
    }
    vDisable(GL_BLEND); vEnable(GL_DEPTH_TEST); vDepthMask(1); vUniform1i(u_screen, 0);
    M4 id = m4_id(); vUniformMatrix4fv(u_model, 1, 0, id.m);
}
void vr_capture(int which, const Setup *s, int prim, int n, const VrVert *v, const float M[12]) {
    if (!g_active) return;
    if (which == 2) { scn_capture(s, prim, n, v, M); return; }
    if (which < 0 || which > 1) return;
    MModel *md = &g_mdl[which];
    if (md->frame != g_vr_frame_no) { md->frame = g_vr_frame_no; md->cur = 0; memcpy(md->ref, M, 48); affine_inv(M, md->ref_inv); md->camd = sqrtf(M[3] * M[3] + M[7] * M[7] + M[11] * M[11]); }
    g_exam_seen = which + 1;
    if (md->cur >= 48) return;
    MPart *pt = &md->part[md->cur++];
    // the draw's triangles, in the same order the game assembles them (video.c draw_prims)
    static int *ix; static int cap; if (n * 6 > cap) { free(ix); cap = n * 6 + 64; ix = malloc(sizeof *ix * (size_t)cap); }
    int nt = 0;
    switch (prim) {
    case 0: case 1: for (int k = 0; k + 3 < n; k += 4) { int t[6] = { k, k + 1, k + 2, k, k + 2, k + 3 }; memcpy(ix + nt * 3, t, 24); nt += 2; } break;
    case 2: for (int k = 0; k + 2 < n; k += 3) { ix[nt * 3] = k; ix[nt * 3 + 1] = k + 1; ix[nt * 3 + 2] = k + 2; nt++; } break;
    case 3: for (int k = 0; k + 2 < n; k++) { if (k & 1) { ix[nt * 3] = k + 1; ix[nt * 3 + 1] = k; } else { ix[nt * 3] = k; ix[nt * 3 + 1] = k + 1; } ix[nt * 3 + 2] = k + 2; nt++; } break;
    case 4: for (int k = 1; k + 1 < n; k++) { ix[nt * 3] = 0; ix[nt * 3 + 1] = k; ix[nt * 3 + 2] = k + 1; nt++; } break;
    default: return;
    }
    if (pt->ntri != nt) {   // first time (or a different mesh): keep its shape in the reference frame
        GLSave sv; gl_save(&sv);   // the game's own GL state stays as it was
        free(pt->pos); free(pt->nrm); free(pt->uv); free(pt->scr); free(pt->vis); free(pt->cpu);
        pt->ntri = nt; pt->pos = malloc(36 * (size_t)nt); pt->nrm = malloc(36 * (size_t)nt); pt->uv = malloc(24 * (size_t)nt);
        pt->scr = malloc(24 * (size_t)nt); pt->vis = malloc(3 * (size_t)nt); pt->cpu = malloc(36 * 3 * (size_t)nt);
        float rel[12]; aff_mul(md->ref_inv, M, rel);
        for (int c = 0; c < nt * 3; c++) {
            const VrVert *q = &v[ix[c]]; aff_pt(rel, q->pos, pt->pos + c * 3); aff_dir(rel, q->nrm, pt->nrm + c * 3);
            float *nn = pt->nrm + c * 3; float l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]); if (l > 0) for (int i = 0; i < 3; i++) nn[i] /= l;
            pt->uv[c * 2] = q->uv[0]; pt->uv[c * 2 + 1] = q->uv[1];
        }
        // its texture: the biggest one the draw uses (the clothes' colour maps); the body itself is lit skin
        TexEntry *best = NULL;
        for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (e && e->px && (!best || e->w * e->h > best->w * best->h)) best = e; }   // clothes: colour map; body: its marks
        if (best) {
            u8 *px = malloc((size_t)best->w * best->h * 4);
            for (int i = 0; i < best->w * best->h; i++) { u32 c = best->px[i]; px[i * 4] = c >> 24; px[i * 4 + 1] = c >> 16; px[i * 4 + 2] = c >> 8; px[i * 4 + 3] = c; }
            if (!pt->tex) vGenTextures(1, &pt->tex);
            vBindTexture(GL_TEXTURE_2D, pt->tex); vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, best->w, best->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            vGenerateMipmap(GL_TEXTURE_2D); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT); free(px);
        }
        if (!pt->vao) { vGenVertexArrays(1, &pt->vao); vGenBuffers(1, &pt->vbo); }
        vBindVertexArray(pt->vao); vBindBuffer(GL_ARRAY_BUFFER, pt->vbo); vBufferData(GL_ARRAY_BUFFER, 36 * 3 * (size_t)nt, NULL, GL_DYNAMIC_DRAW);
        vEnableVertexAttribArray(0); vVertexAttribPointer(0, 3, GL_FLOAT, 0, 36, (void *)0);
        vEnableVertexAttribArray(1); vVertexAttribPointer(1, 2, GL_FLOAT, 0, 36, (void *)12);
        vEnableVertexAttribArray(2); vVertexAttribPointer(2, 4, GL_FLOAT, 0, 36, (void *)20);
        vBindVertexArray(0);
        if (md->cur > md->nparts) md->nparts = md->cur;
        // bounds of the whole model (reference frame)
        for (int c = 0; c < nt * 3; c++) for (int i = 0; i < 3; i++) {
            float x = pt->pos[c * 3 + i];
            if (md->hi[0] <= md->lo[0] && c == 0 && md->cur == 1) { md->lo[i] = md->hi[i] = x; }
            if (x < md->lo[i]) md->lo[i] = x; if (x > md->hi[i]) md->hi[i] = x;
        }
        float ext[3] = { md->hi[0] - md->lo[0], md->hi[1] - md->lo[1], md->hi[2] - md->lo[2] };
        md->axis = ext[0] > ext[1] ? (ext[0] > ext[2] ? 0 : 2) : (ext[1] > ext[2] ? 1 : 2);
        md->placed = 0;
        fprintf(stderr, "[vr] %s part %d: %d triangles%s\n", which ? "personal effects" : "body", md->cur - 1, nt, best ? " (textured)" : "");
        gl_restore(&sv);
    }
    for (int c = 0; c < nt * 3; c++) { const VrVert *q = &v[ix[c]]; pt->scr[c * 2] = q->sx; pt->scr[c * 2 + 1] = q->sy; pt->vis[c] = q->w > 0; }
    pt->frame = g_vr_frame_no;
}
// turn the model so the side the game is looking at faces up, its long axis along the bed (head toward the doors)
static void mdl_place(int k) {
    MModel *md = &g_mdl[k]; if (!md->nparts || !g_spot_ok[k]) return;
    float a[3] = { 0, 0, 0 }; a[md->axis] = 1;
    float c[3] = { md->ref[8], md->ref[9], md->ref[10] };   // toward the game's camera, in model space (3rd row of the view rotation)
    float cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]); if (cl < 1e-6f) return; for (int i = 0; i < 3; i++) c[i] /= cl;
    float d = c[0] * a[0] + c[1] * a[1] + c[2] * a[2]; float up[3] = { c[0] - d * a[0], c[1] - d * a[1], c[2] - d * a[2] };
    float ul = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]); if (ul < 1e-4f) return; for (int i = 0; i < 3; i++) up[i] /= ul;
    float sd[3] = { a[1] * up[2] - a[2] * up[1], a[2] * up[0] - a[0] * up[2], a[0] * up[1] - a[1] * up[0] };
    // model basis (sd, up, a) -> world (+x, +y, -z): R = Bw * Bm^T
    for (int i = 0; i < 3; i++) { md->R[i * 3 + 0] = sd[i]; md->R[i * 3 + 1] = up[i]; md->R[i * 3 + 2] = -a[i]; }   // stored as columns of Bm, used transposed below
    float L = md->hi[md->axis] - md->lo[md->axis];
    if (k == 0) md->scale = L > 0 ? 1.78f / L : 0.01f;
    else md->scale = g_mdl[0].scale > 0 ? g_mdl[0].scale : (L > 0 ? 1.6f / L : 0.01f);
    // lowest point toward the bed, centred on the bed
    float mid[3] = { (md->lo[0] + md->hi[0]) / 2, (md->lo[1] + md->hi[1]) / 2, (md->lo[2] + md->hi[2]) / 2 }, lowest = 1e9f;
    for (int p = 0; p < md->nparts; p++) { MPart *pt = &md->part[p]; for (int c = 0; c < pt->ntri * 3; c += 7) {
        float *q = pt->pos + c * 3; float h = (q[0] - mid[0]) * up[0] + (q[1] - mid[1]) * up[1] + (q[2] - mid[2]) * up[2]; if (h < lowest) lowest = h; } }
    for (int i = 0; i < 3; i++) md->T[i] = g_spot[k][i];
    md->T[1] += -lowest * md->scale + 0.005f;
    // world = T + scale * (sd.(p-mid)) x + (up.(p-mid)) y - (a.(p-mid)) z
    float w_mid[3]; (void)w_mid;
    md->T[0] -= 0; md->placed = 1;
    memcpy(md->lit_R, md->R, 36);
    (void)mid;
}
static M4 exam_matrix(int k) {   // turned as the game sees it, but always the same size, floating ~1.1 m in front of you
    MModel *md = &g_mdl[k]; M4 r = m4_id(); const float *ref = g_chk_frozen ? g_chk_ref : md->ref;
    float L = md->hi[md->axis] - md->lo[md->axis], sc = L > 0 ? 0.95f / L : 0.01f;
    float mid[3] = { (md->lo[0] + md->hi[0]) / 2, (md->lo[1] + md->hi[1]) / 2, (md->lo[2] + md->hi[2]) / 2 };
    for (int i = 0; i < 3; i++) {
        float l = sqrtf(ref[i * 4] * ref[i * 4] + ref[i * 4 + 1] * ref[i * 4 + 1] + ref[i * 4 + 2] * ref[i * 4 + 2]); if (l < 1e-6f) l = 1;
        for (int j = 0; j < 3; j++) r.m[j * 4 + i] = ref[i * 4 + j] / l * sc;
    }
    const float c[3] = { 0, -0.18f, -1.15f };
    for (int i = 0; i < 3; i++) r.m[12 + i] = c[i] - (r.m[i] * mid[0] + r.m[4 + i] * mid[1] + r.m[8 + i] * mid[2]);
    return m4_mul(g_exam_A, r);
}
static M4 mdl_matrix(int k) {   // model (reference frame) -> world, as a column-major M4
    if (g_exam_now == k + 1 && g_exam_anchored) return exam_matrix(k);
    MModel *md = &g_mdl[k]; M4 m = m4_id(); float s = md->scale;
    float mid[3] = { (md->lo[0] + md->hi[0]) / 2, (md->lo[1] + md->hi[1]) / 2, (md->lo[2] + md->hi[2]) / 2 };
    // rows of the rotation: world x = sd, world y = up, world z = -a  (R holds sd, up, -a as columns -> those are the rows of Bm^T)
    for (int j = 0; j < 3; j++) { m.m[j * 4 + 0] = md->R[j * 3 + 0] * s; m.m[j * 4 + 1] = md->R[j * 3 + 1] * s; m.m[j * 4 + 2] = md->R[j * 3 + 2] * s; }
    for (int i = 0; i < 3; i++) m.m[12 + i] = md->T[i] - (m.m[i] * mid[0] + m.m[4 + i] * mid[1] + m.m[8 + i] * mid[2]);
    return m;
}
static void mdl_draw(int k) {
    MModel *md = &g_mdl[k]; if (!md->nparts || (!md->placed && !(g_exam_now == k + 1 && g_exam_anchored))) return;
    M4 W = mdl_matrix(k); vUniformMatrix4fv(u_model, 1, 0, W.m);
    static const float one[4] = { 1, 1, 1, 1 }, zero[3] = { 0, 0, 0 }, skin[4] = { 0.86f, 0.74f, 0.66f, 1 };
    vUniform1i(u_screen, 0); vUniform3fv(u_emis, 1, zero); vUniform1f(u_cut, 0.3f);
    { static const float no[3] = { 0, 0, 0 }; vUniform3fv(u_lit, 1, no); }
    vActiveTexture(GL_TEXTURE1); vBindTexture(GL_TEXTURE_2D, g_white); vActiveTexture(GL_TEXTURE0);
    const float Ld[3] = { 0.25f, 0.9f, -0.35f }; float ll = sqrtf(Ld[0] * Ld[0] + Ld[1] * Ld[1] + Ld[2] * Ld[2]);
    int scan = g_exam_now == k + 1 && g_head_ok; static const float white[4] = { 1, 1, 1, 1 };
    for (int p = 0; p < md->nparts; p++) {
        MPart *pt = &md->part[p]; if (!pt->ntri) continue;
        for (int c = 0; c < pt->ntri * 3; c++) {   // light it where it lies (the lamp above)
            float *q = pt->cpu + c * 9, *nn = pt->nrm + c * 3;
            float nw[3]; for (int i = 0; i < 3; i++) nw[i] = W.m[i] * nn[0] + W.m[4 + i] * nn[1] + W.m[8 + i] * nn[2];
            float nl = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]); float dd = nl > 0 ? (nw[0] * Ld[0] + nw[1] * Ld[1] + nw[2] * Ld[2]) / (nl * ll) : 0;
            float I = 0.5f * (0.42f + 0.58f * (dd > 0 ? dd : 0));
            memcpy(q, pt->pos + c * 3, 12); q[3] = pt->uv[c * 2]; q[4] = pt->uv[c * 2 + 1]; q[5] = q[6] = q[7] = I; q[8] = 1;
            if (k == 0 && scan) {   // the body: the game's examination look, a pale blue glow brighter toward the edges
                float vx = g_head.m[12] - (W.m[0] * q[0] + W.m[4] * q[1] + W.m[8] * q[2] + W.m[12]), vy = g_head.m[13] - (W.m[1] * q[0] + W.m[5] * q[1] + W.m[9] * q[2] + W.m[13]),
                      vz = g_head.m[14] - (W.m[2] * q[0] + W.m[6] * q[1] + W.m[10] * q[2] + W.m[14]);
                float vl = sqrtf(vx * vx + vy * vy + vz * vz), fr = nl > 0 && vl > 0 ? fabsf(nw[0] * vx + nw[1] * vy + nw[2] * vz) / (nl * vl) : 1, rim = 1 - fr;
                float g = 0.35f + 0.65f * rim * rim + 0.25f * (dd > 0 ? dd : 0);
                q[5] = 0.5f * (0.30f * g + 0.55f * rim * rim * rim); q[6] = 0.5f * (0.72f * g + 0.3f * rim * rim * rim); q[7] = 0.5f * (0.92f * g + 0.25f * rim * rim * rim);
            }
        }
        vBindBuffer(GL_ARRAY_BUFFER, pt->vbo); vBufferData(GL_ARRAY_BUFFER, 36 * 3 * (size_t)pt->ntri, pt->cpu, GL_DYNAMIC_DRAW);
        vBindTexture(GL_TEXTURE_2D, pt->tex ? pt->tex : g_white); vUniform4fv(u_base, 1, pt->tex ? one : (k == 0 && scan) ? white : skin);
        vUniform1i(u_screen, k == 0 && pt->tex ? 4 : 0);
        vBindVertexArray(pt->vao); vDrawArrays(GL_TRIANGLES, 0, pt->ntri * 3);
    }
    vUniform1i(u_screen, 0); M4 id = m4_id(); vUniformMatrix4fv(u_model, 1, 0, id.m);
}
// the laser on the model being examined: the spot -> where it is on the game's screen (640x528 frame)
static int mdl_hit(int k, const Ray *r, float *sx, float *sy, float *dist) {
    MModel *md = &g_mdl[k]; if (!md->nparts || (!md->placed && !(g_exam_now == k + 1 && g_exam_anchored))) return 0;
    M4 W = mdl_matrix(k); float s = sqrtf(W.m[0] * W.m[0] + W.m[1] * W.m[1] + W.m[2] * W.m[2]); if (s <= 0) return 0;
    // ray into model space: p_m = W^-1 p (W = s * R, R orthonormal)
    float o[3] = { r->o[0] - W.m[12], r->o[1] - W.m[13], r->o[2] - W.m[14] }, om[3], dm[3];
    for (int i = 0; i < 3; i++) { om[i] = (W.m[i * 4] * o[0] + W.m[i * 4 + 1] * o[1] + W.m[i * 4 + 2] * o[2]) / (s * s);
                                  dm[i] = (W.m[i * 4] * r->d[0] + W.m[i * 4 + 1] * r->d[1] + W.m[i * 4 + 2] * r->d[2]) / s; }
    float best = 1e30f; int hp = -1, ht = -1; float hb[2] = { 0, 0 };
    for (int p = 0; p < md->nparts; p++) { MPart *pt = &md->part[p]; if (pt->frame != md->frame) continue;
        for (int t = 0; t < pt->ntri; t++) {
            const float *A = pt->pos + t * 9, *B = A + 3, *C = A + 6;
            float e1[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, e2[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
            float pv[3] = { dm[1] * e2[2] - dm[2] * e2[1], dm[2] * e2[0] - dm[0] * e2[2], dm[0] * e2[1] - dm[1] * e2[0] };
            float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2]; if (fabsf(det) < 1e-12f) continue;
            float id = 1.f / det, tv[3] = { om[0] - A[0], om[1] - A[1], om[2] - A[2] };
            float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * id; if (u < 0 || u > 1) continue;
            float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
            float w = (dm[0] * qv[0] + dm[1] * qv[1] + dm[2] * qv[2]) * id; if (w < 0 || u + w > 1) continue;
            float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * id; if (tt <= 0 || tt >= best) continue;
            best = tt; hp = p; ht = t; hb[0] = u; hb[1] = w;
        } }
    if (hp < 0) return 0;
    MPart *pt = &md->part[hp]; int c = ht * 3; if (!pt->vis[c] || !pt->vis[c + 1] || !pt->vis[c + 2]) return 0;
    g_hit_part = hp; g_hit_tri = ht; g_hit_b[0] = hb[0]; g_hit_b[1] = hb[1];
    float a0 = 1 - hb[0] - hb[1];
    *sx = a0 * pt->scr[c * 2] + hb[0] * pt->scr[c * 2 + 2] + hb[1] * pt->scr[c * 2 + 4];
    *sy = a0 * pt->scr[c * 2 + 1] + hb[0] * pt->scr[c * 2 + 3] + hb[1] * pt->scr[c * 2 + 5];
    *dist = best; return 1;
}
// the morgue's markers: body (right bed), personal effects (left bed), door (top) -- this frame's or the usual spots
static void morgue_marker(int which, float *x, float *y) {   // which: 0 body, 1 effects, 2 door
    static const float def[3][2] = { { 386, 330 }, { 211, 285 }, { 381, 150 } };
    float bx = def[which][0], by = def[which][1], bd = 90 * 90;
    for (int i = 0; i < g_nmk_last; i++) { float dx = g_mk_last[i][0] - def[which][0], dy = g_mk_last[i][1] - def[which][1];
        if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; bx = g_mk_last[i][0]; by = g_mk_last[i][1]; } }
    *x = bx; *y = by;
}
static int bed_hit(int k, const Ray *r, float *dist) {   // the bed (and whatever lies on it) in the morgue room
    if (!g_spot_ok[k]) return 0;
    float mn[3] = { g_spot[k][0] - 0.42f, g_spot[k][1] - 0.1f, g_spot[k][2] - 1.1f }, mx[3] = { g_spot[k][0] + 0.42f, g_spot[k][1] + 0.45f, g_spot[k][2] + 1.1f };
    return ray_box(r, mn, mx, 0.0f, dist);
}
static int mdl_screen(int k, int part, int tri, const float b[2], float *sx, float *sy) {   // a model spot -> where the game shows it now
    MModel *md = &g_mdl[k]; if (part >= md->nparts) return 0; MPart *pt = &md->part[part]; if (tri >= pt->ntri) return 0;
    int c = tri * 3; float a0 = 1 - b[0] - b[1];
    *sx = a0 * pt->scr[c * 2] + b[0] * pt->scr[c * 2 + 2] + b[1] * pt->scr[c * 2 + 4];
    *sy = a0 * pt->scr[c * 2 + 1] + b[0] * pt->scr[c * 2 + 3] + b[1] * pt->scr[c * 2 + 5];
    return pt->vis[c] && pt->vis[c + 1] && pt->vis[c + 2] && *sx > 0 && *sx < 640 && *sy > 0 && *sy < 528;
}
static void click_at(float sx, float sy, int frames) {   // press A at a spot of the game's screen for a moment
    g_click_px[0] = sx; g_click_px[1] = sy; g_click_t = frames;
}

int vr_draw_skip(const Setup *s, const Out *v, int n, float wd, float ht, float xo, float yo) {
    if (!g_active) return 0;
    float mn = 1e9f, mx = -1e9f;   // screen width of this draw (game pixels)
    for (int i = 0; i < n; i++) if (v[i].w != 0) { float x = v[i].x / v[i].w * wd + xo; if (x < mn) mn = x; if (x > mx) mx = x; }
    (void)ht; (void)yo;
    float ymn = 1e9f, ymx = -1e9f;
    for (int i = 0; i < n; i++) if (v[i].w != 0) { float y = v[i].y / v[i].w * ht + yo; if (y < ymn) ymn = y; if (y > ymx) ymx = y; }
    for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (e && e->w == 512 && e->h == 256) g_bubble_seen = 1; }
    if (g_exam_now && !g_office_now) {   // examining: the game's floating model and its tunnel are the VR bed now; only the flat interface stays
        int flat = 1; for (int i = 0; i < n && flat; i++) if (v[i].w != 1.f) flat = 0;
        if (!flat) return 1;
    }
    {   // the morgue room: a 640x480 painting while the forensics models are loaded (and it isn't the office)
        int pic = 0; for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (e && e->w == 640 && e->h == 480 && !office_picture(e)) pic = 1; }
        if (pic && (g_vr_mdl_hi[0] || g_vr_mdl_hi[1])) { g_vr_morgue_seen = 1; return 1; }
        if (g_morgue_now && !g_exam_now) {
            for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (!e) continue;
                if ((e->w == 264 && e->h == 208) || (e->w == 288 && e->h == 168)) return 1;   // the painted body / clothes on the beds
                if (e->w == 56 && e->h == 56) { if (g_nmk < 8) { g_mk[g_nmk][0] = (mn + mx) / 2; g_mk[g_nmk][1] = (ymn + ymx) / 2; g_nmk++; } return 1; }
                if ((e->w == 64 && e->h == 64) || (e->w == 144 && e->h == 144) || (e->w == 96 && e->h == 96)) return 1;   // marker rings + the pointer
                if (e->w >= 150 && e->h >= 150 && e->w != 640) { static int nlog; if (nlog++ < 12) fprintf(stderr, "[vr] morgue: 2D picture %dx%d at %.0f,%.0f (shown)\n", e->w, e->h, mn, ymn); }
            }
        }
    }
    if (g_vr_office_seen) {   // over the office: full-width untextured bars (letterbox) would cover the room
        int tex = 0; for (int m = 0; m < 8; m++) if (s->tex[m].e || s->tex[m].gl_copy) tex = 1;
        if (!tex && mx - mn >= 600) return 1;
    }
    for (int m = 0; m < 8; m++) {
        TexEntry *e = s->tex[m].e;
        // Little Guy's desktop: its 648x440 wallpaper drawn across the screen (help pages show small pictures of it)
        if (e && e->w == 648 && e->h == 440 && mx - mn > 500) g_vr_desk_seen = 1;
        if (e && e->w == 640 && e->h == 480 && !e->px) { static int n; if (n++ < 3) fprintf(stderr, "[vr] 640x480 texture without pixels\n"); }
        if (e && e->w == 640 && e->h == 480 && office_picture(e)) { g_vr_office_seen = 1; return 1; }
        // over the office: the hotspot markers and the pointer are left out too (the room objects and the laser replace them)
        if (e && g_vr_office_seen && ((e->w == 56 && e->h == 56) || (e->w == 64 && e->h == 64))) return 1;
    }
    // the LIMIT label and hearts (top-left HUD): drawn into their own layer, which the wall plaque shows
    int hud = 0;
    if (getenv("TTPC_VR_HUDDBG") && g_vr_office_seen && mx < 320 && ymx < 140) for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (e) fprintf(stderr, "[hud] %.0f,%.0f-%.0f,%.0f tex %08X %dx%d st %dx%d\n", mn, ymn, mx, ymx, e->addr, e->w, e->h, s->tex[m].w, s->tex[m].h); }
    for (int m = 0; m < 8; m++) { TexEntry *e = s->tex[m].e; if (e && ((e->w == 72 && e->h == 64) || (e->w == 104 && e->h == 80))) hud = 1; }
    if (hud && g_vr_office_seen && mx > mn && ymx > ymn && mx < 320 && ymx < 140) {
        if (mn < g_hr[0]) g_hr[0] = mn; if (ymn < g_hr[1]) g_hr[1] = ymn; if (mx > g_hr[2]) g_hr[2] = mx; if (ymx > g_hr[3]) g_hr[3] = ymx;
        return 2;
    }
    if ((g_vr_office_seen || g_morgue_now || g_exam_now) && g_nui[g_uicur] < 96 && mx > mn && ymx > ymn && mx - mn < 640)   // an interface piece: the pane takes the laser here
        g_ui[g_uicur][g_nui[g_uicur]++] = (Rect){ mn, ymn, mx, ymx };
    return 0;
}
int vr_key_frame(void) { return g_active && (g_office_now || g_morgue_now || g_exam_now); }
void vr_set_layer(unsigned tex) { g_lay_tex = tex; }   // gl.c: clear the frame to the key colour

// ---------------------------------------------------------------- entry points (called from platform.c / gl.c)
int vr_wanted(void) { const char *v = getenv("TTPC_VR"); return v && *v && *v != '0'; }
int vr_active(void) { return g_active; }

void vr_init(void) {
    g_preview = getenv("TTPC_VR_PREVIEW") && *getenv("TTPC_VR_PREVIEW") != '0';
    if (!vr_wanted() && !g_preview) return;
    if (!vgl_load()) return;
    GLSave sv; gl_save(&sv); int ok = common_init(); gl_restore(&sv);   // gl.c keeps its own VAO etc. bound
    if (!ok) { fprintf(stderr, "[vr] off (room didn't load)\n"); return; }
    if (g_preview) { g_active = 1; fprintf(stderr, "[vr] preview in the window (no headset)\n"); return; }
#ifdef TTPC_OPENXR
    if (xr_init()) g_active = 1; else fprintf(stderr, "[vr] off: is SteamVR running and set as the OpenXR runtime?\n");
#else
    fprintf(stderr, "[vr] this build has no OpenXR (Windows only); use TTPC_VR_PREVIEW=1\n");
#endif
}

// After the game frame is in game_tex (w x h). Returns 1 if the desktop window was drawn by VR (preview).
int vr_frame(unsigned game_tex, int w, int h, float x0f, float x1f) {
    if (!g_active) { g_vr_desk_seen = 0; return 0; }
    g_game_tex = game_tex; g_gw = w; g_gh = h; g_x0f = x0f; g_x1f = x1f;
    // desktop up? (seen this frame, or within the last few: some frames skip the wallpaper during fades)
    if (g_vr_desk_seen) g_desk_hold = 4; else if (g_desk_hold > 0) g_desk_hold--;
    g_desk_now = g_desk_hold > 0; int fresh = g_vr_desk_seen; g_vr_desk_seen = 0;
    if (g_vr_case != g_case_shown) {   // a case loaded: put its autopsy report on the desk (vr/case_report_<case>.png)
        g_case_shown = g_vr_case; char path[64]; snprintf(path, sizeof path, "vr/case_report_%d.png", g_vr_case);
        int w, h, c; u8 *px = stbi_load(path, &w, &h, &c, 4);
        if (px) {
            if (!g_case_tex) vGenTextures(1, &g_case_tex);
            vBindTexture(GL_TEXTURE_2D, g_case_tex); vTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            vGenerateMipmap(GL_TEXTURE_2D); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            for (int i = 0; i < g_nmat; i++) if (g_mat[i].name_paper) g_mat[i].tex = g_case_tex;
            free(px); fprintf(stderr, "[vr] case %d: its autopsy report is on the desk\n", g_vr_case);
        }
    }
    g_vr_frame_no++;
    {   float tg = g_niche_open ? 1.f : 0.f; g_niche_t += (tg > g_niche_t ? 1 : -1) * 0.022f; if (fabsf(g_niche_t - tg) < 0.022f) g_niche_t = tg; }
#ifdef TTPC_OPENXR
    if (g_niche_t > 0.5f && !g_preview) {   // a hand at a dish stirs its occupants (once per touch)
        static int near_[7];
        for (int d = 0; d < 7; d++) { int nr = 0;
            if (g_dish_ok[d]) for (int h = 0; h < 2; h++) if (g_hand_ok[h]) {
                float dx = g_hand[h].m[12] - g_dish[d][0], dy = g_hand[h].m[13] - (g_dish[d][1] + 0.04f), dz = g_hand[h].m[14] - g_dish[d][2];
                if (dx * dx + dy * dy + dz * dz < 0.065f * 0.065f) nr = 1; }
            if (nr && !near_[d]) aq_poke(d); else if (nr && d != 1) g_dish_t[d] = 4.f;
            near_[d] = nr; }
    }
#endif
    creature_update();
    if (g_vr_morgue_seen) g_morgue_hold = 4; else if (g_morgue_hold > 0) g_morgue_hold--;
    g_morgue_now = g_morgue_hold > 0; g_vr_morgue_seen = 0;
    if (g_exam_seen) { g_exam_hold = 4; g_exam_now = g_exam_seen; } else if (g_exam_hold > 0 && --g_exam_hold == 0) g_exam_now = 0;
    g_exam_seen = 0;
    memcpy(g_mk_last, g_mk, sizeof g_mk); g_nmk_last = g_nmk; g_nmk = 0;
    for (int k = 0; k < 2; k++) if (g_mdl[k].nparts && (!g_mdl[k].placed || g_exam_now == k + 1)) mdl_place(k);
    g_room = (g_morgue_now || g_exam_now) ? 1 : 0;
    {   static int force = -1; if (force < 0) force = getenv("TTPC_VR_ROOM") ? atoi(getenv("TTPC_VR_ROOM")) : 0; if (force) g_room = force; }   // tests: show a room
    {   static int lr = -1; if (g_room != lr) {   // arriving in a room: the morgue starts by the door you came in, facing the beds
            int first = lr < 0; lr = g_room; fprintf(stderr, "[vr] room: %s\n", g_room ? "morgue" : "office");
            if (g_room == 1) { g_seat[0] = -0.1f; g_seat[1] = 0; g_seat[2] = -0.45f; g_turn = 0; }
            else { g_seat[0] = -1.55f; g_seat[1] = 0; g_seat[2] = -1.62f; g_turn = (float)(M_PI / 2); }
            if (!first) { g_world_from_stage = m4_yaw(g_turn, g_seat[0], g_seat[1], g_seat[2]); g_arrive = 1; }
        } }
    {   static int le = -1; if (g_exam_now != le) { le = g_exam_now; if (g_exam_now) fprintf(stderr, "[vr] examining the %s\n", g_exam_now == 1 ? "body" : "personal effects"); } }
    {   // opening / closing the examination: anchor the void where your head is, fade the room
        static int le2; if (g_exam_now != le2) { le2 = g_exam_now; g_exam_anchored = 0; g_exam_k = 0; }
        if (g_exam_now && !g_exam_anchored && g_head_ok && g_mdl[g_exam_now - 1].nparts) {
            MModel *md = &g_mdl[g_exam_now - 1];
            float fx = -g_head.m[8], fz = -g_head.m[10], yaw = atan2f(-fx, -fz);
            g_exam_A = m4_yaw(yaw, g_head.m[12], g_head.m[13], g_head.m[14]);
            float mid[3] = { (md->lo[0] + md->hi[0]) / 2, (md->lo[1] + md->hi[1]) / 2, (md->lo[2] + md->hi[2]) / 2 };
            float z = md->ref[8] * mid[0] + md->ref[9] * mid[1] + md->ref[10] * mid[2] + md->ref[11];
            g_exam_k = 0.9f / (z < -1 ? -z : 200.f); g_exam_anchored = 1; g_exam_exit_cd = 0;
            fprintf(stderr, "[vr] examination opened in front of you (game depth %.0f -> scale %.4f)\n", -z, g_exam_k);
        }
        g_bubble_now = g_bubble_seen; g_bubble_seen = 0;
        if (g_exam_now && g_exam_anchored && g_chk_st) {
            MModel *md = &g_mdl[g_exam_now - 1]; g_chk_t++;
            int zoomed = md->camd < g_chk_d0 - 8; float dd = fabsf(md->camd - g_chk_prevd); g_chk_prevd = md->camd;
            if (g_chk_st == 1 && g_click_t == 0) {   // waiting for the game to zoom in on the spot
                static int still; still = dd < 0.3f ? still + 1 : 0;
                if (zoomed && still >= 4) { float sx, sy;
                    if (mdl_screen(g_exam_now - 1, g_chk_part, g_chk_tri, g_chk_b, &sx, &sy)) { click_at(sx, sy, 16); fprintf(stderr, "[vr] check (the game zoomed in)\n"); }
                    g_chk_st = 2; g_chk_t = 0; g_chk_quiet = 0; }
                else if (g_chk_t > 60) g_chk_st = 0;   // no zoom: the click was handled directly
            } else if (g_chk_st == 2 && g_click_t == 0) {   // the remark; then out of the close-up
                g_chk_quiet = g_bubble_now ? 0 : g_chk_quiet + 1;
                if (g_chk_t > 20 && g_chk_quiet > 20) { if (zoomed) { g_press_b = 10; fprintf(stderr, "[vr] back out of the close-up\n"); } g_chk_st = 0; }
                else if (g_chk_t > 1200) g_chk_st = 0;
            }
        } else if (!g_exam_now) g_chk_st = 0;
        if (!g_chk_st && g_chk_frozen) {   // un-freeze once the game's camera is back where it was
            MModel *md = g_exam_now ? &g_mdl[g_exam_now - 1] : NULL; static int wait;
            if (!md || fabsf(md->camd - g_chk_d0) < 3 || ++wait > 90) { g_chk_frozen = 0; wait = 0; }
        }
        float target = g_exam_now && g_exam_anchored ? 1.f : 0.f; g_exam_fade += (target - g_exam_fade) * 0.12f;
        if (fabsf(target - g_exam_fade) < 0.01f) g_exam_fade = target;
        // stepping back out of it presses the game's EXIT
        if (g_exam_exit_cd > 0) g_exam_exit_cd--;
        if (g_exam_now && g_exam_anchored && g_head_ok && g_exam_exit_cd == 0 && g_click_t == 0) {
            float dx = g_head.m[12] - g_exam_A.m[12], dz = g_head.m[14] - g_exam_A.m[14];
            if (dx * dx + dz * dz > 0.9f * 0.9f) { click_at(527, 424, 20); g_exam_exit_cd = 150; fprintf(stderr, "[vr] stepped back: EXIT\n"); }
        }
    }
    if (g_vr_office_seen) g_office_hold = 4; else if (g_office_hold > 0) g_office_hold--;
    g_office_now = g_office_hold > 0 && !g_desk_now; g_vr_office_seen = 0;
    g_uicur ^= 1; g_nui[g_uicur] = 0;
    if (g_hr[2] > g_hr[0]) { HEARTS_PX[0] = g_hr[0] - 3; HEARTS_PX[1] = g_hr[1] - 3; HEARTS_PX[2] = g_hr[2] + 3; HEARTS_PX[3] = g_hr[3] + 3; }
    g_hr[0] = g_hr[1] = 1e9f; g_hr[2] = g_hr[3] = -1e9f;
    if (g_office_now || g_desk_now) { if (g_office_now) g_door_req = g_door_req && 1; }
    {   int open = g_door_req && !g_office_now && !g_desk_now;
        if (g_office_now && g_door_t >= 1.f) g_door_req = 0;   // back from the map: the door closes
        float target = open ? 1.f : 0.f; g_door_t += (target - g_door_t) * 0.08f; if (fabsf(target - g_door_t) < 0.002f) g_door_t = target; }
    if (g_desk_now && g_head_ok && g_obj_ok[3] && g_auto_exit == 0) {
        float mx = (g_obj_min[3][0] + g_obj_max[3][0]) / 2 + g_mouse_off[0], mz = (g_obj_min[3][2] + g_obj_max[3][2]) / 2 + g_mouse_off[1];
        float dx = g_head.m[12] - mx, dz = g_head.m[14] - mz;
        if (dx * dx + dz * dz > 1.2f * 1.2f) { g_vr_close_comp = 1; g_auto_exit = -90; fprintf(stderr, "[vr] stepped away from the computer\n"); }
    }
    if (!g_desk_now && g_auto_exit > 0 && g_auto_exit < 30) g_auto_exit = 0;   // it closed
    place_pane();
    gl_chk("enter"); GLSave s; gl_save(&s); gl_chk("save"); int drew = 0;
    if (fresh) {   // keep this desktop frame for the monitor
        if (w != g_desk_w || h != g_desk_h) {
            if (!g_desk_fbo) { vGenFramebuffers(1, &g_desk_fbo); vGenTextures(1, &g_desk_tex); }
            vBindTexture(GL_TEXTURE_2D, g_desk_tex); vTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            vBindFramebuffer(GL_FRAMEBUFFER, g_desk_fbo); vFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_desk_tex, 0);
            g_desk_w = w; g_desk_h = h;
        }
        GLuint src; vGenFramebuffers(1, &src); vBindFramebuffer(0x8CA8, src); vFramebufferTexture2D(0x8CA8, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, game_tex, 0);
        vBindFramebuffer(0x8CA9, g_desk_fbo); vDisable(GL_SCISSOR_TEST); vDisable(GL_FRAMEBUFFER_SRGB);
        vBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        vBindFramebuffer(0x8CA8, 0); vDeleteFramebuffers(1, &src);
        if (!g_desk_have) fprintf(stderr, "[vr] Little Guy's desktop seen: it stays on the monitor now\n");
        g_desk_have = 1;
    }
    if (g_preview) drew = preview_frame();
#ifdef TTPC_OPENXR
    else xr_frame();
#endif
    gl_chk("frame"); gl_restore(&s); gl_chk("restore");
    return drew;
}

// After the keyboard/mouse/pad have been read: the laser overrides the pointer while it's on the screen.
void vr_input(Input *in) {
    if (!g_active) return;
    float px, py, dist;
    int what = laser_target(&g_ray, &px, &py, &dist);
    if (g_mouse_held == 2) in->rmb = 0;   // desktop mode: right mouse is for looking around while you hold the desk mouse
    {   static int desk_prev;
        if (desk_prev && !g_desk_now && g_mouse_held >= 0) { g_mouse_held = -1; fprintf(stderr, "[vr] computer closed: let go of the mouse\n"); }
        desk_prev = g_desk_now; }
    if (g_mouse_held < 0 && g_desk_now) {   // the computer came up some other way: your hand goes to the mouse
#ifdef TTPC_OPENXR
        g_mouse_held = g_preview ? 2 : g_point_hand;
        if (g_mouse_held < 2 && g_hand_ok[g_mouse_held]) { g_mouse_grab[0] = g_hand[g_mouse_held].m[12]; g_mouse_grab[1] = g_hand[g_mouse_held].m[13]; g_mouse_grab[2] = g_hand[g_mouse_held].m[14]; memcpy(g_mouse_grab_off, g_mouse_off, 8); }
#else
        g_mouse_held = 2;
#endif
    }
    if (g_mouse_held >= 0 && g_desk_now) {   // the held desk mouse is the computer's pointer
        px = 0.5f - g_mouse_off[1] / 0.32f; py = DESK_Y0 + (0.5f + g_mouse_off[0] / 0.24f) * (DESK_Y1 - DESK_Y0);
        px = px < 0 ? 0 : px > 1 ? 1 : px; what = 1; g_last_kind = 1;
    }
#ifdef TTPC_OPENXR
    if (!g_preview && panel_on() && g_pan_placed) for (int h = 0; h < 2; h++) if (g_hand_ok[h]) {   // touch the pane with a controller
        float tip[3], tl[3] = { 0, 0, -0.06f }; m4_point(g_hand[h], tl, tip);
        Ray r; memcpy(r.o, tip, 12); r.valid = 1;
        float n[3] = { g_pan_u[1] * g_pan_v[2] - g_pan_u[2] * g_pan_v[1], g_pan_u[2] * g_pan_v[0] - g_pan_u[0] * g_pan_v[2], g_pan_u[0] * g_pan_v[1] - g_pan_u[1] * g_pan_v[0] };
        float nl = len3(n); for (int c = 0; c < 3; c++) r.d[c] = -n[c] / nl;   // straight at the pane
        float bx, by, d; int hit = ray_quad(g_pan_o, g_pan_u, g_pan_v, 4.f / 3, &r, &bx, &by, &d);
        if (!hit) { for (int c = 0; c < 3; c++) r.d[c] = -r.d[c]; hit = ray_quad(g_pan_o, g_pan_u, g_pan_v, 4.f / 3, &r, &bx, &by, &d); d = -d; }
        if (hit && fabsf(d) < 0.03f && bx >= 0 && bx <= 1 && by >= 0 && by <= 1) { px = bx; py = by; what = 2; in->lmb = in->a = 1; }
    }
#endif
    if (g_press_b > 0) { in->b = in->rmb = (g_press_b <= 8 && g_press_b > 2); g_press_b--; g_scripted = 1; }
    if (g_click_t > 0) {   // a press the VR layer makes for you (EXIT, then opening the other bed)
        px = g_click_px[0] / 640.f; py = (g_click_px[1] - g_vr_pic_y0) / (float)g_vr_pic_h; what = 1;
        in->a = in->lmb = (g_click_t <= 14 && g_click_t > 6); g_click_t--; g_scripted = 1;
    }
    if (g_auto_exit > 0 && g_desk_now) {
        px = EXIT_BTN[0]; py = EXIT_BTN[1]; what = 1;
        if (g_auto_exit <= 18 && g_auto_exit > 10) { in->a = 1; in->lmb = 1; }
        if (--g_auto_exit == 0) g_auto_exit = 0;
        if (g_auto_exit == 0) g_auto_exit = -60;   // don't retrigger at once if the computer stays up
    } else if (g_auto_exit < 0) g_auto_exit++;
    if (what) { in->mouse_x = px; in->mouse_y = py; in->mouse_in = 1; }
#ifdef TTPC_OPENXR
    if (!g_preview) {
        int h = g_point_hand;
        in->lmb |= g_trig[h]; in->a |= g_trig[h];
        in->rmb |= g_gripb[h] || g_bb[h]; in->b |= g_gripb[h] || g_bb[h];
        in->minus |= g_ab[0]; in->plus |= g_bb[0];
    }
#endif
    {   // examining: tap the model = A on that spot; grab it (grip, or hold the trigger and drag) = turn it with your hand.
        // The grabbed spot follows your hand around the body's long axis: each frame the body's real angle is read
        // back from the game and its Rotate (D-pad left/right) is held until it matches. Let go and it stays.
        static int st, gsign = 1, probe; static float d0, phi0, th0, txp, typ, thp; static float pr_th; static int pr_dir, pr_n;
        static float h0[3], rgt[3]; static int tpart, ttri; static float tb[2];
        int trig = (in->a || in->lmb) && !g_scripted, grip = 0;
#ifdef TTPC_OPENXR
        if (!g_preview) grip = g_gripb[g_point_hand];
#endif
        if (!g_exam_now || !g_exam_anchored) st = 0;
        else {
            int k = g_exam_now - 1; MModel *md = &g_mdl[k];
            M4 W = exam_matrix(k);
            float A[3] = { W.m[md->axis * 4], W.m[md->axis * 4 + 1], W.m[md->axis * 4 + 2] }, al = sqrtf(A[0] * A[0] + A[1] * A[1] + A[2] * A[2]);
            for (int i = 0; i < 3; i++) A[i] /= al > 0 ? al : 1;
            float mid[3] = { (md->lo[0] + md->hi[0]) / 2, (md->lo[1] + md->hi[1]) / 2, (md->lo[2] + md->hi[2]) / 2 }, C[3]; m4_point(W, mid, C);
            // a basis around the axis: u = world up (or forward) made perpendicular, v = A x u
            float up[3] = { 0, 1, 0 }; if (fabsf(A[1]) > 0.9f) { up[1] = 0; up[2] = -1; }
            float dd = up[0] * A[0] + up[1] * A[1] + up[2] * A[2], u[3] = { up[0] - dd * A[0], up[1] - dd * A[1], up[2] - dd * A[2] };
            float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); for (int i = 0; i < 3; i++) u[i] /= ul;
            float v[3] = { A[1] * u[2] - A[2] * u[1], A[2] * u[0] - A[0] * u[2], A[0] * u[1] - A[1] * u[0] };
            #define ANG(P) atan2f(((P)[0] - C[0]) * v[0] + ((P)[1] - C[1]) * v[1] + ((P)[2] - C[2]) * v[2], ((P)[0] - C[0]) * u[0] + ((P)[1] - C[1]) * u[1] + ((P)[2] - C[2]) * u[2])
            // the body's displayed roll: where a fixed model direction (perpendicular to the axis) points now
            float q[3] = { 0, 0, 0 }; q[(md->axis + 1) % 3] = (md->hi[(md->axis + 1) % 3] - md->lo[(md->axis + 1) % 3]) * 0.5f + 1e-3f;
            float qm[3] = { mid[0] + q[0], mid[1] + q[1], mid[2] + q[2] }, Q[3]; m4_point(W, qm, Q);
            float th = ANG(Q);
            float hand[3] = { g_ray.o[0] + g_ray.d[0] * d0, g_ray.o[1] + g_ray.d[1] * d0, g_ray.o[2] + g_ray.d[2] * d0 };
            if (st == 0 && (trig || grip)) {
                if (g_last_kind == 6) { d0 = dist; st = grip ? 2 : 1; txp = px; typ = py; tpart = g_hit_part; ttri = g_hit_tri; tb[0] = g_hit_b[0]; tb[1] = g_hit_b[1];
                    for (int i = 0; i < 3; i++) h0[i] = g_ray.o[i] + g_ray.d[i] * d0;
                    rgt[0] = g_head.m[0]; rgt[1] = 0; rgt[2] = g_head.m[2]; float rl = sqrtf(rgt[0] * rgt[0] + rgt[2] * rgt[2]); if (rl > 0) { rgt[0] /= rl; rgt[2] /= rl; }
                    phi0 = ANG(h0); th0 = th; thp = th; pr_n = 0; }
                else st = 3;
            }
            if (st == 1) {   // trigger: a tap examines the spot, moving the hand turns it into a grab
                float mv = sqrtf((hand[0] - h0[0]) * (hand[0] - h0[0]) + (hand[1] - h0[1]) * (hand[1] - h0[1]) + (hand[2] - h0[2]) * (hand[2] - h0[2]));
                if (!trig) {   // a tap: check that spot (the zoom the game does is undone for you)
                    click_at(txp * 640.f, typ * g_vr_pic_h + g_vr_pic_y0, 16); st = 0;
                    g_chk_st = 1; g_chk_t = 0; g_chk_part = tpart; g_chk_tri = ttri; g_chk_b[0] = tb[0]; g_chk_b[1] = tb[1]; g_chk_d0 = md->camd; g_chk_prevd = md->camd;
                    memcpy(g_chk_ref, md->ref, 48); g_chk_frozen = 1;
                }
                else if (mv > 0.025f) { st = 2; fprintf(stderr, "[vr] turning the %s by hand\n", k ? "personal effects" : "body"); }
            }
            if (st == 2) {
                if (!trig && !grip) { st = 0; }
                else {
                    // sideways hand movement spins it (about 1 turn per 0.75 m), as if rolling it on its long axis
                    float side = (hand[0] - h0[0]) * rgt[0] + (hand[2] - h0[2]) * rgt[2];
                    float target = th0 + side / 0.12f, err = target - th;
                    while (err > M_PI) err -= 2 * M_PI; while (err < -M_PI) err += 2 * M_PI;
                    int dir = err > 0.06f ? 1 : err < -0.06f ? -1 : 0;
                    // learn which way the game's Rotate turns it (the first few frames of a press tell)
                    if (dir && dir == pr_dir) { if (++pr_n == 8) { float moved = th - pr_th; while (moved > M_PI) moved -= 2 * M_PI; while (moved < -M_PI) moved += 2 * M_PI;
                        if (fabsf(moved) > 0.01f && (moved > 0) != (dir > 0)) { gsign = -gsign; fprintf(stderr, "[vr] rotate direction flipped\n"); } } }
                    else { pr_dir = dir; pr_n = 0; pr_th = th; }
                    g_stick_cmd = dir * gsign;
                }
            }
            if (st == 3 && !trig && !grip) st = 0;
            if (st == 1 || st == 2) { in->a = in->lmb = 0; in->b = in->rmb = 0; }   // the grab never reaches the game as a press
            #undef ANG
            (void)thp; (void)probe;
        }
        g_scripted = 0;
    }
    {   // on the computer: tap = A (open a card); press and hold = pick the card up (the game's A+B) and drag it
        static int st, frames, tap;
        int raw = in->a || in->lmb;
        if (!g_desk_now) st = 0, tap = 0;
        else {
            if (st == 0 && raw) { st = 1; frames = 0; }
            if (st == 1) { if (!raw) { tap = 6; st = 0; } else if (++frames >= 10) { st = 2; fprintf(stderr, "[vr] long press: dragging (A+B)\n"); } }
            if (st == 2 && !raw) st = 0;
            int a = st == 2 || tap > 0, b = st == 2;
            if (tap > 0) tap--;
            in->a = in->lmb = a; if (b) { in->b = 1; in->rmb = 1; }
        }
    }
    { static int prev; int down = in->lmb || in->a;
      if (down && !prev && g_last_kind == 4 && g_office_now) g_door_req = 1;
      if (down && !prev && g_last_kind == 9) { g_niche_open = !g_niche_open; fprintf(stderr, "[vr] the hidden compartment %s\n", g_niche_open ? "opens" : "closes"); }
      if (down && !prev && g_office_now && g_niche_t > 0.5f && g_last_kind != 9) {   // a dish under the laser: stir it (tested on the
          float best; int bd = aim_dish(&g_ray, &best);                             // laser itself, so the game's picture can't take it)
          if (bd >= 0) { aq_poke(bd); g_last_kind = 10; }
          else fprintf(stderr, "[vr] click with the compartment open: laser on kind %d\n", g_last_kind);
      }
      {   // a press that starts on the book or the dish belongs to VR until it's let go, even if the laser slips off meanwhile
          static int swallow; if (down && !prev && (g_last_kind == 9 || g_last_kind == 10)) swallow = 1;
          if (!down) swallow = 0;
          if (swallow) { in->a = in->lmb = 0; }
      }
      if (down && !prev && g_exam_now && (g_last_kind == 7 || g_last_kind == 8) && g_last_kind - 7 != g_exam_now - 1) {
          g_pending_bed = g_last_kind - 7; g_exit_t = 1; fprintf(stderr, "[vr] switching to the %s\n", g_pending_bed ? "personal effects" : "body"); }   // the door: if the game lets you go, it opens
      if (down && !prev && g_last_kind == 5 && g_office_now && g_mouse_held < 0) {   // using the mouse turns the computer on; you keep hold of it
#ifdef TTPC_OPENXR
          g_mouse_held = g_preview ? 2 : g_point_hand;
#else
          g_mouse_held = 2;
#endif
          if (g_mouse_held < 2 && g_hand_ok[g_mouse_held]) { g_mouse_grab[0] = g_hand[g_mouse_held].m[12]; g_mouse_grab[1] = g_hand[g_mouse_held].m[13]; g_mouse_grab[2] = g_hand[g_mouse_held].m[14]; memcpy(g_mouse_grab_off, g_mouse_off, 8); }
          fprintf(stderr, "[vr] took the mouse: computer on\n");
      }
      if (down && !prev) fprintf(stderr, "[vr] click: %s at %.3f %.3f (office %d desktop %d)\n", what == 2 ? "pane" : what == 1 ? "room object/monitor" : "nothing", px, py, g_office_now, g_desk_now);
      prev = down; }
    if (what) {}
    else in->mouse_in = 0;
    if (g_preview) in->stick_up = in->stick_down = in->stick_left = in->stick_right = 0;   // desktop mode: WASD walks you, not the Nunchuk
    if (g_stick_cmd) { in->stick_right = g_stick_cmd > 0; in->stick_left = g_stick_cmd < 0; in->nx = (float)g_stick_cmd; in->ny = 0; in->has_analog = 1; }   // the grab turns the model
    g_stick_cmd = 0;
}

#else   // headless build
int vr_wanted(void) { return 0; }
int vr_active(void) { return 0; }
void vr_init(void) {}
int vr_frame(unsigned t, int w, int h, float a, float b) { (void)t; (void)w; (void)h; (void)a; (void)b; return 0; }
int g_vr_desk_seen, g_vr_office_seen; int g_vr_pic_y0, g_vr_pic_h; int g_vr_case; int g_vr_close_comp; char g_vr_shot[256];
void vr_set_layer(unsigned tex) { (void)tex; }
int vr_draw_skip(const Setup *s, const Out *v, int n, float wd, float ht, float xo, float yo) { (void)s; (void)v; (void)n; (void)wd; (void)ht; (void)xo; (void)yo; return 0; }
int vr_key_frame(void) { return 0; }
void vr_input(Input *in) { (void)in; }
int vr_track(const Setup *s) { (void)s; return -1; }
void vr_capture(int which, const Setup *s, int prim, int n, const VrVert *v, const float M[12]) { (void)which; (void)s; (void)prim; (void)n; (void)v; (void)M; }
#endif
