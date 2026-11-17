/*
 * arcus_title_dx.h — the "Arcus Odyssey DX" title: widened from 4:3 to the canvas, DX mark.
 */
#pragma once
#include <stdint.h>

/* For each presented frame: on the title (on_title) with widescreen on, writes the frame
 * widened and marked into dst and returns 1; otherwise returns 0 (dst untouched) and
 * restarts the animation for the next visit. */
int arcus_title_dx_compose(int on_title, const uint32_t *src, int w, int h, int src_stride,
                           uint32_t *dst, int dst_stride);
