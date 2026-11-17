/* test_present_math.c — integer-scale fit table (SPEC 6.3, 7). Built and run by
 * tests/test_presentation.py:  cc -I game tests/test_present_math.c -lm */
#include "present_math.h"

#include <math.h>
#include <stdio.h>

static int fails;

static void expect(int dw, int dh, int sw, int sh, int x, int y, int w, int h, int s)
{
    PresentRect r = present_integer_fit(dw, dh, sw, sh);
    int ok = r.x == x && r.y == y && r.w == w && r.h == h && r.scale == s;
    printf("%s %4dx%-4d src %dx%d -> %dx %dx%d at (%d,%d)\n", ok ? "PASS" : "FAIL",
           dw, dh, sw, sh, r.scale, r.w, r.h, r.x, r.y);
    if (!ok) {
        printf("     expected %dx %dx%d at (%d,%d)\n", s, w, h, x, y);
        fails++;
    }
}

static void expect_view(int ow, int oh, int w, int h, int s, const char *what)
{
    PresentView v = present_view_fit(ow, oh);
    int ok = v.w == w && v.h == h && v.scale == s;
    printf("%s view %4dx%-4d -> %dx%d at %dx (%s)\n", ok ? "PASS" : "FAIL", ow, oh, v.w, v.h,
           v.scale, what);
    if (!ok) {
        printf("     expected %dx%d at %dx\n", w, h, s);
        fails++;
    }
}

int main(void)
{
    /* Full-screen view sizes (the Mac app's rule; the Switch's two outputs give its views). */
    expect_view(1280, 720, 426, 240, 3, "Switch handheld");
    expect_view(1920, 1080, 480, 270, 4, "Switch docked, 1080p");
    expect_view(2880, 1620, 480, 270, 6, "default window on Retina");
    expect_view(3024, 1890, 432, 270, 7, "MacBook Pro 14 below the notch: exact");
    expect_view(3456, 2160, 432, 270, 8, "MacBook Pro 16 below the notch: tie -> more map");
    expect_view(2560, 1600, 426, 266, 6, "MacBook Air 13");
    expect_view(2560, 1440, 426, 240, 6, "1440p");
    expect_view(3840, 2160, 480, 270, 8, "4K");
    expect_view(5120, 2880, 426, 240, 12, "5K");
    expect_view(1600, 1200, 320, 240, 5, "4:3");
    expect_view(3440, 1440, 480, 240, 6, "ultrawide: width capped");
    expect_view(300, 200, 320, 240, 1, "below 1x");

    expect(1280, 720, 424, 224, 4, 24, 1272, 672, 3);    /* Switch handheld */
    expect(1920, 1080, 424, 224, 112, 92, 1696, 896, 4); /* Switch docked */
    expect(1280, 720, 320, 224, 160, 24, 960, 672, 3);   /* 4:3 view, handheld */
    expect(1920, 1080, 320, 224, 320, 92, 1280, 896, 4); /* 4:3 view, docked */
    expect(2560, 1440, 424, 224, 8, 48, 2544, 1344, 6);  /* 1440p */
    expect(3840, 2160, 424, 224, 12, 72, 3816, 2016, 9);  /* 4K */
    expect(1271, 720, 424, 224, 211, 136, 848, 448, 2);    /* 1 px short of 3x */
    expect(1001, 673, 424, 224, 76, 112, 848, 448, 2);     /* odd size */
    expect(1272, 672, 424, 224, 0, 0, 1272, 672, 3);     /* exact */
    expect(853, 480, 424, 224, 2, 16, 848, 448, 2);      /* 480p */
    expect(300, 200, 424, 224, -62, -12, 424, 224, 1);   /* smaller than 1x: centred crop */

    /* Brightness compensation: mean beam coverage is the same for every line
     * and below 1 for a scanline beam; gain = 1/mean. */
    double m3 = present_beam_mean(3, 7.0, exp), m4 = present_beam_mean(4, 7.0, exp);
    int ok = m3 > 0.5 && m3 < 1.0 && m4 > 0.5 && m4 < 1.0 && present_beam_mean(3, 0.0, exp) == 1.0;
    printf("%s beam mean k=7: 3x %.4f (gain %.3f), 4x %.4f (gain %.3f)\n", ok ? "PASS" : "FAIL",
           m3, 1.0 / m3, m4, 1.0 / m4);
    if (!ok) fails++;
    printf("%d failure(s)\n", fails);
    return fails != 0;
}
