// OpenGL 3.3 backend: draws the decoded GX pipeline on the host GPU.
//
// * The EFB is an off-screen framebuffer (640x528 x scale, RGBA8 + 24-bit depth). XFB copies blit it
//   to the window; EFB clears are scissored glClears.
// * Vertices arrive transformed (video.c does the GX vertex pipeline on the CPU). Here they are mapped
//   through the GX viewport into GL clip space, keeping w so interpolation stays perspective-correct.
//   The EFB is stored upside down relative to GL (row 0 = top), handled in the projection.
// * One "uber" fragment shader runs the TEV combiner from uniforms (stages, inputs, swap tables,
//   konst/register colors) plus the alpha test; blending, depth and color masks map to GL state.
// * Textures come from the decoder's cache (decoded RGBA); a GL texture is (re)uploaded only when the
//   cached copy changed.
#include "platform.h"
#include "cpu.h"
#include "gx_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_gpu;

#ifdef TTPC_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

// ---------------------------------------------------------------- function loading
#define GLFUNCS \
    X(void, Clear, (GLbitfield)) X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) X(void, ClearDepth, (GLdouble)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) X(void, DepthFunc, (GLenum)) X(void, DepthMask, (GLboolean)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, BlendEquation, (GLenum)) X(void, DrawArrays, (GLenum, GLint, GLsizei)) X(GLenum, GetError, (void)) X(const GLubyte *, GetString, (GLenum)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) X(void, PixelStorei, (GLenum, GLint)) \
    X(void, GenTextures, (GLsizei, GLuint *)) X(void, BindTexture, (GLenum, GLuint)) X(void, ActiveTexture, (GLenum)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, CompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *)) X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) X(void, GenerateMipmap, (GLenum)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint *)) X(void, BindRenderbuffer, (GLenum, GLuint)) \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) X(void, BindVertexArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) X(void, UseProgram, (GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform1iv, (GLint, GLsizei, const GLint *)) X(void, Uniform2fv, (GLint, GLsizei, const GLfloat *)) X(void, Uniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Uniform1fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, GenQueries, (GLsizei, GLuint *)) X(void, BeginQuery, (GLenum, GLuint)) X(void, EndQuery, (GLenum)) \
    X(void, GetQueryObjectiv, (GLuint, GLenum, GLint *)) X(void, GetQueryObjectui64v, (GLuint, GLenum, unsigned long long *))

#define X(ret, name, args) static ret (APIENTRY *gl##name##_) args;
GLFUNCS
#undef X

static int load_gl(void) {
    int ok = 1;
#define X(ret, name, args) gl##name##_ = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!gl##name##_) { fprintf(stderr, "[gl] missing gl%s\n", #name); ok = 0; }
    GLFUNCS
#undef X
    return ok;
}

// ---------------------------------------------------------------- shaders
static const char *VS =
    "#version 330 core\n"
    "in vec4 aPos; in vec4 aC0; in vec4 aC1;\n"
    "in vec3 aT0; in vec3 aT1; in vec3 aT2; in vec3 aT3; in vec3 aT4; in vec3 aT5; in vec3 aT6; in vec3 aT7;\n"
    "out vec4 vC0; out vec4 vC1;\n"
    "out vec3 vT0; out vec3 vT1; out vec3 vT2; out vec3 vT3; out vec3 vT4; out vec3 vT5; out vec3 vT6; out vec3 vT7;\n"
    "void main() { gl_Position = aPos; vC0 = aC0; vC1 = aC1;\n"
    "  vT0 = aT0; vT1 = aT1; vT2 = aT2; vT3 = aT3; vT4 = aT4; vT5 = aT5; vT6 = aT6; vT7 = aT7; }\n";

static const char *FS =
    "#version 330 core\n"
    "in vec4 vC0; in vec4 vC1;\n"
    "in vec3 vT0; in vec3 vT1; in vec3 vT2; in vec3 vT3; in vec3 vT4; in vec3 vT5; in vec3 vT6; in vec3 vT7;\n"
    "uniform int uNst;\n"
    "uniform ivec4 uCsel[16]; uniform ivec4 uAsel[16];\n"
    "uniform ivec4 uCop[16];  // bias, sub, clamp, scale\n"
    "uniform ivec4 uAop[16];\n"
    "uniform ivec4 uSt[16];   // cdest, adest, texmap (-1 none), texcoord\n"
    "uniform ivec4 uRsw[16]; uniform ivec4 uTsw[16];\n"
    "uniform int uChan[16];\n"
    "uniform vec4 uK[16]; uniform float uKA[16];\n"
    "uniform vec4 uReg[4];\n"
    "uniform ivec4 uAT; uniform int uAOp;  // alpha test: comp0, ref0, comp1, ref1; logic op\n"
    "uniform int uNbump; uniform ivec4 uIS[4]; uniform int uIndCmd[16];\n"
    "uniform vec4 uIM[6]; uniform vec2 uCS[8]; uniform vec2 uMS[8];\n"
    "uniform sampler2D uTex0, uTex1, uTex2, uTex3, uTex4, uTex5, uTex6, uTex7;\n"
    "out vec4 oColor;\n"
    "vec3 tc(int i) { if (i == 0) return vT0; if (i == 1) return vT1; if (i == 2) return vT2; if (i == 3) return vT3;\n"
    "  if (i == 4) return vT4; if (i == 5) return vT5; if (i == 6) return vT6; return vT7; }\n"
    "vec4 samp(int m, vec2 uv) { if (m == 0) return texture(uTex0, uv); if (m == 1) return texture(uTex1, uv);\n"
    "  if (m == 2) return texture(uTex2, uv); if (m == 3) return texture(uTex3, uv); if (m == 4) return texture(uTex4, uv);\n"
    "  if (m == 5) return texture(uTex5, uv); if (m == 6) return texture(uTex6, uv); return texture(uTex7, uv); }\n"
    "vec4 sw(vec4 c, ivec4 s) { return vec4(c[s.x], c[s.y], c[s.z], c[s.w]); }\n"
    "vec4 R[4];\n"
    "vec3 cin(int sel, vec4 T, vec4 C, vec4 K) {\n"
    "  if (sel < 8) { vec4 r = R[sel / 2]; return (sel % 2 == 0) ? r.rgb : vec3(r.a); }\n"
    "  if (sel == 8) return T.rgb; if (sel == 9) return vec3(T.a); if (sel == 10) return C.rgb; if (sel == 11) return vec3(C.a);\n"
    "  if (sel == 12) return vec3(1.0); if (sel == 13) return vec3(0.5); if (sel == 14) return K.rgb; return vec3(0.0); }\n"
    "float ain(int sel, vec4 T, vec4 C, float KA) {\n"
    "  if (sel < 4) return R[sel].a; if (sel == 4) return T.a; if (sel == 5) return C.a; if (sel == 6) return KA; return 0.0; }\n"
    "bool cmpf(int c, int v, int r) { if (c == 0) return false; if (c == 1) return v < r; if (c == 2) return v == r;\n"
    "  if (c == 3) return v <= r; if (c == 4) return v > r; if (c == 5) return v != r; if (c == 6) return v >= r; return true; }\n"
    "float wrapc(float x, int w) { if (w == 0) return x; if (w >= 6) return 0.0; float n = float(256 >> (w - 1)); return x - n * floor(x / n); }\n"
    "vec2 coordt(int i) { vec3 t = tc(i); return t.xy / (t.z == 0.0 ? 1.0 : t.z) * uCS[i]; }   // in texels\n"
    "void main() {\n"
    "  const float bias_t[4] = float[4](0.0, 0.5, -0.5, 0.0); const float scale_t[4] = float[4](1.0, 2.0, 4.0, 0.5);\n"
    "  for (int i = 0; i < 4; i++) R[i] = uReg[i];\n"
    "  vec4 outc = vec4(0.0);\n"
    "  ivec3 ind[4];\n"
    "  for (int i = 0; i < 4; i++) { ind[i] = ivec3(0); if (i < uNbump) {\n"
    "    ivec4 is = uIS[i]; vec2 u = coordt(is.y) / vec2(float(1 << is.z), float(1 << is.w));\n"
    "    vec4 c = samp(is.x, u / uMS[is.x]); ind[i] = ivec3(round(c.a * 255.0), round(c.b * 255.0), round(c.g * 255.0)); } }\n"
    "  vec2 prev = vec2(0.0);\n"
    "  for (int s = 0; s < uNst; s++) {\n"
    "    ivec4 st = uSt[s];\n"
    "    vec4 T = vec4(1.0); int cmd = uIndCmd[s]; int bump = 0;\n"
    "    if (cmd == 0) {\n"
    "      if (st.z >= 0) { vec3 t = tc(st.w); T = sw(samp(st.z, t.xy / (t.z == 0.0 ? 1.0 : t.z)), uTsw[s]); prev = coordt(st.w); }\n"
    "    } else {   // indirect: offset this stage's texel coordinate by a matrix times the indirect texture value\n"
    "      int bt = cmd & 3, fmt = (cmd >> 2) & 3, bi = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;\n"
    "      int wS = (cmd >> 13) & 7, wT = (cmd >> 16) & 7, addp = (cmd >> 20) & 1;\n"
    "      ivec3 ic = ind[bt];\n"
    "      if (bs != 0) { int v = ic[bs - 1]; bump = fmt == 0 ? (v & 0xF8) : fmt == 1 ? ((v << 3) & 0xF8) : fmt == 2 ? ((v << 4) & 0xF0) : ((v << 5) & 0xE0); }\n"
    "      int msk = fmt == 0 ? 255 : fmt == 1 ? 31 : fmt == 2 ? 15 : 7; ic &= ivec3(msk);\n"
    "      int bv = fmt == 0 ? -128 : 1;\n"
    "      vec3 v = vec3(ic) + vec3((bi & 1) != 0 ? bv : 0, (bi & 2) != 0 ? bv : 0, (bi & 4) != 0 ? bv : 0);\n"
    "      vec2 uv = st.w >= 0 ? coordt(st.w) : vec2(0.0);\n"
    "      vec2 off = vec2(0.0); int mi = (mid & 3) - 1;\n"
    "      if (mi >= 0) { float sc = exp2(uIM[2 * mi].w - 17.0);\n"
    "        if ((mid >> 2) == 0) off = vec2(dot(uIM[2 * mi].xyz, v), dot(uIM[2 * mi + 1].xyz, v)) * sc;\n"
    "        else if ((mid >> 2) == 1) off = uv * v.x / 256.0 * sc;\n"
    "        else if ((mid >> 2) == 2) off = uv * v.y / 256.0 * sc; }\n"
    "      vec2 tcd = vec2(wrapc(uv.x, wS), wrapc(uv.y, wT)) + off;\n"
    "      if (addp != 0) tcd += prev;\n"
    "      prev = tcd;\n"
    "      if (st.z >= 0) T = sw(samp(st.z, tcd / uMS[st.z]), uTsw[s]);\n"
    "    }\n"
    "    vec4 C = vec4(0.0);\n"
    "    if (uChan[s] == 0) C = sw(vC0, uRsw[s]); else if (uChan[s] == 1) C = sw(vC1, uRsw[s]);\n"
    "    else if (uChan[s] == 5) C = vec4(float(bump) / 255.0); else if (uChan[s] == 6) C = vec4(float(bump | (bump >> 5)) / 255.0);\n"
    "    ivec4 cs = uCsel[s]; ivec4 co = uCop[s];\n"
    "    vec3 a = cin(cs.x, T, C, uK[s]), b = cin(cs.y, T, C, uK[s]), c = cin(cs.z, T, C, uK[s]), d = cin(cs.w, T, C, uK[s]);\n"
    "    vec3 l = a * (1.0 - c) + b * c;\n"
    "    vec3 v = co.x == 3 ? d + l : (d + (co.y != 0 ? -l : l) + bias_t[co.x]) * scale_t[co.w];\n"
    "    v = co.z != 0 ? clamp(v, 0.0, 1.0) : clamp(v, -4.0, 4.0);\n"
    "    ivec4 as = uAsel[s]; ivec4 ao = uAop[s];\n"
    "    float aa = ain(as.x, T, C, uKA[s]), ab = ain(as.y, T, C, uKA[s]), ac = ain(as.z, T, C, uKA[s]), ad = ain(as.w, T, C, uKA[s]);\n"
    "    float al = aa * (1.0 - ac) + ab * ac;\n"
    "    float av = ao.x == 3 ? ad + al : (ad + (ao.y != 0 ? -al : al) + bias_t[ao.x]) * scale_t[ao.w];\n"
    "    av = ao.z != 0 ? clamp(av, 0.0, 1.0) : clamp(av, -4.0, 4.0);\n"
    "    R[st.x].rgb = v; R[st.y].a = av;\n"
    "    outc = vec4(R[st.x].rgb, R[st.y].a);\n"
    "  }\n"
    "  outc = clamp(outc, 0.0, 1.0);\n"
    "  int va = int(outc.a * 255.0 + 0.5);\n"
    "  bool a0 = cmpf(uAT.x, va, uAT.y), a1 = cmpf(uAT.z, va, uAT.w);\n"
    "  bool pass = uAOp == 0 ? (a0 && a1) : uAOp == 1 ? (a0 || a1) : uAOp == 2 ? (a0 != a1) : (a0 == a1);\n"
    "  if (!pass) discard;\n"
    "  oColor = outc;\n"
    "}\n";

static GLuint g_prog, g_vao, g_vbo, g_fbo, g_color, g_depth;

// ---------------------------------------------------------------- EFB -> texture copies on the GPU
// The copied rectangle is drawn from the EFB colour texture into a texture of its own, converted to
// the requested format (intensity, RGB565 alpha, single channels...). Draws that sample the copy's
// destination address use that texture: no GPU->CPU readback, and copies keep the internal resolution.
static const char *CVS =
    "#version 330 core\n"
    "uniform vec4 uRect; out vec2 vUV;\n"
    "void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
    "  vUV = uRect.xy + p * uRect.zw; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *CFS =
    "#version 330 core\n"
    "uniform sampler2D uSrc; uniform int uFmt; uniform int uInten; in vec2 vUV; out vec4 o;\n"
    "void main() { vec4 c = texture(uSrc, vUV);\n"
    "  float y = clamp(dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);\n"
    "  float i = uInten != 0 ? y : c.r;\n"
    "  if (uFmt == 0 || uFmt == 1 || uFmt == 8) o = vec4(i);\n"
    "  else if (uFmt == 2 || uFmt == 3) o = vec4(i, i, i, c.a);\n"
    "  else if (uFmt == 4) o = vec4(c.rgb, 1.0);\n"
    "  else if (uFmt == 7) o = vec4(c.a);\n"
    "  else if (uFmt == 9) o = vec4(c.g);\n"
    "  else if (uFmt == 10) o = vec4(c.b);\n"
    "  else if (uFmt == 11) o = vec4(c.r, c.r, c.r, c.g);\n"
    "  else if (uFmt == 12) o = vec4(c.g, c.g, c.g, c.b);\n"
    "  else o = c; }\n";
typedef struct { u32 dst; GLuint tex, fbo; int w, h, tw, th; } CopyTex;
static CopyTex g_ct[64]; static int g_nct;
static GLuint g_cprog, g_cvao; static GLint g_cu_rect, g_cu_fmt, g_cu_inten;
static int g_scale = 1, g_W, g_H;
static long long g_rt_budget;   // texture pack GPU memory budget (below)
// Ultrawide: the EFB is g_NW native pixels wide instead of 640, the game's 640 sitting in the middle
// (g_M on each side). Perspective geometry the game draws beyond its own 16:9 frame lands in the
// sides; 2D stays centred, except full-width 2D (fades, overlays, backgrounds), which is stretched.
static int g_M = 0, g_NW = 640;
static void wide_x(int x0, int x1, int *o0, int *o1) {   // game x range -> EFB x range (native px)
    if (x0 <= 0 && x1 >= 640) { *o0 = 0; *o1 = g_NW; } else { *o0 = x0 + g_M; *o1 = x1 + g_M; }
}
static struct { GLint nst, csel, asel, cop, aop, st, rsw, tsw, chan, k, ka, reg, at, aop2, nbump, is, indcmd, im, cs, ms; } U;

// ---------------------------------------------------------------- per-configuration shaders
// The uber shader above reads the whole TEV setup from uniforms, so every pixel walks the general
// case (dynamic loops, indexed register files). For each TEV/alpha-test configuration the game uses we
// also build a copy of it with those integer settings baked in as constants: the compiler then unrolls
// and folds it down to the few instructions that configuration needs. Building happens in the
// background where the driver supports it (KHR/ARB_parallel_shader_compile); until a program is ready,
// and for at most a few new programs per frame otherwise, draws use the uber shader.
typedef struct { GLint k, ka, reg, im, cs, ms; } FU;   // float uniforms (per program)
#define CFG_INTS (1 + 7 * 64 + 16 + 16 + 5 + 1 + 16)
typedef struct { int used, state; u64 key; int cfg[CFG_INTS]; GLuint prog; FU u; } Spec;   // state 1 building, 2 ready, 3 failed
static Spec *g_spec; static int g_nspec, g_spec_cap = 4096;
static int g_par_compile, g_spec_budget, g_spec_off;
static GLuint g_vs_obj;
static FU g_uber_fu;
extern u64 g_pf_spec_built, g_pf_spec_draws, g_pf_uber_draws;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader_(type); glShaderSource_(s, 1, &src, NULL); glCompileShader_(s);
    GLint ok; glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog_(s, sizeof log, NULL, log); fprintf(stderr, "[gl] shader error:\n%s\n", log); return 0; }
    return s;
}

// ---------------------------------------------------------------- vertex layout
typedef struct { float pos[4], c0[4], c1[4], t[8][3]; } GV;   // 36 floats

int gpu_init(int scale) {
    if (!load_gl()) return 0;
    g_scale = scale < 1 ? 1 : scale; g_W = g_NW * g_scale; g_H = 528 * g_scale;
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return 0;
    g_vs_obj = vs;
    g_prog = glCreateProgram_(); glAttachShader_(g_prog, vs); glAttachShader_(g_prog, fs);
    static const char *attr[] = { "aPos", "aC0", "aC1", "aT0", "aT1", "aT2", "aT3", "aT4", "aT5", "aT6", "aT7" };
    for (int i = 0; i < 11; i++) glBindAttribLocation_(g_prog, i, attr[i]);
    glLinkProgram_(g_prog);
    GLint ok; glGetProgramiv_(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog_(g_prog, sizeof log, NULL, log); fprintf(stderr, "[gl] link error:\n%s\n", log); return 0; }
    glUseProgram_(g_prog);
#define LOC(f, n) U.f = glGetUniformLocation_(g_prog, n)
    LOC(nst, "uNst"); LOC(csel, "uCsel"); LOC(asel, "uAsel"); LOC(cop, "uCop"); LOC(aop, "uAop"); LOC(st, "uSt");
    LOC(rsw, "uRsw"); LOC(tsw, "uTsw"); LOC(chan, "uChan"); LOC(k, "uK"); LOC(ka, "uKA"); LOC(reg, "uReg");
    LOC(at, "uAT"); LOC(aop2, "uAOp");
    LOC(nbump, "uNbump"); LOC(is, "uIS"); LOC(indcmd, "uIndCmd"); LOC(im, "uIM"); LOC(cs, "uCS"); LOC(ms, "uMS");
    for (int i = 0; i < 8; i++) { char n[8]; snprintf(n, sizeof n, "uTex%d", i); glUniform1i_(glGetUniformLocation_(g_prog, n), i); }
    g_uber_fu = (FU){ U.k, U.ka, U.reg, U.im, U.cs, U.ms };
    g_par_compile = SDL_GL_ExtensionSupported("GL_KHR_parallel_shader_compile") || SDL_GL_ExtensionSupported("GL_ARB_parallel_shader_compile");
    {   // let the driver use several threads for background builds
        typedef void (APIENTRY *MaxThr)(GLuint);
        MaxThr f = (MaxThr)SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsKHR");
        if (!f) f = (MaxThr)SDL_GL_GetProcAddress("glMaxShaderCompilerThreadsARB");
        if (f) f(0xFFFFFFFFu);
    }
    g_spec_off = getenv("TTPC_UBER") != NULL;
    g_spec = calloc((size_t)g_spec_cap, sizeof *g_spec);
    fprintf(stderr, "[gl] per-configuration shaders %s (background builds %s)\n", g_spec_off ? "off" : "on", g_par_compile ? "yes" : "no");

    glGenVertexArrays_(1, &g_vao); glBindVertexArray_(g_vao);
    glGenBuffers_(1, &g_vbo); glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);
    glVertexAttribPointer_(0, 4, GL_FLOAT, GL_FALSE, sizeof(GV), (void *)offsetof(GV, pos));
    glVertexAttribPointer_(1, 4, GL_FLOAT, GL_FALSE, sizeof(GV), (void *)offsetof(GV, c0));
    glVertexAttribPointer_(2, 4, GL_FLOAT, GL_FALSE, sizeof(GV), (void *)offsetof(GV, c1));
    for (int i = 0; i < 8; i++) glVertexAttribPointer_(3 + i, 3, GL_FLOAT, GL_FALSE, sizeof(GV), (void *)(offsetof(GV, t) + 12 * i));
    for (int i = 0; i < 11; i++) glEnableVertexAttribArray_(i);

    // EFB
    glGenTextures_(1, &g_color); glBindTexture_(GL_TEXTURE_2D, g_color);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, g_W, g_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers_(1, &g_depth); glBindRenderbuffer_(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_W, g_H);
    glGenFramebuffers_(1, &g_fbo); glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[gl] EFB framebuffer incomplete\n"); return 0; }
    glViewport_(0, 0, g_W, g_H);
    glClearColor_(0, 0, 0, 0); glClearDepth_(1.0); glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable_(GL_SCISSOR_TEST);
    glEnable_(GL_DEPTH_CLAMP);               // GX does not clip against near/far like GL would
    glPixelStorei_(GL_UNPACK_ALIGNMENT, 4); glPixelStorei_(GL_PACK_ALIGNMENT, 1);
    {   // EFB copy conversion program
        GLuint cv = compile(GL_VERTEX_SHADER, CVS), cf = compile(GL_FRAGMENT_SHADER, CFS);
        if (!cv || !cf) return 0;
        g_cprog = glCreateProgram_(); glAttachShader_(g_cprog, cv); glAttachShader_(g_cprog, cf); glLinkProgram_(g_cprog);
        glGetProgramiv_(g_cprog, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "[gl] copy shader link failed\n"); return 0; }
        glUseProgram_(g_cprog);
        g_cu_rect = glGetUniformLocation_(g_cprog, "uRect"); g_cu_fmt = glGetUniformLocation_(g_cprog, "uFmt"); g_cu_inten = glGetUniformLocation_(g_cprog, "uInten");
        glUniform1i_(glGetUniformLocation_(g_cprog, "uSrc"), 0);
        glGenVertexArrays_(1, &g_cvao);
        glUseProgram_(g_prog); glBindVertexArray_(g_vao);
    }
    // texture pack format: DXT5 where the GPU has it (every desktop GPU), size limit and memory budget from the settings
    g_tp_compress = g_cfg.tex_compress && SDL_GL_ExtensionSupported("GL_EXT_texture_compression_s3tc");
    { static const int lim[3] = { 1024, 2048, 4096 }; g_tp_limit = lim[g_cfg.tex_limit < 0 ? 0 : g_cfg.tex_limit > 2 ? 2 : g_cfg.tex_limit]; }
    g_rt_budget = (long long)(g_cfg.tex_vram_mb > 256 ? g_cfg.tex_vram_mb : 256) << 20;
    g_gpu = 1;
    fprintf(stderr, "[gl] backend ready: %s | %s | EFB %dx%d\n", (const char *)glGetString_(GL_RENDERER), (const char *)glGetString_(GL_VERSION), g_W, g_H);
    return 1;
}

// ---------------------------------------------------------------- textures
// Texture pack replacements on the GPU, one per (replacement id, variant). Finished images come from
// the loader threads (texpack.c); at most ~24 MB is uploaded per frame so a scene full of new
// textures never stalls a frame, and textures unused for a few seconds are dropped when the total
// goes over the memory budget (they come back from the disk cache when needed again).
typedef struct { GLuint tex; u32 bytes; u32 last; u8 failed; } RTex;
static RTex *g_rt; static int g_nrt;
static u32 g_frame_no; static long long g_rt_bytes, g_up_left;
u64 g_pf_tp_uploads, g_pf_tp_upbytes;
static GLuint repl_texture(int id, int var) {
    if (!g_rt) { g_nrt = g_texpack_count * 3; g_rt = calloc((size_t)(g_nrt ? g_nrt : 1), sizeof *g_rt); }
    int i = id * 3 + var; if (i < 0 || i >= g_nrt) return 0;
    RTex *r = &g_rt[i];
    if (r->tex) { r->last = g_frame_no; return r->tex; }
    if (r->failed) return 0;
    int failed;
    TPImage *im = texpack_get(id, var, &failed, g_up_left > 0);
    if (failed) { r->failed = 1; return 0; }
    if (!im) return 0;
    glGenTextures_(1, &r->tex); glBindTexture_(GL_TEXTURE_2D, r->tex);
    int w = im->w, h = im->h;
    for (int l = 0; l < im->nlevels; l++) {
        if (im->compressed) glCompressedTexImage2D_(GL_TEXTURE_2D, l, im->compressed == 2 ? 0x83F1 /* DXT1 */ : 0x83F3 /* DXT5 */, w, h, 0, (GLsizei)im->size[l], im->data[l]);
        else glTexImage2D_(GL_TEXTURE_2D, l, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im->data[l]);
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1;
    }
    glTexParameteri_(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, im->nlevels - 1);
    r->bytes = (u32)im->bytes; r->last = g_frame_no; g_rt_bytes += (long long)im->bytes;
    g_up_left -= (long long)im->bytes; g_pf_tp_uploads++; g_pf_tp_upbytes += im->bytes;
    texpack_free_image(im);
    return r->tex;
}
static void repl_frame(void) {   // once per presented frame: upload allowance, eviction
    g_frame_no++;
    g_up_left = 24ll << 20;
    if (g_rt && g_rt_bytes > g_rt_budget && (g_frame_no & 31) == 0) {
        // drop the least recently used textures (not used for 5+ seconds) until under budget
        for (int pass = 0; pass < 64 && g_rt_bytes > g_rt_budget; pass++) {
            int best = -1; u32 oldest = g_frame_no;
            for (int i = 0; i < g_nrt; i++) if (g_rt[i].tex && g_rt[i].last < oldest) { oldest = g_rt[i].last; best = i; }
            if (best < 0 || g_frame_no - oldest < 300) break;
            glDeleteTextures_(1, &g_rt[best].tex); g_rt[best].tex = 0; g_rt_bytes -= g_rt[best].bytes; g_rt[best].bytes = 0;
        }
    }
}
void gpu_texpack_stats(char *b, int n) {
    snprintf(b, (size_t)n, "texture pack %lld MB on GPU, %d waiting", g_rt_bytes >> 20, g_texpack_count ? texpack_pending() : 0);
}
void gpu_texpack_flush(void) {   // options menu: pack switched off/on
    if (!g_rt) return;
    for (int i = 0; i < g_nrt; i++) if (g_rt[i].tex) { glDeleteTextures_(1, &g_rt[i].tex); g_rt[i].tex = 0; }
    memset(g_rt, 0, sizeof *g_rt * (size_t)g_nrt); g_rt_bytes = 0;
}

static void bind_texture(int unit, const TexV *t) {
    glActiveTexture_(GL_TEXTURE0 + unit);
    static const GLint wrapm[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE };
    if (t->gl_copy) {   // an EFB copy kept on the GPU
        glBindTexture_(GL_TEXTURE_2D, t->gl_copy);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapm[t->ws & 3]); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapm[t->wt & 3]);
        GLint f = t->linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        return;
    }
    TexEntry *e = t->e;
    if (!e || !e->px) { glBindTexture_(GL_TEXTURE_2D, 0); return; }
    static const GLint wrap[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE };
    if (e->rid >= 0 && (g_cfg.textures || texpack_is_prompt(e->rid))) {   // texture pack replacement (or button prompt), once it has loaded
        GLuint rt = repl_texture(e->rid, e->rvar);
        glActiveTexture_(GL_TEXTURE0 + unit);
        if (rt) {
            glBindTexture_(GL_TEXTURE_2D, rt);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[t->ws & 3]); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[t->wt & 3]);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            return;
        }
    }
    if (!e->gl_tex) glGenTextures_(1, &e->gl_tex);
    glBindTexture_(GL_TEXTURE_2D, e->gl_tex);
    if (e->gl_version != e->version) {
        // px holds R<<24|G<<16|B<<8|A in host order: GL_UNSIGNED_INT_8_8_8_8 reads exactly that.
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, e->w, e->h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, e->px);
        e->gl_version = e->version;
    }
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[t->ws & 3]);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[t->wt & 3]);
    GLint f = t->linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

// ---------------------------------------------------------------- state
static void spec_source(const int *c, char *out, size_t cap) {
    // FS with the integer uniforms replaced by constants
    size_t o = 0;
    #define P(...) (o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__))
    const char *p = FS, *nl = strchr(p, '\n');
    P("%.*s\n#pragma optionNV(unroll all)\n", (int)(nl - p), p); p = nl + 1;   // #version line
    const int *q = c;
    int nst = *q++;
    P("const int uNst = %d;\n", nst);
    static const char *arr[6] = { "uCsel", "uAsel", "uCop", "uAop", "uSt", "uRsw" };
    for (int a = 0; a < 7; a++) {
        P("const ivec4 %s[16] = ivec4[16](", a < 6 ? arr[a] : "uTsw");
        for (int i = 0; i < 16; i++, q += 4) P("%sivec4(%d,%d,%d,%d)", i ? "," : "", q[0], q[1], q[2], q[3]);
        P(");\n");
    }
    P("const int uChan[16] = int[16]("); for (int i = 0; i < 16; i++) P("%s%d", i ? "," : "", *q++); P(");\n");
    P("const int uIndCmd[16] = int[16]("); for (int i = 0; i < 16; i++) P("%s%d", i ? "," : "", *q++); P(");\n");
    P("const ivec4 uAT = ivec4(%d,%d,%d,%d);\n", q[0], q[1], q[2], q[3]); q += 4;
    P("const int uAOp = %d;\n", *q++);
    P("const int uNbump = %d;\n", *q++);
    P("const ivec4 uIS[4] = ivec4[4]("); for (int i = 0; i < 4; i++, q += 4) P("%sivec4(%d,%d,%d,%d)", i ? "," : "", q[0], q[1], q[2], q[3]); P(");\n");
    while (*p) {   // the rest, minus the integer uniform declarations
        nl = strchr(p, '\n'); int len = nl ? (int)(nl - p) + 1 : (int)strlen(p);
        if (strncmp(p, "uniform int", 11) && strncmp(p, "uniform ivec4", 13)) P("%.*s", len, p);
        p += len;
    }
    #undef P
}
static int spec_ready(Spec *e) {   // finish a program whose build has completed
    if (e->state != 1) return e->state == 2;
    if (g_par_compile) { GLint done = 0; glGetProgramiv_(e->prog, 0x91B1 /* GL_COMPLETION_STATUS */, &done); if (!done) return 0; }
    GLint ok = 0; glGetProgramiv_(e->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; glGetProgramInfoLog_(e->prog, sizeof log, NULL, log);
        static int nerr; if (nerr++ < 3) fprintf(stderr, "[gl] specialised shader failed: %s\n", log);
        e->state = 3; return 0;
    }
    glUseProgram_(e->prog);
    for (int i = 0; i < 8; i++) { char n[8]; snprintf(n, sizeof n, "uTex%d", i); glUniform1i_(glGetUniformLocation_(e->prog, n), i); }
    e->u = (FU){ glGetUniformLocation_(e->prog, "uK"), glGetUniformLocation_(e->prog, "uKA"), glGetUniformLocation_(e->prog, "uReg"),
                 glGetUniformLocation_(e->prog, "uIM"), glGetUniformLocation_(e->prog, "uCS"), glGetUniformLocation_(e->prog, "uMS") };
    e->state = 2; g_pf_spec_built++;
    return 1;
}
// The program for this configuration if it is ready (0: use the uber shader).
static GLuint spec_program(const int *cfg, FU *fu) {
    if (g_spec_off || !g_spec) return 0;
    u64 h = 1469598103934665603ull;
    for (int i = 0; i < CFG_INTS; i++) { h ^= (u32)cfg[i]; h *= 1099511628211ull; }
    int i = (int)(h % (u64)g_spec_cap);
    for (int probe = 0; probe < g_spec_cap; probe++, i = (i + 1) % g_spec_cap) {
        Spec *e = &g_spec[i];
        if (!e->used) {
            if (g_nspec >= g_spec_cap * 3 / 4) return 0;   // table full: stay on the uber shader
            if (!g_par_compile && g_spec_budget <= 0) return 0;   // a few synchronous builds per frame at most
            g_spec_budget--;
            e->used = 1; e->key = h; memcpy(e->cfg, cfg, sizeof e->cfg); g_nspec++;
            static char src[65536]; spec_source(cfg, src, sizeof src);
            GLuint fs = glCreateShader_(GL_FRAGMENT_SHADER); const char *sp = src;
            glShaderSource_(fs, 1, &sp, NULL); glCompileShader_(fs);
            e->prog = glCreateProgram_(); glAttachShader_(e->prog, g_vs_obj); glAttachShader_(e->prog, fs);
            static const char *attr[] = { "aPos", "aC0", "aC1", "aT0", "aT1", "aT2", "aT3", "aT4", "aT5", "aT6", "aT7" };
            for (int a = 0; a < 11; a++) glBindAttribLocation_(e->prog, a, attr[a]);
            glLinkProgram_(e->prog);
            e->state = 1;
            if (!spec_ready(e)) return 0;
            *fu = e->u; return e->prog;
        }
        if (e->key == h && !memcmp(e->cfg, cfg, sizeof e->cfg)) {
            if (!spec_ready(e)) return 0;
            *fu = e->u; return e->prog;
        }
    }
    return 0;
}

static void set_state(const Setup *s) {
    GLint csel[64], asel[64], cop[64], aop[64], st[64], rsw[64], tsw[64], chan[16]; GLfloat k[64], ka[16], reg[16];
    for (int i = 0; i < s->nst; i++) {
        const Stage *g = &s->st[i];
        for (int j = 0; j < 4; j++) { csel[4 * i + j] = g->csel[j]; asel[4 * i + j] = g->asel[j]; rsw[4 * i + j] = g->rsw[j]; tsw[4 * i + j] = g->tsw[j]; k[4 * i + j] = g->K[j]; }
        cop[4 * i] = g->cb; cop[4 * i + 1] = g->csub; cop[4 * i + 2] = g->cclamp; cop[4 * i + 3] = g->cscale;
        aop[4 * i] = g->ab; aop[4 * i + 1] = g->asub; aop[4 * i + 2] = g->aclamp; aop[4 * i + 3] = g->ascale;
        st[4 * i] = g->cdest; st[4 * i + 1] = g->adest; st[4 * i + 2] = g->map; st[4 * i + 3] = g->coord;
        chan[i] = g->chan; ka[i] = g->KA;
    }
    for (int r = 0; r < 4; r++) for (int j = 0; j < 4; j++) reg[4 * r + j] = s->reg[r][j];
    int n = s->nst;
    // the integer configuration (layout must match spec_source)
    static int cfg[CFG_INTS]; memset(cfg, 0, sizeof cfg);
    {   int *q = cfg; *q++ = n;
        const GLint *arrs[7] = { csel, asel, cop, aop, st, rsw, tsw };
        for (int a = 0; a < 7; a++) { memcpy(q, arrs[a], sizeof(GLint) * 4 * n); q += 64; }
        memcpy(q, chan, sizeof(GLint) * n); q += 16;
        for (int i = 0; i < n; i++) q[i] = (GLint)s->indcmd[i]; q += 16;
        *q++ = s->ac_c0; *q++ = s->ac_r0; *q++ = s->ac_c1; *q++ = s->ac_r1; *q++ = s->ac_op;
        int any = 0; for (int i = 0; i < n; i++) any |= s->indcmd[i];
        *q++ = any ? s->nbump : 0;
        if (any) for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) *q++ = s->is[i][j];
    }
    FU fu; GLuint sp = spec_program(cfg, &fu);
    if (sp) {   // specialised: only the float settings are uniforms
        glUseProgram_(sp); g_pf_spec_draws++;
        glUniform4fv_(fu.k, n, k); glUniform1fv_(fu.ka, n, ka); glUniform4fv_(fu.reg, 4, reg);
        int any = 0; for (int i = 0; i < n; i++) any |= s->indcmd[i];
        if (any) {
            GLfloat ms[16];
            for (int m = 0; m < 8; m++) { ms[2 * m] = s->tex[m].w > 0 ? (float)s->tex[m].w : 1.f; ms[2 * m + 1] = s->tex[m].h > 0 ? (float)s->tex[m].h : 1.f; }
            glUniform4fv_(fu.im, 6, &s->im[0][0]); glUniform2fv_(fu.cs, 8, &s->cs[0][0]); glUniform2fv_(fu.ms, 8, ms);
        }
        for (int m = 0; m < 8; m++) if (s->tex[m].e || s->tex[m].gl_copy) bind_texture(m, &s->tex[m]);
        goto fixed_function;
    }
    glUseProgram_(g_prog); g_pf_uber_draws++;
    glUniform1i_(U.nst, n);
    glUniform4iv_(U.csel, n, csel); glUniform4iv_(U.asel, n, asel); glUniform4iv_(U.cop, n, cop); glUniform4iv_(U.aop, n, aop);
    glUniform4iv_(U.st, n, st); glUniform4iv_(U.rsw, n, rsw); glUniform4iv_(U.tsw, n, tsw); glUniform1iv_(U.chan, n, chan);
    glUniform4fv_(U.k, n, k); glUniform1fv_(U.ka, n, ka); glUniform4fv_(U.reg, 4, reg);
    GLint at[4] = { s->ac_c0, s->ac_r0, s->ac_c1, s->ac_r1 }; glUniform4iv_(U.at, 1, at); glUniform1i_(U.aop2, s->ac_op);
    {   // indirect texturing
        GLint ic[16], is[16]; GLfloat ms[16];
        int any = 0; for (int i = 0; i < n; i++) { ic[i] = (GLint)s->indcmd[i]; any |= ic[i]; }
        glUniform1iv_(U.indcmd, n, ic);
        if (any) {
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) is[4 * i + j] = s->is[i][j];
            for (int m = 0; m < 8; m++) { ms[2 * m] = s->tex[m].w > 0 ? (float)s->tex[m].w : 1.f; ms[2 * m + 1] = s->tex[m].h > 0 ? (float)s->tex[m].h : 1.f; }
            glUniform1i_(U.nbump, s->nbump); glUniform4iv_(U.is, 4, is); glUniform4fv_(U.im, 6, &s->im[0][0]);
            glUniform2fv_(U.cs, 8, &s->cs[0][0]); glUniform2fv_(U.ms, 8, ms);
        } else glUniform1i_(U.nbump, 0);
    }
    for (int m = 0; m < 8; m++) if (s->tex[m].e || s->tex[m].gl_copy) bind_texture(m, &s->tex[m]);

fixed_function:
    // depth
    static const GLenum zf[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    if (s->zm & 1) { glEnable_(GL_DEPTH_TEST); glDepthFunc_(zf[(s->zm >> 1) & 7]); glDepthMask_((s->zm & 16) ? GL_TRUE : GL_FALSE); }
    else { glDisable_(GL_DEPTH_TEST); glDepthMask_(GL_FALSE); }
    // blending (PE BLENDMODE): bit0 enable, bit3 color update, bit4 alpha update, 5-7 dst factor,
    // 8-10 src factor, bit11 subtract (dst - src)
    u32 bm = s->bm;
    glColorMask_((bm & 8) != 0, (bm & 8) != 0, (bm & 8) != 0, (bm & 16) != 0);
    if (bm & 0x800) { glEnable_(GL_BLEND); glBlendEquation_(GL_FUNC_REVERSE_SUBTRACT); glBlendFunc_(GL_ONE, GL_ONE); }
    else if (bm & 1) {
        static const GLenum src_f[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
        static const GLenum dst_f[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
        glEnable_(GL_BLEND); glBlendEquation_(GL_FUNC_ADD); glBlendFunc_(src_f[(bm >> 8) & 7], dst_f[(bm >> 5) & 7]);
    } else glDisable_(GL_BLEND);
    // scissor (EFB pixels, rows from the top)
    int x0 = s->sx0, y0 = s->sy0, x1 = s->sx1 + 1, y1 = s->sy1 + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 640) x1 = 640; if (y1 > 528) y1 = 528;
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    int ex0, ex1; wide_x(x0, x1, &ex0, &ex1);
    glScissor_(ex0 * g_scale, (528 - y1) * g_scale, (ex1 - ex0) * g_scale, (y1 - y0) * g_scale);
}

// Ultrawide side fill: 2D that reaches the edge of the game's frame (backgrounds, panel strips,
// borders) is continued into the side margin as a mirror image of its own edge strip. Mirroring
// keeps to the part of the texture the game drew there, so atlases and clamped textures stay clean.
#define GVF (int)(sizeof(GV) / sizeof(float))
static int clip_x(const GV *in, int n, GV *out, float x, int keep_above, float half) {
    // keep the part of polygon `in` with X >= x (keep_above) or X <= x; X = EFB column of pos[0]
    int m = 0;
    for (int i = 0; i < n; i++) {
        const GV *a = &in[i], *b = &in[(i + 1) % n];
        float xa = (a->pos[0] + 1.f) * half - x, xb = (b->pos[0] + 1.f) * half - x;
        if (!keep_above) { xa = -xa; xb = -xb; }
        if (xa >= 0) out[m++] = *a;
        if ((xa >= 0) != (xb >= 0)) {
            float t = xa / (xa - xb); const float *fa = (const float *)a, *fb = (const float *)b; float *fo = (float *)&out[m++];
            for (int k = 0; k < GVF; k++) fo[k] = fa[k] + (fb[k] - fa[k]) * t;
        }
    }
    return m;
}
static int mirror_side(const GV *tri, GV *out, float lo, float hi, float axis, float half) {
    GV p1[8], p2[8];
    int n = clip_x(tri, 3, p1, lo, 1, half); if (n < 3) return 0;
    n = clip_x(p1, n, p2, hi, 0, half); if (n < 3) return 0;
    for (int i = 0; i < n; i++) { float X = (p2[i].pos[0] + 1.f) * half; p2[i].pos[0] = (2.f * axis - X) / half - 1.f; }
    int m = 0;
    for (int i = 1; i + 1 < n; i++) { out[m++] = p2[0]; out[m++] = p2[i]; out[m++] = p2[i + 1]; }
    return m;
}
// Repeating patterns are laid as many copies of one identical tile (same texture, same texture
// coordinates, same size). Counted per frame; a tile seen 3+ times last frame repeats outwards.
#define TSIG 8192
static u64 g_tsig[2][TSIG]; static u8 g_tcnt[2][TSIG]; static int g_tcur;
// tiles recognised recently stay tiles for a second, so a pattern drawn a little differently from one frame to the
// next doesn't switch between repeated and not (the sides flickered on the title screen)
#define TKEEP 256
static u64 g_tkeep[TKEEP]; static u32 g_tkeep_t[TKEEP]; static u32 g_tframe;
static int tile_kept(u64 sig, int mark) {
    int i = (int)(sig % TKEEP);
    for (int k = 0; k < 4; k++) { int j = (i + k) % TKEEP;
        if (g_tkeep[j] == sig) { if (mark) g_tkeep_t[j] = g_tframe; return g_tframe - g_tkeep_t[j] < 60; } }
    if (mark) { int best = i; for (int k = 0; k < 4; k++) { int j = (i + k) % TKEEP; if (!g_tkeep[j] || g_tframe - g_tkeep_t[j] > g_tframe - g_tkeep_t[best]) best = j; }
        g_tkeep[best] = sig; g_tkeep_t[best] = g_tframe; }
    return 0;
}
static int tile_seen(u64 sig, int add) {   // returns last frame's count
    int i = (int)(sig % TSIG), prev = g_tcur ^ 1, r = 0;
    for (int k = 0; k < 32; k++) { int j = (i + k) % TSIG; if (g_tsig[prev][j] == sig) { r = g_tcnt[prev][j]; break; } if (!g_tsig[prev][j]) break; }
    if (add) for (int k = 0; k < 32; k++) { int j = (i + k) % TSIG;
        if (g_tsig[g_tcur][j] == sig) { if (g_tcnt[g_tcur][j] < 255) g_tcnt[g_tcur][j]++; break; }
        if (!g_tsig[g_tcur][j]) { g_tsig[g_tcur][j] = sig; g_tcnt[g_tcur][j] = 1; break; } }
    if (r >= 3) tile_kept(sig, 1); else if (tile_kept(sig, 0)) r = 3;
    return r;
}
// VR: HUD pieces the room shows somewhere else (the LIMIT hearts) are drawn into their own layer, keyed like the frame
static GLuint g_lay_fbo, g_lay_color, g_lay_last_fbo, g_lay_last; static int g_lay_w, g_lay_h, g_lay_lw, g_lay_lh, g_lay_used;
static void lay_clear(void) {
    glBindFramebuffer_(GL_FRAMEBUFFER, g_lay_fbo); glDisable_(GL_SCISSOR_TEST); glColorMask_(1, 1, 1, 1);
    glClearColor_(1, 0, 1, 1); glClear_(GL_COLOR_BUFFER_BIT); glEnable_(GL_SCISSOR_TEST); glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
}
static void lay_ensure(void) {
    if (g_lay_fbo && g_lay_w == g_W && g_lay_h == g_H) return;
    if (!g_lay_fbo) { glGenFramebuffers_(1, &g_lay_fbo); glGenTextures_(1, &g_lay_color); }
    glBindTexture_(GL_TEXTURE_2D, g_lay_color);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, g_W, g_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_lay_fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_lay_color, 0);
    glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    g_lay_w = g_W; g_lay_h = g_H; lay_clear();
}
// a comic panel standing still: the same piece in the same place last frame (pieces sliding past the edge, as on
// the map pages, are left alone - picked up on some frames and not others, the sides flickered)
static int comic_steady(const Setup *s, float mn, float mx, float y0, float y1) {
    u64 sig = (s->tex[0].e ? s->tex[0].e->data_hash : (u64)s->tex[0].gl_copy) * 0x9E3779B97F4A7C15ull ^ 0xC0311Cull;
    sig ^= (u64)(int)mn * 131 + (u64)(int)mx * 65537 + (u64)(int)y0 * 7919 + (u64)(int)y1 * 104729;
    if (!sig) sig = 1;
    return tile_seen(sig, 1) >= 1;
}
static void tile_frame(void) { g_tframe++; g_tcur ^= 1; memset(g_tsig[g_tcur], 0, sizeof g_tsig[0]); memset(g_tcnt[g_tcur], 0, sizeof g_tcnt[0]); }
void gpu_draw(const Setup *s, const Out *v, int n) {
    static GV *buf; static int cap;
    if (n * 7 > cap) { free(buf); cap = n * 7 + 1024; buf = malloc(sizeof *buf * (size_t)cap); }
    float wd = s->vp[0], ht = s->vp[1], zr = s->vp[2], xo = s->vp[3] - 342.f, yo = s->vp[4] - 342.f, fz = s->vp[5];
    int vr_sk = vr_draw_skip(s, v, n, wd, ht, xo, yo); if (vr_sk == 1) return;
    if (vr_sk == 2) lay_ensure();
    // ultrawide: 2D (w == 1 everywhere) spanning the game's whole width is stretched to the EFB's
    float xs = 1.f, xm = (float)g_M; int touch_l = 0, touch_r = 0; float tile_w = 0, edge_l = 0, edge_r = 0;
    int comic = 0, comic_l = 0; float cy0 = 0, cy1 = 0;   // a comic page's corner panel (see below)
    if (g_M) {
        int flat = 1; float mn = 1e9f, mx = -1e9f, hy0 = 1e9f, hy1 = -1e9f;
        for (int i = 0; i < n && flat; i++) {
            if (v[i].w != 1.f) { flat = 0; break; }
            float x = v[i].x * wd + xo; if (x < mn) mn = x; if (x > mx) mx = x;
            float y = v[i].y * ht + yo; if (y < hy0) hy0 = y; if (y > hy1) hy1 = y;
        }
        if (flat && g_cfg.wide_fill && n <= 6) {   // one quad: is it a tile of a repeating pattern?
            float u0 = 1e9f, u1 = -1e9f, t0 = 1e9f, t1 = -1e9f, ymn = 1e9f, ymx = -1e9f;
            for (int i = 0; i < n; i++) { float u = v[i].uv[0][0], t = v[i].uv[0][1], y = v[i].y * ht + yo;
                if (u < u0) u0 = u; if (u > u1) u1 = u; if (t < t0) t0 = t; if (t > t1) t1 = t; if (y < ymn) ymn = y; if (y > ymx) ymx = y; }
            u64 sig = (s->tex[0].e ? s->tex[0].e->data_hash : (u64)s->tex[0].gl_copy) * 0x9E3779B97F4A7C15ull;
            sig ^= (u64)(int)((u1 - u0) * 1000) * 31 + (u64)(int)((t1 - t0) * 1000) * 1009 + (u64)(int)(mx - mn) * 65537 + (u64)(int)(ymx - ymn) * 7919 + (u64)(int)(u0 * 1000) * 131;
            if (!sig) sig = 1;
            if (tile_seen(sig, 1) >= 3) tile_w = mx - mn;
        }
        // reaches the frame edge; edge_l/edge_r = its outer edge in EFB columns (it may already reach
        // part of the way into the side)
        // a layer drawing a copy of the screen itself (post effects): its texture already holds the
        // sides, so it is stretched to the full width like the copy was
        // Full-screen layers are stretched (pictures would show mirrored duplicates of what is near
        // their edge); narrow strips at the frame edge (borders) are mirrored outwards; tiles of a
        // repeating pattern are repeated outwards; other partial pieces of a picture are left alone.
        // (a layer wider than the screen is a picture zoomed past its edges - a character close up - unless it
        // also covers the whole height (a panning background): stretching it drew that piece wider than the rest)
        // Only a layer the size of the screen is stretched (fades, screen copies, backgrounds at their own size).
        // Anything reaching past the screen's edges - a zoomed background, a close-up character's pieces - is left
        // at its own scale: what lies past the edge already fills the sides, and stretching some pieces of a
        // character but not others drew it in two widths.
        if (flat && mn <= 0.5f && mx >= 639.5f && mn >= -8.f && mx <= 648.f && hy0 <= 8.f && hy1 >= 470.f && hy0 >= -8.f && hy1 <= 540.f) { xs = (float)g_NW / 640.f; xm = 0; }
        // (a strip to mirror is a border: taller than a pointer or an icon pushed against the edge - those mirrored
        // into the side showed a second, flipped pointer)
        // A comic page's corner panels (the frame of panels around the office, Naomi's room, the story pages):
        // a strip at the top reaching a side edge lends that side a tall panel of its own (below).
        else if (flat && g_cfg.wide_fill && (mn <= 1.f || mx >= 639.f) && mx - mn >= 120.f && mx - mn <= 400.f &&
                 hy1 - hy0 >= 24.f && hy1 - hy0 <= 200.f && (hy0 <= 2.f || hy1 >= 476.f) && !tile_w &&
                 comic_steady(s, mn, mx, hy0, hy1)) {
            comic = hy0 <= 2.f; cy0 = hy0; cy1 = hy1; comic_l = mn <= 1.f; }
        else if (flat && g_cfg.wide_fill && (tile_w > 0 || (mx - mn <= 120.f && hy1 - hy0 >= 96.f))) {
            touch_l = mn <= 0.5f && mn + g_M > 0.5f; touch_r = mx >= 639.5f && mx + g_M < g_NW - 0.5f; edge_l = mn + g_M; edge_r = mx + g_M; }
    }
    float half = g_NW * 0.5f;
    for (int i = 0; i < n; i++) {
        const Out *o = &v[i]; GV *g = &buf[i];
        float w = o->w;
        // screen x = x/w*wd + xo (0..640), y likewise (0..528, down), z = (z/w*zr + fz)/2^24 (0..1)
        float sxw = o->x * wd + xo * w, syw = o->y * ht + yo * w, szw = (o->z * zr + fz * w) / 16777215.f;
        g->pos[0] = (sxw * xs + xm * w) / half - w;
        g->pos[1] = w - syw / 264.f;
        g->pos[2] = 2.f * szw - w;
        g->pos[3] = w;
        memcpy(g->c0, o->col[0], 16); memcpy(g->c1, o->col[1], 16);
        memcpy(g->t, o->uv, sizeof g->t);
    }
    { static int dbg = -1; if (dbg < 0) dbg = getenv("TTPC_FILL_DBG") != NULL;
      if (dbg && (touch_l || touch_r || xs != 1.f)) { static int nl; if (nl++ < 4000) fprintf(stderr, "[fill] n %d x %.1f..%.1f tile %.0f L%d R%d stretch %d tex %dx%d copy %u\n",
          n, edge_l - g_M, edge_r - g_M, tile_w, touch_l, touch_r, xs != 1.f, s->tex[0].w, s->tex[0].h, s->tex[0].gl_copy); } }
    if ((touch_l || touch_r) && tile_w >= 4.f && tile_w < 320.f && !(touch_l && touch_r)) {
        // a small piece at the edge (one tile of a grid laid over the screen, like the title's dot
        // pattern): repeat it outwards at its own width until the side is covered
        int reps = (int)ceilf(g_M / tile_w) + 1, m = n;
        if ((size_t)n * (reps + 1) > (size_t)cap) { GV *nb = malloc(sizeof *nb * (size_t)n * (reps + 2)); if (nb) { memcpy(nb, buf, sizeof *nb * (size_t)n); free(buf); buf = nb; cap = n * (reps + 2); } else reps = 0; }
        float step = (touch_l ? -tile_w : tile_w) / half;   // in clip x (w == 1)
        reps = (int)ceilf((touch_l ? edge_l : g_NW - edge_r) / tile_w);
        for (int k = 1; k <= reps; k++)
            for (int i = 0; i < n; i++) { buf[m] = buf[i]; buf[m].pos[0] += step * k; m++; }
        n = m;
    } else if (touch_l || touch_r) {   // bigger 2D (backgrounds, panels): mirror the edge strips
        int m = n; float M = (float)g_M;
        for (int i = 0; i + 2 < n; i += 3) {
            // mirror about the layer's own outer edge, just enough to reach the screen edge
            if (touch_l) m += mirror_side(&buf[i], &buf[m], edge_l, 2 * edge_l, edge_l, half);
            if (touch_r) m += mirror_side(&buf[i], &buf[m], 2 * edge_r - g_NW, edge_r, edge_r, half);
        }
        n = m;
    }
    if (comic && n >= 6 && g_M >= 24) {   // (one tall panel down each side, the page's own corner art turned upright)
        int m = n;
            float u0[8], u1[8], v0[8], v1[8];
            for (int k = 0; k < 8; k++) { u0[k] = v0[k] = 1e9f; u1[k] = v1[k] = -1e9f;
                for (int i = 0; i < 6; i++) { float a = buf[i].t[k][0], b = buf[i].t[k][1];
                    if (a < u0[k]) u0[k] = a; if (a > u1[k]) u1[k] = a; if (b < v0[k]) v0[k] = b; if (b > v1[k]) v1[k] = b; } }
            float gap = 6.f, X0 = comic_l ? gap : g_NW - g_M + gap, X1 = comic_l ? g_M - 2 * gap : g_NW - gap;
            if (!comic_l) X0 += gap;
            float Y0 = gap, Y1 = 480.f - gap;
            if (Y1 - Y0 > 40.f) {
                // corners of the side panel: (X0,Y0) (X1,Y0) (X1,Y1) (X0,Y1), the picture turned a quarter
                float X[4] = { X0, X1, X1, X0 }, Y[4] = { Y0, Y0, Y1, Y1 };
                int ord[6] = { 0, 1, 2, 0, 2, 3 };
                for (int j = 0; j < 6; j++) { int c = ord[j]; GV g = buf[0];
                    g.pos[0] = X[c] / half - 1.f; g.pos[1] = 1.f - Y[c] / 264.f; g.pos[3] = 1.f;
                    for (int k = 0; k < 8; k++) {   // x along the side panel = the picture's v, y = its u
                        float fx = c == 1 || c == 2 ? 1.f : 0.f, fy = c >= 2 ? 1.f : 0.f;
                        g.t[k][0] = u0[k] + (u1[k] - u0[k]) * (comic_l ? fy : 1.f - fy);
                        g.t[k][1] = v0[k] + (v1[k] - v0[k]) * (comic_l ? 1.f - fx : fx); }
                    buf[m++] = g; }
            }
        n = m;
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, vr_sk == 2 && g_lay_fbo ? g_lay_fbo : g_fbo);
    glViewport_(0, 0, g_W, g_H);
    set_state(s);
    glBufferData_(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof(GV), buf, GL_STREAM_DRAW);
    static int od = -1; static GLuint oq; if (od < 0) { od = getenv("TTPC_OVERDRAW") != NULL; if (od) glGenQueries_(1, &oq); }
    if (od) glBeginQuery_(0x8914 /* GL_SAMPLES_PASSED */, oq);
    glDrawArrays_(GL_TRIANGLES, 0, n);
    if (vr_sk == 2) { glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo); g_lay_used = 1; }
    if (od) { glEndQuery_(0x8914); GLint r = 0; glGetQueryObjectiv_(oq, 0x8866, &r);
        extern u64 g_pf_frags; g_pf_frags += (u64)r;
        if (getenv("TTPC_OVERDRAW")[0] == '2' && r > 50000 * g_scale * g_scale)
            fprintf(stderr, "[od] draw %d verts nst %d tex0 %dx%d bm %X: %.2f screens\n", n, s->nst, s->tex[0].w, s->tex[0].h, s->bm, r / (double)(g_W * g_H)); }
}

void gpu_clear(int x0, int y0, int w, int h, u32 c, float z, int color, int alpha, int depth) {
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    if (color && vr_key_frame()) c = 0xFF00FFFFu;   // VR office: the frame starts on the key colour (the painting is left out)
    int ex0, ex1; wide_x(x0, x0 + w, &ex0, &ex1);
    glScissor_(ex0 * g_scale, (528 - y0 - h) * g_scale, (ex1 - ex0) * g_scale, h * g_scale);
    glColorMask_(color, color, color, alpha); glDepthMask_(depth ? GL_TRUE : GL_FALSE);
    glClearColor_((c >> 24) / 255.f, ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f);
    glClearDepth_(z);
    GLbitfield bits = (color || alpha ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0);
    if (bits) glClear_(bits);
}

extern SDL_Window *g_sdl_window;
int g_view_x, g_view_y, g_view_w, g_view_h;   // where the picture sits in the window (for the mouse)
// Display options (set by the options menu, menu.c)
int g_disp_stretch, g_disp_sharp;
static GLuint g_last_fbo, g_last_tex; static int g_last_w, g_last_h;   // the last displayed picture
static int g_pic_x, g_pic_y, g_pic_w, g_pic_h;   // where the whole picture goes (wider than g_view_* when ultrawide)
static void view_rect(int ww, int wh) {   // fit the picture into the window: 4:3, 16:9, stretched, or ultrawide
    long long ax = g_disp_stretch == 1 ? 16 : 4, ay = g_disp_stretch == 1 ? 9 : 3;
    if (g_disp_stretch == 3) { ax = 16LL * g_NW; ay = 9LL * 640; }   // 16:9 widened by the side margins
    int vw = ww, vh = (int)(ww * ay / ax);
    if (vh > wh) { vh = wh; vw = (int)(wh * ax / ay); }
    if (g_disp_stretch == 2) { vw = ww; vh = wh; }
    g_pic_x = (ww - vw) / 2; g_pic_y = (wh - vh) / 2; g_pic_w = vw; g_pic_h = vh;
    // the game's own 640 columns (for the pointer)
    g_view_x = g_pic_x + (int)((long long)vw * g_M / g_NW); g_view_w = (int)((long long)vw * 640 / g_NW);
    g_view_y = g_pic_y; g_view_h = vh;
}
// Ultrawide margin for the window's shape: 16:9 game picture widened to the window (up to 32:9).
static void wide_update(int ww, int wh) {
    int nw = 640;
    if (g_disp_stretch == 3 && wh > 0) {
        double a = (double)ww / wh, base = 16.0 / 9.0;
        if (a > base) { if (a > 32.0 / 9.0) a = 32.0 / 9.0; nw = ((int)(640.0 * a / base + 0.5) + 1) & ~1; }
    }
    if (nw == g_NW) return;
    g_NW = nw; g_M = (nw - 640) / 2; g_W = g_NW * g_scale;
    glBindTexture_(GL_TEXTURE_2D, g_color);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, g_W, g_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer_(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_W, g_H);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glDisable_(GL_SCISSOR_TEST); glViewport_(0, 0, g_W, g_H);
    glClearColor_(0, 0, 0, 0); glClearDepth_(1.0); glColorMask_(1, 1, 1, 1); glDepthMask_(GL_TRUE); glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable_(GL_SCISSOR_TEST);
    fprintf(stderr, "[gl] ultrawide: %d columns (%d each side), internal %dx%d\n", g_NW, g_M, g_W, g_H);
}
// perf (TTPC_PERFLOG): GPU time per frame from timer queries, time blocked in SwapWindow, EFB copies
extern u64 g_pf_gpu_ns, g_pf_gpu_frames, g_pf_swap_ns, g_pf_copies, g_pf_copy_allocs;
u64 host_now_ns(void);
static GLuint g_tq[4]; static int g_tq_n, g_tq_active;
static void gpu_timer_frame(void) {
    if (!g_tq[0]) { glGenQueries_(4, g_tq); }
    if (g_tq_active) glEndQuery_(0x88BF);   // GL_TIME_ELAPSED
    // collect finished queries (oldest first) without waiting
    static int head;
    while (head < g_tq_n) {
        GLuint q = g_tq[head & 3]; GLint ready = 0;
        glGetQueryObjectiv_(q, 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &ready);
        if (!ready) break;
        unsigned long long ns = 0; glGetQueryObjectui64v_(q, 0x8866 /* GL_QUERY_RESULT */, &ns);
        g_pf_gpu_ns += ns; g_pf_gpu_frames++; head++;
    }
    if (g_tq_n - head < 4) { glBeginQuery_(0x88BF, g_tq[g_tq_n & 3]); g_tq_n++; g_tq_active = 1; }
    else g_tq_active = 0;
}
void gpu_present(int x0, int y0, int w, int h) {
    int ww, wh; SDL_GL_GetDrawableSize(g_sdl_window, &ww, &wh);
    view_rect(ww, wh);
    int vw = g_pic_w, vh = g_pic_h;
    int ex0, ex1; wide_x(x0, x0 + w, &ex0, &ex1);
    // keep a copy of the displayed picture (the options menu shows it, paused, behind itself)
    int lw = (ex1 - ex0) * g_scale, lh = h * g_scale;
    { static int dbg = -1, n; if (dbg < 0) dbg = getenv("TTPC_PRESENT_DBG") != NULL;   // tests: where the picture goes
      if (dbg && n++ % 120 == 0) fprintf(stderr, "[present] win %dx%d pic %d,%d %dx%d src x %d..%d y %d h %d | EFB %dx%d\n", ww, wh, g_pic_x, g_pic_y, g_pic_w, g_pic_h, ex0, ex1, y0, h, g_W, g_H); }
    if (!g_last_fbo) { glGenFramebuffers_(1, &g_last_fbo); glGenTextures_(1, &g_last_tex); }
    if (lw != g_last_w || lh != g_last_h) {
        glBindTexture_(GL_TEXTURE_2D, g_last_tex);
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindFramebuffer_(GL_FRAMEBUFFER, g_last_fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_last_tex, 0);
        g_last_w = lw; g_last_h = lh;
    }
    glDisable_(GL_SCISSOR_TEST);
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, g_last_fbo);
    glBlitFramebuffer_(ex0 * g_scale, (528 - y0 - h) * g_scale, ex1 * g_scale, (528 - y0) * g_scale, 0, 0, lw, lh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_vr_pic_y0 = y0; g_vr_pic_h = h;
    if (g_lay_fbo && g_lay_used) {
        if (!g_lay_last_fbo) { glGenFramebuffers_(1, &g_lay_last_fbo); glGenTextures_(1, &g_lay_last); }
        if (lw != g_lay_lw || lh != g_lay_lh) {
            glBindTexture_(GL_TEXTURE_2D, g_lay_last);
            glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer_(GL_FRAMEBUFFER, g_lay_last_fbo);
            glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_lay_last, 0);
            g_lay_lw = lw; g_lay_lh = lh;
        }
        glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_lay_fbo); glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, g_lay_last_fbo);
        glBlitFramebuffer_(ex0 * g_scale, (528 - y0 - h) * g_scale, ex1 * g_scale, (528 - y0) * g_scale, 0, 0, lw, lh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        vr_set_layer(g_lay_last); g_lay_used = 0; lay_clear(); glDisable_(GL_SCISSOR_TEST);
    } else vr_set_layer(0);
    if (!vr_frame(g_last_tex, lw, lh, (float)(g_M - (ex0 - 0)) / (ex1 - ex0), (float)(g_M + 640 - ex0) / (ex1 - ex0))) {   // VR draws the headset (and, in preview, the window) from the picture
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_last_fbo);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor_(0, 0, 0, 1); glColorMask_(1, 1, 1, 1); glClear_(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer_(0, 0, lw, lh, g_pic_x, wh - g_pic_y - vh, g_pic_x + vw, wh - g_pic_y, GL_COLOR_BUFFER_BIT, g_disp_sharp ? GL_NEAREST : GL_LINEAR);
    }
    glEnable_(GL_SCISSOR_TEST);
    if (g_tq_active) { glEndQuery_(0x88BF); g_tq_active = 0; }
    u64 ts = host_now_ns();
    SDL_GL_SwapWindow(g_sdl_window);
    g_pf_swap_ns += host_now_ns() - ts;
    gpu_timer_frame();
    g_spec_budget = 4;
    repl_frame();
    tile_frame();
    wide_update(ww, wh);   // between frames: the window's shape (or the setting) may have changed
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    static int nframes; GLenum err = nframes++ < 600 ? glGetError_() : 0; static int nerr;   // glGetError stalls threaded drivers: early frames only
    if (err && nerr++ < 10) fprintf(stderr, "[gl] error 0x%04X\n", err);
}

// Change the internal resolution (EFB scale) while running.
int gpu_get_scale(void) { return g_scale; }
void gpu_set_scale(int scale) {
    if (scale < 1) scale = 1; if (scale > 8) scale = 8;
    if (scale == g_scale) return;
    g_scale = scale; g_W = g_NW * scale; g_H = 528 * scale;
    glBindTexture_(GL_TEXTURE_2D, g_color);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, g_W, g_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindRenderbuffer_(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_W, g_H);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glDisable_(GL_SCISSOR_TEST); glViewport_(0, 0, g_W, g_H);
    glClearColor_(0, 0, 0, 0); glClearDepth_(1.0); glColorMask_(1, 1, 1, 1); glDepthMask_(GL_TRUE); glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable_(GL_SCISSOR_TEST);
    fprintf(stderr, "[gl] internal resolution %dx%d\n", g_W, g_H);
}

// ---------------------------------------------------------------- overlay (options menu)
#include "font_atlas.h"
static const char *OVS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos; layout(location = 1) in vec2 aUV; layout(location = 2) in vec4 aCol;\n"
    "uniform vec2 uScreen; out vec2 vUV; out vec4 vCol;\n"
    "void main() { vUV = aUV; vCol = aCol; gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0); }\n";
static const char *OFS =
    "#version 330 core\n"
    "uniform sampler2D uTex; uniform int uMode; in vec2 vUV; in vec4 vCol; out vec4 o;\n"
    "void main() { if (uMode == 0) o = vCol; else if (uMode == 1) o = vec4(vCol.rgb, vCol.a * texture(uTex, vUV).r);\n"
    "  else o = vec4(texture(uTex, vUV).rgb * vCol.rgb, 1.0); }\n";
typedef struct { float x, y, u, v, r, g, b, a; } OV;
static GLuint g_oprog, g_ovao, g_ovbo, g_ofont; static GLint g_ou_screen, g_ou_mode;
static OV g_ov[6 * 4096]; static int g_nov, g_omode = -1; static int g_ow, g_oh;
static int overlay_init(void) {
    if (g_oprog) return 1;
    GLuint v = compile(GL_VERTEX_SHADER, OVS), f = compile(GL_FRAGMENT_SHADER, OFS);
    if (!v || !f) return 0;
    g_oprog = glCreateProgram_(); glAttachShader_(g_oprog, v); glAttachShader_(g_oprog, f); glLinkProgram_(g_oprog);
    g_ou_screen = glGetUniformLocation_(g_oprog, "uScreen"); g_ou_mode = glGetUniformLocation_(g_oprog, "uMode");
    glUseProgram_(g_oprog); glUniform1i_(glGetUniformLocation_(g_oprog, "uTex"), 0);
    glGenVertexArrays_(1, &g_ovao); glBindVertexArray_(g_ovao);
    glGenBuffers_(1, &g_ovbo); glBindBuffer_(GL_ARRAY_BUFFER, g_ovbo);
    glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, sizeof(OV), (void *)0);
    glVertexAttribPointer_(1, 2, GL_FLOAT, GL_FALSE, sizeof(OV), (void *)8);
    glVertexAttribPointer_(2, 4, GL_FLOAT, GL_FALSE, sizeof(OV), (void *)16);
    for (int i = 0; i < 3; i++) glEnableVertexAttribArray_(i);
    glGenTextures_(1, &g_ofont); glBindTexture_(GL_TEXTURE_2D, g_ofont);
    glPixelStorei_(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_R8, FONT_W, FONT_H, 0, GL_RED, GL_UNSIGNED_BYTE, g_font_px);
    glPixelStorei_(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUseProgram_(g_prog); glBindVertexArray_(g_vao); glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);
    return 1;
}
static void ov_flush(void) {
    if (!g_nov) return;
    glBindBuffer_(GL_ARRAY_BUFFER, g_ovbo);
    glBufferData_(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(OV) * g_nov), g_ov, GL_STREAM_DRAW);
    glUniform1i_(g_ou_mode, g_omode);
    glActiveTexture_(GL_TEXTURE0); glBindTexture_(GL_TEXTURE_2D, g_omode == 1 ? g_ofont : g_last_tex);
    glDrawArrays_(GL_TRIANGLES, 0, g_nov);
    g_nov = 0;
}
static void ov_quad(int mode, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float c[4]) {
    if (mode != g_omode) { ov_flush(); g_omode = mode; }
    if (g_nov + 6 > (int)(sizeof g_ov / sizeof g_ov[0])) ov_flush();
    OV q[4] = { { x0, y0, u0, v0, c[0], c[1], c[2], c[3] }, { x1, y0, u1, v0, c[0], c[1], c[2], c[3] },
                { x0, y1, u0, v1, c[0], c[1], c[2], c[3] }, { x1, y1, u1, v1, c[0], c[1], c[2], c[3] } };
    g_ov[g_nov++] = q[0]; g_ov[g_nov++] = q[1]; g_ov[g_nov++] = q[2];
    g_ov[g_nov++] = q[1]; g_ov[g_nov++] = q[3]; g_ov[g_nov++] = q[2];
}
// Start an overlay frame: the last game picture (dimmed by `dim`) fills the window as usual.
int gpu_overlay_begin(int *w, int *h, float dim) {
    if (!overlay_init()) return 0;
    int ww, wh; SDL_GL_GetDrawableSize(g_sdl_window, &ww, &wh); g_ow = ww; g_oh = wh;
    view_rect(ww, wh);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0); glViewport_(0, 0, ww, wh);
    glDisable_(GL_SCISSOR_TEST); glDisable_(GL_DEPTH_TEST); glDisable_(GL_CULL_FACE);
    glColorMask_(1, 1, 1, 1); glClearColor_(0, 0, 0, 1); glClear_(GL_COLOR_BUFFER_BIT);
    glEnable_(GL_BLEND); glBlendEquation_(GL_FUNC_ADD); glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram_(g_oprog); glBindVertexArray_(g_ovao);
    GLfloat sc[2] = { (float)ww, (float)wh }; glUniform2fv_(g_ou_screen, 1, sc);
    g_omode = -1; g_nov = 0;
    if (g_last_tex) { float c[4] = { 1 - dim, 1 - dim, 1 - dim, 1 };
        ov_quad(2, (float)g_pic_x, (float)g_pic_y, (float)(g_pic_x + g_pic_w), (float)(g_pic_y + g_pic_h), 0, 1, 1, 0, c); }
    *w = ww; *h = wh;
    return 1;
}
void gpu_overlay_rect(float x, float y, float w, float h, const float rgba[4]) { ov_quad(0, x, y, x + w, y + h, 0, 0, 0, 0, rgba); }
// Text at (x, y) = top-left, `px` = line height in pixels. Returns the width drawn.
float gpu_overlay_text(float x, float y, float px, const char *s, const float rgba[4]) {
    float k = px / FONT_LINE, x0 = x;
    for (; *s; s++) {
        int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?';
        const short *g = g_font_glyph[c - 32];
        if (c != ' ') ov_quad(1, x + g[4] * k, y, x + (g[4] + g[2]) * k, y + g[3] * k,
                              (float)g[0] / FONT_W, (float)g[1] / FONT_H, (float)(g[0] + g[2]) / FONT_W, (float)(g[1] + g[3]) / FONT_H, rgba);
        x += g[5] * k;
    }
    return x - x0;
}
float gpu_overlay_text_width(float px, const char *s) {
    float k = px / FONT_LINE, w = 0;
    for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += g_font_glyph[c - 32][5] * k; }
    return w;
}
void gpu_overlay_end(void) {
    ov_flush();
    SDL_GL_SwapWindow(g_sdl_window);
    // back to the game's state
    glDisable_(GL_BLEND); glUseProgram_(g_prog); glBindVertexArray_(g_vao);
    glBindBuffer_(GL_ARRAY_BUFFER, g_vbo);   // gpu_draw uploads into the bound buffer
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo); glViewport_(0, 0, g_W, g_H); glEnable_(GL_SCISSOR_TEST);
}

// The whole displayed picture, side margins included (captures, frame dumps); *out is reused.
int gpu_read_rgb_wide(int x0, int y0, int w, int h, u8 **out) {
    int ex0, ex1; wide_x(x0, x0 + w, &ex0, &ex1);
    int W = (ex1 - ex0) * g_scale, H = h * g_scale;
    static u8 *tmp, *res; static size_t cap;
    size_t need = (size_t)W * H * 3; if (cap < need) { free(tmp); free(res); tmp = malloc(need); res = malloc(need); cap = need; }
    if (!tmp || !res) return 0;
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_fbo);
    glReadPixels_(ex0 * g_scale, (528 - y0 - h) * g_scale, W, H, GL_RGB, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < H; y++) memcpy(res + (size_t)y * W * 3, tmp + (size_t)(H - 1 - y) * W * 3, (size_t)W * 3);
    *out = res; return W | H << 16;
}
int gpu_read_rgb(int x0, int y0, int w, int h, u8 *out) {
    int W = w * g_scale, H = h * g_scale;
    static u8 *tmp; static size_t cap;
    size_t need = (size_t)W * H * 3; if (cap < need) { free(tmp); tmp = malloc(need); cap = need; }
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_fbo);
    glReadPixels_((x0 + g_M) * g_scale, (528 - y0 - h) * g_scale, W, H, GL_RGB, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < H; y++) memcpy(out + (size_t)y * W * 3, tmp + (size_t)(H - 1 - y) * W * 3, (size_t)W * 3);
    return W | H << 16;
}

// Copy an EFB rectangle (1x EFB pixels) into the GPU texture registered for `dst`.
// ow/oh: the destination texture size in texels (after optional halving).
int gpu_copy_to_texture(u32 dst, int x0, int y0, int w, int h, int ow, int oh, int fmt, int inten) {
    dst &= 0x1FFFFFFF;
    CopyTex *c = NULL;
    for (int k = 0; k < g_nct; k++) if (g_ct[k].dst == dst) { c = &g_ct[k]; break; }
    if (!c) { c = &g_ct[g_nct < 64 ? g_nct++ : (int)(dst >> 5) & 63]; memset(c, 0, sizeof *c); c->dst = dst; }
    int ex0, ex1; wide_x(x0, x0 + w, &ex0, &ex1);   // full-width copies take the sides too (and keep their detail)
    int tw = (int)((long long)ow * g_scale * (ex1 - ex0) / (w > 0 ? w : 1)), th = oh * g_scale;
    g_pf_copies++;
    if (!c->tex) { glGenTextures_(1, &c->tex); glGenFramebuffers_(1, &c->fbo); }
    if (c->tw != tw || c->th != th) {
        g_pf_copy_allocs++;
        glBindTexture_(GL_TEXTURE_2D, c->tex);
        glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer_(GL_FRAMEBUFFER, c->fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->tex, 0);
        c->tw = tw; c->th = th;
    }
    c->w = ow; c->h = oh;
    glBindFramebuffer_(GL_FRAMEBUFFER, c->fbo);
    glViewport_(0, 0, tw, th);
    glDisable_(GL_SCISSOR_TEST); glDisable_(GL_BLEND); glDisable_(GL_DEPTH_TEST);
    glColorMask_(1, 1, 1, 1); glDepthMask_(GL_FALSE);
    glUseProgram_(g_cprog); glBindVertexArray_(g_cvao);
    glActiveTexture_(GL_TEXTURE0); glBindTexture_(GL_TEXTURE_2D, g_color);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);   // half-scale copies average texels
    // source rect in EFB texture coordinates (the EFB is stored bottom-up): top row maps to v = 0 here
    float u0 = (float)ex0 / g_NW, uw = (float)(ex1 - ex0) / g_NW;
    float v0 = (float)(528 - y0) / 528.f, vh = -(float)h / 528.f;
    glUniform4f_(g_cu_rect, u0, v0, uw, vh);
    glUniform1i_(g_cu_fmt, fmt); glUniform1i_(g_cu_inten, inten);
    glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    // back to normal drawing
    glUseProgram_(g_prog); glBindVertexArray_(g_vao);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo); glViewport_(0, 0, g_W, g_H);
    glEnable_(GL_SCISSOR_TEST);
    return 1;
}
// GPU texture holding an EFB copy made to `addr` with size w x h, or 0.
u32 gpu_copy_lookup(u32 addr, int w, int h) {
    addr &= 0x1FFFFFFF;
    for (int k = 0; k < g_nct; k++) if (g_ct[k].dst == addr && g_ct[k].w == w && g_ct[k].h == h) return g_ct[k].tex;
    return 0;
}

int gpu_read_rgba(int x0, int y0, int w, int h, u32 *out) {
    int W = w * g_scale, H = h * g_scale;
    static u8 *tmp; static size_t cap;
    size_t need = (size_t)W * H * 4; if (cap < need) { free(tmp); tmp = malloc(need); cap = need; }
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_fbo);
    glReadPixels_((x0 + g_M) * g_scale, (528 - y0 - h) * g_scale, W, H, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    int s = g_scale, n = s * s;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {   // box-filter down to native resolution
        u32 acc[4] = { 0, 0, 0, 0 };
        for (int dy = 0; dy < s; dy++) for (int dx = 0; dx < s; dx++) {
            const u8 *p = tmp + ((size_t)(H - 1 - (y * s + dy)) * W + x * s + dx) * 4;
            for (int k = 0; k < 4; k++) acc[k] += p[k];
        }
        out[y * w + x] = (acc[0] / n) << 24 | (acc[1] / n) << 16 | (acc[2] / n) << 8 | (acc[3] / n);
    }
    return 1;
}

#else   // no SDL: no GPU backend
int gpu_copy_to_texture(u32 dst, int x0, int y0, int w, int h, int ow, int oh, int fmt, int inten) { (void)dst; (void)x0; (void)y0; (void)w; (void)h; (void)ow; (void)oh; (void)fmt; (void)inten; return 0; }
u32 gpu_copy_lookup(u32 addr, int w, int h) { (void)addr; (void)w; (void)h; return 0; }
int gpu_read_rgba(int x0, int y0, int w, int h, u32 *out) { (void)x0; (void)y0; (void)w; (void)h; (void)out; return 0; }
int gpu_init(int scale) { (void)scale; return 0; }
int g_disp_stretch, g_disp_sharp;
int gpu_get_scale(void) { return 1; }
void gpu_set_scale(int scale) { (void)scale; }
int gpu_overlay_begin(int *w, int *h, float dim) { (void)w; (void)h; (void)dim; return 0; }
void gpu_overlay_rect(float x, float y, float w, float h, const float rgba[4]) { (void)x; (void)y; (void)w; (void)h; (void)rgba; }
float gpu_overlay_text(float x, float y, float px, const char *s, const float rgba[4]) { (void)x; (void)y; (void)px; (void)s; (void)rgba; return 0; }
float gpu_overlay_text_width(float px, const char *s) { (void)px; (void)s; return 0; }
void gpu_overlay_end(void) {}
void gpu_draw(const Setup *s, const Out *v, int n) { (void)s; (void)v; (void)n; }
void gpu_clear(int x0, int y0, int w, int h, u32 c, float z, int color, int alpha, int depth) { (void)x0; (void)y0; (void)w; (void)h; (void)c; (void)z; (void)color; (void)alpha; (void)depth; }
void gpu_present(int x0, int y0, int w, int h) { (void)x0; (void)y0; (void)w; (void)h; }
int gpu_read_rgb(int x0, int y0, int w, int h, u8 *out) { (void)x0; (void)y0; (void)w; (void)h; (void)out; return 0; }
int gpu_read_rgb_wide(int x0, int y0, int w, int h, u8 **out) { (void)x0; (void)y0; (void)w; (void)h; (void)out; return 0; }
void gpu_texpack_stats(char *b, int n) { if (n) b[0] = 0; }
void gpu_texpack_flush(void) {}
#endif
