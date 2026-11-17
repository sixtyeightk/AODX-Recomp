/*
 * crt.glsl — presenter shader for the native Arcus Odyssey build.
 *
 * Written for this project. The approach (a Gaussian beam per source line and a
 * short Gaussian horizontally, in linear light, plus an optional aperture-grille
 * mask) follows Timothy Lottes' crt-lottes, which is public domain; no code is
 * copied from it or from any GPL shader. See shaders/THIRD-PARTY.md.
 *
 * One source, two dialects. The presenter (game/arcus_gl.c) prepends a prelude:
 *   desktop GL 2.1:  #version 120
 *   GLES 2.0:        #version 100 + a float precision statement
 * followed by "#define VERTEX" or "#define FRAGMENT", "#define CRT 0|1" and "#define PASS 0|1|2".
 * CRT 0 is the plain integer-scaled image (preset Off). With CRT 1:
 *   PASS 0  single pass (fallback without framebuffer objects): 3 lines x 3 taps per pixel
 *   PASS 1  horizontal softening per source line, rendered at (output width x source lines)
 *           into an offscreen texture, stored gamma-encoded (sqrt) to keep 8-bit precision
 *   PASS 2  vertical beam + grille at output resolution: 3 taps of the PASS 1 texture
 * Two passes do the same maths with ~2.4x fewer texture reads (docked 4x: 5.7M vs 13.7M).
 *
 * Geometry: the quad covers exactly the integer-scaled viewport, so each source
 * line spans `u_scale` output rows and every line samples the beam at the same
 * offsets (no moire). v_pix is the output-pixel position inside the viewport,
 * y down, at pixel centres.
 */

varying vec2 v_pix;

#ifdef VERTEX
attribute vec2 a_pos;        /* clip-space corner of the viewport quad */
uniform vec2 u_out_size;     /* viewport size in output pixels (= src * scale) */
void main()
{
    v_pix = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5) * u_out_size;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
#endif

#ifdef FRAGMENT
uniform sampler2D u_tex;     /* frame as RGBA bytes of ARGB8888 words: .bgr is RGB */
uniform vec2 u_tex_size;     /* texture allocation in texels */
uniform vec2 u_src_size;     /* active image in source pixels */
uniform float u_scale;       /* integer scale */
/* x: beam hardness k (weight exp(-k*d^2), d in source lines)
 * y: horizontal hardness (same form, d in source pixels; ~0.5 px wide)
 * z: aperture-grille dark level (1.0 = mask off)
 * w: brightness gain (1 / mean beam coverage, from the CPU) */
uniform vec4 u_crt;

vec3 texel_rgb(vec2 p)       /* p = integer source pixel */
{
    return texture2D(u_tex, (p + 0.5) / u_tex_size).bgr;
}

#if CRT && PASS != 2
vec3 fetch_lin(vec2 p)       /* linear light; black outside the image */
{
    vec2 inside = step(vec2(0.0), p) * step(p, u_src_size - 1.0);
    vec3 c = texel_rgb(clamp(p, vec2(0.0), u_src_size - 1.0));
    return c * c * (inside.x * inside.y);
}

vec3 line_rgb(float y, float sx)   /* source line y, softened around source x */
{
    float cx = floor(sx);
    float dx = sx - (cx + 0.5);
    float wl = exp(-u_crt.y * (dx + 1.0) * (dx + 1.0));
    float wc = exp(-u_crt.y * dx * dx);
    float wr = exp(-u_crt.y * (dx - 1.0) * (dx - 1.0));
    vec3 c = fetch_lin(vec2(cx - 1.0, y)) * wl
           + fetch_lin(vec2(cx, y)) * wc
           + fetch_lin(vec2(cx + 1.0, y)) * wr;
    return c / (wl + wc + wr);
}
#endif

#if CRT && PASS == 2
vec3 hline_lin(float y)      /* PASS 1 output for source line y (this column), linear light */
{
    float inside = step(0.0, y) * step(y, u_src_size.y - 1.0);
    /* PASS 1 drew line y at framebuffer row (lines - 1 - y): GL's origin is bottom-left. */
    vec2 p = vec2(floor(v_pix.x), u_src_size.y - 1.0 - clamp(y, 0.0, u_src_size.y - 1.0));
    vec3 c = texture2D(u_tex, (p + 0.5) / u_tex_size).rgb;
    return c * c * inside;
}
#endif

void main()
{
    vec2 s = v_pix / u_scale;            /* source-pixel position */
#if CRT && PASS == 1
    /* v_pix.y is already a source line here (the target is source-lines tall). */
    gl_FragColor = vec4(sqrt(line_rgb(floor(v_pix.y), s.x)), 1.0);
#elif CRT && PASS == 2
    float ly = floor(s.y);
    float dy = s.y - (ly + 0.5);
    float w0 = exp(-u_crt.x * dy * dy);
    float wu = exp(-u_crt.x * (dy + 1.0) * (dy + 1.0));
    float wd = exp(-u_crt.x * (dy - 1.0) * (dy - 1.0));
    vec3 col = hline_lin(ly) * w0 + hline_lin(ly - 1.0) * wu + hline_lin(ly + 1.0) * wd;
    col *= u_crt.w;
    if (u_crt.z < 1.0) {                 /* aperture grille: R, G, B stripes */
        float m = mod(floor(gl_FragCoord.x), 3.0);
        vec3 mask = vec3(u_crt.z);
        if (m < 0.5) mask.r = 1.0;
        else if (m < 1.5) mask.g = 1.0;
        else mask.b = 1.0;
        col *= mask * (3.0 / (1.0 + 2.0 * u_crt.z));
    }
    gl_FragColor = vec4(sqrt(clamp(col, 0.0, 1.0)), 1.0);
#elif CRT
    float ly = floor(s.y);
    float dy = s.y - (ly + 0.5);         /* -0.5..0.5 inside the line */
    float w0 = exp(-u_crt.x * dy * dy);
    float wu = exp(-u_crt.x * (dy + 1.0) * (dy + 1.0));
    float wd = exp(-u_crt.x * (dy - 1.0) * (dy - 1.0));
    vec3 col = line_rgb(ly, s.x) * w0
             + line_rgb(ly - 1.0, s.x) * wu
             + line_rgb(ly + 1.0, s.x) * wd;
    col *= u_crt.w;
    if (u_crt.z < 1.0) {                 /* aperture grille: R, G, B stripes */
        float m = mod(floor(gl_FragCoord.x), 3.0);
        vec3 mask = vec3(u_crt.z);
        if (m < 0.5) mask.r = 1.0;
        else if (m < 1.5) mask.g = 1.0;
        else mask.b = 1.0;
        col *= mask * (3.0 / (1.0 + 2.0 * u_crt.z));
    }
    gl_FragColor = vec4(sqrt(clamp(col, 0.0, 1.0)), 1.0);
#else
    gl_FragColor = vec4(texel_rgb(floor(s)), 1.0);
#endif
}
#endif
