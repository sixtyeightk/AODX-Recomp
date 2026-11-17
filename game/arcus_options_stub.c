/*
 * arcus_options_stub.c — temporary stand-in for Track C's arcus_options.c.
 *
 * Compiled only while game/arcus_options.c does not exist (see CMakeLists.txt).
 * Provides the SPEC.md §6 defaults and a settings overlay that is never open,
 * so the input module links and behaves as in normal play.
 */
#include "arcus_host.h"
#include "arcus_input.h"

ArcusSettings g_arcus_settings = {
    .crt_preset      = ARCUS_CRT_SCANLINES,
    .widescreen      = 1,
    .autofire_period = 12,
    .color_lut       = 0,
    .rewind          = 1,
    .no_slowdown     = 0,
};

/* Stand-in overlay state: − (ARCUS_NAV_MENU) toggles it, nothing is drawn.
 * While "open" the game receives no input, as with the real overlay. */
int arcus_overlay_open(void)
{
    static int open;
    static uint64_t last_frame = UINT64_MAX;
    uint64_t f = arcus_input_frame_number();
    if (f != last_frame) {
        last_frame = f;
        if (arcus_input_nav_edges() & ARCUS_NAV_MENU) open = !open;
    }
    return open;
}
