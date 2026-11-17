/*
 * arcus_saves.c — save states with metadata and the 2x2 save/load grid (see arcus_saves.h).
 */
#include "arcus_saves.h"
#include "arcus_options.h"
#include "arcus_video.h"

#include "game_spec.h"
#include "overlay_draw.h"
#include "runtime_evidence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__SWITCH__) && !defined(GENESIS_SWITCH_STUBS)
#include <switch.h>
#endif

/* runner (main.c / glue.c) */
int runner_save_state_file(const char *path);
int runner_load_state_file(const char *path);
uint8_t glue_peek8(uint32_t addr);
#include "genesis_runtime.h"   /* g_cpu */

#define TW 128
#define TH 68
#define RAM_PLAYING 0xFF8292u
#define RAM_AREA    0xFF828Eu

typedef struct {
    char     magic[8];          /* "ARCSTM1" */
    uint32_t rom_crc, build_fp;
    uint32_t playtime;          /* frames of gameplay */
    uint32_t reserved;
    int64_t  saved_at;          /* UTC seconds */
    uint8_t  area, act, pad[6];
    uint32_t thumb[TW * TH];    /* ARGB */
} Meta;

static const char MAGIC[8] = "ARCSTM1";

static struct {
    uint32_t playtime;
    int quick_session;
    int pending_slot, pending_frames;   /* a save waiting for the resume point, or -1 */
    int loaded;                 /* metas cached */
    int used[ARCUS_SLOT_COUNT], valid[ARCUS_SLOT_COUNT];
    Meta meta[ARCUS_SLOT_COUNT];
} S = { .pending_slot = -1 };

/* ---- identity --------------------------------------------------------------------- */
static uint32_t build_fp(void) { return genesis_build_exe_fingerprint(); }

/* Acts start at areas 00 03 05 18 1C 21 2A 32 (notes/re-findings.md). */
static int act_of(int area)
{
    static const uint8_t starts[8] = { 0x00, 0x03, 0x05, 0x18, 0x1C, 0x21, 0x2A, 0x32 };
    int act = 1;
    for (int i = 0; i < 8; i++) if (area >= starts[i]) act = i + 1;
    return act;
}

/* ---- paths: relative for the runner, absolute for our own files -------------------- */
static void names(int slot, char *rel, size_t rn, char *meta_abs, size_t mn, char *state_abs, size_t sn)
{
    char base[32];
    if (slot == ARCUS_SLOT_QUICK) snprintf(base, sizeof base, "quick");
    else snprintf(base, sizeof base, "slot%d", slot + 1);
    if (rel) snprintf(rel, rn, "states/%s.bin", base);
    if (meta_abs) snprintf(meta_abs, mn, "%sstates/%s.meta", arcus_data_dir(), base);
    if (state_abs) snprintf(state_abs, sn, "%sstates/%s.bin", arcus_data_dir(), base);
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int read_meta(int slot, Meta *m)
{
    char mp[600];
    names(slot, NULL, 0, mp, sizeof mp, NULL, 0);
    FILE *f = fopen(mp, "rb");
    if (!f) return 0;
    int ok = fread(m, 1, sizeof *m, f) == sizeof *m && !memcmp(m->magic, MAGIC, sizeof MAGIC);
    fclose(f);
    return ok;
}

static int stamp_ok(const Meta *m)
{
    return m->rom_crc == g_game_spec.expected_rom_crc32 && m->build_fp == build_fp();
}

static void pending_tick(void);

/* ---- playtime ------------------------------------------------------------------------ */
void arcus_saves_tick(int game_ticked)
{
    if (game_ticked) pending_tick();
    if (arcus_video_on_title()) S.playtime = 0;          /* a new game starts from here */
    else if (game_ticked && glue_peek8(RAM_PLAYING)) S.playtime++;
}

void arcus_saves_on_reset(void)
{
    S.playtime = 0;
    S.quick_session = 0;
    S.pending_slot = -1;
}

int arcus_saves_quick_this_session(void) { return S.quick_session; }

/* ---- the resume point ------------------------------------------------------------------
 * A state holds RAM, the CPU registers and the chips, but not the native call stack of the
 * recompiled game. So a state is only taken while the game waits for VBlank in its gameplay
 * frame loop (main program, $6C7E-$741A, running on the reset stack $FFFC00), and a load
 * restarts the game there on a fresh stack (save_resume_pc, arcus_spec.c), whatever it was
 * doing before. The loop waits twice per pass: JSR $1A9E at $6D3C and at $6DF8. Parked in
 * that leaf, the top of the 68K stack is its return address ($6D42 / $6DFE).
 *
 * The load resumes AT the JSR, not after it: a frame can end with the wait in either of
 * its two loops (waiting for VBlank to start, or still in VBlank waiting for it to end).
 * Running the wait again polls the restored VDP and finishes it exactly as it would have;
 * resuming after it skipped the rest of the wait and ran a loop pass one frame early.
 * game.toml seeds both JSRs (and $6C7E/$6C86) as functions so the game can enter there. */
static const struct { uint32_t ret, resume; } WAITS[] = {
    { 0x006D42u, 0x006D3Cu },
    { 0x006DFEu, 0x006DF8u },
};

uint32_t arcus_saves_resume_pc(void)
{
    uint32_t sp = g_cpu.A[7] & 0xFFFFFFu;
    if (sp < 0xFF0000u || sp > 0xFFFBFCu) return 0;
    uint32_t ret = 0;
    for (int i = 0; i < 4; i++) ret = (ret << 8) | glue_peek8(sp + (uint32_t)i);
    for (size_t i = 0; i < sizeof WAITS / sizeof WAITS[0]; i++)
        if (ret == WAITS[i].ret) return WAITS[i].resume;
    return 0;
}

int arcus_saves_at_resume_point(void) { return arcus_saves_resume_pc() != 0; }

/* ---- save / load -------------------------------------------------------------------- */
static int save_now(int slot);

int arcus_saves_request(int slot)
{
    if (arcus_saves_at_resume_point()) return save_now(slot) ? ARCUS_SAVE_DONE : ARCUS_SAVE_FAILED;
    if (!glue_peek8(RAM_PLAYING)) return ARCUS_SAVE_NOT_HERE;  /* title, act card, map... */
    S.pending_slot = slot;                                      /* a frame or two away */
    S.pending_frames = 0;
    return ARCUS_SAVE_QUEUED;
}

/* Once per frame, before the game runs: a queued save completes at the next resume point. */
static void pending_tick(void)
{
    if (S.pending_slot < 0) return;
    int slot = S.pending_slot;
    if (arcus_saves_at_resume_point()) {
        S.pending_slot = -1;
        int ok = save_now(slot);
        if (slot == ARCUS_SLOT_QUICK) arcus_video_toast(ok ? "Quick saved" : "Quick save failed");
    } else if (++S.pending_frames > 30) {                    /* paused, a door, a cut-scene */
        S.pending_slot = -1;
        arcus_video_toast("Can't save here");
    }
}

int arcus_saves_save(int slot) { return arcus_saves_request(slot) == ARCUS_SAVE_DONE; }

static int save_now(int slot)
{
    char rel[64], mp[600], dir[600];
    snprintf(dir, sizeof dir, "%sstates", arcus_data_dir());
    arcus_ensure_dir(dir);
    names(slot, rel, sizeof rel, mp, sizeof mp, NULL, 0);
    if (!runner_save_state_file(rel)) return 0;
    static Meta m;                                       /* 35 KB: not on the stack */
    memset(&m, 0, sizeof m);
    memcpy(m.magic, MAGIC, sizeof MAGIC);
    m.rom_crc = g_game_spec.expected_rom_crc32;
    m.build_fp = build_fp();
    m.playtime = S.playtime;
    m.saved_at = (int64_t)time(NULL);
    m.area = glue_peek8(RAM_AREA);
    m.act = (uint8_t)act_of(m.area);
    arcus_video_thumbnail(m.thumb, TW, TH);
    FILE *f = fopen(mp, "wb");
    int ok = f && fwrite(&m, 1, sizeof m, f) == sizeof m;
    if (f && fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "[arcus-saves] could not write %s\n", mp);
    if (slot == ARCUS_SLOT_QUICK && ok) S.quick_session = 1;
    S.loaded = 0;
    return ok;
}

int arcus_saves_load(int slot, const char **why)
{
    char rel[64], sp[600];
    names(slot, rel, sizeof rel, NULL, 0, sp, sizeof sp);
    static Meta m;
    if (!file_exists(sp)) { *why = "Empty"; return 0; }
    if (!read_meta(slot, &m) || !stamp_ok(&m)) { *why = "Saved by another version"; return 0; }
    if (!runner_load_state_file(rel)) { *why = "Load failed"; return 0; }
    S.playtime = m.playtime;
    return 1;
}

/* ---- grid ------------------------------------------------------------------------------ */
void arcus_saves_refresh(void)
{
    for (int i = 0; i < ARCUS_SLOT_COUNT; i++) {
        char sp[600];
        names(i, NULL, 0, NULL, 0, sp, sizeof sp);
        S.used[i] = file_exists(sp);
        S.valid[i] = S.used[i] && read_meta(i, &S.meta[i]) && stamp_ok(&S.meta[i]);
        if (!S.valid[i] && S.used[i] && !read_meta(i, &S.meta[i])) memset(&S.meta[i], 0, sizeof(Meta));
    }
    S.loaded = 1;
}

int arcus_saves_slot_valid(int slot) { if (!S.loaded) arcus_saves_refresh(); return S.valid[slot]; }
int arcus_saves_slot_used(int slot)  { if (!S.loaded) arcus_saves_refresh(); return S.used[slot]; }

static void fmt_playtime(uint32_t frames, char *out, size_t n)
{
    uint32_t s = frames / 60;
    snprintf(out, n, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}

static void fmt_date(int64_t t, char *out, size_t n)
{
    static const char *const M[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int mon = 0, day = 1, hour = 0, min = 0;
#if defined(__SWITCH__) && !defined(GENESIS_SWITCH_STUBS)
    TimeCalendarTime ct;
    TimeCalendarAdditionalInfo info;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule((u64)t, &ct, &info))) {
        mon = ct.month - 1; day = ct.day; hour = ct.hour; min = ct.minute;
    }
#else
    time_t tt = (time_t)t;
    struct tm lt;
    if (localtime_r(&tt, &lt)) { mon = lt.tm_mon; day = lt.tm_mday; hour = lt.tm_hour; min = lt.tm_min; }
#endif
    snprintf(out, n, "%s %d %02d:%02d", M[(mon % 12 + 12) % 12], day, hour, min);
}

static void dim_rect(uint32_t *buf, int stride, int h, int x, int y, int w, int hh)
{
    for (int yy = y; yy < y + hh && yy < h; yy++)
        for (int xx = x; xx < x + w && xx < stride; xx++) {
            if (yy < 0 || xx < 0) continue;
            uint32_t *p = buf + (size_t)yy * stride + xx;
            *p = 0xFF000000u | ((*p >> 1) & 0x7F7F7Fu);
        }
}

static void outline(uint32_t *buf, int stride, int h, int x, int y, int w, int hh, uint32_t col)
{
    gen_ovl_fill_rect(buf, stride, h, x, y, w, 1, col);
    gen_ovl_fill_rect(buf, stride, h, x, y + hh - 1, w, 1, col);
    gen_ovl_fill_rect(buf, stride, h, x, y, 1, hh, col);
    gen_ovl_fill_rect(buf, stride, h, x + w - 1, y, 1, hh, col);
}

void arcus_saves_draw_grid(uint32_t *buf, int w, int h, int stride, int mode, int sel,
                           int confirm, const char *status)
{
    if (!S.loaded) arcus_saves_refresh();
    /* Darken the paused frame, then a 50% panel. */
    for (int y = 0; y < h; y++) {
        uint32_t *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++) row[x] = 0xFF000000u | ((row[x] >> 2) & 0x3F3F3Fu);
    }
    const int gap = 10, tile_w = TW, tile_h = TH + 22;
    const int grid_w = 2 * tile_w + gap, grid_h = 2 * tile_h + 6;
    int x0 = (w - grid_w) / 2, y0 = (h - grid_h - 14) / 2 + 14;
    const char *title = mode == ARCUS_GRID_SAVE ? "Save state" : "Load state";
    arcus_small_text(buf, stride, h, x0, y0 - 13, title, 0xFFFFE080u);
    const char *hint = "A: select   B: back";
    arcus_small_text(buf, stride, h, x0 + grid_w - arcus_small_text_width(hint), y0 - 13, hint, 0xFF9098B0u);

    for (int i = 0; i < ARCUS_SLOT_COUNT; i++) {
        int tx = x0 + (i % 2) * (tile_w + gap), ty = y0 + (i / 2) * (tile_h + 6);
        int greyed = mode == ARCUS_GRID_SAVE && i == ARCUS_SLOT_QUICK;
        const Meta *m = &S.meta[i];
        /* thumbnail area */
        if (S.used[i] && m->magic[0]) {
            for (int y = 0; y < TH; y++)
                for (int x = 0; x < TW; x++)
                    if (ty + y >= 0 && ty + y < h && tx + x < stride)
                        buf[(size_t)(ty + y) * stride + tx + x] = m->thumb[y * TW + x];
            if (!S.valid[i] || greyed) dim_rect(buf, stride, h, tx, ty, TW, TH);
        } else {
            gen_ovl_fill_rect(buf, stride, h, tx, ty, TW, TH, 0xFF181C28u);
        }
        const char *center = !S.used[i] ? "Empty" : !S.valid[i] ? "Old version" : NULL;
        if (center)
            arcus_small_text(buf, stride, h, tx + (TW - arcus_small_text_width(center)) / 2,
                             ty + TH / 2 - 4, center, 0xFFB0B8D0u);
        /* captions */
        dim_rect(buf, stride, h, tx, ty + TH, TW, 21);
        char name[24], act[16], play[24], date[24];
        if (i == ARCUS_SLOT_QUICK) snprintf(name, sizeof name, "Quick save");
        else snprintf(name, sizeof name, "Slot %d", i + 1);
        uint32_t col = greyed ? 0xFF606878u : (i == sel ? 0xFFFFFFFFu : 0xFFC8D0E0u);
        arcus_small_text(buf, stride, h, tx + 3, ty + TH + 2, name, col);
        if (S.used[i] && m->magic[0]) {
            snprintf(act, sizeof act, "Act %d", m->act ? m->act : 1);
            arcus_small_text(buf, stride, h, tx + TW - 3 - arcus_small_text_width(act), ty + TH + 2, act, col);
            fmt_playtime(m->playtime, play, sizeof play);
            fmt_date(m->saved_at, date, sizeof date);
            arcus_small_text(buf, stride, h, tx + 3, ty + TH + 12, play, col);
            arcus_small_text(buf, stride, h, tx + TW - 3 - arcus_small_text_width(date), ty + TH + 12, date, col);
        }
        if (i == sel) outline(buf, stride, h, tx - 1, ty - 1, TW + 2, tile_h + 1,
                              confirm ? 0xFFFFC060u : 0xFFE0E8FFu);
    }
    const char *msg = status && status[0] ? status
                    : confirm ? "Press A again to overwrite" : NULL;
    if (msg)
        arcus_small_text(buf, stride, h, x0 + (grid_w - arcus_small_text_width(msg)) / 2,
                         y0 + grid_h + 2, msg, 0xFF80FF80u);
}
