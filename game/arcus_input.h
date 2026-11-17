/*
 * arcus_input.h — Arcus Odyssey controls (SPEC.md §6.1), host side.
 *
 * Every platform reads its controllers into an ArcusHostPad (Switch-labelled
 * buttons, sticks in -1..1 with y up). One mapping turns that into the
 * Genesis 6-button pad, so the Switch (libnx pad), the Mac (SDL
 * GameController + keyboard) and the test injection all share it:
 *
 *   left stick / d-pad -> d-pad          right stick -> Z up, Y down, X left, Mode right
 *   A (right)  -> C                      B (bottom), ZL -> B
 *   Y (left) -> A                        X, L, R -> nothing (L / R: quick save / load)
 *   +  -> Start                          -  -> ARCUS_NAV_MENU (no game input)
 *   ZR -> autofire C: pressed 2 frames, released period-2, from the frame ZR goes down
 *   In the item window: A (attack) -> A (choose),
 *   Y (item) -> C (close), ZR nothing; a face button held as it opens or closes is
 *   ignored until released.
 *
 * Each stick axis is active above ARCUS_STICK_THRESHOLD of full deflection,
 * which gives 8 equal 45-degree sectors. The runner reaches this module
 * through GameSpec.input_frame / GameSpec.input_mask (arcus_spec.c).
 *
 * Test hooks (environment variables, inert when unset):
 *   ARCUS_INPUT_INJECT=<file>   scripted host-controller states, see arcus_input.c
 *   ARCUS_INPUT_TRACE=<file>    per-frame log: frame, both Genesis masks, nav edges,
 *                               and the RAM bytes listed in ARCUS_INPUT_TRACE_ADDRS
 *                               (comma-separated hex 68K addresses); lines start at
 *                               ARCUS_INPUT_TRACE_FROM (wall frame, default 0)
 *   ARCUS_INPUT_AUTOFIRE_PERIOD=<n>  overrides g_arcus_settings.autofire_period
 *   ARCUS_INPUT_PAD_TYPE=3      report 3-button pads (original-controls reference)
 */
#pragma once

#include <stdint.h>

#define ARCUS_STICK_THRESHOLD 0.38f

/* Host buttons, by Switch label (position: A right, B bottom, X top, Y left). */
enum {
    ARCUS_HB_A      = 1u << 0,
    ARCUS_HB_B      = 1u << 1,
    ARCUS_HB_X      = 1u << 2,
    ARCUS_HB_Y      = 1u << 3,
    ARCUS_HB_L      = 1u << 4,
    ARCUS_HB_R      = 1u << 5,
    ARCUS_HB_ZL     = 1u << 6,
    ARCUS_HB_ZR     = 1u << 7,
    ARCUS_HB_PLUS   = 1u << 8,
    ARCUS_HB_MINUS  = 1u << 9,
    ARCUS_HB_UP     = 1u << 10,
    ARCUS_HB_DOWN   = 1u << 11,
    ARCUS_HB_LEFT   = 1u << 12,
    ARCUS_HB_RIGHT  = 1u << 13,
    ARCUS_HB_LSTICK = 1u << 14,   /* left stick click (L3): hold to rewind */
    ARCUS_HB_RSTICK = 1u << 15,   /* right stick click (R3): mini map */
};

typedef struct ArcusHostPad {
    uint32_t buttons;          /* ARCUS_HB_* */
    float lx, ly, rx, ry;      /* -1..1, +x right, +y up */
    int connected;
} ArcusHostPad;

/* Per-player autofire state (ZR). */
typedef struct ArcusAutofire {
    int held;                  /* ZR was down last frame */
    uint32_t phase;            /* frames since ZR went down */
} ArcusAutofire;

/* Genesis pad bits (genesis_bus.h GPAD_* layout). */
enum {
    ARCUS_GP_UP = 0x0001, ARCUS_GP_DOWN = 0x0002, ARCUS_GP_LEFT = 0x0004, ARCUS_GP_RIGHT = 0x0008,
    ARCUS_GP_B = 0x0010, ARCUS_GP_C = 0x0020, ARCUS_GP_A = 0x0040, ARCUS_GP_START = 0x0080,
    ARCUS_GP_X = 0x0100, ARCUS_GP_Y = 0x0200, ARCUS_GP_Z = 0x0400, ARCUS_GP_MODE = 0x0800,
};

/* The mapping above for one frame; advances the autofire state. */
uint16_t arcus_input_map(const ArcusHostPad *pad, ArcusAutofire *af, int autofire_period);

/* GameSpec hooks. load_settings: 6-button pads on both ports unless
 * settings.ini sets pad_type (runs after the runner loaded settings.ini). */
void     arcus_input_load_settings(const char *settings_path);
void     arcus_input_frame(uint64_t wall_frame);
uint16_t arcus_input_mask(int player);

/* This frame's sticks of a player as v = { lx, ly, rx, ry } (-1..1, +y up): the raw host
 * sticks, the left one from the d-pad when the stick is centred (it moves the same way).
 * For the stick display (ARCUS_SHOW_STICKS, arcus_video.c). Returns 1 if connected. */
int arcus_input_sticks(int player, float v[4]);
/* Raw host buttons (ARCUS_HB_*) of the latest sample. */
uint32_t arcus_input_host_buttons(int player);
/* The strongest vertical deflection of any stick of either player (-1..1, up > 0). */
float arcus_input_stick_y(void);
float arcus_input_stick_x(void);   /* the same, horizontal (right > 0) */

/* Wall frame of the latest arcus_input_frame/arcus_input_poll sample. */
uint64_t arcus_input_frame_number(void);
