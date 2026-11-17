/*
 * present_math.h — integer-scale fit used by the presenter (header-only so the
 * scaling-math unit test compiles it without SDL or GL).
 *
 * The largest integer scale whose image fits the drawable, centred with black
 * borders: 424x224 in 1280x720 (Switch handheld) -> 3x = 1272x672 at (4,24);
 * in 1920x1080 (docked) -> 4x = 1696x896 at (112,92).
 */
#pragma once

typedef struct PresentRect { int x, y, w, h, scale; } PresentRect;

static inline PresentRect present_integer_fit(int drawable_w, int drawable_h,
                                              int src_w, int src_h)
{
    PresentRect r = { 0, 0, 0, 0, 1 };
    if (src_w <= 0 || src_h <= 0 || drawable_w <= 0 || drawable_h <= 0) return r;
    int sx = drawable_w / src_w, sy = drawable_h / src_h;
    int s = sx < sy ? sx : sy;
    if (s < 1) s = 1;              /* smaller than 1x: show 1x, cropped by the window */
    r.scale = s;
    r.w = src_w * s;
    r.h = src_h * s;
    r.x = (drawable_w - r.w) / 2;  /* may be negative below 1x: centred crop */
    r.y = (drawable_h - r.h) / 2;
    return r;
}

/* Full-screen view for an output of out_w x out_h pixels: w x h game pixels shown at an
 * integer scale. h is 240..270 lines (the --tall ROM's range:
 * 8 rows above the raster, the rest below), w is 320..480 columns, even (320 + 2 x margin).
 * Of the two largest scales that fit 320 x 240, the one covering more of the output wins;
 * on a tie the smaller one (more map). The Switch's outputs give its fixed views:
 * 1280x720 -> 426x240 at 3x, 1920x1080 -> 480x270 at 4x. */
typedef struct PresentView { int w, h, scale; } PresentView;

static inline PresentView present_view_at(int out_w, int out_h, int k)
{
    PresentView v;
    v.scale = k;
    v.h = out_h / k;
    v.w = out_w / k;
    if (v.h > 270) v.h = 270;
    if (v.h < 240) v.h = 240;
    if (v.w > 480) v.w = 480;
    if (v.w < 320) v.w = 320;
    v.w &= ~1;
    return v;
}

static inline PresentView present_view_fit(int out_w, int out_h)
{
    int k = out_h / 240, kw = out_w / 320;
    if (kw < k) k = kw;
    if (k < 1) return present_view_at(320, 240, 1);   /* below 1x: the smallest view, cropped */
    PresentView best = present_view_at(out_w, out_h, k);
    if (k > 1) {
        PresentView v = present_view_at(out_w, out_h, k - 1);
        long a = (long)best.w * best.h * k * k, b = (long)v.w * v.h * (k - 1) * (k - 1);
        if (b >= a) best = v;
    }
    return best;
}

/* Mean beam coverage of one source line rendered `scale` rows tall with the CRT
 * shader's Gaussian beam exp(-k*d^2) (d in source lines, the line and its two
 * neighbours, sampled at output-row centres) for a flat field. Its reciprocal is
 * the brightness gain that keeps the average light output equal to the plain
 * image. */
static inline double present_beam_mean(int scale, double k,
                                       double (*expfn)(double))
{
    if (scale < 1 || k <= 0.0) return 1.0;
    double sum = 0.0;
    for (int r = 0; r < scale; r++) {
        double dy = (r + 0.5) / scale - 0.5;
        for (int n = -1; n <= 1; n++) sum += expfn(-k * (dy - n) * (dy - n));
    }
    return sum / scale;
}
