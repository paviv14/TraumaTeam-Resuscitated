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
    X(void, Clear, (GLbitfield)) X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) X(GLboolean, IsEnabled, (GLenum)) X(void, DepthFunc, (GLenum)) X(void, DepthMask, (GLboolean)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) X(void, BlendFunc, (GLenum, GLenum)) X(void, GetIntegerv, (GLenum, GLint *)) \
    X(void, GenTextures, (GLsizei, GLuint *)) X(void, BindTexture, (GLenum, GLuint)) X(void, ActiveTexture, (GLenum)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, GenerateMipmap, (GLenum)) X(void, PixelStorei, (GLenum, GLint)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, BindFramebuffer, (GLenum, GLuint)) X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint *)) X(void, BindRenderbuffer, (GLenum, GLuint)) X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) X(void, UseProgram, (GLuint)) X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform1f, (GLint, GLfloat)) X(void, Uniform2f, (GLint, GLfloat, GLfloat)) X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
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
typedef struct { float base[4], emis[3]; GLuint tex, etex; int blend, mask; float cutoff; int screen; } Mat;
typedef struct { GLuint vao; int n; int mat; } Prim;
static Mat *g_mat; static int g_nmat;
static Prim *g_prim; static int g_nprim;
static GLuint g_white, g_prog, g_lineprog, g_linevao, g_linevbo;
static GLint u_mvp, u_base, u_emis, u_tex, u_etex, u_cut, u_screen, u_scr_k, u_line_mvp, u_line_col;
static float g_scr_o[3], g_scr_u[3], g_scr_v[3];   // screen: top-left corner, right edge, down edge (world)
static int g_have_screen;
static int g_active, g_preview;
static M4 g_world_from_stage;   // where the play area sits in the room
static float g_seat[3] = { -0.55f, 0.0f, -1.62f };   // Naomi's chair (glTF frame), facing -x (the desk)
static float g_turn = (float)(M_PI / 2);   // play area yaw (stage -z -> room -x)

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

static int load_room(const char *path) {
    cgltf_options opt = {0}; cgltf_data *d = NULL;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success || cgltf_load_buffers(&opt, d, path) != cgltf_result_success) {
        fprintf(stderr, "[vr] can't load %s\n", path); if (d) cgltf_free(d); return 0;
    }
    GLuint *img_lin = calloc(d->images_count + 1, sizeof *img_lin);
    g_nmat = (int)d->materials_count; g_mat = calloc(g_nmat + 1, sizeof *g_mat);
    for (int i = 0; i < g_nmat; i++) {
        cgltf_material *m = &d->materials[i]; Mat *o = &g_mat[i];
        memcpy(o->base, m->pbr_metallic_roughness.base_color_factor, 16); memcpy(o->emis, m->emissive_factor, 12);
        if (m->has_emissive_strength) for (int k = 0; k < 3; k++) o->emis[k] *= m->emissive_strength.emissive_strength;
        const cgltf_texture *bt = m->pbr_metallic_roughness.base_color_texture.texture, *et = m->emissive_texture.texture;
        o->tex = bt ? upload_image(bt->image, 1) : 0; o->etex = et ? upload_image(et->image, 1) : 0;
        o->blend = m->alpha_mode == cgltf_alpha_mode_blend; o->mask = m->alpha_mode == cgltf_alpha_mode_mask; o->cutoff = m->alpha_cutoff;
        if (o->blend) { o->mask = 1; o->cutoff = 0.5f; o->blend = 0; }   // leaves: cut-out reads better than sorting in VR
        o->screen = m->name && !strcmp(m->name, "screen");
    }
    int cap = 0;
    for (cgltf_size ni = 0; ni < d->nodes_count; ni++) {
        cgltf_node *nd = &d->nodes[ni]; if (!nd->mesh) continue;
        float wm[16]; cgltf_node_transform_world(nd, wm); M4 W; memcpy(W.m, wm, 64);
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
            }
            int ni_ = pr->indices ? (int)pr->indices->count : nv; u32 *ix = malloc(4 * ni_);
            for (int k = 0; k < ni_; k++) ix[k] = pr->indices ? (u32)cgltf_accessor_read_index(pr->indices, k) : (u32)k;
            int mi = pr->material ? (int)(pr->material - d->materials) : g_nmat;
            if (mi < g_nmat && g_mat[mi].screen && !g_have_screen) {   // corners from the UVs: (0,0) top-left, u right, v down
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
            Prim *o = &g_prim[g_nprim++]; o->n = ni_; o->mat = mi;
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
    fprintf(stderr, "[vr] room %s: %d materials, %d draws%s\n", path, g_nmat, g_nprim, g_have_screen ? "" : " (no 'screen' material!)");
    return g_nprim > 0;
}

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aP; layout(location=1) in vec2 aT; layout(location=2) in vec4 aC;\n"
    "uniform mat4 uMVP; out vec2 vT; out vec4 vC;\n"
    "void main() { gl_Position = uMVP * vec4(aP, 1.0); vT = aT; vC = aC; }\n";
static const char *FS =
    "#version 330 core\n"
    "in vec2 vT; in vec4 vC; uniform vec4 uBase; uniform vec3 uEmis; uniform sampler2D uTex, uETex; uniform float uCut;\n"
    "uniform int uScreen; uniform vec2 uScrK; out vec4 o;\n"
    "void main() {\n"
    "  if (uScreen == 1) {   // the game picture, letterboxed to keep its shape; it holds sRGB values -> linear\n"
    "    vec2 t = (vT - 0.5) * uScrK + 0.5;\n"
    "    if (t.x < 0.0 || t.x > 1.0 || t.y < 0.0 || t.y > 1.0) { o = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    vec3 c = texture(uTex, vec2(t.x, 1.0 - t.y)).rgb; o = vec4(pow(c, vec3(2.2)), 1.0); return; }\n"
    "  vec4 b = uBase * texture(uTex, vT); if (b.a < uCut) discard;\n"
    "  vec3 c = b.rgb * vC.rgb * 2.0 + uEmis * texture(uETex, vT).rgb;   // baked light was stored at half range\n"
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
typedef struct { float o[3], d[3]; int valid; } Ray;
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

static void draw_scene(M4 proj, M4 view, GLuint game_tex, int gw, int gh) {
    M4 vp = m4_mul(proj, view);
    vUseProgram(g_prog); vUniformMatrix4fv(u_mvp, 1, 0, vp.m);
    vUniform1i(u_tex, 0); vUniform1i(u_etex, 1);
    // keep the game's shape on the screen: shrink the longer side of the screen's UV range
    float sw = sqrtf(g_scr_u[0] * g_scr_u[0] + g_scr_u[1] * g_scr_u[1] + g_scr_u[2] * g_scr_u[2]);
    float sh = sqrtf(g_scr_v[0] * g_scr_v[0] + g_scr_v[1] * g_scr_v[1] + g_scr_v[2] * g_scr_v[2]);
    float qa = sh > 0 ? sw / sh : 1, ga = gh > 0 ? (float)gw / gh : 4.f / 3;
    float kx = qa > ga ? qa / ga : 1, ky = qa > ga ? 1 : ga / qa;
    vUniform2f(u_scr_k, kx, ky);
    for (int i = 0; i < g_nprim; i++) {
        Prim *p = &g_prim[i]; Mat *m = p->mat < g_nmat ? &g_mat[p->mat] : NULL;
        static const float one[4] = { 1, 1, 1, 1 }, zero[3] = { 0, 0, 0 };
        vActiveTexture(GL_TEXTURE1); vBindTexture(GL_TEXTURE_2D, m && m->etex ? m->etex : g_white);
        vActiveTexture(GL_TEXTURE0);
        if (m && m->screen && game_tex) { vBindTexture(GL_TEXTURE_2D, game_tex); vUniform1i(u_screen, 1); }
        else { vBindTexture(GL_TEXTURE_2D, m && m->tex ? m->tex : g_white); vUniform1i(u_screen, 0); }
        vUniform4fv(u_base, 1, m ? m->base : one); vUniform3fv(u_emis, 1, m ? m->emis : zero);
        vUniform1f(u_cut, m && m->mask ? m->cutoff : -1.f);
        vBindVertexArray(p->vao); vDrawElements(GL_TRIANGLES, p->n, GL_UNSIGNED_INT, 0);
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

// pointer: where the laser meets the screen (0..1 across the game picture), honouring the letterbox
static int ray_screen(const Ray *r, int gw, int gh, float *px, float *py, float *dist) {
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

typedef struct GLSave GLSave; static void gl_save(GLSave *s); static void gl_restore(const GLSave *s);
// ---------------------------------------------------------------- shared setup
static GLuint g_game_tex; static int g_gw = 640, g_gh = 480;
static int common_init(void) {
    g_prog = build(VS, FS); g_lineprog = build(LVS, LFS);
    u_mvp = vGetUniformLocation(g_prog, "uMVP"); u_base = vGetUniformLocation(g_prog, "uBase"); u_emis = vGetUniformLocation(g_prog, "uEmis");
    u_tex = vGetUniformLocation(g_prog, "uTex"); u_etex = vGetUniformLocation(g_prog, "uETex"); u_cut = vGetUniformLocation(g_prog, "uCut");
    u_screen = vGetUniformLocation(g_prog, "uScreen"); u_scr_k = vGetUniformLocation(g_prog, "uScrK");
    u_line_mvp = vGetUniformLocation(g_lineprog, "uMVP"); u_line_col = vGetUniformLocation(g_lineprog, "uCol");
    vGenVertexArrays(1, &g_linevao); vGenBuffers(1, &g_linevbo);
    static const u8 white[4] = { 255, 255, 255, 255 };
    vGenTextures(1, &g_white); vBindTexture(GL_TEXTURE_2D, g_white); vTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); vTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const char *room = getenv("TTPC_VR_SCENE"); if (!room || !*room) room = "vr/naomi_office.glb";
    if (!load_room(room)) return 0;
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
    const u8 *k = SDL_GetKeyboardState(NULL);
    if (k[SDL_SCANCODE_F2] && !f2_prev) { captured = !captured; SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE); }
    f2_prev = k[SDL_SCANCODE_F2];
    int dx = 0, dy = 0; SDL_GetRelativeMouseState(&dx, &dy);
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
    head = m4_mul(head, pit);
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
static XrSwapchain g_sc[2]; static int g_scw[2], g_sch[2]; static XrSwapchainImageOpenGLKHR *g_scimg[2]; static uint32_t g_scn[2];
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
        XRC(xrEnumerateSwapchainImages(g_sc[e], 0, &g_scn[e], NULL));
        g_scimg[e] = calloc(g_scn[e], sizeof **g_scimg); for (uint32_t i = 0; i < g_scn[e]; i++) g_scimg[e][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        XRC(xrEnumerateSwapchainImages(g_sc[e], g_scn[e], &g_scn[e], (XrSwapchainImageBaseHeader *)g_scimg[e]));
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
                XrFovf f = views[e].fov;
                M4 proj = m4_frustum(tanf(f.angleLeft), tanf(f.angleRight), tanf(f.angleDown), tanf(f.angleUp), 0.03f, 50.f);
                draw_scene(proj, m4_rigid_inv(eye), g_game_tex, g_gw, g_gh);
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
int vr_frame(unsigned game_tex, int w, int h) {
    if (!g_active) return 0;
    g_game_tex = game_tex; g_gw = w; g_gh = h;
    gl_chk("enter"); GLSave s; gl_save(&s); gl_chk("save"); int drew = 0;
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
    if (ray_screen(&g_ray, g_gw, g_gh, &px, &py, &dist)) { in->mouse_x = px; in->mouse_y = py; in->mouse_in = 1; }
    else in->mouse_in = 0;
    if (g_preview) in->stick_up = in->stick_down = in->stick_left = in->stick_right = 0;   // desktop mode: WASD walks you, not the Nunchuk
#ifdef TTPC_OPENXR
    if (!g_preview) {
        int h = g_point_hand;
        in->lmb |= g_trig[h]; in->a |= g_trig[h];
        in->rmb |= g_gripb[h] || g_bb[h]; in->b |= g_gripb[h] || g_bb[h];
        in->minus |= g_ab[0]; in->plus |= g_bb[0];
    }
#endif
}

#else   // headless build
int vr_wanted(void) { return 0; }
int vr_active(void) { return 0; }
void vr_init(void) {}
int vr_frame(unsigned t, int w, int h) { (void)t; (void)w; (void)h; return 0; }
void vr_input(Input *in) { (void)in; }
#endif
