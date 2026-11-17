/*
 * arcus_video.c — presentation for the native Arcus Odyssey build (track C).
 *
 *  - Widescreen (SPEC 6.2): during gameplay the VDP renders ARCUS_VIEW_EXTRA
 *    (52) more plane/sprite columns per side, giving 424x224. Every other
 *    screen (H32 intro/title, H40 menus) is centred in the same 424 canvas with
 *    black bars, so the output width never changes. widescreen=0 keeps a
 *    constant 320 canvas (H32 screens pillarboxed inside it).
 *    In 4:3 (and in the camera-locked arenas $19/$1A/$20, whose side strips
 *    show a seam) $FFFA08 is set every frame so the ROM keeps 4:3 bounds.
 *    Mechanism: the engine VDP's margin/canvas path (gvdp_set_ws_extra /
 *    _canvas / _bar_black) driven from HostPresenter.pre_raster.
 *  - HUD row: the window plane stays 320 wide; the side strips show the map.
 *  - Presenter (SPEC 6.3/6.4): raw GL through SDL's GL context with the CRT
 *    shader and integer scaling; fallback = SDL_Renderer with integer scaling
 *    and CPU scanlines.
 *
 * Debug / test environment:
 *   ARCUS_VIEW=native         leave the VDP alone (authentic 320/256 output)
 *   ARCUS_PRESENTER=sdl       force the SDL_Renderer fallback
 *   ARCUS_WINDOW=WxH          initial window size (Mac; default 3x the canvas);
 *                             also turns off Retina scaling, so it is the drawable
 *   ARCUS_HIDDEN_WINDOW=1     create the window hidden (GL tests)
 *   ARCUS_PRESENT_DUMP=path   write the PRESENTED image (drawable size) as PNG;
 *                             "%d" in the path is replaced by the present count
 *   ARCUS_PRESENT_DUMP_AT=N[,M...]  present counts to dump at (1-based; equals
 *                             the runner frame number while not paused)
 *   ARCUS_SCREENSHOT_PRESENTED=1  input-script SCREENSHOTs save the next PRESENTED
 *                             image (drawable size, CRT filter included) instead of
 *                             the 1x canvas
 *   ARCUS_RESIZE_AT=N:WxH     resize the window at present N (dock/undock test)
 *   ARCUS_WS_CHECK=1          log undrawn plane-B cells in the 424 view each
 *                             gameplay frame: "[arcus-ws] frame=F empty=N
 *                             sprx9=S cols=C" (see ws_empty_cells)
 */
#include "arcus_video.h"
#include "arcus_gl.h"
#include "arcus_host.h"
#include "arcus_minimap.h"
#include "arcus_magic.h"
#include "arcus_title_dx.h"
#include "present_math.h"

#include "game_video.h"
#include "host_present.h"
#include "genesis_machine.h"
#include "genesis_vdp.h"
#include "genesis_host_mem.h"
#if defined(__SWITCH__) || defined(GENESIS_SWITCH_STUBS)
#include "platform_switch.h"
#endif
#include "video/color_lut.h"
#include "png_write.h"
#include "overlay_draw.h"
#include "video/genesis_dac.h"

#include <SDL2/SDL.h>
#include "arcus_input.h"   /* arcus_input_sticks (stick display) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_STRIDE 512
static int play_fade256(const uint32_t *frame, int w, int h);
static void fade_overlays(uint32_t *tmp, const uint32_t *frame, int w, int h, int f);
#define BUF_LINES  480

static struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int want_gl, gles, gl;
    int native_view;                   /* ARCUS_VIEW=native */
    int frame_extra;                   /* margin the current frame renders with */
    int view_top, view_rows;           /* map rows shown above / in total (arcus_video_view_rect) */
    uint32_t last[BUF_STRIDE * BUF_LINES];   /* last frame, colour-processed */
    uint32_t tmp[BUF_STRIDE * BUF_LINES];    /* last frame + overlay */
    uint32_t dx[BUF_STRIDE * BUF_LINES];     /* the DX title, before colour processing */
    int last_w, last_h;
    unsigned presents;                 /* draw/present count */
    ColorLut lut;
    int lut_built;
    uint8_t lut_level[256];   /* 8-bit channel -> nearest Genesis DAC level index */
    char toast[40];          /* short message drawn over gameplay */
    int toast_frames;
    /* SDL fallback */
    SDL_Texture *fb_tex;
    int fb_tex_w, fb_tex_h;
    uint32_t *fb_rows;
    size_t fb_rows_cap;
    /* dump */
    const char *dump_path;
    unsigned dump_at[32];
    int dump_n;
    char shot_path[512];   /* ARCUS_SCREENSHOT_PRESENTED: the next present goes here */
    int ws_check;
    unsigned frames;
    unsigned resize_at;
    int resize_w, resize_h;
} V;

/* ---- presets ------------------------------------------------------------- */
static ArcusCrtParams crt_params(int preset)
{
    ArcusCrtParams p = { 0, 0.f, 0.f, 1.f };
    switch (preset) {
    /*                                    beam_k pix_k mask */
    case ARCUS_CRT_SCANLINES:      p = (ArcusCrtParams){ 1, 7.0f, 6.0f, 1.00f }; break;
    case ARCUS_CRT_SCANLINES_MASK: p = (ArcusCrtParams){ 1, 7.0f, 6.0f, 0.45f }; break;
    case ARCUS_CRT_STRONG:         p = (ArcusCrtParams){ 1, 12.0f, 4.0f, 0.55f }; break;
    default: break;
    }
    return p;
}

/* ---- widescreen policy ---------------------------------------------------- */
/* RAM of the native ROM (build/arcus-native.bin, track B). */
#define RAM_PLAYING   0xFF8292u   /* non-zero while a level is being played */
#define RAM_AREA      0xFF828Eu   /* current area */
#define RAM_FORCE_43  0xFFFA08u   /* non-zero (rewritten every frame): behave like the 4:3 ROM */

/* Camera-locked arenas whose side strips show a seam: shown 4:3. */
static const uint8_t AUTO_43_AREAS[] = { 0x19, 0x1A, 0x20 };

int arcus_gameplay_view(const GVDP *vdp)
{
    /* Track B's rule, verified on every screen: H40 (reg12 & $81) and a level
     * in play. Damage/magic flashes briefly write reg12 = $81; still H40. */
    return (vdp->reg[0x0C] & 0x81) == 0x81 && glue_peek8(RAM_PLAYING) != 0;
}

static int auto_43_area(void)
{
    uint8_t area = glue_peek8(RAM_AREA);
    for (size_t i = 0; i < sizeof AUTO_43_AREAS; i++)
        if (AUTO_43_AREAS[i] == area) return 1;
    return 0;
}

static PresentView tall_view(void);
static int tall_height(void) { return tall_view().h; }

/* Full-screen view: the picture fills the screen at an integer scale (present_view_fit):
 * 426 x 240 (Switch handheld 3x, 1278 of 1280 px), 480 x 270 (docked 4x), or on the Mac
 * whatever fits the window. */
static int view_extra(void)
{
    PresentView v = tall_view();
    return v.h ? (v.w - ARCUS_NATIVE_W) / 2 : ARCUS_VIEW_EXTRA;
}

void arcus_video_view_rect(int *x0, int *y0, int *w, int *h)
{
    *x0 = -V.frame_extra;
    *w = 320 + 2 * V.frame_extra;
    *y0 = -V.view_top;
    *h = V.view_rows;
}

/* genesis_machine.c emit_tall: frames with tall rows put raster line 0 at output row
 * gvdp_tall_top() and the HUD band in the last output rows; other frames are centred. */
void arcus_video_raster_origin(int out_w, int out_h, int *x0, int *y0)
{
    int active = gvdp_screen_height(&g_machine.vdp);
    *x0 = (out_w - ARCUS_NATIVE_W) / 2;
    *y0 = (gvdp_tall_height() > active && gvdp_tall_rows()) ? gvdp_tall_top() : (out_h - active) / 2;
}

int arcus_video_hud_top(int out_h)
{
    const GVDP *v = &g_machine.vdp;
    int active = gvdp_screen_height(v);
    unsigned wy = v->reg[0x12];
    int hud = (wy & 0x80) ? active - (int)(wy & 0x1F) * 8 : 0;   /* window band at the bottom */
    if (hud <= 0 || hud >= active) return -1;
    if (gvdp_tall_height() > active && gvdp_tall_rows()) return out_h - hud;
    return (out_h - active) / 2 + active - hud;
}

int arcus_video_canvas_width(void)
{
    return g_arcus_settings.widescreen ? ARCUS_NATIVE_W + 2 * view_extra() : ARCUS_NATIVE_W;
}

/* Port of tests/test_widescreen.py empty_cells(): plane-B cells holding tile 0
 * inside the widened view, above the HUD (rows 0..23). Inside the drawn map the
 * game never leaves tile 0, except where the map itself has void areas (e.g.
 * the corners of some isometric acts); *empty_cols counts 8-px columns that are
 * tile 0 over the whole height, the signature of a column the game never drew. */
static int ws_empty_cells(const GVDP *v, int extra, int *empty_cols)
{
    uint16_t base = (uint16_t)((v->reg[4] & 7) << 13);
    uint16_t hs = (uint16_t)((v->reg[13] & 0x3F) << 10);
    int hscroll = (v->vram[(uint16_t)(hs + 2)] << 8 | v->vram[(uint16_t)(hs + 3)]) & 0x3FF;
    int vscroll = v->vsram[1] & 0x3FF;
    int x0 = ((-(hscroll) - extra) % 512 + 512) % 512;
    int empty = 0, cols = 0;
    for (int sx = 0; sx < 320 + 2 * extra; sx += 8) {
        int col = ((x0 + sx) % 512) / 8, in_col = 0;
        for (int sy = 0; sy < 192; sy += 8) {
            int row = ((sy + vscroll) % 256) / 8;
            uint16_t a = (uint16_t)(base + (row * 64 + col) * 2);
            if (((v->vram[a] << 8 | v->vram[(uint16_t)(a + 1)]) & 0x7FF) == 0) { empty++; in_col++; }
        }
        if (in_col == 24) cols++;
    }
    *empty_cols = cols;
    return empty;
}

/* Sprites on screen whose SAT X word has bit 9 set. The engine VDP reads 10 X
 * bits (so negative X never wraps into the widened view); hardware reads 9, so
 * such a sprite would draw differently from hardware. Expected 0. */
static int sprites_x_bit9(const GVDP *v)
{
    uint16_t sat = (uint16_t)((v->reg[5] & 0x7F) << 9);
    int link = 0, n = 0;
    for (int i = 0; i < 80; i++) {
        uint16_t e = (uint16_t)(sat + link * 8);
        int y = ((v->vram[e] << 8 | v->vram[(uint16_t)(e + 1)]) & 0x3FF) - 128;
        int x = (v->vram[(uint16_t)(e + 6)] << 8 | v->vram[(uint16_t)(e + 7)]) & 0x3FF;
        if ((x & 0x200) && y > -32 && y < 224) n++;
        link = v->vram[(uint16_t)(e + 3)] & 0x7F;
        if (!link) break;
    }
    return n;
}

/* Full-screen view (ARCUS_TALL_BUILD, the --tall ROM build/arcus-tall.bin): 240..270 lines,
 * 320..480 columns, fitted to the output (present_view_fit). The Switch's screen is
 * 1280x720 handheld or 1920x1080 docked; ARCUS_TALL=240|270 forces one of those two (tests);
 * on the Mac it is the window's drawable, so resizing or going full screen re-fits the view
 * (as docking does on the Switch). Gameplay shows 8 map rows above the 224-line raster and
 * the rest below it, with the HUD moved to the bottom (the ROM keeps map rows R0..R0+19
 * drawn, 35 metatiles wide); camera-locked arenas, 4:3 and every other screen are
 * letterboxed. h = 0: not the full-screen build. */
static PresentView tall_view(void)
{
    static int env = -1;
    PresentView none = { 0, 0, 0 };
    if (env < 0) {
        const char *e = getenv("ARCUS_TALL");
        env = e ? atoi(e) : 0;
        if (env != 240 && env != 270) env = 0;
#ifdef ARCUS_TALL_BUILD
        if (!env) env = 1;                      /* full-screen build: always on */
#endif
        if (env) fprintf(stderr, "[arcus-video] full-screen view\n");
    }
    if (!env) return none;
    int ow = 1920, oh = 1080;
    if (env == 240) { ow = 1280; oh = 720; }
    else if (env == 1) {
#if defined(__SWITCH__) || defined(GENESIS_SWITCH_STUBS)
        if (!genesis_switch_docked()) { ow = 1280; oh = 720; }
#else
        int w = 0, h = 0;                       /* before the window exists: 1080p */
        if (V.renderer && SDL_GetRendererOutputSize(V.renderer, &w, &h) == 0 && w > 0 && h > 0) {
            ow = w;
            oh = h;
        }
#endif
    }
    return present_view_fit(ow, oh);
}

static int locked_area(void)
{
    static const uint8_t LOCKED[] = { 0x12, 0x13, 0x17, 0x19, 0x1A, 0x20, 0x33, 0x34 };
    uint8_t area = glue_peek8(RAM_AREA);
    for (size_t i = 0; i < sizeof LOCKED; i++)
        if (LOCKED[i] == area) return 1;
    return 0;
}

/* After the view grows (docking, widescreen back on) the camera can sit past the new
 * stops, where the wider/taller view shows the map wrapping; the game only applies its
 * stops while scrolling. Walk it back inside, at most 4 px per frame (the drawers keep up),
 * doing exactly what one of the game's own scroll steps does ($F1A0-$F456): camera and
 * ring move together, both players keep their map position (they are in screen
 * coordinates), and vertical steps also advance the water phase $8080. Skipped where the
 * camera is scripted or extra layers scroll with it (parallax areas, $829D/$829E). */
#define RAM_CAM_X     0xFF8100u
#define RAM_CAM_Y     0xFF8102u
#define RAM_RING_X    0xFF8110u
#define RAM_RING_Y    0xFF8112u
#define RAM_P1_X      0xFF8118u
#define RAM_P2_X      0xFF811Au
#define RAM_P1_Y      0xFF811Cu
#define RAM_P2_Y      0xFF811Eu
#define RAM_MAP_W     0xFF0C00u
#define RAM_MAP_H     0xFF0C02u

static void move_players(uint32_t p1, uint32_t p2, int d)
{
    glue_poke16(p1, (uint16_t)(glue_peek16(p1) + d));
    if (glue_peek8(0xFF8284u) != 1)                     /* 2-player game */
        glue_poke16(p2, (uint16_t)(glue_peek16(p2) + d));
}

static void nudge_camera(int h)
{
    static const uint8_t PARALLAX[] = { 0x05, 0x1C, 0x21, 0x22, 0x32 };
    uint8_t area = glue_peek8(RAM_AREA);
    if (!glue_peek8(RAM_PLAYING) || glue_peek8(0xFF829Du) || glue_peek8(0xFF829Eu)) return;
    for (size_t i = 0; i < sizeof PARALLAX; i++)
        if (PARALLAX[i] == area) return;
    int W = glue_peek16(RAM_MAP_W) * 16, H = glue_peek16(RAM_MAP_H) * 16, E = view_extra();
    if (W <= 0 || H <= 0 || W > 8192 || H > 8192) return;            /* loading */

    if (W >= 320 + 2 * E) {
        int cam = glue_peek16(RAM_CAM_X);
        if (cam >= W - 16) cam -= W;                    /* -16..-1 are stored as W-16..W-1 */
        int lo = E - 16, hi = W - (E + 336), d = 0;
        if (cam < lo) d = lo - cam < 4 ? lo - cam : 4;  /* camera right */
        else if (cam > hi) d = -(cam - hi < 4 ? cam - hi : 4);
        if (d) {
            int nc = cam + d;
            glue_poke16(RAM_CAM_X, (uint16_t)(nc < 0 ? nc + W : nc));
            glue_poke16(RAM_RING_X, (uint16_t)((glue_peek16(RAM_RING_X) - d) & 0x3FF));
            move_players(RAM_P1_X, RAM_P2_X, -d);
        }
    }
    {
        int cam = glue_peek16(RAM_CAM_Y);
        if (cam >= H - 16) cam -= H;
        int lo = 8 - 16, hi = H - 210 - (h - 8 - 192), d = 0;
        if (hi < lo) return;                            /* map shorter than the view */
        if (cam < lo) d = lo - cam < 4 ? lo - cam : 4;  /* camera down */
        else if (cam > hi) d = -(cam - hi < 4 ? cam - hi : 4);
        if (d) {
            int nc = cam + d;
            glue_poke16(RAM_CAM_Y, (uint16_t)(nc < 0 ? nc + H : nc));
            glue_poke16(RAM_RING_Y, (uint16_t)((glue_peek16(RAM_RING_Y) + d) & 0x1FF));
            move_players(RAM_P1_Y, RAM_P2_Y, -d);
            int ph = glue_peek8(0xFF8080u) + d;          /* water phase, mod $90 */
            glue_poke8(0xFF8080u, (uint8_t)((ph % 0x90 + 0x90) % 0x90));
        }
    }
}

static void tall_frame(int gameplay, int narrow)
{
    PresentView view = tall_view();
    int h = view.h;
    if (!h) return;
    static PresentView logged;
    if (view.w != logged.w || view.h != logged.h || view.scale != logged.scale) {
        fprintf(stderr, "[arcus-video] view %dx%d at %dx\n", view.w, view.h, view.scale);
        logged = view;
    }
    gvdp_set_tall(h, 8);   /* <= 16 - scroll speed: the up drawer runs a frame late */
    /* The --tall ROM's camera stops and player limits (patch/tall.asm) read the view size
     * from RAM; the game clears RAM at boot/reset, so write it before every frame. */
    glue_poke16(0xFFFA0Au, (uint16_t)view_extra());
    /* (patch/tall.asm ignores them in camera-locked arenas and 4:3, by itself: the area can
     * change mid-frame, before this hook sees it.) */
    glue_poke16(0xFFFA0Cu, 8);                          /* rows above raster line 0 */
    glue_poke16(0xFFFA0Eu, (uint16_t)(h - 8 - 224));    /* HUD moved down */
    glue_poke16(0xFFFA10u, (uint16_t)(h - 8 - 192));    /* map visible below the playfield */
    /* The ROM's map drawers write planes A ($C000) and B ($E000) into the tall VRAM; the
     * VDP shows a plane tall only while its name table was last written there (static
     * backdrops such as area 3's water or the final arena's planet stay normal planes).
     * When plane A points at plane B's table (area 4) it follows plane B. Camera-locked
     * arenas and narrowed (4:3) areas keep the original vertical framing (letterboxed). */
    unsigned mask = 0;
    V.view_top = 0;
    V.view_rows = 192;
    if (gameplay) {
        const GVDP *v = &g_machine.vdp;
        mask = 3u;
        if (((v->reg[2] & 0x38u) << 10) == 0xE000u && ((v->reg[4] & 7u) << 13) == 0xE000u)
            mask |= 4u;
        if (narrow || locked_area()) mask |= 8u;
        else {
            nudge_camera(h);
            V.view_top = 8;
            V.view_rows = h - 32;                       /* all but the HUD band */
        }
    }
    gvdp_set_tall_planes(mask);
}

static void pre_raster(void)
{
    V.frames++;
    V.view_top = 0;
    V.view_rows = 192;
    if (V.native_view) { V.frame_extra = 0; return; }
    const GVDP *vdp = &g_machine.vdp;
    int narrow = !g_arcus_settings.widescreen || auto_43_area();
    /* The ROM widens 2P bounds and camera stops for the 424 view; in 4:3 it
     * must behave like the 4:3 ROM. The game clears RAM at boot/reset, so the
     * flag is rewritten before every frame. */
    if (narrow) glue_poke8(RAM_FORCE_43, 1);
    int extra = (!narrow && arcus_gameplay_view(vdp)) ? view_extra() : 0;
    gvdp_set_ws_extra(extra);
    gvdp_set_ws_canvas(arcus_video_canvas_width());
    gvdp_set_ws_bar_black(1);
    V.frame_extra = extra;
    tall_frame(arcus_gameplay_view(vdp), narrow);
    arcus_minimap_frame();
    if (V.ws_check && !extra && glue_peek8(RAM_PLAYING))
        fprintf(stderr, "[arcus-ws] frame=%u narrow reg12=%02X\n", V.frames, vdp->reg[0x0C]);
    if (V.ws_check && extra) {
        int cols, cells = ws_empty_cells(vdp, ARCUS_VIEW_EXTRA, &cols);
        fprintf(stderr, "[arcus-ws] frame=%u empty=%d sprx9=%d cols=%d\n", V.frames, cells,
                sprites_x_bit9(vdp), cols);
    }
}

/* ---- GameVideo: HUD side strips ------------------------------------------- */
static int gv_configure(const char *mode) { (void)mode; return 1; }
static int gv_enabled(void) { return 0; }   /* never the custom renderer */
static int gv_width(int dw, int dh, int nw, int nh) { (void)dw; (void)dh; (void)nw; (void)nh; return 0; }
static void gv_scanline(const GVDP *vdp, int line, const uint32_t *native, int nw,
                        uint32_t *out, int width)
{ (void)vdp; (void)line; (void)native; (void)nw; (void)out; (void)width; }


const GameVideo g_arcus_game_video = {
    gv_configure, gv_enabled, gv_width, gv_scanline, NULL,
};

/* ---- presenter ------------------------------------------------------------ */
extern int (*g_runner_screenshot_hook)(const char *path);

static int screenshot_presented(const char *path)
{
    snprintf(V.shot_path, sizeof V.shot_path, "%s", path);
    return 1;
}

static void parse_dump_env(void)
{
    if (getenv("ARCUS_SCREENSHOT_PRESENTED") && getenv("ARCUS_SCREENSHOT_PRESENTED")[0] == '1')
        g_runner_screenshot_hook = screenshot_presented;
    V.dump_path = getenv("ARCUS_PRESENT_DUMP");
    if (V.dump_path && !*V.dump_path) V.dump_path = NULL;
    const char *at = getenv("ARCUS_PRESENT_DUMP_AT");
    for (const char *p = at; p && *p && V.dump_n < 32;) {
        V.dump_at[V.dump_n++] = (unsigned)strtoul(p, NULL, 10);
        p = strchr(p, ',');
        if (p) p++;
    }
}

static const char *dump_path_now(char *buf, size_t n)
{
    if (V.shot_path[0]) {
        snprintf(buf, n, "%s", V.shot_path);
        V.shot_path[0] = 0;
        return buf;
    }
    if (!V.dump_path) return NULL;
    int hit = V.dump_n == 0;
    for (int i = 0; i < V.dump_n; i++)
        if (V.dump_at[i] == V.presents) hit = 1;
    if (!hit) return NULL;
    if (strstr(V.dump_path, "%d")) snprintf(buf, n, V.dump_path, (int)V.presents);
    else snprintf(buf, n, "%s", V.dump_path);
    return buf;
}

static void configure_window(int *w, int *h, uint32_t *flags)
{
    V.native_view = getenv("ARCUS_VIEW") && !strcmp(getenv("ARCUS_VIEW"), "native");
    V.ws_check = getenv("ARCUS_WS_CHECK") && getenv("ARCUS_WS_CHECK")[0] == '1';
    parse_dump_env();
    const char *rz = getenv("ARCUS_RESIZE_AT");
    if (rz && sscanf(rz, "%u:%dx%d", &V.resize_at, &V.resize_w, &V.resize_h) != 3) V.resize_at = 0;

    const char *drv = SDL_GetCurrentVideoDriver();
    const char *force = getenv("ARCUS_PRESENTER");
    V.want_gl = !(force && !strcmp(force, "sdl")) &&
                !(drv && (!strcmp(drv, "dummy") || !strcmp(drv, "offscreen") ||
                          !strcmp(drv, "evdev")));
#ifdef __SWITCH__
    V.gles = 1;
#else
    V.gles = getenv("ARCUS_GLES") && getenv("ARCUS_GLES")[0] == '1';
#endif
    if (V.want_gl) {
        *flags |= SDL_WINDOW_OPENGL;
#ifdef __APPLE__
        /* Retina: present at native pixels (sharper integer scale). Not with
         * ARCUS_WINDOW, whose size tests read as the drawable size. */
        if (!getenv("ARCUS_WINDOW")) *flags |= SDL_WINDOW_ALLOW_HIGHDPI;
#endif
        if (V.gles) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        /* SDL_RENDER_DRIVER in the environment still wins (default priority). */
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, V.gles ? "opengles2" : "opengl");
    }
    *flags |= SDL_WINDOW_RESIZABLE;   /* Switch: SDL resizes 1280x720 <-> 1920x1080 */
#ifdef __SWITCH__
    *w = 1280;
    *h = 720;
#else
    int cw = V.native_view ? 320 : arcus_video_canvas_width();
    int th = V.native_view ? 0 : tall_height();      /* full-screen view: 240 or 270 lines */
    *w = cw * 3;
    *h = (th ? th : 224) * 3;
    const char *ws = getenv("ARCUS_WINDOW");
    int ew, eh;
    if (ws && sscanf(ws, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) { *w = ew; *h = eh; }
    if (getenv("ARCUS_HIDDEN_WINDOW") && getenv("ARCUS_HIDDEN_WINDOW")[0] == '1') {
        *flags &= ~(uint32_t)SDL_WINDOW_SHOWN;
        *flags |= SDL_WINDOW_HIDDEN;
    }
#endif
}

static void init(SDL_Window *window, SDL_Renderer *renderer)
{
    V.window = window;
    V.renderer = renderer;
#if !defined(__SWITCH__)
    /* The smallest full-screen view at 1x (not with ARCUS_WINDOW: tests size it exactly). */
    if (!getenv("ARCUS_WINDOW")) SDL_SetWindowMinimumSize(window, 320, 240);
#endif
    SDL_RendererInfo ri;
    memset(&ri, 0, sizeof ri);
    SDL_GetRendererInfo(renderer, &ri);
    const char *name = ri.name ? ri.name : "?";
    int gl_renderer = !strcmp(name, "opengl") || !strcmp(name, "opengles2");
    if (V.want_gl && gl_renderer) {
        SDL_RenderFlush(renderer);
        V.gles = !strcmp(name, "opengles2");
        V.gl = arcus_gl_init(window, V.gles);
    }
    fprintf(stderr, "[arcus-video] presenter: %s (SDL renderer %s)%s\n",
            V.gl ? "GL + CRT shader" : "SDL_Renderer + CPU scanlines", name,
            V.native_view ? ", native VDP view" : "");
}

/* SDL_Renderer fallback: integer scale; CRT presets become per-row brightness
 * (the beam profile without neighbour mixing or mask). */
static void fallback_draw(const uint32_t *buf, int w, int h, const ArcusCrtParams *crt,
                          const char *dump)
{
    SDL_Renderer *r = V.renderer;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(r, &ow, &oh);
    PresentRect fit = present_integer_fit(ow, oh, w, h);
    int k = crt->enabled ? fit.scale : 1;
    if (k > 8) k = 8;
    int th = h * k;
    if (!V.fb_tex || V.fb_tex_w < w || V.fb_tex_h < th) {
        if (V.fb_tex) SDL_DestroyTexture(V.fb_tex);
        V.fb_tex_w = BUF_STRIDE;
        V.fb_tex_h = th > 224 * 8 ? th : 224 * 8;
        V.fb_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                     V.fb_tex_w, V.fb_tex_h);
        if (!V.fb_tex) return;
        SDL_SetTextureScaleMode(V.fb_tex, SDL_ScaleModeNearest);
    }
    const uint32_t *src = buf;
    int pitch_px = BUF_STRIDE;
    if (k > 1) {
        size_t need = (size_t)BUF_STRIDE * th;
        if (V.fb_rows_cap < need) {
            free(V.fb_rows);
            V.fb_rows = (uint32_t *)malloc(need * sizeof(uint32_t));
            V.fb_rows_cap = V.fb_rows ? need : 0;
            if (!V.fb_rows) return;
        }
        double mean = present_beam_mean(k, crt->beam_k, exp);
        unsigned mul[8];
        for (int rr = 0; rr < k; rr++) {
            double dy = (rr + 0.5) / k - 0.5, wsum = 0.0;
            for (int n = -1; n <= 1; n++) wsum += exp(-crt->beam_k * (dy - n) * (dy - n));
            mul[rr] = (unsigned)(sqrt(wsum / mean) * 256.0 + 0.5);   /* gamma ~2 */
        }
        for (int y = 0; y < h; y++) {
            const uint32_t *s = buf + (size_t)y * BUF_STRIDE;
            for (int rr = 0; rr < k; rr++) {
                uint32_t *d = V.fb_rows + (size_t)(y * k + rr) * BUF_STRIDE;
                unsigned m = mul[rr];
                for (int x = 0; x < w; x++) {
                    uint32_t c = s[x];
                    unsigned rC = ((c >> 16 & 0xFF) * m) >> 8, gC = ((c >> 8 & 0xFF) * m) >> 8,
                             bC = ((c & 0xFF) * m) >> 8;
                    if (rC > 255) rC = 255;
                    if (gC > 255) gC = 255;
                    if (bC > 255) bC = 255;
                    d[x] = 0xFF000000u | rC << 16 | gC << 8 | bC;
                }
            }
        }
        src = V.fb_rows;
    }
    SDL_Rect up = { 0, 0, w, th };
    SDL_UpdateTexture(V.fb_tex, &up, src, pitch_px * (int)sizeof(uint32_t));
    SDL_RenderSetLogicalSize(r, 0, 0);
    SDL_RenderSetScale(r, 1.f, 1.f);
    SDL_RenderSetViewport(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_Rect dst = { fit.x, fit.y, fit.w, fit.h };
    SDL_RenderCopy(r, V.fb_tex, &up, &dst);
    if (dump) {
        uint32_t *px = (uint32_t *)malloc((size_t)ow * oh * 4);
        if (px && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, px, ow * 4) == 0 &&
            png_write_argb(dump, px, ow, oh, ow) == 0)
            fprintf(stderr, "[arcus-video] presented frame %dx%d -> %s\n", ow, oh, dump);
        free(px);
    }
}

/* Mac: the window follows the WIDESCREEN setting. The view's shape comes from the window's
 * (whatever fits it), so a window opened in 4:3 would keep a 4:3 view after 16:9 is chosen.
 * On a change, keep the height and centre, and set the width to 16:9 or to the 4:3 view's
 * (320 x the view height). Not in full screen or a maximised window: there the bars are right. */
static void follow_widescreen_setting(void)
{
#if !defined(__SWITCH__) && !defined(GENESIS_SWITCH_STUBS)
    static int last = -1;
    int ws = g_arcus_settings.widescreen;
    if (ws == last) return;
    int first = last < 0;
    last = ws;
    if (first || !V.window || V.native_view) return;
    if (SDL_GetWindowFlags(V.window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) return;
    int ww, wh, wx, wy, th = tall_height();
    SDL_GetWindowSize(V.window, &ww, &wh);
    SDL_GetWindowPosition(V.window, &wx, &wy);
    int nw = ws ? (wh * 16 + 4) / 9 : (wh * ARCUS_NATIVE_W + (th ? th : 224) / 2) / (th ? th : 224);
    if (nw == ww || wh <= 0) return;
    SDL_SetWindowSize(V.window, nw, wh);
    SDL_SetWindowPosition(V.window, wx - (nw - ww) / 2, wy);
    fprintf(stderr, "[arcus-video] widescreen %s: window %dx%d -> %dx%d\n", ws ? "on" : "off", ww, wh, nw, wh);
#endif
}

static void present_buffer(const uint32_t *buf, int w, int h)
{
    V.presents++;
    follow_widescreen_setting();
    if (V.resize_at && V.presents == V.resize_at) {
        fprintf(stderr, "[arcus-video] test resize to %dx%d\n", V.resize_w, V.resize_h);
        SDL_SetWindowSize(V.window, V.resize_w, V.resize_h);
    }
    ArcusCrtParams crt = crt_params(g_arcus_settings.crt_preset);
    char pathbuf[600];
    const char *dump = dump_path_now(pathbuf, sizeof pathbuf);
    if (V.gl) {
        SDL_RenderFlush(V.renderer);
        if (arcus_gl_draw(buf, w, h, BUF_STRIDE, &crt, dump)) return;
    }
    fallback_draw(buf, w, h, &crt, dump);
}

void arcus_video_toast(const char *text)
{
    snprintf(V.toast, sizeof V.toast, "%s", text);
    V.toast_frames = 90;
}

/* Title menu (and its Music Mode page): a hint for the config menu. RAM signature measured
 * on the native ROM: $FF800D = 20 and music track $FF8082 = $FF (silence) on the title,
 * not playing a level, not the attract demo ($FF8089, which shares the other values). */
static int on_title_menu(void);
int arcus_video_on_title(void) { return on_title_menu(); }
int arcus_video_play_fade(void) { return play_fade256(NULL, 0, 0); }

static int on_title_menu(void)
{
    return glue_peek8(0xFF800Du) == 20 && glue_peek8(0xFF8082u) == 0xFF &&
           glue_peek8(0xFF8089u) == 0 && glue_peek8(RAM_PLAYING) == 0;
}

/* The title screen for the DX title: the same signature, H32, without the level flag
 * ($FF8292 stays set after the first game, so the title after a game over has it). */
static int dx_title_screen(void)
{
    return gvdp_screen_width(&g_machine.vdp) == 256 && glue_peek8(0xFF800Du) == 20 &&
           glue_peek8(0xFF8082u) == 0xFF && glue_peek8(0xFF8089u) == 0;
}

/* Small 5x8 font (game/small_font.h, generated by tools/gen_small_font.py): the title hint
 * and the save/load grid captions; the engine's overlay font is 8x8 upper case. */
#include "small_font.h"

int arcus_small_text_width(const char *t) { return (int)strlen(t) * 6 - 1; }

void arcus_small_text(uint32_t *buf, int stride, int h, int x, int y, const char *t, uint32_t col)
{
    for (; *t; t++, x += 6) {
        int c = (unsigned char)*t;
        if (c < 32 || c > 126) continue;
        for (int r = 0; r < 8; r++) {
            if (y + r < 0 || y + r >= h) continue;
            for (int k = 0; k < 5; k++)
                if (x + k >= 0 && x + k < stride && (small_font[c - 32][r] & (0x10 >> k)))
                    buf[(size_t)(y + r) * stride + x + k] = col;
        }
    }
}


/* The stick display, for showing how a capture was played (ARCUS_SHOW_STICKS=1, never in
 * the menu): player 1's left and right sticks above the HUD's left and right ends (over the
 * magic info line), each a white circle outline, 50% dark inside so it reads on any ground,
 * with a filled white dot where the stick points. */
static int sticks_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("ARCUS_SHOW_STICKS"); on = e && e[0] == '1'; }
    return on;
}

#define STICK_R   22.0f      /* ring radius (canvas px) */
#define STICK_DOT 7.0f       /* dot radius */

static void draw_stick(uint32_t *buf, int w, int h, int stride, int cx, int cy, float sx, float sy)
{
    const float R = STICK_R, r = STICK_DOT;
    float m = sqrtf(sx * sx + sy * sy);
    if (m > 1.0f) { sx /= m; sy /= m; }
    float dx = cx + sx * (R - r - 2.5f), dy = cy - sy * (R - r - 2.5f);
    for (int y = cy - (int)R - 2; y <= cy + (int)R + 2; y++)
        for (int x = cx - (int)R - 2; x <= cx + (int)R + 2; x++) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            float ox = x + 0.5f - cx, oy = y + 0.5f - cy, ex = x + 0.5f - dx, ey = y + 0.5f - dy;
            float d = sqrtf(ox * ox + oy * oy);
            uint32_t *p = buf + (size_t)y * stride + x;
            if (fabsf(d - R) < 1.0f || ex * ex + ey * ey <= r * r) *p = 0xFFFFFFFFu;
            else if (d < R) *p = 0xFF000000u | ((*p >> 1) & 0x7F7F7Fu);
        }
}

static void draw_sticks(uint32_t *buf, int w, int h, int stride)
{
    float v[4];
    if (!arcus_input_sticks(0, v)) return;
    int rx, ry, R = (int)STICK_R;
    arcus_video_raster_origin(w, h, &rx, &ry);
    int hud = arcus_video_hud_top(h);
    /* above the magic info line (one small-font line, 10 px, right over the HUD) */
    int cy = hud > 0 ? hud - 10 - R - 3 : h - R - 4;
    draw_stick(buf, w, h, stride, rx + R + 2, cy, v[0], v[1]);                    /* HUD's left end */
    draw_stick(buf, w, h, stride, rx + ARCUS_NATIVE_W - R - 3, cy, v[2], v[3]);   /* right end */
}

static void draw_toast(uint32_t *buf, int w, int h, int stride)
{
    (void)w;
    int tw = arcus_small_text_width(V.toast) + 8, th = 12;
    for (int y = 6; y < 6 + th && y < h; y++)                 /* black at 50% */
        for (int x = 6; x < 6 + tw && x < stride; x++) {
            uint32_t *p = buf + (size_t)y * stride + x;
            *p = 0xFF000000u | ((*p >> 1) & 0x7F7F7Fu);
        }
    arcus_small_text(buf, stride, h, 10, 8, V.toast, 0xFFF0E8C8u);
}

/* The config-menu hint under the title menu, in the small font on a 50% black strip:
 * centred, midway between the bottom of "SEGA ENTERPRISES LTD." (picture row 215 of the
 * 256 x 224 title, centred in the canvas) and the bottom of the screen. Shown once the
 * title picture is up (not on the black frames before it). The desktop build names its
 * keyboard key (a pad's − works too). */
#if defined(__SWITCH__) || defined(GENESIS_SWITCH_STUBS)
#define TITLE_HINT "Press - for config menu"
#else
#define TITLE_HINT "Press ESC for config menu"
#endif
#define TITLE_TEXT_BOTTOM 215
static int title_hint_visible(const uint32_t *buf, int w, int h, int stride)
{
    static int off = -1;                    /* ARCUS_NO_TITLE_HINT: trailer captures (TRAILER.md) */
    if (off < 0) off = getenv("ARCUS_NO_TITLE_HINT") != NULL;
    if (off || !on_title_menu() || gvdp_screen_width(&g_machine.vdp) != 256 || w < 256 || h < 224) return 0;
    int ox = (w - 256) / 2, oy = (h - 224) / 2;
    for (int y = oy + 8; y < oy + 224; y += 16)
        for (int x = ox + 8; x < ox + 256; x += 16)
            if (buf[(size_t)y * stride + x] & 0xFFFFFFu) return 1;
    return 0;
}

static void draw_title_hint(uint32_t *buf, int w, int h, int stride)
{
    int top = (h - 224) / 2 + TITLE_TEXT_BOTTOM + 1;            /* first free row */
    int ty = (top + h) / 2 - 4, tw = arcus_small_text_width(TITLE_HINT), tx = (w - tw) / 2;
    if (ty < top + 1) ty = top + 1;
    if (ty + 8 > h) ty = h - 8;
    for (int y = ty - 1; y < ty + 9 && y < h; y++)                 /* black at 50% */
        for (int x = tx - 3; x < tx + tw + 3 && x < w; x++) {
            if (y < top || x < 0) continue;
            uint32_t *p = buf + (size_t)y * stride + x;
            *p = 0xFF000000u | ((*p >> 1) & 0x7F7F7Fu);
        }
    arcus_small_text(buf, stride, h, tx, ty, TITLE_HINT, 0xFFF0E8C8u);
}

/* Frame timing, logged once a minute (log.txt on the Switch; stderr is buffered there and
 * flushed at pauses). Per presented frame:
 *   work    = runner iteration start -> draw start: game + VDP + audio, no vsync wait
 *   present = draw: colour pass + upload + shader draw (the swap/vsync wait follows draw)
 *   late    = frames whose start-to-start interval exceeded 18 ms (a missed refresh)
 * Intervals touching a paused iteration (overlay, rewind badge) are not counted. */
#define PERF_WINDOW 3600
static struct {
    Uint64 freq, iter_start, last_draw_start;
    int paused_since_draw;
    float work[PERF_WINDOW], present[PERF_WINDOW];
    int n, late;
} T;

static double ms(Uint64 a, Uint64 b) { return (double)(b - a) * 1000.0 / (double)T.freq; }

void arcus_video_frame_begin(void)
{
    if (!T.freq) T.freq = SDL_GetPerformanceFrequency();
    T.iter_start = SDL_GetPerformanceCounter();
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void perf_report(void)
{
    float w[PERF_WINDOW], p[PERF_WINDOW];
    memcpy(w, T.work, sizeof(float) * T.n);
    memcpy(p, T.present, sizeof(float) * T.n);
    qsort(w, T.n, sizeof(float), cmp_float);
    qsort(p, T.n, sizeof(float), cmp_float);
    int i50 = T.n / 2, i99 = T.n * 99 / 100;
    fprintf(stderr, "[perf] %d frames: work p50 %.2f p99 %.2f max %.2f ms | present p50 %.2f p99 %.2f "
            "max %.2f ms | late %d (crt %d, lut %d, %dx%d)\n", T.n, w[i50], w[i99], w[T.n - 1],
            p[i50], p[i99], p[T.n - 1], T.late, g_arcus_settings.crt_preset,
            g_arcus_settings.color_lut, V.last_w, V.last_h);
    T.n = 0;
    T.late = 0;
}

static void perf_account(Uint64 start, Uint64 end)
{
    if (!T.freq || !T.iter_start) return;
    int clean = !T.paused_since_draw && T.last_draw_start;
    if (clean) {
        if (ms(T.last_draw_start, start) > 18.0) T.late++;
        T.work[T.n] = (float)ms(T.iter_start, start);
        T.present[T.n] = (float)ms(start, end);
        if (++T.n == PERF_WINDOW) perf_report();
    }
    T.paused_since_draw = 0;
    T.last_draw_start = start;
}

static int draw(const uint32_t *argb, int width, int height, int stride)
{
    Uint64 t_start = SDL_GetPerformanceCounter();
    if (width > BUF_STRIDE) width = BUF_STRIDE;
    if (height > BUF_LINES) height = BUF_LINES;
    if (g_arcus_settings.color_lut && !V.lut_built) {
        color_lut_build(&V.lut, SCREEN_CRT, -1.0);
        /* The engine's CRT model adds a black floor ("a CRT never reaches true black"),
         * which lifts every colour; on an OLED panel black then isn't black. Remove it:
         * rescale each channel so the table's black is 0 and its top stays put. */
        {
            uint32_t k = V.lut.table[0], top = V.lut.table[511];
            int bl[3] = { (int)(k >> 16) & 0xFF, (int)(k >> 8) & 0xFF, (int)k & 0xFF };
            int tp[3] = { (int)(top >> 16) & 0xFF, (int)(top >> 8) & 0xFF, (int)top & 0xFF };
            for (int i = 0; i < 512; i++) {
                uint32_t c = V.lut.table[i], o = 0xFF000000u;
                for (int ch = 0; ch < 3; ch++) {
                    int sh = 16 - ch * 8, v = (int)(c >> sh) & 0xFF;
                    int span = tp[ch] - bl[ch] > 0 ? tp[ch] - bl[ch] : 1;
                    int n = (v - bl[ch]) * tp[ch] / span;
                    o |= (uint32_t)(n < 0 ? 0 : n > 255 ? 255 : n) << sh;
                }
                V.lut.table[i] = o;
            }
        }
        /* Per-channel nearest DAC level, once: the engine's per-pixel search
         * (3 x 8 compares per pixel) cost several ms a frame on the Switch. */
        for (int c = 0; c < 256; c++) V.lut_level[c] = (uint8_t)genesis_dac_nearest_normal_idx((uint32_t)c);
        V.lut_built = 1;
    }
    if (arcus_title_dx_compose(dx_title_screen(), argb, width, height, stride, V.dx, BUF_STRIDE)) {
        argb = V.dx;
        stride = BUF_STRIDE;
    }
    for (int y = 0; y < height; y++) {
        const uint32_t *s = argb + (size_t)y * stride;
        uint32_t *d = V.last + (size_t)y * BUF_STRIDE;
        if (g_arcus_settings.color_lut) {
            const uint8_t *L = V.lut_level;
            for (int x = 0; x < width; x++) {
                uint32_t c = s[x];
                d[x] = V.lut.table[(L[(c >> 16) & 0xFF] | (L[(c >> 8) & 0xFF] << 3) |
                                    (L[c & 0xFF] << 6)) & 0x1FF];
            }
        } else {
            memcpy(d, s, (size_t)width * sizeof(uint32_t));
        }
    }
    V.last_w = width;
    V.last_h = height;
    int minimap = arcus_minimap_visible();
    int magic = arcus_magic_visible();
    int fade = play_fade256(V.last, width, height);         /* every frame: keeps the reference */
    int hint = title_hint_visible(V.last, width, height, BUF_STRIDE);
    int sticks = sticks_on();
    if (V.toast_frames > 0 || minimap || magic || hint || sticks) {
        memcpy(V.tmp, V.last, sizeof V.tmp);
        if (minimap) arcus_minimap_draw(V.tmp, width, height, BUF_STRIDE);
        if (magic) arcus_magic_draw(V.tmp, width, height, BUF_STRIDE);
        if (minimap || magic) fade_overlays(V.tmp, V.last, width, height, fade);
        if (hint) draw_title_hint(V.tmp, width, height, BUF_STRIDE);
        if (sticks) draw_sticks(V.tmp, width, height, BUF_STRIDE);
        if (V.toast_frames > 0) {
            V.toast_frames--;
            draw_toast(V.tmp, width, height, BUF_STRIDE);
        }
        present_buffer(V.tmp, width, height);
    } else {
        present_buffer(V.last, width, height);
    }
    perf_account(t_start, SDL_GetPerformanceCounter());
    return 1;
}

/* The play overlays (mini map, MAGIC INFO, quick magic) fade with the game. Area changes fade
 * the picture through the palette ($77E2: each step scales the ROM palettes by d7 / 7 into
 * $B000 and copies them to CRAM); the overlays are drawn in fixed colours, so they would stay
 * bright over a black screen. Palette lines 2-3 come from one table in every area ($7BAE),
 * so the fade is CRAM lines 2-3 now over that table: the median, per colour channel, of
 * current / table (a channel the game animates past the table is left out). The picture's
 * content (a shorter health bar) doesn't change it. 256 = unfaded.
 * While the next area loads, the game has the display off (VDP register 1 bit 6) with the
 * palette already back at full: the screen is black, so the fade is 0 then. */
#define ROM_PAL_FIXED 0x007BAEu
#define VDP_DISPLAY_ON 0x40u
static int play_fade256(const uint32_t *frame, int w, int h)
{
    (void)frame; (void)w; (void)h;
    const GVDP *v = &g_machine.vdp;
    int r[96], n = 0;
    for (int i = 0; i < 32; i++) {
        uint16_t ref = glue_peek16(ROM_PAL_FIXED + 2u * (uint32_t)i) & 0x0EEE, cur = v->cram[32 + i] & 0x0EEE;
        for (int sh = 1; sh <= 9; sh += 4) {
            int a = (ref >> sh) & 7, b = (cur >> sh) & 7;
            if (a && b <= a) r[n++] = b * 256 / a;
        }
    }
    if (n < 8) return 256;                    /* not the game's palette (title, intro): no fade */
    if (!(v->reg[1] & VDP_DISPLAY_ON)) return 0;
    for (int i = 1; i < n; i++)               /* insertion sort: at most 96 */
        for (int j = i; j > 0 && r[j - 1] > r[j]; j--) { int t = r[j]; r[j] = r[j - 1]; r[j - 1] = t; }
    return r[n / 2];
}

/* tmp = frame + (tmp - frame) * fade: overlay pixels move toward the game picture. */
static void fade_overlays(uint32_t *tmp, const uint32_t *frame, int w, int h, int f)
{
    if (f >= 256) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * BUF_STRIDE + x;
            uint32_t a = frame[i], b = tmp[i];
            if (a == b) continue;
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
                out |= (uint32_t)(ca + (cb - ca) * f / 256) << sh;
            }
            tmp[i] = out;
        }
}

/* Thumbnail of the last presented game frame: box-filtered down to tw x th (aspect kept,
 * centred on black). Called while paused, so V.last is the frame the player sees. */
void arcus_video_thumbnail(uint32_t *out, int tw, int th)
{
    int w = V.last_w ? V.last_w : 0, h = V.last_h ? V.last_h : 0;
    for (int i = 0; i < tw * th; i++) out[i] = 0xFF000000u;
    if (w <= 0 || h <= 0) return;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            int x0 = x * w / tw, x1 = (x + 1) * w / tw, y0 = y * h / th, y1 = (y + 1) * h / th;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t c = V.last[(size_t)yy * BUF_STRIDE + xx];
                    r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF; n++;
                }
            if (n) out[y * tw + x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
}

void arcus_video_present_paused(void (*draw_overlay)(uint32_t *buf, int w, int h, int stride))
{
    T.paused_since_draw = 1;
    int w = V.last_w ? V.last_w : arcus_video_canvas_width(), h = V.last_h ? V.last_h : 224;
    memcpy(V.tmp, V.last, sizeof V.tmp);
    /* The host layers of draw() (V.last has neither), so they don't flash while L3 rewinds
     * (2 of every 3 iterations are paused presents) and stay put under the menu. */
    if (arcus_minimap_visible()) arcus_minimap_draw(V.tmp, w, h, BUF_STRIDE);
    if (arcus_magic_visible()) arcus_magic_draw(V.tmp, w, h, BUF_STRIDE);
    fade_overlays(V.tmp, V.last, w, h, play_fade256(V.last, w, h));
    if (title_hint_visible(V.last, w, h, BUF_STRIDE)) draw_title_hint(V.tmp, w, h, BUF_STRIDE);
    if (draw_overlay) draw_overlay(V.tmp, w, h, BUF_STRIDE);
    present_buffer(V.tmp, w, h);
    SDL_RenderPresent(V.renderer);
}

void arcus_video_present_custom(void (*draw)(SDL_Renderer *r, int w, int h))
{
    T.paused_since_draw = 1;
    V.presents++;
    SDL_Renderer *r = V.renderer;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(r, &ow, &oh);
    SDL_RenderSetLogicalSize(r, 0, 0);
    SDL_RenderSetScale(r, 1.f, 1.f);
    SDL_RenderSetViewport(r, NULL);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    draw(r, ow, oh);
    char pathbuf[600];
    const char *dump = dump_path_now(pathbuf, sizeof pathbuf);
    if (dump) {
        uint32_t *px = (uint32_t *)malloc((size_t)ow * oh * 4);
        if (px && SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, px, ow * 4) == 0 &&
            png_write_argb(dump, px, ow, oh, ow) == 0)
            fprintf(stderr, "[arcus-video] presented frame %dx%d -> %s\n", ow, oh, dump);
        free(px);
    }
    SDL_RenderPresent(r);
}

int arcus_options_pause_frame(int *running);   /* arcus_options.c */

const HostPresenter g_arcus_presenter = {
    configure_window, init, pre_raster, arcus_options_pause_frame, draw,
    1,   /* owns_hotkeys: − opens our overlay; no runner F-key/chord menus */
};
