/*
 * arcus_minimap.h — the mini map (R3): the current area's shape, revealed as you explore.
 */
#pragma once
#include <stdint.h>

/* Before each game frame (pre-raster): rebuilds the area's cell map when needed and marks
 * what the view shows as seen. */
void arcus_minimap_frame(void);
/* R3: show / hide (kept in g_arcus_settings.minimap). */
void arcus_minimap_toggle(void);
/* Shown this frame (switched on, a level in play). */
int  arcus_minimap_visible(void);
/* Draws it into the top-right corner of a presented frame (w x h, ARGB). */
void arcus_minimap_draw(uint32_t *buf, int w, int h, int stride);
