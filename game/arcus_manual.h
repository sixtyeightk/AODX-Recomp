/*
 * arcus_manual.h — the extras viewer: a book from arcus_extras.h (the original manual, the
 * game guide, a magazine review) at the screen's full resolution.
 */
#pragma once
#include <stdint.h>

/* Open the viewer on a book (arcus_extras_book index), at its start. */
void arcus_manual_open(int book);
/* One frame of the open viewer, from the paused config menu: nav = ARCUS_NAV_* edges (OK
 * zooms a page book), held = host buttons held by either player (L / R turn pages or move a
 * screen; the D-pad scrolls / moves a zoomed page), stick_x / stick_y = the strongest stick
 * deflections (-1..1, right / up > 0; they move too). Presents the frame. Returns 0 once B
 * closed it, -1 when − closed the whole menu. */
int  arcus_manual_frame(uint32_t nav, uint32_t held, float stick_x, float stick_y);
