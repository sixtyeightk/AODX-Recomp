/*
 * arcus_options.h — settings (g_arcus_settings), settings.ini persistence and
 * the pause/settings overlay (SPEC 6.5). Track C.
 */
#pragma once
#include "arcus_host.h"

/* GameSpec.load_settings: read [arcus] from settings.ini (the engine reads its
 * own sections from the same file). The argument is the engine's path; the
 * file actually used is arcus_settings_path(). */
void arcus_options_load_settings(const char *engine_settings_path);
int  arcus_save_settings(void);

/* Directory for settings.ini and states/ (trailing slash): next to the
 * executable on the Mac, sdmc:/switch/arcus-odyssey-dx/ on the Switch. */
const char *arcus_data_dir(void);
const char *arcus_settings_path(void);
/* states/slotN.bin (slot 1..3; saves themselves live in arcus_saves.c) */
void arcus_state_path(int slot, char *buf, int n);
/* mkdir that tolerates an existing directory. */
void arcus_ensure_dir(const char *path);

/* GameSpec.main_cpu_divisor: 2 with "no slowdown", else 1 (original). */
unsigned arcus_main_cpu_divisor(void);

/* HostPresenter.pause_frame: samples arcus_input_nav_edges() once per frame,
 * opens/closes the overlay on ARCUS_NAV_MENU and, while open, runs it and
 * returns 1 (the game is paused). */
int  arcus_options_pause_frame(int *running);
