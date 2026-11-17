/*
 * arcus_host.h — interfaces shared by the native host-side modules.
 *
 *   arcus_options.c  (track C) owns g_arcus_settings, the settings overlay and settings.ini
 *   arcus_video.c    (track C) owns presentation: widescreen/pillarbox, scaling, CRT shader
 *   arcus_input.c    (track D) owns controls: twin-stick, autofire, menu/navigation edges
 *   platform/switch/ (track D) owns libnx glue
 *
 * Keep this header small and stable; extend it only additively.
 */
#pragma once

#include <stdint.h>

/* ---- Settings (defined in arcus_options.c; defaults per SPEC.md §6) ------------------ */
enum { ARCUS_CRT_OFF, ARCUS_CRT_SCANLINES, ARCUS_CRT_SCANLINES_MASK, ARCUS_CRT_STRONG };

typedef struct ArcusSettings {
    int crt_preset;        /* ARCUS_CRT_*, default ARCUS_CRT_SCANLINES */
    int widescreen;        /* 1 = 16:9 gameplay view (424 px), 0 = original 4:3; default 1 */
    int autofire_period;   /* ZR autofire cycle: 12 Medium (default, 5/s), 10 Fast, 8 Insane */
    int color_lut;         /* 0 = raw palette (default), 1 = CRT colour LUT */
    int rewind;            /* 1 = on (default): hold L3 to step back up to 15 s */
    int no_slowdown;       /* slowdown reduction: 0 = off (default), 1 = extra 68K budget */
    int cheat_health;      /* cheats (arcus_cheats.c), all off by default */
    int cheat_crystals;    /* magic power crystals kept at 6 */
    int minimap;           /* 1 = mini map shown (R3 toggles), default 1 */
    int title_dx;          /* 1 = DX title (widened, DX mark; widescreen only), default 1 */
    int no_respawn;        /* 1 = killed enemies stay dead in the area (patch/native.asm OnceTest), default 0 */
    int magic_info;        /* 1 = magic readout above the HUD + magic window preview (arcus_magic.c), default 1 */
    int quick_magic;       /* 1 = hold X to change the magic level (arcus_magic.c), default 1 */
} ArcusSettings;

extern ArcusSettings g_arcus_settings;

/* ---- Host input events (defined in arcus_input.c) ------------------------------------- */
/* Navigation edges for the overlay, sampled once per frame. While the overlay is open the
 * game receives no input. */
enum {
    ARCUS_NAV_UP     = 1u << 0,
    ARCUS_NAV_DOWN   = 1u << 1,
    ARCUS_NAV_LEFT   = 1u << 2,
    ARCUS_NAV_RIGHT  = 1u << 3,
    ARCUS_NAV_OK     = 1u << 4,   /* Switch A (right face button) / keyboard Enter */
    ARCUS_NAV_BACK   = 1u << 5,   /* Switch B (bottom face button) / keyboard Escape */
    ARCUS_NAV_MENU   = 1u << 6,   /* Switch − / keyboard Tab-free key (F1): open/close overlay */
};
uint32_t arcus_input_nav_edges(void);
/* Re-sample the controllers and recompute the navigation edges. The runner
 * already does this once per frame; call it once per iteration of a modal loop
 * that runs instead of the frame loop. */
void arcus_input_poll(void);

/* Overlay state (defined in arcus_options.c): input routes to the overlay while open. */
int arcus_overlay_open(void);
