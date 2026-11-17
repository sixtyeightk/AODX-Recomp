/*
 * arcus_video.h — presentation for the native Arcus Odyssey build (track C):
 * 424x224 widescreen gameplay / pillarbox, HUD side strips, GL presenter with
 * the CRT shader and integer scaling, SDL_Renderer + CPU-scanline fallback.
 */
#pragma once
#include <stdint.h>

struct HostPresenter;
struct GameVideo;
struct GVDP;

#define ARCUS_VIEW_W        424   /* 16:9 gameplay view at 224 lines */
#define ARCUS_VIEW_EXTRA    52    /* extra plane/sprite columns per side */
#define ARCUS_NATIVE_W      320

extern const struct HostPresenter g_arcus_presenter;
extern const struct GameVideo     g_arcus_game_video;

/* The 424-view predicate (track B's rule): H40 ((reg12 & $81) == $81) and a
 * level in play (RAM $FF8292 != 0). */
int arcus_gameplay_view(const struct GVDP *vdp);

/* Output width the presenter keeps constant for the current settings
 * (424 with widescreen on, 320 for the original 4:3 view). */
int arcus_video_canvas_width(void);
/* The map area the current gameplay frame shows, relative to the game's 320 x 192 playfield
 * (screen 0,0 = map camera + 16): x0/y0 <= 0, w x h. */
void arcus_video_view_rect(int *x0, int *y0, int *w, int *h);

/* Where the current frame's 320-wide H40 raster lands in a presented frame out_w x out_h:
 * the output position of raster pixel (0, 0). */
void arcus_video_raster_origin(int out_w, int out_h, int *x0, int *y0);
/* First output row of the HUD (the window plane's bottom band) in a presented frame out_h
 * rows tall, or -1 if the frame has none. */
int arcus_video_hud_top(int out_h);

/* Present the last game frame with `draw_overlay` composited on top (used by
 * the pause overlay). */
void arcus_video_present_paused(void (*draw_overlay)(uint32_t *buf, int w, int h, int stride));

/* Present a frame drawn by `draw` straight to the output at its full resolution (SDL
 * renderer calls on the output's w x h pixels), not the game picture: the manual viewer. */
struct SDL_Renderer;
void arcus_video_present_custom(void (*draw)(struct SDL_Renderer *r, int w, int h));

/* Show a short message over gameplay for ~1.5 s (quick save/load). */
void arcus_video_toast(const char *text);

/* Start of a runner iteration (called first thing in pause_frame), for frame timing. */
void arcus_video_frame_begin(void);

/* Small 5x8 font (sentence case, digits): draw text / its width in pixels. */
void arcus_small_text(uint32_t *buf, int stride, int h, int x, int y, const char *t, uint32_t col);
int arcus_small_text_width(const char *t);
/* 1 while the title menu (or its Music Mode page) is shown. */
int arcus_video_on_title(void);

/* How lit the game picture is, from the palette: 256 = as the ROM palette (normal play),
 * lower while an area change fades it, 0 while it is black (display off for loading). */
int arcus_video_play_fade(void);
/* Box-filtered thumbnail (tw x th ARGB) of the last presented game frame. */
void arcus_video_thumbnail(uint32_t *out, int tw, int th);
