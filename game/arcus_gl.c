/*
 * arcus_gl.c — raw-GL presenter: one textured quad through shaders/crt.glsl.
 *
 * GL functions are loaded with SDL_GL_GetProcAddress (no loader library). On
 * the Switch, define ARCUS_GL_DIRECT and link GLESv2 to take the addresses of
 * the GLES2 entry points directly instead (Mesa's eglGetProcAddress also
 * returns core functions, so either works).
 */
#include "arcus_gl.h"
#include "present_math.h"
#include "png_write.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ARCUS_GL_DIRECT
#include <GLES2/gl2.h>
#endif

/* ---- minimal GL 2.1 / GLES 2.0 subset ------------------------------------ */
#ifndef ARCUS_GL_DIRECT
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
#endif
#if defined(_WIN32)
#define AGL_API __stdcall
#else
#define AGL_API
#endif

#define A_TEXTURE_2D 0x0DE1
#define A_TEXTURE_MIN_FILTER 0x2801
#define A_TEXTURE_MAG_FILTER 0x2800
#define A_NEAREST 0x2600
#define A_TEXTURE_WRAP_S 0x2802
#define A_TEXTURE_WRAP_T 0x2803
#define A_CLAMP_TO_EDGE 0x812F
#define A_RGBA 0x1908
#define A_UNSIGNED_BYTE 0x1401
#define A_TEXTURE0 0x84C0
#define A_FRAGMENT_SHADER 0x8B30
#define A_VERTEX_SHADER 0x8B31
#define A_COMPILE_STATUS 0x8B81
#define A_LINK_STATUS 0x8B82
#define A_COLOR_BUFFER_BIT 0x4000
#define A_TRIANGLE_STRIP 0x0005
#define A_FLOAT 0x1406
#define A_CURRENT_PROGRAM 0x8B8D
#define A_ACTIVE_TEXTURE 0x84E0
#define A_TEXTURE_BINDING_2D 0x8069
#define A_ARRAY_BUFFER 0x8892
#define A_ARRAY_BUFFER_BINDING 0x8894
#define A_VIEWPORT 0x0BA2
#define A_SCISSOR_TEST 0x0C11
#define A_BLEND 0x0BE2
#define A_CULL_FACE 0x0B44
#define A_DEPTH_TEST 0x0B71
#define A_FRAMEBUFFER 0x8D40
#define A_FRAMEBUFFER_BINDING 0x8CA6
#define A_COLOR_ATTACHMENT0 0x8CE0
#define A_FRAMEBUFFER_COMPLETE 0x8CD5
#define A_COLOR_CLEAR_VALUE 0x0C22
#define A_UNPACK_ALIGNMENT 0x0CF5
#define A_UNPACK_ROW_LENGTH 0x0CF2
#define A_EXTENSIONS 0x1F03
#define A_PACK_ALIGNMENT 0x0D05
#define A_VERTEX_ATTRIB_ARRAY_ENABLED 0x8622
#define A_VERSION 0x1F02
#define A_RENDERER 0x1F01
#define A_SHADING_LANGUAGE_VERSION 0x8B8C

#define GL_FUNCS(X) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glClear, (GLbitfield)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(GLboolean, glIsEnabled, (GLenum)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glGetFloatv, (GLenum, GLfloat *)) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glDeleteShader, (GLuint)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glDisableVertexAttribArray, (GLuint)) \
    X(void, glGetVertexAttribiv, (GLuint, GLenum, GLint *)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, glFinish, (void))

/* Framebuffer objects: core in GLES2; ARB/EXT on desktop GL 2.1. Only the
 * presented-image dump (tests) needs them. */
#define GL_FBO_FUNCS(X) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum))

#define DECL(ret, name, args) typedef ret (AGL_API *PFN_##name) args; static PFN_##name p_##name;
GL_FUNCS(DECL)
GL_FBO_FUNCS(DECL)
#undef DECL

extern const char arcus_crt_glsl[];   /* shaders/crt.glsl, NUL-terminated (shader_embed.S) */

static struct {
    int ready, gles, have_fbo;
    int row_length;             /* UNPACK_ROW_LENGTH exists (desktop GL, GLES3, EXT_unpack_subimage) */
    struct SDL_Window *window;
    GLuint prog[4], tex;
    GLint u_out_size[4], u_tex[4], u_tex_size[4], u_src_size[4], u_scale[4], u_crt[4];
    int tex_w, tex_h;
    GLuint hfbo, htex;          /* two-pass CRT: pass 1 target (output width x source lines) */
    int htex_w, htex_h, two_pass;
    int last_dw, last_dh;
} G;

static const char PRELUDE_DESKTOP[] = "#version 120\n";
static const char PRELUDE_GLES[] =
    "#version 100\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n";

const char *arcus_gl_shader_prelude(int gles) { return gles ? PRELUDE_GLES : PRELUDE_DESKTOP; }

static int load_funcs(void)
{
    int ok = 1;
#ifdef ARCUS_GL_DIRECT
#define LOAD(ret, name, args) p_##name = (PFN_##name)(void *)&name;
    GL_FUNCS(LOAD)
    GL_FBO_FUNCS(LOAD)
#undef LOAD
#else
#define LOAD(ret, name, args) \
    p_##name = (PFN_##name)SDL_GL_GetProcAddress(#name); \
    if (!p_##name) { fprintf(stderr, "[arcus-gl] missing %s\n", #name); ok = 0; }
    GL_FUNCS(LOAD)
#undef LOAD
#define LOADFBO(ret, name, args) \
    p_##name = (PFN_##name)SDL_GL_GetProcAddress(#name); \
    if (!p_##name) p_##name = (PFN_##name)SDL_GL_GetProcAddress(#name "EXT");
    GL_FBO_FUNCS(LOADFBO)
#undef LOADFBO
#endif
    G.have_fbo = p_glGenFramebuffers && p_glDeleteFramebuffers && p_glBindFramebuffer &&
                 p_glFramebufferTexture2D && p_glCheckFramebufferStatus;
    return ok;
}

/* Programs: 0 plain, 1 CRT single pass, 2 CRT pass 1 (horizontal), 3 CRT pass 2 (vertical). */
static GLuint compile(GLenum stage, int gles, int crt)
{
    char defs[96];
    snprintf(defs, sizeof defs, "#define %s\n#define CRT %d\n#define PASS %d\n",
             stage == A_VERTEX_SHADER ? "VERTEX" : "FRAGMENT", crt ? 1 : 0, crt >= 2 ? crt - 1 : 0);
    const GLchar *src[3] = { arcus_gl_shader_prelude(gles), defs, arcus_crt_glsl };
    GLuint sh = p_glCreateShader(stage);
    p_glShaderSource(sh, 3, src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, A_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "[arcus-gl] %s shader (CRT %d) failed:\n%s\n",
                stage == A_VERTEX_SHADER ? "vertex" : "fragment", crt, log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_program(int gles, int crt)
{
    GLuint vs = compile(A_VERTEX_SHADER, gles, crt);
    GLuint fs = vs ? compile(A_FRAGMENT_SHADER, gles, crt) : 0;
    if (!fs) { if (vs) p_glDeleteShader(vs); return 0; }
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glBindAttribLocation(p, 0, "a_pos");
    p_glLinkProgram(p);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(p, A_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        p_glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "[arcus-gl] link (CRT %d) failed:\n%s\n", crt, log);
        return 0;
    }
    return p;
}

/* ---- GL state the SDL renderer caches; restored after every draw --------- */
typedef struct SavedState {
    GLint program, active_tex, tex2d, array_buf, fbo, viewport[4], attr0, unpack_row, unpack_align;
    GLfloat clear[4];
    GLboolean scissor, blend, cull, depth;
} SavedState;

static void save_state(SavedState *s)
{
    p_glGetIntegerv(A_CURRENT_PROGRAM, &s->program);
    p_glGetIntegerv(A_ACTIVE_TEXTURE, &s->active_tex);
    p_glActiveTexture(A_TEXTURE0);
    p_glGetIntegerv(A_TEXTURE_BINDING_2D, &s->tex2d);
    p_glGetIntegerv(A_ARRAY_BUFFER_BINDING, &s->array_buf);
    s->fbo = 0;
    if (G.have_fbo) p_glGetIntegerv(A_FRAMEBUFFER_BINDING, &s->fbo);
    p_glGetIntegerv(A_VIEWPORT, s->viewport);
    p_glGetVertexAttribiv(0, A_VERTEX_ATTRIB_ARRAY_ENABLED, &s->attr0);
    p_glGetFloatv(A_COLOR_CLEAR_VALUE, s->clear);
    s->unpack_row = 0;
    if (G.row_length) p_glGetIntegerv(A_UNPACK_ROW_LENGTH, &s->unpack_row);
    p_glGetIntegerv(A_UNPACK_ALIGNMENT, &s->unpack_align);
    s->scissor = p_glIsEnabled(A_SCISSOR_TEST);
    s->blend = p_glIsEnabled(A_BLEND);
    s->cull = p_glIsEnabled(A_CULL_FACE);
    s->depth = p_glIsEnabled(A_DEPTH_TEST);
}

static void restore_state(const SavedState *s)
{
    p_glUseProgram((GLuint)s->program);
    p_glActiveTexture(A_TEXTURE0);
    p_glBindTexture(A_TEXTURE_2D, (GLuint)s->tex2d);
    p_glActiveTexture((GLenum)s->active_tex);
    p_glBindBuffer(A_ARRAY_BUFFER, (GLuint)s->array_buf);
    if (G.have_fbo) p_glBindFramebuffer(A_FRAMEBUFFER, (GLuint)s->fbo);
    p_glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    if (s->attr0) p_glEnableVertexAttribArray(0); else p_glDisableVertexAttribArray(0);
    p_glClearColor(s->clear[0], s->clear[1], s->clear[2], s->clear[3]);
    if (G.row_length) p_glPixelStorei(A_UNPACK_ROW_LENGTH, s->unpack_row);
    p_glPixelStorei(A_UNPACK_ALIGNMENT, s->unpack_align);
    if (s->scissor) p_glEnable(A_SCISSOR_TEST);
    if (s->blend) p_glEnable(A_BLEND);
    if (s->cull) p_glEnable(A_CULL_FACE);
    if (s->depth) p_glEnable(A_DEPTH_TEST);
}

static void neutral_state(void)
{
    p_glDisable(A_SCISSOR_TEST);
    p_glDisable(A_BLEND);
    p_glDisable(A_CULL_FACE);
    p_glDisable(A_DEPTH_TEST);
}

int arcus_gl_init(struct SDL_Window *window, int gles)
{
    memset(&G, 0, sizeof G);
    G.window = window;
    G.gles = gles;
    if (!SDL_GL_GetCurrentContext()) {
        fprintf(stderr, "[arcus-gl] no current GL context\n");
        return 0;
    }
    if (!load_funcs()) return 0;
    const char *ver = (const char *)p_glGetString(A_VERSION);
    const char *sl = (const char *)p_glGetString(A_SHADING_LANGUAGE_VERSION);
    const char *rn = (const char *)p_glGetString(A_RENDERER);
    fprintf(stderr, "[arcus-gl] %s | GLSL %s | %s | %s\n", ver ? ver : "?", sl ? sl : "?",
            rn ? rn : "?", gles ? "GLES2 dialect" : "GL 2.1 dialect");
    /* SDL's GL renderer leaves UNPACK_ROW_LENGTH at its last texture's width (the manual's
     * pages): reset it for our uploads where the context has it. */
    const char *ext = (const char *)p_glGetString(A_EXTENSIONS);
    G.row_length = !gles || (ver && strstr(ver, "OpenGL ES 3")) || (ext && strstr(ext, "GL_EXT_unpack_subimage"));
    SavedState st;
    save_state(&st);
    G.prog[0] = build_program(gles, 0);
    G.prog[1] = G.prog[0] ? build_program(gles, 1) : 0;
    if (!G.prog[1]) { restore_state(&st); return 0; }
    if (G.have_fbo) {
        G.prog[2] = build_program(gles, 2);
        G.prog[3] = G.prog[2] ? build_program(gles, 3) : 0;
    }
    G.two_pass = G.prog[2] && G.prog[3] && !getenv("ARCUS_CRT_SINGLE");   /* env: A/B test */
    fprintf(stderr, "[arcus-gl] CRT shader: %s\n", G.two_pass ? "two passes" : "single pass");
    for (int i = 0; i < 4; i++) {
        if (!G.prog[i]) continue;
        G.u_out_size[i] = p_glGetUniformLocation(G.prog[i], "u_out_size");
        G.u_tex[i] = p_glGetUniformLocation(G.prog[i], "u_tex");
        G.u_tex_size[i] = p_glGetUniformLocation(G.prog[i], "u_tex_size");
        G.u_src_size[i] = p_glGetUniformLocation(G.prog[i], "u_src_size");
        G.u_scale[i] = p_glGetUniformLocation(G.prog[i], "u_scale");
        G.u_crt[i] = p_glGetUniformLocation(G.prog[i], "u_crt");
    }
    p_glGenTextures(1, &G.tex);
    restore_state(&st);
    G.ready = 1;
    return 1;
}

int arcus_gl_ready(void) { return G.ready; }

static void ensure_texture(int w, int h)
{
    if (w <= G.tex_w && h <= G.tex_h) return;
    int tw = 512, th = 256;
    while (tw < w) tw *= 2;
    while (th < h) th *= 2;
    p_glBindTexture(A_TEXTURE_2D, G.tex);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_MIN_FILTER, A_NEAREST);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_MAG_FILTER, A_NEAREST);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_WRAP_S, A_CLAMP_TO_EDGE);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_WRAP_T, A_CLAMP_TO_EDGE);
    p_glTexImage2D(A_TEXTURE_2D, 0, A_RGBA, tw, th, 0, A_RGBA, A_UNSIGNED_BYTE, NULL);
    G.tex_w = tw;
    G.tex_h = th;
}

static void set_uniforms(int i, float out_w, float out_h, float tex_w, float tex_h,
                         int width, int height, int scale, const ArcusCrtParams *crt)
{
    p_glUseProgram(G.prog[i]);
    if (G.u_tex[i] >= 0) p_glUniform1i(G.u_tex[i], 0);
    if (G.u_out_size[i] >= 0) p_glUniform2f(G.u_out_size[i], out_w, out_h);
    if (G.u_tex_size[i] >= 0) p_glUniform2f(G.u_tex_size[i], tex_w, tex_h);
    if (G.u_src_size[i] >= 0) p_glUniform2f(G.u_src_size[i], (float)width, (float)height);
    if (G.u_scale[i] >= 0) p_glUniform1f(G.u_scale[i], (float)scale);
    if (i && crt && G.u_crt[i] >= 0) {
        double mean = present_beam_mean(scale, crt->beam_k, exp);
        p_glUniform4f(G.u_crt[i], crt->beam_k, crt->pix_k, crt->mask_dark, (float)(1.0 / mean));
    }
}

static void draw_quad(void)
{
    static const GLfloat quad[8] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
    p_glBindBuffer(A_ARRAY_BUFFER, 0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, A_FLOAT, 0, 0, quad);
    p_glDrawArrays(A_TRIANGLE_STRIP, 0, 4);
    p_glDisableVertexAttribArray(0);
}

/* Pass 1 target: (width * scale) x height RGBA, nearest, no mipmaps (NPOT is fine in GLES2). */
static int ensure_hpass(int w, int h)
{
    if (G.hfbo && G.htex_w == w && G.htex_h == h) return 1;
    if (!G.htex) p_glGenTextures(1, &G.htex);
    if (!G.hfbo) p_glGenFramebuffers(1, &G.hfbo);
    p_glBindTexture(A_TEXTURE_2D, G.htex);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_MIN_FILTER, A_NEAREST);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_MAG_FILTER, A_NEAREST);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_WRAP_S, A_CLAMP_TO_EDGE);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_WRAP_T, A_CLAMP_TO_EDGE);
    p_glTexImage2D(A_TEXTURE_2D, 0, A_RGBA, w, h, 0, A_RGBA, A_UNSIGNED_BYTE, NULL);
    p_glBindFramebuffer(A_FRAMEBUFFER, G.hfbo);
    p_glFramebufferTexture2D(A_FRAMEBUFFER, A_COLOR_ATTACHMENT0, A_TEXTURE_2D, G.htex, 0);
    int ok = p_glCheckFramebufferStatus(A_FRAMEBUFFER) == A_FRAMEBUFFER_COMPLETE;
    G.htex_w = w;
    G.htex_h = h;
    if (!ok) {
        fprintf(stderr, "[arcus-gl] pass-1 framebuffer incomplete: single-pass CRT\n");
        G.two_pass = 0;
    }
    return ok;
}

/* Clear the target to black and draw the image over the integer-fit rect. `target` is the
 * framebuffer the final pass draws into (the window's, or the test dump's). */
static void render(int dw, int dh, int width, int height, const ArcusCrtParams *crt, GLuint target)
{
    PresentRect r = present_integer_fit(dw, dh, width, height);
    int crt_on = crt && crt->enabled;
    p_glActiveTexture(A_TEXTURE0);

    if (crt_on && G.two_pass && ensure_hpass(width * r.scale, height)) {
        /* Pass 1: source frame -> horizontally softened lines at output width. */
        p_glBindFramebuffer(A_FRAMEBUFFER, G.hfbo);
        p_glViewport(0, 0, width * r.scale, height);
        p_glBindTexture(A_TEXTURE_2D, G.tex);
        set_uniforms(2, (float)(width * r.scale), (float)height, (float)G.tex_w, (float)G.tex_h,
                     width, height, r.scale, crt);
        draw_quad();
        /* Pass 2: vertical beam + grille into the target. */
        p_glBindFramebuffer(A_FRAMEBUFFER, target);
        p_glViewport(0, 0, dw, dh);
        p_glClearColor(0.f, 0.f, 0.f, 1.f);
        p_glClear(A_COLOR_BUFFER_BIT);
        p_glViewport(r.x, dh - (r.y + r.h), r.w, r.h);   /* GL origin is bottom-left */
        p_glBindTexture(A_TEXTURE_2D, G.htex);
        set_uniforms(3, (float)r.w, (float)r.h, (float)G.htex_w, (float)G.htex_h,
                     width, height, r.scale, crt);
        draw_quad();
        return;
    }

    if (G.have_fbo) p_glBindFramebuffer(A_FRAMEBUFFER, target);
    p_glViewport(0, 0, dw, dh);
    p_glClearColor(0.f, 0.f, 0.f, 1.f);
    p_glClear(A_COLOR_BUFFER_BIT);
    p_glViewport(r.x, dh - (r.y + r.h), r.w, r.h);   /* GL origin is bottom-left */
    p_glBindTexture(A_TEXTURE_2D, G.tex);
    set_uniforms(crt_on ? 1 : 0, (float)r.w, (float)r.h, (float)G.tex_w, (float)G.tex_h,
                 width, height, r.scale, crt);
    draw_quad();
}

static void dump_png(int dw, int dh, int width, int height, const ArcusCrtParams *crt,
                     const char *path)
{
    if (!G.have_fbo) { fprintf(stderr, "[arcus-gl] no framebuffer objects: dump skipped\n"); return; }
    GLuint tex = 0, fbo = 0;
    p_glGenTextures(1, &tex);
    p_glBindTexture(A_TEXTURE_2D, tex);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_MIN_FILTER, A_NEAREST);
    p_glTexParameteri(A_TEXTURE_2D, A_TEXTURE_MAG_FILTER, A_NEAREST);
    p_glTexImage2D(A_TEXTURE_2D, 0, A_RGBA, dw, dh, 0, A_RGBA, A_UNSIGNED_BYTE, NULL);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(A_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(A_FRAMEBUFFER, A_COLOR_ATTACHMENT0, A_TEXTURE_2D, tex, 0);
    if (p_glCheckFramebufferStatus(A_FRAMEBUFFER) == A_FRAMEBUFFER_COMPLETE) {
        render(dw, dh, width, height, crt, fbo);
        uint8_t *rgba = (uint8_t *)malloc((size_t)dw * dh * 4);
        uint32_t *argb = (uint32_t *)malloc((size_t)dw * dh * 4);
        if (rgba && argb) {
            p_glPixelStorei(A_PACK_ALIGNMENT, 1);
            p_glReadPixels(0, 0, dw, dh, A_RGBA, A_UNSIGNED_BYTE, rgba);
            for (int y = 0; y < dh; y++) {                 /* bottom-up -> top-down */
                const uint8_t *s = rgba + (size_t)(dh - 1 - y) * dw * 4;
                uint32_t *d = argb + (size_t)y * dw;
                for (int x = 0; x < dw; x++, s += 4)
                    d[x] = 0xFF000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
            }
            if (png_write_argb(path, argb, dw, dh, dw) == 0)
                fprintf(stderr, "[arcus-gl] presented frame %dx%d -> %s\n", dw, dh, path);
        }
        free(rgba);
        free(argb);
    } else {
        fprintf(stderr, "[arcus-gl] dump framebuffer incomplete\n");
    }
    p_glBindFramebuffer(A_FRAMEBUFFER, 0);
    p_glDeleteFramebuffers(1, &fbo);
    p_glDeleteTextures(1, &tex);
}

void arcus_gl_clear(void)
{
    if (!G.ready) return;
    SavedState st;
    save_state(&st);
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(G.window, &dw, &dh);
    p_glDisable(A_SCISSOR_TEST);
    p_glViewport(0, 0, dw, dh);
    p_glClearColor(0.f, 0.f, 0.f, 1.f);
    p_glClear(A_COLOR_BUFFER_BIT);
    restore_state(&st);
}

int arcus_gl_draw(const uint32_t *argb, int width, int height, int stride,
                  const ArcusCrtParams *crt, const char *dump_path)
{
    if (!G.ready || width <= 0 || height <= 0) return 0;
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(G.window, &dw, &dh);
    if (dw <= 0 || dh <= 0) return 0;

    SavedState st;
    save_state(&st);
    neutral_state();

    if (dw != G.last_dw || dh != G.last_dh) {
        /* Drawable changed (window resize, Switch dock/undock): clear every
         * buffer of the swap chain once so no stale border survives. Two
         * cleared swaps here plus the cleared frame the runner presents. */
        if (G.last_dw) {
            for (int i = 0; i < 2; i++) {
                p_glViewport(0, 0, dw, dh);
                p_glClearColor(0.f, 0.f, 0.f, 1.f);
                p_glClear(A_COLOR_BUFFER_BIT);
                SDL_GL_SwapWindow(G.window);
            }
        }
        PresentRect r = present_integer_fit(dw, dh, width, height);
        fprintf(stderr, "[arcus-gl] drawable %dx%d: %dx%d at %dx -> %dx%d+%d+%d\n",
                dw, dh, width, height, r.scale, r.w, r.h, r.x, r.y);
        G.last_dw = dw;
        G.last_dh = dh;
    }

    ensure_texture(stride, height);
    p_glActiveTexture(A_TEXTURE0);
    p_glBindTexture(A_TEXTURE_2D, G.tex);
    p_glPixelStorei(A_UNPACK_ALIGNMENT, 4);
    if (G.row_length) p_glPixelStorei(A_UNPACK_ROW_LENGTH, 0);    /* whole rows of the stride */
    /* GLES2 has no UNPACK_ROW_LENGTH: upload whole rows of the stride; the
     * shader never samples past the active width. */
    p_glTexSubImage2D(A_TEXTURE_2D, 0, 0, 0, stride, height, A_RGBA, A_UNSIGNED_BYTE, argb);

    if (dump_path) dump_png(dw, dh, width, height, crt, dump_path);
    render(dw, dh, width, height, crt, (GLuint)st.fbo);
    restore_state(&st);
    return 1;
}
