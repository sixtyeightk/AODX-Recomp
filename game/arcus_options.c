/*
 * arcus_options.c — settings, settings.ini and the pause/settings overlay
 * (SPEC 6.5). Track C.
 *
 * The overlay is drawn at the game's resolution with the engine's bitmap font
 * (overlay_draw.c, upper-case ASCII), over a darkened copy of the last frame,
 * and then goes through the same presenter (so it gets the CRT look too). The
 * game is paused while it is open: HostPresenter.pause_frame returns 1 and the
 * runner skips the tick.
 *
 * Rewind (setting on): holding L3 (left stick click) steps back through the engine's
 * snapshot ring (local_states.c), one snapshot (6 frames) every 3 frames, with
 * the game paused in between and given no input.
 *
 * Test hook: ARCUS_NAV_SCRIPT="N[-M]:KEY[+KEY],..." injects navigation edges at
 * pause_frame call N (or calls N..M; one call per runner loop iteration,
 * 1-based). KEY is UP DOWN LEFT RIGHT OK BACK MENU, or REWIND (L3 held; QSAVE/QLOAD = L/R
 * held).
 */
#include "arcus_options.h"
#include "genesis_host_mem.h"
#include "audio.h"
#include "arcus_saves.h"
#include "runtime_evidence.h"
#include "arcus_input.h"
#include "arcus_video.h"
#include "arcus_cheats.h"
#include "arcus_minimap.h"
#include "arcus_magic.h"
#include "arcus_manual.h"
#include "platform_switch.h"   /* GENESIS_PLATFORM_SWITCH, the memory log */
#include "arcus_extras.h"

#include "app_config.h"
#include "local_states.h"
#include "overlay_draw.h"

#include <SDL2/SDL.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SETTINGS_VERSION 6        /* 2: rewind on, HUD sides removed; 3: autofire Medium; 4: CRT colour off;
                                     5: CRT colour on (full-screen view, true black); 6: mini map on */
#define REWIND_DEPTH     150      /* snapshots */
#define REWIND_INTERVAL  6        /* frames between snapshots: 150 x 6 = 15 s */

ArcusSettings g_arcus_settings = {
    .crt_preset      = ARCUS_CRT_SCANLINES,
    .widescreen      = 1,
    .autofire_period = 12,            /* Medium: 5 shots/s */
    .color_lut       = 1,
    .rewind          = 1,
    .no_slowdown     = 0,
    .title_dx        = 1,
    .minimap         = 1,
    .magic_info      = 1,
    .quick_magic     = 1,
};

/* runner (main.c) */
const char *exe_relative(const char *filename);
int runner_save_state_file(const char *path);
int runner_load_state_file(const char *path);
int runner_cold_reset(void);

/* ---- paths ----------------------------------------------------------------- */
/* The runner's base directory: next to the executable on the Mac, the
 * platform base dir sdmc:/switch/arcus-odyssey-dx/ on the Switch (engine E4). */
const char *arcus_data_dir(void)
{
    static char dir[512];
    if (!dir[0]) snprintf(dir, sizeof dir, "%s", exe_relative(""));
    return dir;
}

const char *arcus_settings_path(void)
{
    static char p[600];
    snprintf(p, sizeof p, "%ssettings.ini", arcus_data_dir());
    return p;
}

void arcus_state_path(int slot, char *buf, int n)
{
    snprintf(buf, (size_t)n, "%sstates/slot%d.bin", arcus_data_dir(), slot);
}

void arcus_ensure_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "[arcus-options] mkdir %s failed\n", path);
}

/* ---- settings.ini ---------------------------------------------------------- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void sanitize(ArcusSettings *s)
{
    s->crt_preset = clampi(s->crt_preset, ARCUS_CRT_OFF, ARCUS_CRT_STRONG);
    s->widescreen = s->widescreen != 0;
    if (s->autofire_period != 12 && s->autofire_period != 10 && s->autofire_period != 8)
        s->autofire_period = 12;
    s->color_lut = s->color_lut != 0;
    s->rewind = s->rewind != 0;
    s->no_slowdown = s->no_slowdown != 0;
    s->cheat_health = s->cheat_health != 0;
    s->cheat_crystals = s->cheat_crystals != 0;
    s->minimap = s->minimap != 0;
    s->title_dx = s->title_dx != 0;
    s->no_respawn = s->no_respawn != 0;
    s->magic_info = s->magic_info != 0;
    s->quick_magic = s->quick_magic != 0;
}

/* An older settings.ini was migrated on load: rewritten (as the current version) the next
 * time the menu closes, like a changed option. */
static int s_migrated;

void arcus_options_load_settings(const char *engine_settings_path)
{
    (void)engine_settings_path;   /* the same file: arcus_settings_path() */
    FILE *f = fopen(arcus_settings_path(), "rb");
    int version = SETTINGS_VERSION;   /* no file: fresh defaults */
    if (f) {
        char line[256];
        int in_arcus = 0;
        version = 1;                  /* a file without the key predates versioning */
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '[') { in_arcus = !strncmp(s, "[arcus]", 7); continue; }
            if (!in_arcus) continue;
            char key[64];
            int v;
            if (sscanf(s, "%63[a-z_] = %d", key, &v) != 2) continue;
            if      (!strcmp(key, "crt_preset"))      g_arcus_settings.crt_preset = v;
            else if (!strcmp(key, "widescreen"))      g_arcus_settings.widescreen = v;
            else if (!strcmp(key, "autofire_period")) g_arcus_settings.autofire_period = v;
            else if (!strcmp(key, "color_lut"))       g_arcus_settings.color_lut = v;
            else if (!strcmp(key, "rewind"))          g_arcus_settings.rewind = v;
            else if (!strcmp(key, "no_slowdown"))     g_arcus_settings.no_slowdown = v;
            else if (!strcmp(key, "cheat_health"))    g_arcus_settings.cheat_health = v;
            else if (!strcmp(key, "cheat_crystals"))  g_arcus_settings.cheat_crystals = v;
            else if (!strcmp(key, "minimap"))         g_arcus_settings.minimap = v;
            else if (!strcmp(key, "title_dx"))        g_arcus_settings.title_dx = v;
            else if (!strcmp(key, "no_respawn"))      g_arcus_settings.no_respawn = v;
            else if (!strcmp(key, "magic_info"))      g_arcus_settings.magic_info = v;
            else if (!strcmp(key, "quick_magic"))     g_arcus_settings.quick_magic = v;
            else if (!strcmp(key, "settings_version")) version = v;
        }
        fclose(f);
    }
    if (version < 2) g_arcus_settings.rewind = 1;   /* v2: rewind became on by default */
    if (version < 3) g_arcus_settings.autofire_period = 12;   /* v3: autofire speed Medium */
    if (version < 5) g_arcus_settings.color_lut = 1;          /* v5: CRT colour on (v4 had it off) */
    if (version < 6) g_arcus_settings.minimap = 1;            /* v6: mini map on (R3 hides it) */
    s_migrated = version < SETTINGS_VERSION;
    sanitize(&g_arcus_settings);
    /* The runner configures its rewind ring from g_app_config after this hook:
     * 150 snapshots every 6 frames = 15 s (about 24 MB), whatever [rewind] says. */
    g_app_config.rewind_enabled  = g_arcus_settings.rewind;
    g_app_config.rewind_depth    = REWIND_DEPTH;
    g_app_config.rewind_interval = REWIND_INTERVAL;
}

int arcus_save_settings(void)
{
    g_app_config.rewind_depth    = REWIND_DEPTH;
    g_app_config.rewind_interval = REWIND_INTERVAL;
    const char *path = arcus_settings_path();
    g_app_config.rewind_enabled = g_arcus_settings.rewind;
    /* The engine's sections first (app_config_save rewrites the file), then ours. */
    if (!app_config_save(path)) {
        fprintf(stderr, "[arcus-options] could not write %s\n", path);
        return 0;
    }
    FILE *f = fopen(path, "ab");
    if (!f) return 0;
    const ArcusSettings *s = &g_arcus_settings;
    fprintf(f, "[arcus]\n"
               "settings_version = %d\n"
               "crt_preset = %d        ; CRT shader: 0 off, 1 scanlines, 2 scanlines+grille, 3 strong\n"
               "color_lut = %d         ; 1 = CRT colour\n"
               "widescreen = %d        ; 1 = 16:9 gameplay (424 px), 0 = original 4:3\n"
               "autofire_period = %d  ; ZR autofire: 12 medium (5/s), 10 fast (6/s), 8 insane (7.5/s)\n"
               "rewind = %d            ; hold L3, 15 s\n"
               "no_slowdown = %d       ; slowdown reduction\n"
               "cheat_health = %d\n"
               "cheat_crystals = %d\n"
               "minimap = %d           ; R3 shows / hides it\n"
               "title_dx = %d          ; 1 = DX title screen (widescreen)\n"
               "no_respawn = %d        ; 1 = killed enemies stay dead until you leave the area\n"
               "magic_info = %d        ; 1 = magic level, spell and crystals above the HUD; window preview\n"
               "quick_magic = %d       ; 1 = hold X, up / down, release to change the magic level\n\n",
            SETTINGS_VERSION, s->crt_preset, s->color_lut, s->widescreen, s->autofire_period,
            s->rewind, s->no_slowdown, s->cheat_health, s->cheat_crystals,
            s->minimap, s->title_dx, s->no_respawn, s->magic_info, s->quick_magic);
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok) fprintf(stderr, "[arcus-options] saved %s\n", path);
    return ok;
}

unsigned arcus_main_cpu_divisor(void) { return g_arcus_settings.no_slowdown ? 2u : 1u; }

/* ---- overlay ----------------------------------------------------------------- */
enum {
    E_RESUME, E_RESET, E_SAVE, E_LOAD, E_CONTROLS, E_EXTRAS, E_CRT, E_COLOUR, E_WIDE, E_AUTOFIRE,
    E_REWIND, E_SPEED, E_RESPAWN, E_MAGIC_INFO, E_QUICK_MAGIC, E_CHEATS, E_COUNT
};

/* The cheats page (page 4). */
enum { C_HEALTH, C_CRYSTALS, C_COUNT };

static struct {
    int open;
    int sel;
    int page;              /* 0 = menu, 1 = controls, 2 = save grid, 3 = load grid, 4 = cheats,
                              5 = extras, 6 = the extras viewer, 7 = the reviews list */
    int cheat_sel;
    int controls_tab;      /* desktop controls page: 0 controller (first), 1 keyboard */
    int extras_sel, list_sel, list_kind;   /* page 7: the list of X_ART or X_REVIEWS */
    int viewer_from;       /* the page the viewer goes back to (5 or 7) */
    int confirm_reset;     /* RESET pressed once: the next A resets */
    int grid_sel;          /* 0-3: slot 1, slot 2 / slot 3, quick save */
    int grid_confirm;      /* overwriting a used slot: the next A saves */
    int dirty;
    char status[48];
    unsigned calls;
    /* scripted nav */
    int script_parsed;
    unsigned script_at[64], script_to[64];
    uint32_t script_keys[64];
    int script_n;
    int rewinding;
    unsigned rewind_phase;
    int just_opened;       /* opened by arcus_overlay_open() this iteration */
    uint32_t prev_quick;   /* L/R held last iteration (quick save/load edges) */
    uint32_t prev_map;     /* R3 held last iteration */
    int open_wanted;       /* menu pressed during play: opens at the resume point */
    unsigned open_wanted_at;
    unsigned nav_shift;    /* frames scripted nav waited for that: later entries move with it */
} O;

#define RAM_PLAYING    0xFF8292u   /* 1 during play (arcus_saves.c) */
#define RAM_NO_RESPAWN 0xFFFA13u   /* read by the ROM's enemy removal (patch/native.asm OnceTest) */

static void open_overlay(void)
{
    /* Paused now: storage writes (slow on the Switch SD card) can't stall a frame. */
    runtime_evidence_flush();
    fflush(stderr);
    O.open = 1;
    O.sel = 0;             /* Resume */
    O.page = 0;
    O.confirm_reset = 0;
    O.status[0] = 0;
#if GENESIS_PLATFORM_SWITCH
    genesis_switch_log_memory("menu");    /* how much the game uses in play */
#endif
}

/* Input routes away from the game while the overlay is open or L3 rewinds.
 * The input module asks before pause_frame runs, so a fresh − press opens the
 * overlay right here: the game gets no input on the opening frame either. */
/* During play the menu opens at the resume point (arcus_saves.c), a frame or two after the
 * press, so a save from its grid is taken there; the game runs on until then. Elsewhere it
 * opens at once. Returns 1 once it is open. */
static int open_when_resumable(void)
{
    if (!O.open_wanted) { O.open_wanted = 1; O.open_wanted_at = O.calls; }
    if (!arcus_saves_at_resume_point() && glue_peek8(RAM_PLAYING) && O.calls - O.open_wanted_at <= 20)
        return 0;
    O.open_wanted = 0;
    open_overlay();
    return 1;
}

int arcus_overlay_open(void)
{
    if (!O.open && !O.rewinding && (O.open_wanted || (arcus_input_nav_edges() & ARCUS_NAV_MENU))) {
        if (open_when_resumable()) O.just_opened = 1;
    }
    return O.open || O.rewinding;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* EXTRAS is listed only when this build embedded something to show. */
static int entry_shown(int e) { return e != E_EXTRAS || arcus_extras_books() > 0; }

static int entries_shown(void)
{
    int n = 0;
    for (int e = 0; e < E_COUNT; e++) n += entry_shown(e);
    return n;
}

static const char *const CRT_NAMES[] = { "OFF", "SCANLINES", "SCANLINES + GRILLE", "STRONG" };

static void entry_text(int e, char *label, size_t ln, char *value, size_t vn)
{
    const ArcusSettings *s = &g_arcus_settings;
    value[0] = 0;
    switch (e) {
    case E_RESUME:   snprintf(label, ln, "RESUME"); break;
    case E_RESET:    snprintf(label, ln, "RESET GAME");
                     if (O.confirm_reset) snprintf(value, vn, "PRESS A AGAIN"); break;
    case E_SAVE:     snprintf(label, ln, "SAVE STATE"); break;
    case E_LOAD:     snprintf(label, ln, "LOAD STATE"); break;
    case E_CONTROLS: snprintf(label, ln, "CONTROLS"); break;
    case E_CRT:      snprintf(label, ln, "CRT SHADER"); snprintf(value, vn, "%s", CRT_NAMES[s->crt_preset]); break;
    case E_WIDE:     snprintf(label, ln, "WIDESCREEN"); snprintf(value, vn, "%s", s->widescreen ? "16:9" : "4:3"); break;
    case E_AUTOFIRE: snprintf(label, ln, "AUTOFIRE SPEED");
                     snprintf(value, vn, "%s", s->autofire_period == 12 ? "MEDIUM" : s->autofire_period == 8 ? "INSANE" : "FAST"); break;
    case E_COLOUR:   snprintf(label, ln, "CRT COLOUR"); snprintf(value, vn, "%s", s->color_lut ? "ON" : "OFF"); break;
    case E_REWIND:   snprintf(label, ln, "REWIND (HOLD L3)"); snprintf(value, vn, "%s", s->rewind ? "ON" : "OFF"); break;
    case E_SPEED:    snprintf(label, ln, "SLOWDOWN REDUCTION"); snprintf(value, vn, "%s", s->no_slowdown ? "ON" : "OFF"); break;
    case E_RESPAWN:  snprintf(label, ln, "ENEMY RESPAWN"); snprintf(value, vn, "%s", s->no_respawn ? "OFF" : "ON"); break;
    case E_MAGIC_INFO:  snprintf(label, ln, "MAGIC INFO"); snprintf(value, vn, "%s", s->magic_info ? "ON" : "OFF"); break;
    case E_QUICK_MAGIC: snprintf(label, ln, "QUICK MAGIC"); snprintf(value, vn, "%s", s->quick_magic ? "ON" : "OFF"); break;
    case E_EXTRAS:   snprintf(label, ln, "EXTRAS"); break;
    case E_CHEATS:   snprintf(label, ln, "CHEATS");
                     if (s->cheat_health | s->cheat_crystals) snprintf(value, vn, "ON"); break;
    }
}

/* Controls page (SPEC.md section 6.1 plus the host buttons). The desktop build has two tabs
 * (LEFT / RIGHT): first a controller picture labelled by what each button does, since its pads
 * are mapped by button position whatever they're called (game/pad_art.h, made by
 * tools/gen_pad_art.py), then the keyboard (arcus_input.c). The Switch shows only its list. */
#if defined(__SWITCH__) || defined(GENESIS_SWITCH_STUBS)
#define CONTROLS_TABS 0
static const char *const CONTROLS[][2] = {
    { "LEFT STICK / D-PAD", "MOVE" },
    { "RIGHT STICK",        "AIM" },
    { "ZR",                 "AUTOFIRE" },
    { "A",                  "ATTACK (HOLD: CHARGE)" },
    { "B / ZL",             "USE MAGIC / ITEM" },
    { "Y",                  "ITEM WINDOW: OPEN / CLOSE" },
    { "A IN ITEM WINDOW",   "CHOOSE" },
    { "HOLD X",             "QUICK MAGIC" },
    { "+",                  "START / PAUSE" },
    { "L",                  "QUICK SAVE" },
    { "R",                  "QUICK LOAD" },
    { "HOLD L3",            "REWIND (WHEN ON)" },
    { "R3",                 "MINI MAP" },
    { "-",                  "CONFIG MENU" },
};
#else
#define CONTROLS_TABS 1
#include "pad_art.h"
static const char *const CONTROLS[][2] = {
    { "ARROWS",             "MOVE" },
    { "V",                  "AUTOFIRE" },
    { "C",                  "ATTACK (HOLD: CHARGE)" },
    { "X",                  "USE MAGIC / ITEM" },
    { "Z",                  "ITEM WINDOW: OPEN / CLOSE" },
    { "C IN ITEM WINDOW",   "CHOOSE" },
    { "HOLD S",             "QUICK MAGIC" },
    { "ENTER",              "START / PAUSE" },
    { "Q",                  "QUICK SAVE" },
    { "W",                  "QUICK LOAD" },
    { "HOLD BACKSPACE",     "REWIND (WHEN ON)" },
    { "M",                  "MINI MAP" },
    { "ESC",                "CONFIG MENU" },
};
#endif
#define CONTROLS_N ((int)(sizeof CONTROLS / sizeof CONTROLS[0]))

/* The dimmed paused frame and a centred panel of width *pw (300 when 0) and `ph` high. */
static void draw_box(uint32_t *buf, int w, int h, int stride, int ph, int *px, int *py, int *pw)
{
    /* Dim the paused frame. */
    for (int y = 0; y < h; y++) {
        uint32_t *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) row[x] = 0xFF000000u | ((row[x] >> 2) & 0x3F3F3Fu);
    }
    if (!*pw) *pw = 300;
    if (*pw > w) *pw = w;
    *px = (w - *pw) / 2;
    *py = (h - ph) / 2;
    if (*py < 0) *py = 0;
    gen_ovl_fill_rect(buf, stride, h, *px, *py, *pw, ph, 0xFF101830u);
    gen_ovl_stroke_rect(buf, stride, h, *px, *py, *pw, ph, 0xFF8090C0u);
}

static void draw_panel(uint32_t *buf, int w, int h, int stride, int rows, int *px, int *py, int *pw)
{
    *pw = 0;
    draw_box(buf, w, h, stride, 24 + rows * 12 + 18, px, py, pw);
}

static int *cheat_flag(int c)
{
    ArcusSettings *s = &g_arcus_settings;
    return c == C_HEALTH ? &s->cheat_health : &s->cheat_crystals;
}

static void draw_cheats(uint32_t *buf, int w, int h, int stride)
{
    static const char *const NAMES[C_COUNT] = { "INFINITE HEALTH", "INFINITE MAGIC" };
    const int line_h = 12;
    int px, py, pw;
    draw_panel(buf, w, h, stride, C_COUNT, &px, &py, &pw);
    gen_ovl_draw_text(buf, stride, h, px + 8, py + 7, "CHEATS", 0xFFFFE080u, 1);
    for (int c = 0; c < C_COUNT; c++) {
        int y = py + 24 + c * line_h;
        int sel = c == O.cheat_sel;
        if (sel) gen_ovl_fill_rect(buf, stride, h, px + 3, y - 2, pw - 6, line_h, 0xFF304878u);
        uint32_t col = sel ? 0xFFFFFFFFu : 0xFFB0B8D0u;
        char v[16];
        snprintf(v, sizeof v, sel ? "< %s >" : "%s", *cheat_flag(c) ? "ON" : "OFF");
        gen_ovl_draw_text(buf, stride, h, px + 10, y, NAMES[c], col, 1);
        gen_ovl_draw_text(buf, stride, h, px + pw - 10 - (int)strlen(v) * 8, y, v, col, 1);
    }
    gen_ovl_draw_text(buf, stride, h, px + 10, py + 24 + C_COUNT * line_h + 4, "B: BACK", 0xFF80FF80u, 1);
}

/* The extras page (page 5): the original manual (US, then Japanese), the game guide, the
 * reviews (a list, page 7), each only when this build has it. */
enum { X_MANUAL, X_MANUAL_JP, X_GUIDE, X_ART, X_REVIEWS, X_KINDS };
static const char *const X_GROUP[X_KINDS] = { "manual", "manual-jp", "guide", "art", "review" };
static const char *const X_LABEL[X_KINDS] = { "ORIGINAL MANUAL", "JAPANESE MANUAL", "MODERN MANUAL", "ARTWORK", "REVIEWS" };
/* Rows that open a list of their group's books (page 7) rather than one book. */
#define X_IS_LIST(k) ((k) == X_ART || (k) == X_REVIEWS)

static int extras_rows(int *kind)
{
    int n = 0;
    for (int k = 0; k < X_KINDS; k++)
        if (arcus_extras_find(X_GROUP[k]) >= 0) kind[n++] = k;
    return n;
}

/* The n-th book of a group, its index, or -1. */
static int group_book(const char *group, int n)
{
    for (int i = 0, k = 0; i < arcus_extras_books(); i++)
        if (!strcmp(arcus_extras_book(i)->group, group) && k++ == n) return i;
    return -1;
}

static void upper(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = 0;
}

static void draw_list_row(uint32_t *buf, int h, int stride, int px, int pw, int y, int sel,
                          const char *left, const char *right)
{
    const int line_h = 12;
    if (sel) gen_ovl_fill_rect(buf, stride, h, px + 3, y - 2, pw - 6, line_h, 0xFF304878u);
    uint32_t col = sel ? 0xFFFFFFFFu : 0xFFB0B8D0u;
    gen_ovl_draw_text(buf, stride, h, px + 10, y, left, col, 1);
    gen_ovl_draw_text(buf, stride, h, px + pw - 10 - (int)strlen(right) * 8, y, right, col, 1);
}

static void draw_extras(uint32_t *buf, int w, int h, int stride)
{
    const int line_h = 12;
    int px, py, pw, kind[X_KINDS], n = extras_rows(kind);
    draw_panel(buf, w, h, stride, n, &px, &py, &pw);
    gen_ovl_draw_text(buf, stride, h, px + 8, py + 7, "EXTRAS", 0xFFFFE080u, 1);
    for (int i = 0; i < n; i++) {
        draw_list_row(buf, h, stride, px, pw, py + 24 + i * line_h, i == O.extras_sel, X_LABEL[kind[i]], "");
    }
    gen_ovl_draw_text(buf, stride, h, px + 10, py + 24 + n * line_h + 4, "A: OPEN   B: BACK", 0xFF80FF80u, 1);
}

/* Page 7: a list of one group's books. REVIEWS: "MAGAZINE (REGION)" and the issue (book
 * detail "issue|score"); ARTWORK: the titles only (its detail is the viewer's caption). */
static void draw_list(uint32_t *buf, int w, int h, int stride)
{
    const int line_h = 12;
    const char *group = X_GROUP[O.list_kind];
    int px, py, pw, n = arcus_extras_group_count(group);
    draw_panel(buf, w, h, stride, n, &px, &py, &pw);
    gen_ovl_draw_text(buf, stride, h, px + 8, py + 7, X_LABEL[O.list_kind], 0xFFFFE080u, 1);
    for (int i = 0; i < n; i++) {
        const ArcusBook *b = arcus_extras_book(group_book(group, i));
        char left[40], issue[40] = "";
        upper(left, sizeof left, b->title);
        if (O.list_kind == X_REVIEWS) {
            upper(issue, sizeof issue, b->detail);
            char *bar = strchr(issue, '|');
            if (bar) *bar = 0;
        }
        draw_list_row(buf, h, stride, px, pw, py + 24 + i * line_h, i == O.list_sel, left, issue);
    }
    gen_ovl_draw_text(buf, stride, h, px + 10, py + 24 + n * line_h + 4, "A: OPEN   B: BACK", 0xFF80FF80u, 1);
}

/* The tab names right of the title; the open one is highlighted. */
static void draw_tabs(uint32_t *buf, int h, int stride, int px, int py, int pw)
{
#if CONTROLS_TABS
    static const char *const TABS[2] = { "CONTROLLER", "KEYBOARD" };
    int x = px + pw - 8;
    for (int t = 1; t >= 0; t--) {
        int tw = (int)strlen(TABS[t]) * 8;
        x -= tw;
        if (t == O.controls_tab) gen_ovl_fill_rect(buf, stride, h, x - 3, py + 5, tw + 6, 12, 0xFF304878u);
        gen_ovl_draw_text(buf, stride, h, x, py + 7, TABS[t], t == O.controls_tab ? 0xFFFFFFFFu : 0xFF8088A0u, 1);
        x -= 14;
    }
#else
    (void)buf; (void)h; (void)stride; (void)px; (void)py; (void)pw;
#endif
}

static void draw_controls(uint32_t *buf, int w, int h, int stride)
{
    const int line_h = 12;
    int px, py, pw;
#if CONTROLS_TABS
    if (O.controls_tab == 0) {
        static const uint32_t COL[5] = { 0, 0xFF202840u, 0xFFB0B8D0u, 0xFFF0E8C8u, 0xFF6078A8u };
        pw = PAD_ART_W + 16;
        draw_box(buf, w, h, stride, 24 + PAD_ART_H + 22, &px, &py, &pw);
        gen_ovl_draw_text(buf, stride, h, px + 8, py + 7, "CONTROLS", 0xFFFFE080u, 1);
        draw_tabs(buf, h, stride, px, py, pw);
        int ax = px + (pw - PAD_ART_W) / 2, ay = py + 24;
        for (int y = 0; y < PAD_ART_H && ay + y < h; y++)
            for (int x = 0; x < PAD_ART_W && ax + x < stride; x++) {
                int c = (pad_art[y][x / 2] >> (x & 1 ? 0 : 4)) & 0xF;
                if (c && ax + x >= 0) buf[(size_t)(ay + y) * stride + ax + x] = COL[c];
            }
        gen_ovl_draw_text(buf, stride, h, px + 10, ay + PAD_ART_H + 6, "B: BACK", 0xFF80FF80u, 1);
        return;
    }
#endif
    const char *const (*rows)[2] = CONTROLS;
    int n = CONTROLS_N;
    draw_panel(buf, w, h, stride, n, &px, &py, &pw);
    gen_ovl_draw_text(buf, stride, h, px + 8, py + 7, "CONTROLS", 0xFFFFE080u, 1);
    draw_tabs(buf, h, stride, px, py, pw);
    for (int i = 0; i < n; i++) {
        int y = py + 24 + i * line_h;
        gen_ovl_draw_text(buf, stride, h, px + 10, y, rows[i][0], 0xFFFFFFFFu, 1);
        gen_ovl_draw_text(buf, stride, h, px + pw - 10 - (int)strlen(rows[i][1]) * 8, y,
                          rows[i][1], 0xFFB0B8D0u, 1);
    }
    gen_ovl_draw_text(buf, stride, h, px + 10, py + 24 + n * line_h + 4, "B: BACK", 0xFF80FF80u, 1);
}

static void draw_overlay(uint32_t *buf, int w, int h, int stride)
{
    const int line_h = 12;
    int px, py, pw;
    if (O.page == 4) {
        draw_cheats(buf, w, h, stride);
        return;
    }
    if (O.page == 7) {
        draw_list(buf, w, h, stride);
        return;
    }
    if (O.page == 5) {
        draw_extras(buf, w, h, stride);
        return;
    }
    if (O.page == 2 || O.page == 3) {
        arcus_saves_draw_grid(buf, w, h, stride, O.page == 2 ? ARCUS_GRID_SAVE : ARCUS_GRID_LOAD,
                              O.grid_sel, O.grid_confirm, O.status);
        return;
    }
    if (O.page == 1) {
        draw_controls(buf, w, h, stride);
        return;
    }
    draw_panel(buf, w, h, stride, entries_shown(), &px, &py, &pw);
    gen_ovl_draw_text(buf, stride, h, px + 8, py + 7, "ARCUS ODYSSEY - PAUSED", 0xFFFFE080u, 1);
    for (int e = 0, row = 0; e < E_COUNT; e++) {
        if (!entry_shown(e)) continue;
        char label[32], value[40];
        entry_text(e, label, sizeof label, value, sizeof value);
        int y = py + 24 + row++ * line_h;
        int sel = e == O.sel;
        if (sel) gen_ovl_fill_rect(buf, stride, h, px + 3, y - 2, pw - 6, line_h, 0xFF304878u);
        uint32_t col = sel ? 0xFFFFFFFFu : 0xFFB0B8D0u;
        gen_ovl_draw_text(buf, stride, h, px + 10, y, label, col, 1);
        if (value[0]) {
            char v[48];
            int arrows = sel && e != E_RESUME && e != E_RESET && e != E_CONTROLS &&
                         e != E_SAVE && e != E_LOAD && e != E_CHEATS && e != E_EXTRAS;
            snprintf(v, sizeof v, arrows ? "< %s >" : "%s", value);
            int vx = px + pw - 10 - (int)strlen(v) * 8;
            gen_ovl_draw_text(buf, stride, h, vx, y, v, col, 1);
        }
    }
    if (O.status[0])
        gen_ovl_draw_text(buf, stride, h, px + 10, py + 24 + entries_shown() * line_h + 4, O.status, 0xFF80FF80u, 1);
}

static int cycle(int v, const int *vals, int n, int dir)
{
    int i = 0;
    while (i < n && vals[i] != v) i++;
    if (i == n) i = 0;
    return vals[((i + dir) % n + n) % n];
}

static void change(int e, int dir)
{
    ArcusSettings *s = &g_arcus_settings;
    switch (e) {
    case E_CRT:  s->crt_preset = (s->crt_preset + dir + 4) % 4; break;
    case E_WIDE: s->widescreen ^= 1; break;
    case E_AUTOFIRE: { static const int p[3] = { 12, 10, 8 };
                       s->autofire_period = cycle(s->autofire_period, p, 3, dir); break; }
    case E_COLOUR: s->color_lut ^= 1; break;
    case E_REWIND:
        s->rewind ^= 1;
        g_app_config.rewind_enabled = s->rewind;
        genesis_local_states_configure(arcus_data_dir(), s->rewind,
                                       g_app_config.rewind_depth, g_app_config.rewind_interval);
        break;
    case E_SPEED: s->no_slowdown ^= 1; break;
    case E_RESPAWN: s->no_respawn ^= 1; break;
    case E_MAGIC_INFO: s->magic_info ^= 1; break;
    case E_QUICK_MAGIC: s->quick_magic ^= 1; break;
    default: return;
    }
    O.dirty = 1;
    O.status[0] = 0;
}

static void close_overlay(void)
{
    O.open = 0;
    O.page = 0;
    O.confirm_reset = 0;
    if (O.dirty || s_migrated) arcus_save_settings();
    O.dirty = 0;
    s_migrated = 0;
}

static void activate(int e, int *running)
{
    char p[600];
    (void)running;
    if (e != E_RESET) O.confirm_reset = 0;
    switch (e) {
    case E_RESUME: close_overlay(); break;
    case E_RESET:
        if (!O.confirm_reset) { O.confirm_reset = 1; break; }
        if (runner_cold_reset()) { arcus_saves_on_reset(); close_overlay(); }
        else snprintf(O.status, sizeof O.status, "RESET FAILED");
        break;
    case E_SAVE:
    case E_LOAD:
        O.page = e == E_SAVE ? 2 : 3;
        O.grid_sel = 0;
        O.grid_confirm = 0;
        O.status[0] = 0;
        arcus_saves_refresh();
        break;
    case E_CONTROLS: O.page = 1; O.controls_tab = 0; break;
    case E_CHEATS:   O.page = 4; O.cheat_sel = 0; break;
    case E_EXTRAS:   O.page = 5; O.extras_sel = 0; break;
    default: change(e, +1); break;
    }
}

/* L / R during play: quick save / quick load (states/quick.bin), separate from slots 1-3.
 * R only works after an L quick save in this session (an older quick save can still be
 * loaded on purpose from the Load grid). */
static void quick_state(uint32_t held)
{
    uint32_t edges = held & ~O.prev_quick;
    O.prev_quick = held;
    if (edges & ARCUS_HB_L) {
        switch (arcus_saves_request(ARCUS_SLOT_QUICK)) {
        case ARCUS_SAVE_DONE:     arcus_video_toast("Quick saved"); break;
        case ARCUS_SAVE_QUEUED:   break;                      /* toasts when it completes */
        case ARCUS_SAVE_NOT_HERE: arcus_video_toast("Save during play"); break;
        default:                  arcus_video_toast("Quick save failed"); break;
        }
    } else if (edges & ARCUS_HB_R) {
        const char *why = NULL;
        if (!arcus_saves_quick_this_session()) arcus_video_toast("No quick save yet");
        else arcus_video_toast(arcus_saves_load(ARCUS_SLOT_QUICK, &why) ? "Quick loaded" : why);
    }
}

/* The save/load grid: d-pad moves, A acts, B back to the menu. */
static void grid_nav(uint32_t nav)
{
    int save = O.page == 2;
    if (nav & (ARCUS_NAV_LEFT | ARCUS_NAV_RIGHT)) { O.grid_sel ^= 1; O.grid_confirm = 0; O.status[0] = 0; }
    if (nav & (ARCUS_NAV_UP | ARCUS_NAV_DOWN))    { O.grid_sel ^= 2; O.grid_confirm = 0; O.status[0] = 0; }
    if (!(nav & ARCUS_NAV_OK)) return;
    int slot = O.grid_sel;
    if (save) {
        if (slot == ARCUS_SLOT_QUICK) { snprintf(O.status, sizeof O.status, "Quick save is on L"); return; }
        if (arcus_saves_slot_used(slot) && !O.grid_confirm) { O.grid_confirm = 1; return; }
        O.grid_confirm = 0;
        /* The menu opens at the resume point during play (open_when_resumable), so this
         * saves now; elsewhere it can't. */
        if (!arcus_saves_at_resume_point()) { snprintf(O.status, sizeof O.status, "Save during play"); return; }
        int ok = arcus_saves_save(slot);
        arcus_saves_refresh();
        snprintf(O.status, sizeof O.status, ok ? "Saved to slot %d" : "Save failed", slot + 1);
    } else {
        const char *why = NULL;
        if (arcus_saves_load(slot, &why)) close_overlay();          /* resume into it */
        else snprintf(O.status, sizeof O.status, "%s", why);
    }
}

#define NAV_SCRIPT_REWIND (1u << 16)   /* script only: L3 is held */
#define NAV_SCRIPT_QSAVE  (1u << 17)   /* script only: L is held */
#define NAV_SCRIPT_QLOAD  (1u << 18)   /* script only: R is held */
#define NAV_SCRIPT_MAP    (1u << 19)   /* script only: R3 is held */

static uint32_t parse_keys(const char *s, const char *end)
{
    static const struct { const char *n; uint32_t b; } K[] = {
        { "UP", ARCUS_NAV_UP }, { "DOWN", ARCUS_NAV_DOWN }, { "LEFT", ARCUS_NAV_LEFT },
        { "RIGHT", ARCUS_NAV_RIGHT }, { "OK", ARCUS_NAV_OK }, { "BACK", ARCUS_NAV_BACK },
        { "MENU", ARCUS_NAV_MENU }, { "REWIND", NAV_SCRIPT_REWIND },
        { "QSAVE", NAV_SCRIPT_QSAVE }, { "QLOAD", NAV_SCRIPT_QLOAD }, { "MAP", NAV_SCRIPT_MAP },
    };
    uint32_t m = 0;
    while (s < end) {
        const char *t = s;
        while (t < end && *t != '+') t++;
        for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
            if ((size_t)(t - s) == strlen(K[i].n) && !strncmp(s, K[i].n, (size_t)(t - s))) m |= K[i].b;
        s = t + 1;
    }
    return m;
}

static uint32_t scripted_nav(unsigned call)
{
    if (!O.script_parsed) {
        O.script_parsed = 1;
        const char *p = getenv("ARCUS_NAV_SCRIPT");
        while (p && *p && O.script_n < 64) {
            const char *colon = strchr(p, ':');
            if (!colon) break;
            const char *end = strchr(colon, ',');
            if (!end) end = colon + strlen(colon);
            char *dash;
            O.script_at[O.script_n] = (unsigned)strtoul(p, &dash, 10);
            O.script_to[O.script_n] = *dash == '-' ? (unsigned)strtoul(dash + 1, NULL, 10)
                                                   : O.script_at[O.script_n];
            O.script_keys[O.script_n++] = parse_keys(colon + 1, end);
            p = *end ? end + 1 : end;
        }
    }
    uint32_t m = 0;
    for (int i = 0; i < O.script_n; i++)
        if (call >= O.script_at[i] && call <= O.script_to[i]) m |= O.script_keys[i];
    return m;
}

static void pace(Uint64 t0)
{
    /* Vsync normally paces the present; without it (headless, vsync off) do
     * not spin: keep roughly 60 Hz. */
    Uint64 el = (SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency();
    if (el < 8) SDL_Delay((Uint32)(16 - el));
}

static void draw_rewind_badge(uint32_t *buf, int w, int h, int stride)
{
    (void)w;
    gen_ovl_fill_rect(buf, stride, h, 6, 6, 84, 14, 0xFF101830u);
    gen_ovl_draw_text(buf, stride, h, 10, 9, "<< REWIND", 0xFFFFE080u, 1);
}

/* L3 held with rewind on. Returns 1 to skip the tick, 0 to run one frame
 * from a just-restored snapshot (so the screen shows it). */
static int rewind_step(void)
{
    Uint64 t0 = SDL_GetPerformanceCounter();
    if (!O.rewinding) {
        /* This iteration's game input was sampled before we took over: skip
         * the tick; from the next one arcus_overlay_open() mutes the pads. */
        O.rewinding = 1;
        O.rewind_phase = 0;
    } else if (++O.rewind_phase % 3 == 0 && genesis_local_states_open_rewind()) {
        genesis_local_states_move(-1);              /* one snapshot older */
        if (genesis_local_states_accept()) {
            fprintf(stderr, "[arcus-rewind] restored the previous snapshot\n");
            return 0;
        }
        genesis_local_states_cancel();
    }
    arcus_video_present_paused(draw_rewind_badge);
    pace(t0);
    return 1;
}

static int pause_result(int skip)
{
    audio_pause(O.open || O.rewinding);   /* no looping sound while the game is held */
    arcus_saves_tick(!skip);              /* playtime: frames the game actually ran */
    /* The game clears RAM at boot and reset: the option is rewritten before every frame. */
    if (!skip) glue_poke8(RAM_NO_RESPAWN, (uint8_t)g_arcus_settings.no_respawn);
    if (!skip) arcus_cheats_frame();
    if (!skip) arcus_magic_frame();          /* quick magic picks, after the crystal cheat */
    return skip;
}

int arcus_options_pause_frame(int *running)
{
    arcus_video_frame_begin();
    O.calls++;
    uint32_t script = scripted_nav(O.calls - O.nav_shift);
    uint32_t nav = arcus_input_nav_edges() | (script & 0xFFFFu);
    uint32_t host = arcus_input_host_buttons(0);
    int rewind_held = (host & ARCUS_HB_LSTICK) || (script & NAV_SCRIPT_REWIND);
    if (!O.open && g_arcus_settings.rewind && rewind_held && !(nav & ARCUS_NAV_MENU)) {
        int r = rewind_step();
        return pause_result(r);
    }
    O.rewinding = 0;

    if (O.just_opened) {
        O.just_opened = 0;
        nav = 0;            /* the − edge that opened it */
    } else if (!O.open) {
        if (!((nav & ARCUS_NAV_MENU) || O.open_wanted) || !open_when_resumable()) {
            if (O.open_wanted) O.nav_shift++;   /* a scripted MENU waits; the next entries too */
            quick_state((host & (ARCUS_HB_L | ARCUS_HB_R)) |
                        ((script & NAV_SCRIPT_QSAVE) ? ARCUS_HB_L : 0) |
                        ((script & NAV_SCRIPT_QLOAD) ? ARCUS_HB_R : 0));
            uint32_t map = (host & ARCUS_HB_RSTICK) || (script & NAV_SCRIPT_MAP);
            if (map && !O.prev_map) {
                arcus_minimap_toggle();
                O.dirty = 1;          /* saved when the config menu next closes */
            }
            O.prev_map = map;
            return pause_result(0);
        }
        nav = 0;
    } else if (O.page == 1) {
        if (CONTROLS_TABS && (nav & (ARCUS_NAV_LEFT | ARCUS_NAV_RIGHT))) O.controls_tab ^= 1;
        if (nav & (ARCUS_NAV_BACK | ARCUS_NAV_OK)) O.page = 0;
        else if (nav & ARCUS_NAV_MENU) { close_overlay(); return pause_result(0); }
        nav = 0;
    } else if (O.page == 4) {
        if (nav & ARCUS_NAV_MENU) { close_overlay(); return pause_result(0); }
        if (nav & ARCUS_NAV_BACK) O.page = 0;
        if (nav & ARCUS_NAV_UP)   O.cheat_sel = (O.cheat_sel + C_COUNT - 1) % C_COUNT;
        if (nav & ARCUS_NAV_DOWN) O.cheat_sel = (O.cheat_sel + 1) % C_COUNT;
        if (nav & (ARCUS_NAV_LEFT | ARCUS_NAV_RIGHT | ARCUS_NAV_OK)) {
            *cheat_flag(O.cheat_sel) ^= 1;
            O.dirty = 1;
        }
        nav = 0;
    } else if (O.page == 5) {
        int kind[X_KINDS], n = extras_rows(kind);
        if (nav & ARCUS_NAV_MENU) { close_overlay(); return pause_result(0); }
        if (nav & ARCUS_NAV_UP)   O.extras_sel = (O.extras_sel + n - 1) % n;
        if (nav & ARCUS_NAV_DOWN) O.extras_sel = (O.extras_sel + 1) % n;
        if (nav & ARCUS_NAV_BACK) O.page = 0;
        else if (nav & ARCUS_NAV_OK) {
            if (X_IS_LIST(kind[O.extras_sel])) { O.page = 7; O.list_kind = kind[O.extras_sel]; O.list_sel = 0; }
            else {
                arcus_manual_open(arcus_extras_find(X_GROUP[kind[O.extras_sel]]));
                O.page = 6;
                O.viewer_from = 5;
            }
        }
        nav = 0;
    } else if (O.page == 7) {
        int n = arcus_extras_group_count(X_GROUP[O.list_kind]);
        if (nav & ARCUS_NAV_MENU) { close_overlay(); return pause_result(0); }
        if (nav & ARCUS_NAV_UP)   O.list_sel = (O.list_sel + n - 1) % n;
        if (nav & ARCUS_NAV_DOWN) O.list_sel = (O.list_sel + 1) % n;
        if (nav & ARCUS_NAV_BACK) O.page = 5;
        else if (nav & ARCUS_NAV_OK) {
            arcus_manual_open(group_book(X_GROUP[O.list_kind], O.list_sel));
            O.page = 6;
            O.viewer_from = 7;
        }
        nav = 0;
    } else if (O.page == 6) {
        /* The extras viewer presents its own full-resolution frames. */
        Uint64 t0 = SDL_GetPerformanceCounter();
        int r = arcus_manual_frame(nav, arcus_input_host_buttons(0) | arcus_input_host_buttons(1),
                                   arcus_input_stick_x(), arcus_input_stick_y());
        if (r < 0) { close_overlay(); return pause_result(0); }
        if (r > 0) { pace(t0); return pause_result(1); }
        O.page = O.viewer_from;
        nav = 0;
    } else if (O.page == 2 || O.page == 3) {
        if (nav & ARCUS_NAV_MENU) { close_overlay(); return pause_result(0); }
        if (nav & ARCUS_NAV_BACK) { O.page = 0; O.status[0] = 0; }
        else grid_nav(nav);
        if (!O.open) return pause_result(0);                   /* loaded: resume */
        nav = 0;
    } else if (nav & (ARCUS_NAV_MENU | ARCUS_NAV_BACK)) {
        close_overlay();
        return pause_result(0);   /* resume this very frame */
    }
    if (nav & (ARCUS_NAV_UP | ARCUS_NAV_DOWN | ARCUS_NAV_LEFT | ARCUS_NAV_RIGHT)) O.confirm_reset = 0;
    if (nav & ARCUS_NAV_UP)    do O.sel = (O.sel + E_COUNT - 1) % E_COUNT; while (!entry_shown(O.sel));
    if (nav & ARCUS_NAV_DOWN)  do O.sel = (O.sel + 1) % E_COUNT; while (!entry_shown(O.sel));
    if (nav & ARCUS_NAV_LEFT)  change(O.sel, -1);
    if (nav & ARCUS_NAV_RIGHT) change(O.sel, +1);
    if (nav & ARCUS_NAV_OK)    activate(O.sel, running);
    if (!O.open) return pause_result(*running ? 0 : 1);

    Uint64 t0 = SDL_GetPerformanceCounter();
    arcus_video_present_paused(draw_overlay);
    pace(t0);
    return pause_result(1);
}
