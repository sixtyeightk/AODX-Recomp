/*
 * arcus_manual.c — the extras viewer (Extras > Original manual / Game guide / Reviews); see
 * arcus_manual.h.
 *
 * A book's pages are JPEGs embedded by the build (arcus_extras.c). They are drawn straight to
 * the output at its full resolution through the SDL renderer (arcus_video_present_custom),
 * not into the game's 426 x 240 picture, where the text would be unreadable.
 *   - Pages (the manual, ~870 x 1500 px a page; magazine reviews): fitted to the height.
 *     Where two fit side by side (docked, Mac full screen, even handheld) they show as
 *     spreads: the first page alone, then 2-3, 4-5, ..., the last alone when it has no
 *     partner. LEFT / RIGHT (or L / R) turn pages. A zooms one page to 150% of the fitted
 *     size (small magazine print); the D-pad / either stick then move around it, L / R turn
 *     pages and A fits again.
 *   - Scroll (the game guide: one tall page, cut by the build into equal-width strips
 *     stacked top to bottom): a column at most 1.25 x the screen height wide, centred; UP /
 *     DOWN or either stick scroll (hold), L / R or LEFT / RIGHT move by a screen.
 *   - A scroll book may have a "frame" book (group "<group>-frame", e.g. the guide's red Power
 *     Jewel): one picture and its spot on the page, "x|y|period". It is drawn over that spot
 *     every other `period` frames, so the picture alternates as it does in the game.
 *   - B goes back, − closes the menu. The background is black (the guide's own colour).
 *   - Quality: a page shown smaller than its scan is shrunk on the CPU (area average) to its
 *     exact on-screen size, so small print stays legible; a larger one is enlarged by the
 *     GPU (linear). Decoded pages and textures are cached for the pages around the current
 *     one; JPEGs are decoded by stb_image (game/third_party).
 *   - The controls line uses the overlay's small font at the game picture's integer scale.
 */
#include "arcus_manual.h"
#include "arcus_extras.h"
#include "arcus_host.h"
#include "arcus_input.h"
#include "arcus_video.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define DECODED_MAX  4
#define TEXTURES_MAX 4
#define STRIPS_MAX   64
#define ZOOM         1.5f

typedef struct { int page, w, h; uint32_t *px; unsigned used; } Decoded;      /* ARGB */
typedef struct { int page, w, h; SDL_Texture *tex; unsigned used; } Texture;

static struct {
    int open;
    int book, kind, pages;
    int page;              /* current page (spreads: any page of the spread) */
    int spreads;           /* the last frame showed spreads (turns go by two) */
    int zoom;              /* pages: 1 = one page at ZOOM x the fitted size */
    float pan_x, pan_y;    /* zoomed: the page's offset left of / above its centred spot */
    float scroll;          /* scroll books: the column's top above the screen top, output px */
    int strip_w, strip_h[STRIPS_MAX];   /* scroll books: each strip's size (stbi_info) */
    uint32_t held;
    float stick_x, stick_y;
    int jump;              /* scroll books: screens to move this frame (-1, 0, +1) */
    uint32_t prev_held;
    unsigned tick;
    Decoded dec[DECODED_MAX];
    Texture tex[TEXTURES_MAX];
    SDL_Texture *hint;
    int hint_w, hint_h;
    /* scroll books: the alternating picture (a "-frame" book), its page spot and period */
    int frame_book, frame_x, frame_y, frame_period;
    uint32_t *frame_px;
    int frame_w, frame_h;
    SDL_Texture *frame_tex;
    int frame_tw;          /* frame_tex's width (its on-screen size) */
    char hint_text[96];
} M;

/* ---- decoded pages and textures -------------------------------------------------------- */

static const Decoded *decoded(int page)
{
    for (int i = 0; i < DECODED_MAX; i++)
        if (M.dec[i].px && M.dec[i].page == page) { M.dec[i].used = M.tick; return &M.dec[i]; }
    const uint8_t *jpg;
    size_t len;
    if (!arcus_extras_page(M.book, page, &jpg, &len)) return NULL;
    int slot = 0;                                              /* least recently used */
    for (int i = 1; i < DECODED_MAX; i++)
        if (!M.dec[i].px || (M.dec[slot].px && M.dec[i].used < M.dec[slot].used)) slot = i;
    Decoded *d = &M.dec[slot];
    free(d->px);
    memset(d, 0, sizeof *d);
    int w, h, comp;
    uint8_t *rgb = stbi_load_from_memory(jpg, (int)len, &w, &h, &comp, 3);
    if (!rgb) { fprintf(stderr, "[arcus-manual] page %d: %s\n", page + 1, stbi_failure_reason()); return NULL; }
    d->px = (uint32_t *)malloc((size_t)w * h * 4);
    if (d->px)
        for (size_t i = 0; i < (size_t)w * h; i++)
            d->px[i] = 0xFF000000u | (uint32_t)rgb[i * 3] << 16 | (uint32_t)rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2];
    stbi_image_free(rgb);
    if (!d->px) return NULL;
    d->page = page;
    d->w = w;
    d->h = h;
    d->used = M.tick;
    return d;
}

/* One axis of an exact area-average shrink: output pixel i covers source [i*r, (i+1)*r)
 * (r = n_src / n_dst), partly covered source pixels weighted by their share. 16.16 weights
 * per output pixel: first source index and its weights (at most ceil(r) + 1). */
typedef struct { int first, count; uint32_t w[64]; } Taps;

static Taps *taps(int ns, int nd)
{
    Taps *t = (Taps *)malloc(sizeof(Taps) * nd);
    if (!t) return NULL;
    double r = (double)ns / nd;
    for (int i = 0; i < nd; i++) {
        double a = i * r, b = (i + 1) * r;
        int f = (int)a, l = (int)ceil(b) - 1;
        if (l >= ns) l = ns - 1;
        if (l - f + 1 > 64) l = f + 63;
        t[i].first = f;
        t[i].count = l - f + 1;
        uint32_t sum = 0;
        for (int k = 0; k < t[i].count; k++) {
            double lo = f + k > a ? f + k : a, hi = f + k + 1 < b ? f + k + 1 : b;
            t[i].w[k] = (uint32_t)((hi - lo) / r * 65536.0 + 0.5);
            sum += t[i].w[k];
        }
        if (sum) t[i].w[t[i].count - 1] += 65536u - sum;     /* weights sum to exactly 1 */
    }
    return t;
}

/* Area-average shrink of src (sw x sh) to tw x th (tw <= sw, th <= sh), with fractional
 * pixel coverage: text strokes keep an even weight at any ratio (whole-pixel boxes alternate
 * 1- and 2-pixel averages, which makes letters look uneven). */
static uint32_t *shrink(const uint32_t *src, int sw, int sh, int tw, int th)
{
    uint32_t *out = (uint32_t *)malloc((size_t)tw * th * 4);
    uint32_t *mid = (uint32_t *)malloc((size_t)tw * sh * 4 * 3);    /* rows shrunk: r, g, b */
    Taps *tx = taps(sw, tw), *ty = taps(sh, th);
    if (!out || !mid || !tx || !ty) { free(out); free(mid); free(tx); free(ty); return NULL; }
    for (int y = 0; y < sh; y++) {
        const uint32_t *row = src + (size_t)y * sw;
        uint32_t *m = mid + (size_t)y * tw * 3;
        for (int x = 0; x < tw; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < tx[x].count; k++) {
                uint32_t c = row[tx[x].first + k], w = tx[x].w[k];
                r += ((c >> 16) & 0xFF) * w; g += ((c >> 8) & 0xFF) * w; b += (c & 0xFF) * w;
            }
            m[x * 3] = r >> 8; m[x * 3 + 1] = g >> 8; m[x * 3 + 2] = b >> 8;   /* 8.8 */
        }
    }
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            uint64_t r = 0, g = 0, b = 0;
            for (int k = 0; k < ty[y].count; k++) {
                const uint32_t *m = mid + ((size_t)(ty[y].first + k) * tw + x) * 3;
                uint64_t w = ty[y].w[k];
                r += m[0] * w; g += m[1] * w; b += m[2] * w;
            }
            uint32_t R = (uint32_t)((r + (1u << 23)) >> 24), G = (uint32_t)((g + (1u << 23)) >> 24),
                     B = (uint32_t)((b + (1u << 23)) >> 24);
            out[(size_t)y * tw + x] = 0xFF000000u | (R > 255 ? 255 : R) << 16 | (G > 255 ? 255 : G) << 8 |
                                      (B > 255 ? 255 : B);
        }
    free(mid); free(tx); free(ty);
    return out;
}

/* The page as a texture for an on-screen size of dw x dh: exactly that size when smaller than
 * the scan (shrunk on the CPU), else the scan itself (enlarged by the GPU). */
static SDL_Texture *texture(SDL_Renderer *r, int page, int dw, int dh)
{
    const Decoded *d = decoded(page);
    if (!d) return NULL;
    int tw = dw < d->w ? dw : d->w, th = dh < d->h ? dh : d->h;
    if (tw < 1 || th < 1) return NULL;
    for (int i = 0; i < TEXTURES_MAX; i++)
        if (M.tex[i].tex && M.tex[i].page == page && M.tex[i].w == tw && M.tex[i].h == th) {
            M.tex[i].used = M.tick;
            return M.tex[i].tex;
        }
    int slot = 0;
    for (int i = 1; i < TEXTURES_MAX; i++)
        if (!M.tex[i].tex || (M.tex[slot].tex && M.tex[i].used < M.tex[slot].used)) slot = i;
    Texture *t = &M.tex[slot];
    if (t->tex) SDL_DestroyTexture(t->tex);
    memset(t, 0, sizeof *t);
    const uint32_t *px = d->px;
    uint32_t *small = NULL;
    if (tw < d->w || th < d->h) {
        small = shrink(d->px, d->w, d->h, tw, th);
        if (!small) return NULL;
        px = small;
    }
    t->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, tw, th);
    if (t->tex) {
        SDL_UpdateTexture(t->tex, NULL, px, tw * 4);
        SDL_SetTextureScaleMode(t->tex, SDL_ScaleModeLinear);
        t->page = page;
        t->w = tw;
        t->h = th;
        t->used = M.tick;
    }
    free(small);
    return t->tex;
}

/* ---- layout ------------------------------------------------------------------------------ */

/* The spread holding `page`: the cover alone, then pairs (2-3, 4-5, ...), the last page alone
 * when it has no partner. *first = its first page; returns the number of pages (1 or 2). */
static int spread_of(int page, int *first)
{
    int n = M.pages;
    if (page <= 0) { *first = 0; return 1; }
    *first = page % 2 ? page : page - 1;
    return *first + 1 < n ? 2 : 1;
}

typedef struct { int scale_ui, strip_h, avail_h; } Frame;

static Frame frame_for(int w, int h)
{
    Frame f;
    f.scale_ui = h / 240 > 1 ? h / 240 : 1;
    f.strip_h = 12 * f.scale_ui;
    f.avail_h = h - f.strip_h - 4 * f.scale_ui;
    (void)w;
    return f;
}

/* Spreads only where two fitted pages fit side by side. */
static int use_spreads(int w, const Frame *f)
{
    const Decoded *d = decoded(M.page);
    if (!d) return 0;
    float pw = (float)f->avail_h * d->w / d->h;
    return 2.f * pw + 2 * f->scale_ui <= (float)w;
}

/* ---- drawing ----------------------------------------------------------------------------- */

static void draw_hint(SDL_Renderer *r, int w, int h, const Frame *f, const char *text)
{
    if (!M.hint || strcmp(text, M.hint_text)) {
        if (M.hint) SDL_DestroyTexture(M.hint);
        M.hint = NULL;
        snprintf(M.hint_text, sizeof M.hint_text, "%s", text);
        int tw = arcus_small_text_width(text) + 6, th = 10;
        uint32_t *buf = (uint32_t *)calloc((size_t)tw * th, 4);
        if (!buf) return;
        arcus_small_text(buf, tw, th, 3, 1, text, 0xFFF0E8C8u);
        M.hint = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (M.hint) {
            SDL_UpdateTexture(M.hint, NULL, buf, tw * 4);
            SDL_SetTextureBlendMode(M.hint, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(M.hint, SDL_ScaleModeNearest);
        }
        M.hint_w = tw;
        M.hint_h = th;
        free(buf);
    }
    if (!M.hint) return;
    SDL_Rect bar = { 0, h - f->strip_h, w, f->strip_h };
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 16, 16, 20, 235);
    SDL_RenderFillRect(r, &bar);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    int k = f->scale_ui;
    SDL_Rect dst = { (w - M.hint_w * k) / 2, h - f->strip_h + (f->strip_h - M.hint_h * k) / 2,
                     M.hint_w * k, M.hint_h * k };
    SDL_RenderCopy(r, M.hint, NULL, &dst);
}

/* An ARTWORK picture's caption (its book detail), shown in the hint strip in place of the
 * page count; NULL for other books (a review's detail is its issue and score). */
static const char *caption(void)
{
    const ArcusBook *b = arcus_extras_book(M.book);
    return b && !strcmp(b->group, "art") && b->detail[0] ? b->detail : NULL;
}

/* Spread / single pages, fitted to the height. */
/* One page at ZOOM x its fitted size, moved by the held D-pad / stick. */
static void draw_zoomed(SDL_Renderer *r, int w, int h, const Frame *f, char *text, size_t tn)
{
    const Decoded *d = decoded(M.page);
    M.spreads = 0;
    if (caption()) snprintf(text, tn, "%s   Move   A zoom in/out   B back", caption());
    else snprintf(text, tn, "%d / %d   Move   L/R turn   A zoom in/out   B back", M.page + 1, M.pages);
    if (!d) return;
    int ph = (int)lroundf(f->avail_h * ZOOM), pw = (int)lroundf((float)ph * d->w / d->h);
    int view = h - f->strip_h;
    float vx = 0.f, vy = 0.f, step = (float)h / 40.f;
    if (M.held & ARCUS_HB_LEFT) vx = -1.f;
    else if (M.held & ARCUS_HB_RIGHT) vx = 1.f;
    else if (fabsf(M.stick_x) > 0.25f) vx = M.stick_x;
    if (M.held & ARCUS_HB_UP) vy = -1.f;
    else if (M.held & ARCUS_HB_DOWN) vy = 1.f;
    else if (fabsf(M.stick_y) > 0.25f) vy = -M.stick_y;
    M.pan_x += vx * step;
    M.pan_y += vy * step;
    float mx = pw > w ? (pw - w) / 2.f : 0.f, my = ph > view ? (float)(ph - view) : 0.f;
    if (M.pan_x < -mx) M.pan_x = -mx;
    if (M.pan_x > mx) M.pan_x = mx;
    if (M.pan_y < 0.f) M.pan_y = 0.f;
    if (M.pan_y > my) M.pan_y = my;
    SDL_Texture *t = texture(r, M.page, pw, ph);
    SDL_Rect dst = { (w - pw) / 2 - (int)lroundf(M.pan_x), (ph > view ? 0 : (view - ph) / 2) - (int)lroundf(M.pan_y),
                     pw, ph };
    if (t) SDL_RenderCopy(r, t, NULL, &dst);
}

static void draw_pages(SDL_Renderer *r, int w, const Frame *f, char *text, size_t tn)
{
    int first, count = spread_of(M.page, &first);
    M.spreads = use_spreads(w, f);
    if (!M.spreads) { first = M.page; count = 1; }
    int pw[2] = { 0, 0 }, ph[2] = { 0, 0 }, gap = 2 * f->scale_ui, total = 0;
    for (int i = 0; i < count; i++) {
        const Decoded *d = decoded(first + i);
        if (!d) continue;
        ph[i] = f->avail_h;
        pw[i] = (int)lroundf((float)f->avail_h * d->w / d->h);
        total += pw[i] + (i ? gap : 0);
    }
    int x = (w - total) / 2, y = 2 * f->scale_ui;
    for (int i = 0; i < count; i++) {
        SDL_Texture *t = pw[i] ? texture(r, first + i, pw[i], ph[i]) : NULL;
        SDL_Rect dst = { x, y, pw[i], ph[i] };
        if (t) SDL_RenderCopy(r, t, NULL, &dst);
        x += pw[i] + gap;
    }
    if (caption()) snprintf(text, tn, M.pages > 1 ? "%s   Left/Right turn   A zoom in/out   B back"
                                                 : "%s   A zoom in/out   B back", caption());
    else if (count == 2) snprintf(text, tn, "%d-%d / %d   Left/Right turn   A zoom in/out   B back", first + 1, first + 2, M.pages);
    else snprintf(text, tn, "%d / %d   Left/Right turn   A zoom in/out   B back", first + 1, M.pages);
    if (M.page + 2 < M.pages) decoded(M.page + 2);         /* decode ahead, one per frame */
}

/* The frame book's picture over its spot (page px, scaled by s, the column at x, scrolled by
 * top) during every other period; shrunk like the page. */
static void draw_frame(SDL_Renderer *r, int x, int top, float s, int view)
{
    if (!M.frame_px || M.frame_period <= 0 || (M.tick / (unsigned)M.frame_period) % 2 == 0) return;
    int fx = x + (int)lroundf(M.frame_x * s), fy = (int)lroundf(M.frame_y * s) - top;
    int fw = (int)lroundf(M.frame_w * s), fh = (int)lroundf(M.frame_h * s);
    if (fw < 1 || fh < 1 || fy + fh <= 0 || fy >= view) return;
    if (!M.frame_tex || M.frame_tw != fw) {
        if (M.frame_tex) SDL_DestroyTexture(M.frame_tex);
        M.frame_tex = NULL;
        uint32_t *px = fw < M.frame_w ? shrink(M.frame_px, M.frame_w, M.frame_h, fw, fh) : M.frame_px;
        int tw = fw < M.frame_w ? fw : M.frame_w, th = fw < M.frame_w ? fh : M.frame_h;
        if (!px) return;
        M.frame_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, tw, th);
        if (M.frame_tex) {
            SDL_UpdateTexture(M.frame_tex, NULL, px, tw * 4);
            SDL_SetTextureScaleMode(M.frame_tex, SDL_ScaleModeLinear);
        }
        if (px != M.frame_px) free(px);
        M.frame_tw = fw;
    }
    SDL_Rect dst = { fx, fy, fw, fh };
    if (M.frame_tex) SDL_RenderCopy(r, M.frame_tex, NULL, &dst);
}

/* The strips stacked in a centred column; scrolled by the held buttons / stick. */
static void draw_scroll(SDL_Renderer *r, int w, int h, const Frame *f, char *text, size_t tn)
{
    if (M.strip_w <= 0) { snprintf(text, tn, "B back"); return; }
    int col = w - 4 * f->scale_ui;
    if (col > (int)(h * 1.25f)) col = (int)(h * 1.25f);
    float s = (float)col / M.strip_w;
    long total = 0;
    for (int i = 0; i < M.pages; i++) total += M.strip_h[i];
    int view = h - f->strip_h;
    float max_scroll = total * s - view > 0 ? total * s - view : 0.f;
    float v = 0.f;
    if (M.held & ARCUS_HB_UP) v = -1.f;
    else if (M.held & ARCUS_HB_DOWN) v = 1.f;
    else if (fabsf(M.stick_y) > 0.25f) v = -M.stick_y;
    M.scroll += v * (float)h / 40.f + (float)M.jump * view * 0.9f;
    if (M.scroll < 0.f) M.scroll = 0.f;
    if (M.scroll > max_scroll) M.scroll = max_scroll;
    int x = (w - col) / 2, top = (int)lroundf(M.scroll);
    long cum = 0;
    for (int i = 0; i < M.pages; i++) {
        int y0 = (int)lroundf(cum * s) - top, y1 = (int)lroundf((cum + M.strip_h[i]) * s) - top;
        cum += M.strip_h[i];
        if (y1 <= 0 || y0 >= view) continue;
        SDL_Texture *t = texture(r, i, col, y1 - y0);
        SDL_Rect dst = { x, y0, col, y1 - y0 };
        if (t) SDL_RenderCopy(r, t, NULL, &dst);
        if (i + 1 < M.pages) decoded(i + 1);                /* the next strip, ahead */
    }
    draw_frame(r, x, top, s, view);
    snprintf(text, tn, "Up/Down scroll   L/R page   B back");
}

static void draw(SDL_Renderer *r, int w, int h)
{
    M.tick++;
    Frame f = frame_for(w, h);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    char text[96];
    if (M.kind == ARCUS_BOOK_SCROLL) draw_scroll(r, w, h, &f, text, sizeof text);
    else if (M.zoom) draw_zoomed(r, w, h, &f, text, sizeof text);
    else draw_pages(r, w, &f, text, sizeof text);
    draw_hint(r, w, h, &f, text);
}

/* ---- control ----------------------------------------------------------------------------- */

static void drop_frame(void)
{
    free(M.frame_px);
    if (M.frame_tex) SDL_DestroyTexture(M.frame_tex);
    M.frame_px = NULL;
    M.frame_tex = NULL;
    M.frame_book = -1;
}

/* The "<group>-frame" book of a scroll book, if any: decode its picture, read its spot. */
static void load_frame(const ArcusBook *b)
{
    drop_frame();
    char group[24];
    snprintf(group, sizeof group, "%s-frame", b->group);
    int fb = arcus_extras_find(group);
    const ArcusBook *f = arcus_extras_book(fb);
    const uint8_t *jpg;
    size_t len;
    if (!f || sscanf(f->detail, "%d|%d|%d", &M.frame_x, &M.frame_y, &M.frame_period) != 3 ||
        !arcus_extras_page(fb, 0, &jpg, &len)) return;
    int w, h, comp;
    uint8_t *rgb = stbi_load_from_memory(jpg, (int)len, &w, &h, &comp, 3);
    if (!rgb) return;
    M.frame_px = (uint32_t *)malloc((size_t)w * h * 4);
    if (M.frame_px)
        for (size_t i = 0; i < (size_t)w * h; i++)
            M.frame_px[i] = 0xFF000000u | (uint32_t)rgb[i * 3] << 16 | (uint32_t)rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2];
    stbi_image_free(rgb);
    M.frame_book = fb;
    M.frame_w = w;
    M.frame_h = h;
}

static void drop_caches(void)
{
    for (int i = 0; i < DECODED_MAX; i++) { free(M.dec[i].px); memset(&M.dec[i], 0, sizeof M.dec[i]); }
    for (int i = 0; i < TEXTURES_MAX; i++) {
        if (M.tex[i].tex) SDL_DestroyTexture(M.tex[i].tex);
        memset(&M.tex[i], 0, sizeof M.tex[i]);
    }
}

void arcus_manual_open(int book)
{
    const ArcusBook *b = arcus_extras_book(book);
    if (!b) return;
    if (book != M.book || !M.open) drop_caches();
    M.open = 1;
    M.book = book;
    M.kind = b->kind;
    M.pages = b->count;
    M.page = 0;
    M.zoom = 0;
    M.scroll = 0.f;
    M.strip_w = 0;
    drop_frame();
    if (M.kind == ARCUS_BOOK_SCROLL) {
        load_frame(b);
        if (M.pages > STRIPS_MAX) M.pages = STRIPS_MAX;
        for (int i = 0; i < M.pages; i++) {
            const uint8_t *jpg;
            size_t len;
            int sw = 0, sh = 0, comp;
            if (arcus_extras_page(book, i, &jpg, &len)) stbi_info_from_memory(jpg, (int)len, &sw, &sh, &comp);
            if (!i) M.strip_w = sw;
            M.strip_h[i] = sh;
        }
    }
    M.prev_held = ~0u;     /* buttons held when it opens don't count as presses */
}

static void turn(int dir)
{
    int first;
    M.pan_x = M.pan_y = 0.f;               /* zoomed: the next page from its top */
    if (!M.spreads) {
        M.page += dir;
    } else {
        int count = spread_of(M.page, &first);
        M.page = dir > 0 ? first + count : first - 1;
    }
    if (M.page < 0) M.page = 0;
    if (M.page > M.pages - 1) M.page = M.pages - 1;
}

int arcus_manual_frame(uint32_t nav, uint32_t held, float stick_x, float stick_y)
{
    if (!M.open) return 0;
    uint32_t edge = held & ~M.prev_held;
    M.prev_held = held;
    M.held = held;
    M.stick_x = stick_x;
    M.stick_y = stick_y;
    M.jump = 0;
    if (nav & ARCUS_NAV_MENU) { M.open = 0; return -1; }
    if (nav & ARCUS_NAV_BACK) { M.open = 0; return 0; }
    int fwd = (nav & ARCUS_NAV_RIGHT) || (edge & ARCUS_HB_R);
    int back = (nav & ARCUS_NAV_LEFT) || (edge & ARCUS_HB_L);
    if (M.kind == ARCUS_BOOK_SCROLL) M.jump = fwd - back;
    else {
        if (M.zoom) {                     /* LEFT / RIGHT move around the page; L / R turn */
            fwd = (edge & ARCUS_HB_R) != 0;
            back = (edge & ARCUS_HB_L) != 0;
        }
        if (fwd) turn(+1);
        if (back) turn(-1);
        if (nav & ARCUS_NAV_OK) {
            M.zoom ^= 1;
            M.pan_x = M.pan_y = 0.f;
        }
    }
    arcus_video_present_custom(draw);
    return 1;
}
