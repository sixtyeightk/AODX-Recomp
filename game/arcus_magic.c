/*
 * arcus_magic.c — clearer magic; see arcus_magic.h.
 *
 * How the game's magic works (select code $1C4FE-$1C596, per player p = 0/1):
 *   $8218 + p character (0 Jedda, 1 Erin, 2 Diana, 3 Bead), $8236 + p magic level,
 *   $8238 + p crystal stock, $823A + p casts left. In the item / magic window the cursor is
 *   row $828C + p (0 = the crystal row) and column $828A + p. Choosing N crystals is refused
 *   if N > stock, N >= 4 for Erin, or N = 5 for anyone but Bead; otherwise the stock drops by
 *   N, the level becomes N and the casts are replaced (not added) from the table at $1E556
 *   (5 bytes per character). $826E + p is the equipped item (0 = magic, the M); the HUD's
 *   item icons and counters are redrawn by $1CD98, which the cast and item code call.
 *
 * MAGIC INFO (display only):
 *   - readout: above each player's half of the HUD panel, the level and spell ("Lv2
 *     Fireball", greyed with no casts left).
 *   - preview: while a player's magic window is on the crystal row, under that window, what
 *     the highlighted choice gives (level, spell, casts) or why the game would refuse it.
 * QUICK MAGIC (hold X): a list of the character's levels above the HUD, Lv1 at the top; UP /
 *   DOWN (d-pad or stick) move, releasing X picks, B cancels. The game gets none of that player's input
 *   meanwhile. A pick happens only after a move, so a tap costs nothing (choosing a level
 *   always spends crystals, even the current one). It makes the window's checks and writes,
 *   equips magic, then calls the game's HUD redraw $1CD98 and the spell-graphics reload
 *   $1BC9E the main loop runs after the window (glue_run_guest_routine). The
 *   window's own redraws ($1C8D2 / $1CA8E) are not called: they draw the window's tiles,
 *   which other graphics use while it is closed.
 *
 * Spell names: the game names none, and the manual (p. 17) gives only themes (Jedda
 * flames, Erin winds, Diana light rays, Bead summons a dark lord from Lv2). These are ours,
 * from each level cast after a real item-window pick, which also loads its graphics
 * (2026-11-10; spawns at $100C4): e.g. Jedda Lv3 / Lv4 send one firebird / four; Diana Lv2 /
 * Lv3 and Bead Lv4 home on enemies.
 */
#include "arcus_magic.h"
#include "arcus_host.h"
#include "arcus_input.h"
#include "arcus_video.h"
#include "genesis_host_mem.h"
#include "genesis_machine.h"
#include "genesis_runtime.h"

#include <stdio.h>
#include <string.h>

#define RAM_HEALTH    0xFF8212u
#define RAM_CHAR      0xFF8218u
#define RAM_LEVEL     0xFF8236u
#define RAM_CRYSTALS  0xFF8238u
#define RAM_CASTS     0xFF823Au
#define RAM_ITEM      0xFF826Eu
#define RAM_WIN_COL   0xFF828Au
#define RAM_WIN_ROW   0xFF828Cu
#define RAM_PLAYERS   0xFF8284u
#define RAM_DEMO      0xFF8089u
#define ROM_CASTS     0x01E556u
#define HUD_REDRAW    0x01CD98u
/* Run by the main loop after the item window ($6FB6): reloads each player's spell graphics
 * (VRAM $52A0 P1, $5F20 P2; by character and level, when magic is equipped with casts and
 * the level is above 1). The level alone only changes how the spell behaves. */
#define SPELL_GFX     0x01BC9Eu
/* The window loop ($1C400-$1C5AF) waits for each frame from inside itself: while it runs,
 * the top of the game's stack holds a return into it, above the main loop's return $6FA6
 * (measured in 1- and 2-player games; in play these slots hold other addresses). */
#define STACK_WINDOW  0xFFFBF8u
#define STACK_MAIN    0xFFFBFCu

static const char *const CHAR_NAME[4] = { "Jedda", "Erin", "Diana", "Bead" };
static const int MAX_LEVEL[4] = { 4, 3, 4, 5 };
static const char *const SPELL[4][5] = {
    { "Stoneskin", "Fireball", "Firebird", "Flame cross", NULL },
    { "Anti-poison", "Gust", "Whirlwind", NULL, NULL },
    { "Healing light", "Guiding light", "Light fracture", "Light rain", NULL },
    { "Cure silence", "Crux fissure", "Void vortex", "Arc lightning", "Dark lord's lightning" },
};

static struct {
    int holding;           /* X held: the list is up */
    int sel;               /* highlighted level */
    int moved;             /* UP / DOWN moved it during this hold */
    int pending;           /* level to pick before the next game frame */
    uint32_t prev_buttons, prev_dirs;
} Q[2];

static int players(void) { return glue_peek8(RAM_PLAYERS) == 2 ? 2 : 1; }

static int character(int p)
{
    int c = glue_peek8(RAM_CHAR + (uint32_t)p);
    return c < 4 ? c : -1;
}

static int casts_for(int c, int n) { return glue_peek8(ROM_CASTS + (uint32_t)(c * 5 + n - 1)); }

static int in_play(void)
{
    return arcus_gameplay_view(&g_machine.vdp) && !glue_peek8(RAM_DEMO);
}

static int window_open(void)
{
    uint32_t r = glue_peek32(STACK_WINDOW) & 0xFFFFFFu;
    return (glue_peek32(STACK_MAIN) & 0xFFFFFFu) == 0x6FA6u && r >= 0x1C400u && r < 0x1C5B0u;
}

int arcus_magic_window_open(void) { return window_open(); }

static int alive(int p)
{
    return p < players() && character(p) >= 0 && glue_peek8(RAM_HEALTH + (uint32_t)p) != 0;
}

/* Why the game would refuse choosing n crystals ($1C528-$1C554), or NULL. */
static const char *refusal(int p, int n, char *buf, size_t len)
{
    int c = character(p);
    if (c < 0) return "";
    if (n > MAX_LEVEL[c]) {
        snprintf(buf, len, "%s tops out at Lv%d", CHAR_NAME[c], MAX_LEVEL[c]);
        return buf;
    }
    int stock = glue_peek8(RAM_CRYSTALS + (uint32_t)p);
    if (n > stock) {
        snprintf(buf, len, "Needs %d crystal%s, you have %d", n, n == 1 ? "" : "s", stock);
        return buf;
    }
    return NULL;
}

/* ---- quick change ---------------------------------------------------------------------- */

int arcus_magic_input(int p, const ArcusHostPad *pad)
{
    if (p < 0 || p > 1) return 0;
    if (!pad) {                                /* the config menu opened: drop the hold */
        Q[p].holding = Q[p].pending = 0;
        Q[p].prev_buttons = Q[p].prev_dirs = 0;
        return 0;
    }
    uint32_t b = pad->buttons, edge = b & ~Q[p].prev_buttons;
    const float t = ARCUS_STICK_THRESHOLD;
    uint32_t dirs = (((b & ARCUS_HB_UP) || pad->ly > t) ? 1u : 0u) |       /* stick: up > 0 */
                    (((b & ARCUS_HB_DOWN) || pad->ly < -t) ? 2u : 0u);
    uint32_t dir_edge = dirs & ~Q[p].prev_dirs;
    Q[p].prev_buttons = b;
    Q[p].prev_dirs = dirs;
    if (!g_arcus_settings.quick_magic || !in_play() || window_open() || !alive(p)) {
        Q[p].holding = 0;
        return 0;
    }
    int c = character(p);
    if (!Q[p].holding) {
        if (!(edge & ARCUS_HB_X)) return 0;
        int lv = glue_peek8(RAM_LEVEL + (uint32_t)p);
        Q[p].holding = 1;
        Q[p].moved = 0;
        Q[p].sel = lv >= 1 && lv <= MAX_LEVEL[c] ? lv : 1;
        return 1;
    }
    if (edge & ARCUS_HB_B) {                   /* cancel (B stays dead until released) */
        Q[p].holding = 0;
        return 1;
    }
    if (b & ARCUS_HB_X) {
        if ((dir_edge & 1u) && Q[p].sel > 1)            { Q[p].sel--; Q[p].moved = 1; }
        if ((dir_edge & 2u) && Q[p].sel < MAX_LEVEL[c]) { Q[p].sel++; Q[p].moved = 1; }
        return 1;
    }
    Q[p].holding = 0;                          /* released */
    char why[48];
    if (Q[p].moved && !refusal(p, Q[p].sel, why, sizeof why)) Q[p].pending = Q[p].sel;
    return 1;
}

void arcus_magic_frame(void)
{
    for (int p = 0; p < 2; p++) {
        int n = Q[p].pending;
        Q[p].pending = 0;
        char why[48];
        if (!n || !g_arcus_settings.quick_magic || !in_play() || window_open() || !alive(p) ||
            refusal(p, n, why, sizeof why))
            continue;
        int c = character(p), casts = casts_for(c, n);
        glue_poke8(RAM_CRYSTALS + (uint32_t)p, (uint8_t)(glue_peek8(RAM_CRYSTALS + (uint32_t)p) - n));
        glue_poke8(RAM_LEVEL + (uint32_t)p, (uint8_t)n);
        glue_poke8(RAM_CASTS + (uint32_t)p, (uint8_t)casts);
        glue_poke8(RAM_ITEM + (uint32_t)p, 0);
        glue_run_guest_routine(HUD_REDRAW);
        glue_run_guest_routine(SPELL_GFX);      /* as after the window: the new spell's tiles */
        fprintf(stderr, "[arcus-magic] P%d quick pick: Lv%d %s, %d casts\n", p + 1, n, SPELL[c][n - 1], casts);
    }
}

/* ---- drawing --------------------------------------------------------------------------- */

#define COL_TEXT    0xFFF0E8C8u
#define COL_DIM     0xFF8C8878u
#define COL_PICK    0xFF304878u
#define LINE_H      10

static void shade(uint32_t *buf, int w, int h, int stride, int x0, int y0, int x1, int y1)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) {
            uint32_t *px = buf + (size_t)y * stride + x;
            *px = 0xFF000000u | ((*px >> 1) & 0x7F7F7Fu);
        }
}

static void fill(uint32_t *buf, int w, int h, int stride, int x0, int y0, int x1, int y1, uint32_t col)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++)
            buf[(size_t)y * stride + x] = col;
}

/* Readout: one line above the player's half of the HUD panel. */
static void draw_readout(uint32_t *buf, int w, int h, int stride, int p, int hud, int rx)
{
    int c = character(p), lv = glue_peek8(RAM_LEVEL + (uint32_t)p);
    if (lv < 1 || lv > MAX_LEVEL[c]) return;                       /* no spell chosen yet */
    int casts = glue_peek8(RAM_CASTS + (uint32_t)p);
    char name[40];
    snprintf(name, sizeof name, "Lv%d %s", lv, SPELL[c][lv - 1]);
    int x = rx + 8 + 160 * p, y = hud - LINE_H + 1;
    shade(buf, w, h, stride, x - 3, y - 1, x + arcus_small_text_width(name) + 3, y + 9);
    arcus_small_text(buf, stride, h, x, y, name, casts ? COL_TEXT : COL_DIM);
}

/* Preview: under the player's magic window (raster x 34 + 160 p .. +108, rows 50..142). */
static void draw_preview(uint32_t *buf, int w, int h, int stride, int p, int rx, int ry)
{
    int c = character(p), n = glue_peek8(RAM_WIN_COL + (uint32_t)p) + 1;
    char line[2][48], why[48];
    uint32_t col[2] = { COL_TEXT, COL_TEXT };
    const int lines = 2;
    const char *no = refusal(p, n, why, sizeof why);
    if (n <= MAX_LEVEL[c]) snprintf(line[0], sizeof line[0], "Lv%d %s", n, SPELL[c][n - 1]);
    else snprintf(line[0], sizeof line[0], "Lv%d", n);
    if (no) {
        snprintf(line[1], sizeof line[1], "%s", no);
        col[0] = col[1] = COL_DIM;
    } else {
        int k = casts_for(c, n);
        snprintf(line[1], sizeof line[1], "%d cast%s", k, k == 1 ? "" : "s");
    }
    int x = rx + 34 + 160 * p, y = ry + 146, tw = 0;
    for (int i = 0; i < lines; i++) {
        int lw = arcus_small_text_width(line[i]);
        if (lw > tw) tw = lw;
    }
    shade(buf, w, h, stride, x - 3, y - 2, x + tw + 3, y + lines * LINE_H);
    for (int i = 0; i < lines; i++) arcus_small_text(buf, stride, h, x, y + i * LINE_H, line[i], col[i]);
}

/* Quick change: the character's levels above the player's half of the HUD panel, the
 * casts right-aligned in a second column. */
static void draw_quick(uint32_t *buf, int w, int h, int stride, int p, int hud, int rx)
{
    int c = character(p), max = MAX_LEVEL[c];
    char name[5][32], casts[5][16], why[48];
    const char *head = "Quick magic", *foot = "Release to pick, B cancels";
    int nw = 0, cw = 0;
    for (int n = 1; n <= max; n++) {
        int k = casts_for(c, n);
        snprintf(name[n - 1], sizeof name[n - 1], "Lv%d %s", n, SPELL[c][n - 1]);
        snprintf(casts[n - 1], sizeof casts[n - 1], "%d cast%s", k, k == 1 ? "" : "s");
        if (arcus_small_text_width(name[n - 1]) > nw) nw = arcus_small_text_width(name[n - 1]);
        if (arcus_small_text_width(casts[n - 1]) > cw) cw = arcus_small_text_width(casts[n - 1]);
    }
    int tw = nw + 12 + cw;
    if (arcus_small_text_width(head) > tw) tw = arcus_small_text_width(head);
    if (arcus_small_text_width(foot) > tw) tw = arcus_small_text_width(foot);
    int bw = tw + 10, bh = (max + 2) * LINE_H + 4;
    int bx = p == 0 ? rx + 4 : rx + ARCUS_NATIVE_W - 4 - bw, by = hud - 2 - bh;
    shade(buf, w, h, stride, bx, by, bx + bw, by + bh);
    int x = bx + 5, y = by + 3;
    arcus_small_text(buf, stride, h, x, y, head, COL_TEXT);
    for (int n = 1; n <= max; n++) {
        int ly = y + n * LINE_H;
        uint32_t col = refusal(p, n, why, sizeof why) ? COL_DIM : COL_TEXT;
        if (n == Q[p].sel) fill(buf, w, h, stride, bx + 2, ly - 1, bx + bw - 2, ly + 9, COL_PICK);
        arcus_small_text(buf, stride, h, x, ly, name[n - 1], col);
        arcus_small_text(buf, stride, h, x + tw - arcus_small_text_width(casts[n - 1]), ly, casts[n - 1], col);
    }
    arcus_small_text(buf, stride, h, x, y + (max + 1) * LINE_H, foot, COL_DIM);
}

int arcus_magic_visible(void)
{
    return in_play() && (g_arcus_settings.magic_info || Q[0].holding || Q[1].holding);
}

void arcus_magic_draw(uint32_t *buf, int w, int h, int stride)
{
    if (!in_play()) return;
    int hud = arcus_video_hud_top(h), rx, ry;
    arcus_video_raster_origin(w, h, &rx, &ry);
    int win = window_open();
    for (int p = 0; p < players(); p++) {
        if (character(p) < 0) continue;
        if (Q[p].holding) {
            if (hud > 0) draw_quick(buf, w, h, stride, p, hud, rx);
            continue;
        }
        if (!g_arcus_settings.magic_info) continue;
        if (win && glue_peek8(RAM_WIN_ROW + (uint32_t)p) == 0) draw_preview(buf, w, h, stride, p, rx, ry);
        if (hud > 0 && glue_peek8(RAM_HEALTH + (uint32_t)p)) draw_readout(buf, w, h, stride, p, hud, rx);
    }
}
