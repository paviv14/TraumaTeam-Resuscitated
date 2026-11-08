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
    X(void, Uniform1iv, (GLint, GLsizei, const GLint *)) X(void, Uniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) X(void, Uniform1fv, (GLint, GLsizei, const GLfloat *))

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
    "void main() {\n"
    "  const float bias_t[4] = float[4](0.0, 0.5, -0.5, 0.0); const float scale_t[4] = float[4](1.0, 2.0, 4.0, 0.5);\n"
    "  for (int i = 0; i < 4; i++) R[i] = uReg[i];\n"
    "  vec4 outc = vec4(0.0);\n"
    "  for (int s = 0; s < uNst; s++) {\n"
    "    ivec4 st = uSt[s];\n"
    "    vec4 T = vec4(1.0);\n"
    "    if (st.z >= 0) { vec3 t = tc(st.w); T = sw(samp(st.z, t.xy / (t.z == 0.0 ? 1.0 : t.z)), uTsw[s]); }\n"
    "    vec4 C = vec4(0.0);\n"
    "    if (uChan[s] == 0) C = sw(vC0, uRsw[s]); else if (uChan[s] == 1) C = sw(vC1, uRsw[s]);\n"
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
static struct { GLint nst, csel, asel, cop, aop, st, rsw, tsw, chan, k, ka, reg, at, aop2; } U;

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
    g_scale = scale < 1 ? 1 : scale; g_W = 640 * g_scale; g_H = 528 * g_scale;
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return 0;
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
    for (int i = 0; i < 8; i++) { char n[8]; snprintf(n, sizeof n, "uTex%d", i); glUniform1i_(glGetUniformLocation_(g_prog, n), i); }

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
    g_gpu = 1;
    fprintf(stderr, "[gl] backend ready: %s | %s | EFB %dx%d\n", (const char *)glGetString_(GL_RENDERER), (const char *)glGetString_(GL_VERSION), g_W, g_H);
    return 1;
}

// ---------------------------------------------------------------- textures
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
    if (!e->gl_tex) glGenTextures_(1, &e->gl_tex);
    glBindTexture_(GL_TEXTURE_2D, e->gl_tex);
    if (e->gl_version != e->version) {
        if (e->hi) {   // texture pack replacement (RGBA bytes), with mipmaps since it is usually larger
            glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, e->hw, e->hh, 0, GL_RGBA, GL_UNSIGNED_BYTE, e->hi);
            glGenerateMipmap_(GL_TEXTURE_2D);
        } else
            // px holds R<<24|G<<16|B<<8|A in host order: GL_UNSIGNED_INT_8_8_8_8 reads exactly that.
            glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, e->w, e->h, 0, GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, e->px);
        e->gl_version = e->version;
    }
    static const GLint wrap[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE };
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap[t->ws & 3]);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[t->wt & 3]);
    GLint f = t->linear || e->hi ? GL_LINEAR : GL_NEAREST;
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, e->hi ? GL_LINEAR_MIPMAP_LINEAR : f);
    glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

// ---------------------------------------------------------------- state
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
    glUniform1i_(U.nst, n);
    glUniform4iv_(U.csel, n, csel); glUniform4iv_(U.asel, n, asel); glUniform4iv_(U.cop, n, cop); glUniform4iv_(U.aop, n, aop);
    glUniform4iv_(U.st, n, st); glUniform4iv_(U.rsw, n, rsw); glUniform4iv_(U.tsw, n, tsw); glUniform1iv_(U.chan, n, chan);
    glUniform4fv_(U.k, n, k); glUniform1fv_(U.ka, n, ka); glUniform4fv_(U.reg, 4, reg);
    GLint at[4] = { s->ac_c0, s->ac_r0, s->ac_c1, s->ac_r1 }; glUniform4iv_(U.at, 1, at); glUniform1i_(U.aop2, s->ac_op);
    for (int m = 0; m < 8; m++) if (s->tex[m].e || s->tex[m].gl_copy) bind_texture(m, &s->tex[m]);

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
    glScissor_(x0 * g_scale, (528 - y1) * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
}

void gpu_draw(const Setup *s, const Out *v, int n) {
    static GV *buf; static int cap;
    if (n > cap) { free(buf); cap = n + 1024; buf = malloc(sizeof *buf * (size_t)cap); }
    float wd = s->vp[0], ht = s->vp[1], zr = s->vp[2], xo = s->vp[3] - 342.f, yo = s->vp[4] - 342.f, fz = s->vp[5];
    for (int i = 0; i < n; i++) {
        const Out *o = &v[i]; GV *g = &buf[i];
        float w = o->w;
        // screen x = x/w*wd + xo (0..640), y likewise (0..528, down), z = (z/w*zr + fz)/2^24 (0..1)
        float sxw = o->x * wd + xo * w, syw = o->y * ht + yo * w, szw = (o->z * zr + fz * w) / 16777215.f;
        g->pos[0] = sxw / 320.f - w;
        g->pos[1] = w - syw / 264.f;
        g->pos[2] = 2.f * szw - w;
        g->pos[3] = w;
        memcpy(g->c0, o->col[0], 16); memcpy(g->c1, o->col[1], 16);
        memcpy(g->t, o->uv, sizeof g->t);
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glViewport_(0, 0, g_W, g_H);
    set_state(s);
    glBufferData_(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof(GV), buf, GL_STREAM_DRAW);
    glDrawArrays_(GL_TRIANGLES, 0, n);
}

void gpu_clear(int x0, int y0, int w, int h, u32 c, float z, int color, int alpha, int depth) {
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    glScissor_(x0 * g_scale, (528 - y0 - h) * g_scale, w * g_scale, h * g_scale);
    glColorMask_(color, color, color, alpha); glDepthMask_(depth ? GL_TRUE : GL_FALSE);
    glClearColor_((c >> 24) / 255.f, ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f);
    glClearDepth_(z);
    GLbitfield bits = (color || alpha ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0);
    if (bits) glClear_(bits);
}

extern SDL_Window *g_sdl_window;
int g_view_x, g_view_y, g_view_w, g_view_h;   // where the picture sits in the window (for the mouse)
void gpu_present(int x0, int y0, int w, int h) {
    int ww, wh; SDL_GL_GetDrawableSize(g_sdl_window, &ww, &wh);
    // fit a 4:3 picture into the window
    int vw = ww, vh = ww * 3 / 4;
    if (vh > wh) { vh = wh; vw = wh * 4 / 3; }
    g_view_x = (ww - vw) / 2; g_view_y = (wh - vh) / 2; g_view_w = vw; g_view_h = vh;
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
    glDisable_(GL_SCISSOR_TEST);
    glClearColor_(0, 0, 0, 1); glColorMask_(1, 1, 1, 1); glClear_(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer_(x0 * g_scale, (528 - y0 - h) * g_scale, (x0 + w) * g_scale, (528 - y0) * g_scale,
                       g_view_x, wh - g_view_y - vh, g_view_x + vw, wh - g_view_y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glEnable_(GL_SCISSOR_TEST);
    SDL_GL_SwapWindow(g_sdl_window);
    glBindFramebuffer_(GL_FRAMEBUFFER, g_fbo);
    GLenum err = glGetError_(); static int nerr;
    if (err && nerr++ < 10) fprintf(stderr, "[gl] error 0x%04X\n", err);
}

int gpu_read_rgb(int x0, int y0, int w, int h, u8 *out) {
    int W = w * g_scale, H = h * g_scale;
    static u8 *tmp; static size_t cap;
    size_t need = (size_t)W * H * 3; if (cap < need) { free(tmp); tmp = malloc(need); cap = need; }
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, g_fbo);
    glReadPixels_(x0 * g_scale, (528 - y0 - h) * g_scale, W, H, GL_RGB, GL_UNSIGNED_BYTE, tmp);
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
    int tw = ow * g_scale, th = oh * g_scale;
    if (!c->tex) { glGenTextures_(1, &c->tex); glGenFramebuffers_(1, &c->fbo); }
    if (c->tw != tw || c->th != th) {
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
    float u0 = (float)x0 / 640.f, uw = (float)w / 640.f;
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
    glReadPixels_(x0 * g_scale, (528 - y0 - h) * g_scale, W, H, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
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
// Copy an EFB rectangle (1x EFB pixels) into the GPU texture registered for `dst`.
// ow/oh: the destination texture size in texels (after optional halving).
int gpu_copy_to_texture(u32 dst, int x0, int y0, int w, int h, int ow, int oh, int fmt, int inten) {
    dst &= 0x1FFFFFFF;
    CopyTex *c = NULL;
    for (int k = 0; k < g_nct; k++) if (g_ct[k].dst == dst) { c = &g_ct[k]; break; }
    if (!c) { c = &g_ct[g_nct < 64 ? g_nct++ : (int)(dst >> 5) & 63]; memset(c, 0, sizeof *c); c->dst = dst; }
    int tw = ow * g_scale, th = oh * g_scale;
    if (!c->tex) { glGenTextures_(1, &c->tex); glGenFramebuffers_(1, &c->fbo); }
    if (c->tw != tw || c->th != th) {
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
    float u0 = (float)x0 / 640.f, uw = (float)w / 640.f;
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

int gpu_read_rgba(int x0, int y0, int w, int h, u32 *out) { (void)x0; (void)y0; (void)w; (void)h; (void)out; return 0; }
int gpu_init(int scale) { (void)scale; return 0; }
void gpu_draw(const Setup *s, const Out *v, int n) { (void)s; (void)v; (void)n; }
void gpu_clear(int x0, int y0, int w, int h, u32 c, float z, int color, int alpha, int depth) { (void)x0; (void)y0; (void)w; (void)h; (void)c; (void)z; (void)color; (void)alpha; (void)depth; }
void gpu_present(int x0, int y0, int w, int h) { (void)x0; (void)y0; (void)w; (void)h; }
int gpu_read_rgb(int x0, int y0, int w, int h, u8 *out) { (void)x0; (void)y0; (void)w; (void)h; (void)out; return 0; }
#endif
