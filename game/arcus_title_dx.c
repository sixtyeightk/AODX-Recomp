/*
 * arcus_title_dx.c — "Arcus Odyssey DX": the title screen opens out from 4:3 to the full
 * widescreen canvas, then a DX mark appears after the logo.
 *
 * The title is an H32 (256 x 224) screen: plane A holds only the logo and the menu text,
 * the picture is plane B plus sprites (sky, castle and forest; cliff, sword and clouds). So
 * the frame is widened from the picture alone: plane B and the sprites are rendered here
 * from VRAM / CRAM (no plane A, so no letters get copied). Plane B does not move on the
 * title, and the bands outside the 256 x 224 picture repeat its outer 24 columns (12 rows
 * top and bottom) mirrored back and forth: sky, forest and rock continue past the old
 * edges. That scenery includes the sprites that never move (the sword and the cliff). The
 * clouds are sprites drifting left in rows at their own speeds, looping through the whole
 * 9-bit sprite X range (screen x -128..383), so the game already places them past both
 * edges: sprites seen moving in the last STILL frames are drawn at their true positions,
 * never mirrored (a mirrored cloud would drift the wrong way). The bands reach at most 85 px out, inside that
 * range, so a cloud leaving the picture carries on into the band and the wrap from -128 to
 * 383 happens out of view. It is redone every frame from the VDP's state, so it follows
 * the game's fades.
 *
 * The animation: 20 frames after the title picture shows (it pops in after ~55 black frames
 * that already have the title's RAM signature), the bands grow from the picture's edges to
 * the canvas over 45 frames (ease-out); then "DX" fades in over 20 frames after "ODYSSEY",
 * spaced like the logo's letters: the user's pixel art, stored as the logo palette's
 * indices and coloured from the game's palette (see DX_PIX).
 *
 * Host-side presentation only: the game, its RAM and its timing are untouched.
 */
#include "arcus_title_dx.h"
#include "arcus_host.h"
#include "genesis_machine.h"
#include "genesis_vdp.h"
#include "video/genesis_dac.h"

#include <stdlib.h>
#include <string.h>

#define PW 256
#define PH 224
#define BAND_X 24
#define BAND_Y 12
#define SX0    128          /* T.bg column of picture x 0: sprites span picture x -128..383 */
#define BGW    512
#define DELAY  20

/* Frames of 4:3 title before the bands grow: DELAY, or ARCUS_TITLE_DX_DELAY (trailer
 * captures only, TRAILER.md: a long 4:3 stretch lets the trailer play the 1991 title and the
 * widening as one continuous shot). */
static int title_delay(void)
{
    static int d = -1;
    if (d < 0) {
        const char *e = getenv("ARCUS_TITLE_DX_DELAY");
        d = e ? atoi(e) : DELAY;
        if (d < 0) d = DELAY;
    }
    return d;
}
#define GROW   45
#define FADE   20

/* "DX", 52 x 36 (hand-drawn by the user, 2026-11-12), placed at picture (244, 60): the rows
 * of "ODYSSEY" (its chrome bands line up there) and 2 px after its Y.
 *
 * Stored the way the game stores its own logo: palette indices, not colours. Each character
 * is an entry of the palette line "ODYSSEY" is drawn with (plane A, line 1 on the title):
 * '1'-'9', 'A'-'C' = entries 1-12 (1-2 the dark red outline and shadow, 3-8 the chrome
 * ramp, 9-C the pale yellows up to white), '.' = transparent. The colours come from the
 * game's CRAM at runtime, so the source holds no art colours, and the mark fades with the
 * screen and follows the colour settings exactly like the logo. Redraw it as a PNG in those
 * 12 colours and convert it with tools/pixel-snap.py --indices. */
#define DX_X 244
#define DX_Y 60
#define DX_W 52
#define DX_H 36
static const char *const DX_PIX[DX_H] = {
    "....19CCCCCCCCCCCCC96.....1CCCCCCCCCCCC1..1CCCCCCCC1",
    "....4122222222222222355...C1222222222211..C122222211",
    "....842222222222222222331.C1222222222211..C12222221.",
    "....C82222222222222222221111112222222111..112222111.",
    "....C1132222222222222222211..412222213.....422212...",
    ".......692222222222222222211.64222221.....642211....",
    "........69333333331133333331..6333331.....93331.....",
    ".........94444444411234444433.C444441....69441......",
    ".........6655555511.275555534.6C55551....95531......",
    ".........5666666611..576666646.C666611..49631.......",
    ".........567777771....77777764.6777771.347711.......",
    ".........588888811....58888884.4888881.38831........",
    ".........699999911....39999993.399999111991.........",
    "........56AAAAAA1.....1AAAAA41.17AAAA11AA31.........",
    "........56BBBBB11.....1BBBBB31.11BBBB1BB31..........",
    "........68CCCCC11....11CCCCC31..1CCCCCCC1...........",
    "........8CCCCCC11....11CCCCC11..4CCCCCC31...........",
    ".......68CCCCCC1.....11CCCCC11.86CCCCCC1............",
    ".......663CCCC11.....13CCCC311.C3CCCCC31............",
    ".......822333311.....323333111.C13333331............",
    ".......622222211....342222211..C32222221............",
    "......662222221.....452222211..522222221............",
    "......612222221.....662222211.C3221222211...........",
    "......822222211....682222211.C32211222211...........",
    "......82222221.....682222211.C22111222221...........",
    ".....663333331....683333331113331.1333331...........",
    ".....624444411...6844444411114411.1444441...........",
    ".....535555511346855555551113551..1155551...........",
    "....446666661346866666661113551....155551...........",
    "...7C67777777777777777711117771....1777711..........",
    "67CC88888888888888888811118811.....1888881..........",
    "939999999999999999991111119922.....19999911.........",
    "B4AAAAAAAAAAAAAAAA111112AAAA2.....31AAAAA11.........",
    "B4BBBBBBBBBBBBB1111111B1BBBB11..B1BBBBBBBBB11.......",
    "B11111111111111111...911111112..9111111111112.......",
    "1111111111111111....1111111112..1111111111112......."
};

#define STILL 30           /* frames without moving after which a sprite is scenery */
#define SYN_COLS (SX0 / 8)  /* band blocks rebuilt on the right: all 128 px */
#define SYN_SRC0 176        /* source blocks: picture x 176..255, the cliff's far side */

static struct {
    unsigned frames;                  /* consecutive title frames with the picture showing */
    uint32_t bg[PH][BGW];             /* picture x -128..383: the scenery (plane B + still
                                         sprites) mirrored past the picture's sides, and the
                                         moving sprites at their true positions */
    uint16_t pos[80][2];              /* per SAT link: last x, y */
    uint8_t moved_ago[80];            /* frames since that link last moved (saturates) */
    /* The right band's cliff rows, rebuilt from the cliff's own 8 x 8 blocks (see
     * synthesize_cliff): per band block, its source block in the picture, or -1. */
    int syn_ready;
    int8_t syn_sx[PH / 8][SYN_COLS], syn_sy[PH / 8][SYN_COLS];
    int16_t spr_top[80];              /* per drawing-order index: the sprite's top row */
} T;

static uint32_t cram_argb(const GVDP *v, unsigned i)
{
    return genesis_dac_cram_to_argb(v->cram[i & 63], 0);
}

static int plane_cells(int code) { return code == 1 ? 64 : code == 3 ? 128 : 32; }

/* Colour index (0 = transparent) and priority of a plane pixel at screen (x, y). */
static int plane_pixel(const GVDP *v, uint16_t base, int hscroll, int vscroll, int x, int y, int *pri, int *line)
{
    int pw = plane_cells(v->reg[16] & 3), ph = plane_cells((v->reg[16] >> 4) & 3);
    int px = ((x - hscroll) % (pw * 8) + pw * 8) % (pw * 8);
    int py = ((y + vscroll) % (ph * 8) + ph * 8) % (ph * 8);
    uint16_t a = (uint16_t)(base + ((py >> 3) * pw + (px >> 3)) * 2);
    uint16_t e = (uint16_t)(v->vram[a] << 8 | v->vram[(uint16_t)(a + 1)]);
    int sx = px & 7, sy = py & 7;
    if (e & 0x0800) sx = 7 - sx;
    if (e & 0x1000) sy = 7 - sy;
    uint8_t b = v->vram[(uint16_t)((e & 0x7FF) * 32 + sy * 4 + sx / 2)];
    *pri = e >> 15;
    *line = (e >> 13) & 3;
    return (sx & 1) ? b & 15 : b >> 4;
}

static int hscroll_of(const GVDP *v, int y, int plane_b)
{
    uint16_t base = (uint16_t)((v->reg[13] & 0x3F) << 10);
    int mode = v->reg[11] & 3, row = mode == 0 ? 0 : mode == 2 ? (y & ~7) : y;
    uint16_t a = (uint16_t)(base + row * 4 + (plane_b ? 2 : 0));
    return (v->vram[a] << 8 | v->vram[(uint16_t)(a + 1)]) & 0x3FF;
}

/* Mirrored repeat of the outer band: distance d (1..) past an edge -> index from that edge. */
static int mirror(int d, int band)
{
    int k = (d - 1) % (2 * band);
    return k < band ? k : 2 * band - 1 - k;
}

/* The SAT link list in drawing order (order[0] on top). */
static int sat_order(const GVDP *v, int order[80])
{
    uint16_t sat = (uint16_t)((v->reg[5] & 0x7F) << 9);
    int n = 0, link = 0;
    do {
        order[n++] = link;
        link = v->vram[(uint16_t)(sat + link * 8 + 3)] & 0x7F;
    } while (link && link < 80 && n < 64);     /* the VDP has 80 sprites at most: a link past
                                                  that (corrupt table) ends the list, so order[]
                                                  never indexes past T.pos / T.moved_ago */
    return n;
}

/* Once per title frame: which sprites move. The clouds drift (every link moves within a
 * few frames); the sword and the cliff never do. Before a link has been seen at all it
 * counts as still. */
static void track_sprites(const GVDP *v)
{
    uint16_t sat = (uint16_t)((v->reg[5] & 0x7F) << 9);
    int order[80], n = sat_order(v, order);
    for (int k = 0; k < n; k++) {
        int l = order[k];
        uint16_t e = (uint16_t)(sat + l * 8);
        uint16_t y = (uint16_t)((v->vram[e] << 8 | v->vram[(uint16_t)(e + 1)]) & 0x3FF);
        uint16_t x = (uint16_t)((v->vram[(uint16_t)(e + 6)] << 8 | v->vram[(uint16_t)(e + 7)]) & 0x1FF);
        if (T.frames == 1) T.moved_ago[l] = STILL;
        else if (x != T.pos[l][0] || y != T.pos[l][1]) T.moved_ago[l] = 0;
        else if (T.moved_ago[l] < STILL) T.moved_ago[l]++;
        T.pos[l][0] = x;
        T.pos[l][1] = y;
    }
}

/* Draws the sprites of one kind (moving or still), last-to-first so earlier links end on
 * top; rank keeps the order between the two passes (lower = in front). Still sprites go
 * on the picture's columns only (they are mirrored with plane B after); moving ones over
 * the whole sprite X range. */
static void draw_sprites(const GVDP *v, int moving, uint8_t bpri[PH][BGW], uint8_t rank[PH][BGW])
{
    uint16_t sat = (uint16_t)((v->reg[5] & 0x7F) << 9);
    int order[80], n = sat_order(v, order);
    int lo = moving ? 0 : SX0, hi = moving ? BGW : SX0 + PW;
    for (int k = n - 1; k >= 0; k--) {
        if ((T.moved_ago[order[k]] < STILL) != moving) continue;
        uint16_t e = (uint16_t)(sat + order[k] * 8);
        int sy = ((v->vram[e] << 8 | v->vram[(uint16_t)(e + 1)]) & 0x3FF) - 128;
        int size = v->vram[(uint16_t)(e + 2)], cw = ((size >> 2) & 3) + 1, ch = (size & 3) + 1;
        uint16_t attr = (uint16_t)(v->vram[(uint16_t)(e + 4)] << 8 | v->vram[(uint16_t)(e + 5)]);
        int sx = ((v->vram[(uint16_t)(e + 6)] << 8 | v->vram[(uint16_t)(e + 7)]) & 0x1FF) - 128;
        int hf = (attr >> 11) & 1, vf = (attr >> 12) & 1, pri = attr >> 15, line = (attr >> 13) & 3;
        T.spr_top[k] = (int16_t)sy;
        for (int yy = 0; yy < ch * 8; yy++) {
            int y = sy + yy;
            if (y < 0 || y >= PH) continue;
            int ty = vf ? ch * 8 - 1 - yy : yy;
            for (int xx = 0; xx < cw * 8; xx++) {
                int x = SX0 + sx + xx;
                if (x < lo || x >= hi || rank[y][x] < k) continue;
                int tx = hf ? cw * 8 - 1 - xx : xx;
                unsigned tile = (unsigned)((attr & 0x7FF) + (tx >> 3) * ch + (ty >> 3));
                uint8_t b = v->vram[(uint16_t)(tile * 32 + (ty & 7) * 4 + (tx & 7) / 2)];
                int c = (tx & 1) ? b & 15 : b >> 4;
                if (c && (pri || !bpri[y][x])) {
                    T.bg[y][x] = cram_argb(v, (unsigned)(line * 16 + c));
                    rank[y][x] = (uint8_t)k;
                }
            }
        }
    }
}

static int rgb_dist(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int x = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
        d += x < 0 ? -x : x;
    }
    return d;
}

static void copy_block(int dx, int dy, int sx, int sy, uint8_t bpri[PH][BGW], uint8_t rank[PH][BGW])
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            T.bg[dy + y][dx + x] = T.bg[sy + y][sx + x];
            bpri[dy + y][dx + x] = bpri[sy + y][sx + x];
            rank[dy + y][dx + x] = rank[sy + y][sx + x];
        }
}

/* The cliff (still sprites) reaches the picture's right edge; mirroring it showed as a
 * kaleidoscope. Instead the right band's cliff rows are rebuilt from the cliff's own 8 x 8
 * blocks (the sprites' tiles; picture x 128..255): block by block, left to right and top
 * to bottom, the source block that best continues what is already there. "Continues": the
 * pixels just left of (and above) the source block in the picture look like the placed
 * neighbour's right column (and bottom row); so the band carries on as the picture would.
 * A block comes from the same block row (the rock's top contour stays at its height),
 * or one row up or down inside the rock; recently used blocks and long straight copies of
 * the picture cost extra, which keeps the band from repeating. Chosen once per title
 * showing (deterministic), then redrawn every frame from the current colours (fades). */
static void synthesize_cliff(uint8_t bpri[PH][BGW], uint8_t rank[PH][BGW], int choose)
{
    int rows = PH / 8, edge = SX0 + PW - 8;
    int8_t cliff[PH / 8];
    for (int by = 0; by < rows; by++) {
        cliff[by] = 0;
        for (int y = by * 8; y < by * 8 + 8; y++)
            for (int x = edge; x < edge + 8; x++)
                if (rank[y][x] != 0xFF) cliff[by] = 1;
    }
    /* Source blocks hold cliff only: no sprite that starts well above the cliff's first row
     * (the sword standing in it), and no light grey or white (the sword's tip is drawn
     * into a cliff sprite; the rock's own colours are all reds and oranges). */
    int first = 0;
    while (first < rows && !cliff[first]) first++;
    static uint8_t usable[PH / 8][PW / 8];
    if (choose)
        for (int by = 0; by < rows; by++)
            for (int sx = 0; sx < PW / 8; sx++) {
                usable[by][sx] = cliff[by];
                for (int y = by * 8; y < by * 8 + 8 && usable[by][sx]; y++)
                    for (int x = SX0 + sx * 8; x < SX0 + sx * 8 + 8; x++) {
                        uint32_t c = T.bg[y][x];
                        int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
                        int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
                        if ((hi > 100 && hi - lo < 60) ||
                            (rank[y][x] != 0xFF && T.spr_top[rank[y][x]] < first * 8 - 8)) {
                            usable[by][sx] = 0;
                            break;
                        }
                    }
            }
    for (int by = 0; by < rows; by++)
        for (int bx = 0; bx < SYN_COLS; bx++) {
            int dx = SX0 + PW + bx * 8, dy = by * 8;
            if (!cliff[by]) { T.syn_sx[by][bx] = -1; continue; }
            if (choose) {
                long best = -1;
                int bsx = 0, bsy = by;
                for (int sy = by - 1; sy <= by + 1; sy++) {
                    if (sy < 0 || sy >= rows || !cliff[sy]) continue;
                    if (sy != by && (!cliff[by - 1 < 0 ? 0 : by - 1] || by == 0 || !cliff[by - 1]))
                        continue;                   /* top contour row: same row only */
                    for (int sx = SYN_SRC0 / 8; sx < PW / 8; sx++) {
                        if (!usable[sy][sx]) continue;
                        int px = SX0 + sx * 8, py = sy * 8;
                        long cost = (PW / 8 - 1 - sx) * 60L;   /* nearer the edge looks closer */
                        for (int i = 0; i < 8; i++)     /* left neighbour vs. the source's left */
                            cost += rgb_dist(T.bg[dy + i][dx - 1], T.bg[py + i][px - 1]);
                        if (by > 0 && cliff[by - 1] && py > 0)
                            for (int i = 0; i < 8; i++) /* the block above vs. the source's above */
                                cost += rgb_dist(T.bg[dy - 1][dx + i], T.bg[py - 1][px + i]);
                        for (int k = 1; k <= 3; k++)    /* reuse nearby */
                            if (bx - k >= 0 && T.syn_sx[by][bx - k] == sx && T.syn_sy[by][bx - k] == sy)
                                cost += 4000;
                        for (int k = -2; k <= 2; k++)
                            if (by > 0 && bx + k >= 0 && bx + k < SYN_COLS &&
                                T.syn_sx[by - 1][bx + k] == sx && T.syn_sy[by - 1][bx + k] == sy - 1)
                                cost += 1500;           /* the column copied straight down */
                        int run = 0;                    /* a straight copy of the picture */
                        for (int k = 1; k <= 3 && bx - k >= 0; k++)
                            if (T.syn_sx[by][bx - k] == sx - k && T.syn_sy[by][bx - k] == sy) run++;
                            else break;
                        if (run >= 2) cost += 3000;
                        if (best < 0 || cost < best) { best = cost; bsx = sx; bsy = sy; }
                    }
                }
                T.syn_sx[by][bx] = (int8_t)bsx;
                T.syn_sy[by][bx] = (int8_t)bsy;
            }
            copy_block(dx, dy, SX0 + T.syn_sx[by][bx] * 8, T.syn_sy[by][bx] * 8, bpri, rank);
        }
}

static void render_background(const GVDP *v)
{
    static uint8_t bpri[PH][BGW], rank[PH][BGW];
    uint16_t base = (uint16_t)((v->reg[4] & 7) << 13);
    uint32_t backdrop = cram_argb(v, v->reg[7] & 0x3F);
    memset(rank, 0xFF, sizeof rank);
    for (int y = 0; y < PH; y++) {
        int hs = hscroll_of(v, y, 1), vs = v->vsram[1] & 0x3FF;
        for (int x = 0; x < PW; x++) {
            int pri, line, c = plane_pixel(v, base, hs, vs, x, y, &pri, &line);
            T.bg[y][SX0 + x] = c ? cram_argb(v, (unsigned)(line * 16 + c)) : backdrop;
            bpri[y][SX0 + x] = (uint8_t)(c && pri);
        }
    }
    draw_sprites(v, 0, bpri, rank);             /* scenery sprites: sword, cliff */
    for (int y = 0; y < PH; y++)
        for (int k = 1; k <= SX0; k++) {        /* the scenery past the sides: mirrored */
            int l = SX0 + mirror(k, BAND_X), r = SX0 + PW - 1 - mirror(k, BAND_X);
            T.bg[y][SX0 - k] = T.bg[y][l];
            bpri[y][SX0 - k] = bpri[y][l];
            rank[y][SX0 - k] = rank[y][l];
            if (SX0 + PW - 1 + k < BGW) {
                T.bg[y][SX0 + PW - 1 + k] = T.bg[y][r];
                bpri[y][SX0 + PW - 1 + k] = bpri[y][r];
                rank[y][SX0 + PW - 1 + k] = rank[y][r];
            }
        }
    synthesize_cliff(bpri, rank, !T.syn_ready);  /* the cliff's rows on the right */
    T.syn_ready = 1;
    draw_sprites(v, 1, bpri, rank);             /* moving sprites (clouds): true positions */
}

static uint32_t mix(uint32_t a, uint32_t b, int alpha256)
{
    uint32_t r = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) {
        int x = (int)((a >> sh) & 0xFF), y = (int)((b >> sh) & 0xFF);
        r |= (uint32_t)(x + (((y - x) * alpha256) >> 8)) << sh;
    }
    return r;
}

/* The DX mark, at the picture's (DX_X, DX_Y) of a frame whose picture is at (ox, oy): each
 * pixel is its palette entry in the logo's palette line, read from CRAM this frame. */
static void draw_dx(const GVDP *v, uint32_t *buf, int w, int h, int stride, int ox, int oy, int alpha)
{
    uint16_t base = (uint16_t)((v->reg[2] & 0x38) << 10);
    int hs = hscroll_of(v, DX_Y, 0), vs = v->vsram[0] & 0x3FF;
    /* The logo's palette line: the one "ODYSSEY" (picture x 150..249) is drawn with. No
     * logo on this screen (Music Mode): no mark. */
    int logo = 0, line = -1;
    for (int y = DX_Y; y < DX_Y + DX_H; y++)
        for (int x = 150; x < 250; x++) {
            int pri, l, c = plane_pixel(v, base, hs, vs, x, y, &pri, &l);
            if (!c) continue;
            logo++;
            if (line < 0) line = l;
        }
    if (logo < 250 || line < 0) return;
    for (int r = 0; r < DX_H; r++)
        for (int c = 0; c < DX_W; c++) {
            char ch = DX_PIX[r][c];
            if (ch == '.') continue;
            int x = ox + DX_X + c, y = oy + DX_Y + r;
            if (x < 0 || x >= w || y < 0 || y >= h) continue;
            unsigned entry = (unsigned)(ch <= '9' ? ch - '0' : ch - 'A' + 10);
            uint32_t *p = buf + (size_t)y * stride + x;
            *p = mix(*p, cram_argb(v, (unsigned)line * 16u + entry), alpha);
        }
}

int arcus_title_dx_compose(int on_title, const uint32_t *src, int w, int h, int src_stride,
                           uint32_t *dst, int dst_stride)
{
    const GVDP *v = &g_machine.vdp;
    if (!on_title || !g_arcus_settings.widescreen || !g_arcus_settings.title_dx || gvdp_screen_width(v) != PW || w <= PW || w > PW + 2 * SX0 || h < PH) {
        T.frames = 0;
        T.syn_ready = 0;
        return 0;
    }
    int ox = (w - PW) / 2, oy = (h - PH) / 2;
    /* Only while the picture shows (not black). Before it: wait for it. After it, e.g. on
     * leaving for the password screen, the game blanks the picture and loads the next
     * screen's tiles while the title's signature is still in RAM: the bands, drawn from
     * the VDP, would show those tiles, so they stay black like the picture. */
    int shown = 0;
    for (int y = oy; y < oy + PH && !shown; y += 8)
        for (int x = ox; x < ox + PW && !shown; x += 8)
            shown = (src[(size_t)y * src_stride + x] & 0xFFFFFFu) != 0;
    if (!shown) return 0;
    T.frames++;
    track_sprites(v);
    /* Reveal: half-width / half-height of the shown rectangle, eased out. */
    int t = (int)T.frames - title_delay(), p = 0;               /* progress 0..256, cubic ease-out */
    if (t >= GROW) p = 256;
    else if (t > 0) {
        int q = 256 - t * 256 / GROW;
        p = 256 - q * q / 256 * q / 256;
    }
    int rx = PW / 2 + ((w / 2 - PW / 2) * p >> 8), ry = PH / 2 + ((h - h / 2 - PH / 2) * p >> 8);
    int cx = w / 2, cy = oy + PH / 2;
    if (p) render_background(v);
    for (int y = 0; y < h; y++) {
        const uint32_t *s = src + (size_t)y * src_stride;
        uint32_t *d = dst + (size_t)y * dst_stride;
        int py = y - oy, iny = py >= 0 && py < PH;
        int show_y = y >= cy - ry && y < cy + ry;
        int by = iny ? py : py < 0 ? mirror(-py, BAND_Y) : PH - 1 - mirror(py - PH + 1, BAND_Y);
        for (int x = 0; x < w; x++) {
            int px = x - ox;
            if (iny && px >= 0 && px < PW) { d[x] = s[x]; continue; }
            if (!p || !show_y || x < cx - rx || x >= cx + rx) { d[x] = s[x]; continue; }
            d[x] = T.bg[by][SX0 + px];          /* |px| < 128 past the picture: in range */
        }
    }
    int a = (int)T.frames - title_delay() - GROW;
    if (a > 0) draw_dx(v, dst, w, h, dst_stride, ox, oy, a >= FADE ? 256 : a * 256 / FADE);
    return 1;
}
