/*
 * arcus_minimap.c — the mini map: the current area's walkable shape, revealed as the view
 * covers it, with the view box and player dots. R3 (right stick click) shows/hides it.
 *
 * Built live from RAM, the way the game builds its collision buffer ($9378):
 *   - the area map: $FF0C00 width/height in metatiles, then 16x16-metatile chunks. The
 *     collision plane is plane B (chunk table $FF0000, chunk data $FF0C04) or, in areas
 *     $05 $1C $21 $22 $23 $32 $34, plane A (chunk table $FF0100, data $FF0C00 + $800A);
 *   - each metatile's collision at $FF0A00 + 2 * id: four nibbles, one per 8x8 cell
 *     (top-left, top-right, bottom-left, bottom-right);
 *   - a cell value's class (ROM $1142A): 0 and 8 floor, 1 7 13 wall, 6 pit; the rest are
 *     diagonal half-floor edges ($1102A shapes), shown as floor so diagonal corridors stay
 *     joined.
 * The map is one pixel per 8x8 cell when it fits the box, else each pixel covers a block
 * of cells and shows floor if any revealed cell in it is floor. What has been seen is kept
 * per area (bits per metatile) until the title menu next shows (a new game: RESET GAME, the
 * game's own reset, game over; the play-time counter uses the same rule). Loading a state
 * keeps it. ARCUS_MINIMAP_REVEAL_ALL=1 (tests) shows
 * every area as if fully explored.
 */
#include "arcus_minimap.h"
#include "arcus_host.h"
#include "arcus_video.h"
#include "genesis_host_mem.h"
#include "genesis_machine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RAM_AREA     0xFF828Eu
#define RAM_PLAYING  0xFF8292u
#define RAM_DEMO     0xFF8089u
#define RAM_PLAYERS  0xFF8284u
#define RAM_CAM_X    0xFF8100u
#define RAM_CAM_Y    0xFF8102u
#define RAM_P1_X     0xFF8118u
#define RAM_P1_Y     0xFF811Cu
#define RAM_MAP_W    0xFF0C00u
#define RAM_MAP_H    0xFF0C02u
#define RAM_A_DATA   0xFF800Au
#define RAM_COLL     0xFF0A00u

#define AREAS    0x35
#define MAX_MT   (192 * 144)            /* largest area, 32 KB of cells at most */
#define MAX_CW   (2 * 192)
#define MAX_CH   (2 * 144)
#define BOX_W    112                    /* largest box (canvases >= 400 wide; 88 below) */
#define BOX_H    80

static struct {
    uint8_t seen[AREAS][MAX_MT / 8];    /* per area, one bit per metatile */
    int area, W, H;                     /* the area the cell map below is of */
    uint8_t floor[MAX_CH][MAX_CW / 8];  /* one bit per 8x8 cell */
    unsigned rebuild_in;                /* frames until the next rebuild */
    int reveal_all;                     /* -1 = not read yet */
    /* The box as last drawn: 1 = seen floor. Redone only when something changed. */
    uint8_t img[BOX_H][BOX_W];
    int img_den, img_max_w, dirty;
    /* Area changes: the game bumps the area number ($BD6E) before its loader ($6B04) has
     * placed the camera, so for a few frames the new area comes with the old area's
     * camera. Marking then would reveal the old view's spot on the new map. */
    int last_area, last_cx, last_cy;    /* the last valid frame's area and camera */
    int settle_cx, settle_cy, settle;   /* settle > 0: waiting for the camera to move */
    int any_seen;                       /* something is marked (cleared on the title) */
} M = { .area = -1, .reveal_all = -1, .last_area = -1 };

static int plane_a_collision(uint8_t area)
{
    static const uint8_t A[] = { 0x05, 0x1C, 0x21, 0x22, 0x23, 0x32, 0x34 };
    for (size_t i = 0; i < sizeof A; i++)
        if (A[i] == area) return 1;
    return 0;
}

static int floor_value(int v) { return v != 1 && v != 6 && v != 7 && v != 13; }

static void build(uint8_t area, int W, int H)
{
    uint32_t chunks = 0xFF0000u, data = 0xFF0C04u;
    if (plane_a_collision(area)) { chunks = 0xFF0100u; data = 0xFF0C00u + glue_peek16(RAM_A_DATA); }
    memset(M.floor, 0, sizeof M.floor);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            uint8_t idx = glue_peek8(chunks + (uint32_t)((r >> 4) * (W >> 4) + (c >> 4)));
            uint8_t mt = glue_peek8(data + (uint32_t)idx * 256u + (uint32_t)((r & 15) * 16 + (c & 15)));
            uint8_t a0 = glue_peek8(RAM_COLL + mt * 2u), a1 = glue_peek8(RAM_COLL + mt * 2u + 1u);
            int v[4] = { a0 >> 4, a0 & 15, a1 >> 4, a1 & 15 };
            for (int k = 0; k < 4; k++)
                if (floor_value(v[k])) {
                    int x = c * 2 + (k & 1), y = r * 2 + (k >> 1);
                    M.floor[y][x >> 3] |= (uint8_t)(0x80 >> (x & 7));
                }
        }
    M.area = area;
    M.W = W;
    M.H = H;
    M.dirty = 1;
}

/* The area in play and its size, or 0 while loading / outside gameplay. */
static int current(uint8_t *area, int *W, int *H)
{
    if (!arcus_gameplay_view(&g_machine.vdp) || glue_peek8(RAM_DEMO)) return 0;
    *area = glue_peek8(RAM_AREA);
    *W = glue_peek16(RAM_MAP_W);
    *H = glue_peek16(RAM_MAP_H);
    return *area < AREAS && *W >= 16 && *H >= 16 && *W <= 192 && *H <= 144 &&
           (*W & 15) == 0 && (*H & 15) == 0;
}

void arcus_minimap_frame(void)
{
    uint8_t area;
    int W, H;
    if (M.any_seen && arcus_video_on_title()) {           /* a new game starts from here */
        memset(M.seen, 0, sizeof M.seen);
        M.any_seen = 0;
        M.dirty = 1;
    }
    if (!current(&area, &W, &H)) { M.area = -1; return; }
    int cam_x = glue_peek16(RAM_CAM_X), cam_y = glue_peek16(RAM_CAM_Y);
    if (M.last_area >= 0 && area != M.last_area) {
        M.settle_cx = M.last_cx;
        M.settle_cy = M.last_cy;
        M.settle = 120;                 /* at most 2 s, should the camera not move */
    }
    M.last_area = area;
    M.last_cx = cam_x;
    M.last_cy = cam_y;
    if (M.settle) {
        if (cam_x == M.settle_cx && cam_y == M.settle_cy && --M.settle) return;
        M.settle = 0;
        M.rebuild_in = 0;               /* the map is loaded now: rebuild below */
    }
    /* (Re)build on entering an area and every second after (doors and switches change
     * the map; the first frames of an area can still be loading). */
    if (area != M.area || W != M.W || H != M.H || !M.rebuild_in) {
        build(area, W, H);
        M.rebuild_in = 60;
    }
    M.rebuild_in--;

    /* Nothing is seen while the picture is faded or black: an area change moves the camera
     * in steps while the next area loads with the display off ($828E first, then camera Y,
     * then X), and a half-moved camera would mark a view the player never had. */
    if (arcus_video_play_fade() < 256) return;

    /* Reveal the metatiles the view shows, clipped to the map: where the (tall) view reaches
     * past an edge, the plane wraps on screen, but that is not the far side of the map
     * having been seen. Camera -16..-1 is stored as W*16-16..W*16-1 (as for the view box). */
    int x0, y0, vw, vh;
    arcus_video_view_rect(&x0, &y0, &vw, &vh);
    int camx = cam_x, camy = cam_y;
    if (camx >= W * 16 - 16) camx -= W * 16;
    if (camy >= H * 16 - 16) camy -= H * 16;
    int cx = camx + 16 + x0, cy = camy + 16 + y0;
    int c0 = cx < 0 ? 0 : cx >> 4, c1 = (cx + vw - 1) >> 4;
    int r0 = cy < 0 ? 0 : cy >> 4, r1 = (cy + vh - 1) >> 4;
    if (c1 >= W) c1 = W - 1;
    if (r1 >= H) r1 = H - 1;
    uint8_t *seen = M.seen[area];
    if (M.reveal_all < 0) M.reveal_all = getenv("ARCUS_MINIMAP_REVEAL_ALL") != NULL;
    if (M.reveal_all) memset(seen, 0xFF, sizeof M.seen[0]);
    for (int y = r0; y <= r1; y++)
        for (int x = c0; x <= c1; x++) {
            int i = y * W + x;
            uint8_t bit = (uint8_t)(0x80 >> (i & 7));
            if (!(seen[i >> 3] & bit)) { seen[i >> 3] |= bit; M.dirty = 1; M.any_seen = 1; }
        }
}

void arcus_minimap_toggle(void)
{
    g_arcus_settings.minimap = !g_arcus_settings.minimap;
}

int arcus_minimap_visible(void)
{
    uint8_t area;
    int W, H;
    return g_arcus_settings.minimap && current(&area, &W, &H) && area == M.area;
}

static void blend(uint32_t *p, uint32_t col, int alpha256)
{
    uint32_t d = *p;
    int r = (int)((d >> 16) & 0xFF), g = (int)((d >> 8) & 0xFF), b = (int)(d & 0xFF);
    r += (((int)((col >> 16) & 0xFF) - r) * alpha256) >> 8;
    g += (((int)((col >> 8) & 0xFF) - g) * alpha256) >> 8;
    b += (((int)(col & 0xFF) - b) * alpha256) >> 8;
    *p = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

void arcus_minimap_draw(uint32_t *buf, int w, int h, int stride)
{
    const int CW = M.W * 2, CH = M.H * 2;             /* cells */
    const int max_w = w >= 400 ? BOX_W : 88, max_h = BOX_H;
    /* One pixel per cell when it fits, else one per den x den cells. */
    int den = 1;
    while (CW / den > max_w || CH / den > max_h) den++;
    int bw = CW / den, bh = CH / den;
    int bx = w - bw - 6, by = 6;
    if (bx < 0 || by + bh > h) return;

    if (M.dirty || den != M.img_den || max_w != M.img_max_w) {
        const uint8_t *seen = M.seen[M.area];
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                int fl = 0;
                for (int cy = y * den; cy < (y + 1) * den && !fl; cy++)
                    for (int cx = x * den; cx < (x + 1) * den && !fl; cx++) {
                        int i = (cy >> 1) * M.W + (cx >> 1);
                        fl = (seen[i >> 3] & (0x80 >> (i & 7))) && (M.floor[cy][cx >> 3] & (0x80 >> (cx & 7)));
                    }
                M.img[y][x] = (uint8_t)fl;
            }
        M.dirty = 0;
        M.img_den = den;
        M.img_max_w = max_w;
    }
    /* Floor seen: near-opaque light grey; the rest of the area's bounds: the frame at 30%
     * (the playfield is busy and bright; at 50% the map was hard to read). */
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            blend(buf + (size_t)(by + y) * stride + bx + x, M.img[y][x] ? 0xFFD0D8E8u : 0xFF000000u,
                  M.img[y][x] ? 236 : 180);

    /* The view box, clipped to the area (the camera keeps the view inside the map; where
     * the map is smaller than the view, the box is the whole area). Camera -16..-1 is
     * stored as W-16..W-1. */
    int x0, y0, vw, vh;
    arcus_video_view_rect(&x0, &y0, &vw, &vh);
    int MW = M.W * 16, MH = M.H * 16;
    int camx = glue_peek16(RAM_CAM_X), camy = glue_peek16(RAM_CAM_Y);
    if (camx >= MW - 16) camx -= MW;
    if (camy >= MH - 16) camy -= MH;
#define BOX_X(mx) ((mx) / 8 / den)
#define BOX_Y(my) ((my) / 8 / den)
    int vx = camx + 16 + x0, vy = camy + 16 + y0;
    int l = BOX_X(vx < 0 ? 0 : vx), t = BOX_Y(vy < 0 ? 0 : vy);
    int r = BOX_X(vx + vw > MW ? MW : vx + vw) - 1, b = BOX_Y(vy + vh > MH ? MH : vy + vh) - 1;
    if (r >= bw) r = bw - 1;
    if (b >= bh) b = bh - 1;
    for (int x = l; x <= r; x++) {
        blend(buf + (size_t)(by + t) * stride + bx + x, 0xFFFFFFFFu, 220);
        blend(buf + (size_t)(by + b) * stride + bx + x, 0xFFFFFFFFu, 220);
    }
    for (int y = t + 1; y < b; y++) {
        blend(buf + (size_t)(by + y) * stride + bx + l, 0xFFFFFFFFu, 220);
        blend(buf + (size_t)(by + y) * stride + bx + r, 0xFFFFFFFFu, 220);
    }
    static const uint32_t DOT[2] = { 0xFFFFD040u, 0xFF60D0FFu };
    int players = glue_peek8(RAM_PLAYERS) == 2 ? 2 : 1;
    for (int p = 0; p < players; p++) {
        if (!glue_peek8(0xFF8212u + (uint32_t)p)) continue;      /* dead */
        int px = ((camx + glue_peek16(RAM_P1_X + 2u * p) - 112) % MW + MW) % MW;
        int py = ((camy + glue_peek16(RAM_P1_Y + 2u * p) - 112 + 8) % MH + MH) % MH;
        int dx = BOX_X(px), dy = BOX_Y(py);
        for (int k = -1; k <= 1; k++)
            for (int j = -1; j <= 1; j++) {
                int xx = dx + j, yy = dy + k;
                if (xx < 0 || yy < 0 || xx >= bw || yy >= bh || (j && k)) continue;
                buf[(size_t)(by + yy) * stride + bx + xx] = DOT[p];
            }
    }
#undef BOX_X
#undef BOX_Y
}
