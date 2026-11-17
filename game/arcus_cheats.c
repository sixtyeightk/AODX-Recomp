/*
 * arcus_cheats.c — config-menu cheats, applied as RAM writes once per game frame.
 *
 * The published Game Genie codes (gamehacking.org game 14742) patch ROM branches, which a
 * static recompile has already baked into C. Their Action Replay forms rewrite RAM every
 * frame instead, and those work unchanged here:
 *   - infinite health: life gauge $8212 + p, refilled to the max $8214 + p (FF8212:000C).
 *     The codes call this "infinite energy".
 *   - infinite magic: the magic power crystal stock $8238 + p kept at 6 (the most the game
 *     lets a player carry: pickups stop at 6, $E5A6), so changing level is free; and, while
 *     a level is set ($8236 + p = 1..5), the charges $823A + p (the HUD's M) held at 99. In
 *     the item / magic window choosing level N spends N crystals, sets the level and the
 *     charges from the table at $1E556; each cast takes a charge. After one does, the cheat
 *     puts 99 back and calls the game's HUD redraw $1CD98 (glue_run_guest_routine) so the M
 *     shows it; the HUD prints two decimal digits ($1CEBE). Not while the item window is
 *     open (nor for 8 frames after it last looked open): the game owns the HUD then, and its
 *     pick is topped up once it closes. No charges
 *     without a level: the published FF823A code does that, and casts then loaded "level 0"
 *     magic graphics and drew garbage. Switching it off empties the stock (0) and gives the
 *     chosen spell its normal charges for its level from the table.
 * Only while a level is in play (not the attract demo) and only for living players, so a
 * dead player stays dead and menus that reuse this RAM as scratch are untouched.
 */
#include "arcus_cheats.h"
#include "arcus_host.h"
#include "genesis_host_mem.h"
#include "genesis_machine.h"
#include "arcus_video.h"
#include "arcus_magic.h"
#include "genesis_runtime.h"

#define RAM_HEALTH     0xFF8212u   /* byte per player */
#define RAM_HEALTH_MAX 0xFF8214u
#define RAM_CRYSTALS   0xFF8238u   /* byte per player: magic power crystal stock */
#define CRYSTALS_MAX   6
#define RAM_CHAR       0xFF8218u
#define RAM_LEVEL      0xFF8236u
#define RAM_CASTS      0xFF823Au   /* byte per player: charges left (the HUD's M) */
#define CASTS_INFINITE 99
#define ROM_CASTS      0x01E556u   /* charges per level, 5 bytes per character */
#define HUD_REDRAW     0x01CD98u
#define SPELL_GFX      0x01BC9Eu   /* reloads the spell graphics (skipped by the game at 0 casts) */
#define RAM_PLAYERS    0xFF8284u
#define RAM_DEMO       0xFF8089u

/* The player's spell level, or 0 if none is set. */
static int level(int p)
{
    int lv = glue_peek8(RAM_LEVEL + p);
    return glue_peek8(RAM_CHAR + p) < 4 && lv >= 1 && lv <= 5 ? lv : 0;
}

void arcus_cheats_frame(void)
{
    static int crystals_was_on;
    const ArcusSettings *s = &g_arcus_settings;
    int crystals_off_now = crystals_was_on && !s->cheat_crystals;
    crystals_was_on = s->cheat_crystals;
    if (!(s->cheat_health | s->cheat_crystals | crystals_off_now)) return;
    if (!arcus_gameplay_view(&g_machine.vdp) || glue_peek8(RAM_DEMO)) return;
    int players = glue_peek8(RAM_PLAYERS) == 2 ? 2 : 1, hud = 0, gfx = 0;
    /* The window check misses the one frame the window spends handling a pick (the stack
     * holds other calls then): the window counts as open until it has looked closed for 8. */
    static int window_hold;
    window_hold = arcus_magic_window_open() ? 8 : window_hold > 0 ? window_hold - 1 : 0;
    int window = window_hold > 0;
    for (int p = 0; p < players; p++) {
        int lv = level(p);
        if (crystals_off_now) {
            glue_poke8(RAM_CRYSTALS + p, 0);
            if (lv) {
                glue_poke8(RAM_CASTS + p, glue_peek8(ROM_CASTS + glue_peek8(RAM_CHAR + p) * 5 + lv - 1));
                hud = 1;
            }
        }
        uint8_t hp = glue_peek8(RAM_HEALTH + p);
        if (!hp) continue;
        if (s->cheat_health) {
            uint8_t max = glue_peek8(RAM_HEALTH_MAX + p);
            if (max && hp < max) glue_poke8(RAM_HEALTH + p, max);
        }
        if (s->cheat_crystals) {
            glue_poke8(RAM_CRYSTALS + p, CRYSTALS_MAX);
            if (lv && !window && glue_peek8(RAM_CASTS + p) != CASTS_INFINITE) {
                /* From 0 casts the game hasn't loaded the spell's graphics (the window's close
                 * skips them then): load them, as a pick would. */
                if (!glue_peek8(RAM_CASTS + p)) gfx = 1;
                glue_poke8(RAM_CASTS + p, CASTS_INFINITE);
                hud = 1;
            }
        }
    }
    if (hud && !window) glue_run_guest_routine(HUD_REDRAW);
    if (gfx && !window) glue_run_guest_routine(SPELL_GFX);
}
