/*
 * arcus_saves.h — save states with metadata (thumbnail, act, playtime, date) and the 2x2
 * save/load grid of the config menu.
 *
 * Slots: 0-2 = "Slot 1-3" (states/slotN.bin), 3 = the quick save (states/quick.bin, L/R).
 * Each state has a sidecar states/<name>.meta holding the build stamp (ROM CRC32 + build
 * fingerprint), the playtime, the area/act, the local date and a 128x68 thumbnail. A state
 * whose stamp doesn't match the running build (or that has no .meta) is refused: a saved
 * state is a snapshot of one exact build's internals.
 */
#pragma once

#include <stdint.h>

enum { ARCUS_SLOT_QUICK = 3, ARCUS_SLOT_COUNT = 4 };
enum { ARCUS_GRID_SAVE = 0, ARCUS_GRID_LOAD = 1 };

/* Once per runner iteration: game_ticked = the game advanced a frame (not paused). Counts
 * playtime during gameplay and resets it on the title screen. */
void arcus_saves_tick(int game_ticked);
/* Menu Reset: forget this session's quick save and the playtime. */
void arcus_saves_on_reset(void);

/* Save / load a slot (0-3). Load returns 0 with *why set ("Empty", "Saved by another
 * version", ...) when refused.
 *
 * A state is only taken at the gameplay frame loop's VBlank wait (the resume point; see
 * arcus_saves.c). arcus_saves_request saves now when the game is there; during play it
 * queues the save for the next one (a frame or two; a toast reports it for the quick save);
 * elsewhere (title, act card, map) it refuses. arcus_saves_save = saved right now. */
enum { ARCUS_SAVE_FAILED = 0, ARCUS_SAVE_DONE = 1, ARCUS_SAVE_QUEUED = 2, ARCUS_SAVE_NOT_HERE = 3 };
int arcus_saves_request(int slot);
int arcus_saves_save(int slot);
/* 1 while the game is parked at the resume point. */
int arcus_saves_at_resume_point(void);
/* The PC a load of the current machine state resumes at (the return point of the wait the
 * state was taken in), or 0. Read after the load has restored RAM and registers. */
uint32_t arcus_saves_resume_pc(void);
int arcus_saves_load(int slot, const char **why);
/* The R button: only after an L quick save in this session. */
int arcus_saves_quick_this_session(void);

/* The grid: re-read the four .meta files (call when the page opens and after a save), then
 * draw it. sel = 0-3 (row-major: slot 1, slot 2 / slot 3, quick). */
void arcus_saves_refresh(void);
void arcus_saves_draw_grid(uint32_t *buf, int w, int h, int stride, int mode, int sel,
                           int confirm, const char *status);
/* 1 if the slot holds a state from this build. */
int arcus_saves_slot_valid(int slot);
/* 1 if the slot has any state file (valid or not). */
int arcus_saves_slot_used(int slot);
