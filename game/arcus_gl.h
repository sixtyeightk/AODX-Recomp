/*
 * arcus_gl.h — raw-GL frame presenter (desktop GL 2.1 / GLSL 1.20 on the Mac,
 * GLES 2.0 / GLSL ES 1.00 on the Switch) with the CRT shader from
 * shaders/crt.glsl.
 *
 * It draws into the GL context that SDL's "opengl"/"opengles2" renderer created
 * for the window and restores the GL state that renderer caches, so the stock
 * SDL_Renderer paths (save-state browser, launcher UI) keep working and
 * SDL_RenderPresent() still does the buffer swap.
 */
#pragma once
#include <stdint.h>

struct SDL_Window;

typedef struct ArcusCrtParams {
    int   enabled;      /* 0 = plain integer-scaled nearest image */
    float beam_k;       /* vertical beam hardness */
    float pix_k;        /* horizontal softening hardness */
    float mask_dark;    /* aperture grille dark level, 1.0 = off */
} ArcusCrtParams;

/* Compile the shader programs in the current context. Returns 1 on success;
 * 0 means use the SDL_Renderer fallback. */
int  arcus_gl_init(struct SDL_Window *window, int gles);
int  arcus_gl_ready(void);
const char *arcus_gl_shader_prelude(int gles);   /* for the test that validates it */

/* Upload `argb` (stride in pixels) and draw it integer-scaled into the
 * window's back buffer (black borders). If dump_path is set, the same image is
 * also rendered into an offscreen framebuffer of the drawable size and written
 * as a PNG. Returns 1 if it drew. */
int  arcus_gl_draw(const uint32_t *argb, int width, int height, int stride,
                   const ArcusCrtParams *crt, const char *dump_path);

/* Black frame (used after a drawable-size change). */
void arcus_gl_clear(void);
