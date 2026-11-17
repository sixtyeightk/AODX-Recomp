/*
 * arcus_magic.h — clearer magic: the HUD readout and the magic window preview (MAGIC INFO),
 * and quick change (QUICK MAGIC: hold X, UP / DOWN, release to pick).
 */
#pragma once
#include <stdint.h>

struct ArcusHostPad;

/* Controls, from arcus_input_frame for each player while the config menu is closed: runs
 * quick change for this frame's buttons. Returns 1 while quick change owns the player's
 * controls (the game then gets no input from them). */
int  arcus_magic_input(int player, const struct ArcusHostPad *pad);
/* Before a game frame runs (pause_result): makes the picks released since the last one. */
void arcus_magic_frame(void);
/* 1 while a player's item / magic window is open (the game owns the HUD then). */
int  arcus_magic_window_open(void);
/* Something to draw on this frame. */
int  arcus_magic_visible(void);
/* Draws the readout / preview / quick change list into a presented frame (w x h, ARGB). */
void arcus_magic_draw(uint32_t *buf, int w, int h, int stride);
