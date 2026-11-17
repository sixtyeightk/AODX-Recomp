/*
 * arcus_cheats.h — config-menu cheats (infinite health, infinite crystals).
 */
#pragma once

/* Called once before every game frame that runs: applies the cheats switched on in
 * g_arcus_settings to RAM. No-op outside gameplay. */
void arcus_cheats_frame(void);
