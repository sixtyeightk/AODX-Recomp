/*
 * arcus_spec.c — GameSpec for Arcus Odyssey (Wolf Team, 1991), USA NTSC,
 * recompiled from our patched ROM (../build/arcus-native.bin).
 *
 * Vectors: reset $00020E, V-int $0003D8, H-int $0003DA (see game.toml).
 * Dispatchers use recomp_call_addr(), which routes through the generated
 * dispatch table and the Tier-3 interpreter floor on a miss.
 */
#include "game_spec.h"
#include "genesis_runtime.h"
#include "arcus_input.h"
#include "audio.h"
#include "runtime_evidence.h"
#include "arcus_options.h"   /* track C: settings, overlay, game speed */
#include "arcus_video.h"     /* track C: widescreen view, presenter */
#include "arcus_saves.h"     /* save-state resume point */
#include "host_present.h"

#include <stddef.h>
#include <stdint.h>

/* game/rom_embed.S: the ROM linked into the executable. */
extern const uint8_t arcus_rom_data[];

/* After the runner read settings.ini. Host modules that need settings chain here. */
static void arcus_load_settings(const char *settings_path)
{
    arcus_input_load_settings(settings_path);
    arcus_options_load_settings(settings_path);
    /* Speaker output: 20 Hz DC blocker (the FM/PSG mix's DC offset steps as
     * channels key on/off and clicks on pause/resume) and +6 dB with a soft knee,
     * matching Genesis Plus GX's loudness (tests/compare_audio.py: native mix
     * measured ~6 dB quieter). */
    audio_set_output_stage(20, 200);
    /* Evidence files are written only while paused (overlay, HOME) and at exit: a
     * periodic write to the Switch SD card stalled a frame by 130-140 ms. */
    runtime_evidence_set_deferred(1);
}

static void arcus_call_entry_point(void) { recomp_call_addr(0x00020Eu); }

/* Save-state resume. A state is only taken at the gameplay frame loop's VBlank wait
 * (arcus_saves.c), so a load restarts the game fiber on the reset stack just after that
 * wait, whatever native code was running before (the title, the character select, another
 * area). Without it the restored RAM ran under a stale native stack: the loaded picture
 * froze on the title, or the game jumped into data and the player stuck in place. Arcus has
 * no game-mode byte; the return address on the restored stack says which wait it was. */
static uint32_t arcus_save_resume_pc(uint8_t game_mode)
{
    (void)game_mode;
    return arcus_saves_resume_pc();
}
static void arcus_call_vblank(void)      { recomp_call_addr(0x0003D8u); }
static void arcus_call_hblank(void)      { recomp_call_addr(0x0003DAu); }

const GameSpec g_game_spec = {
    .display_name           = "Arcus Odyssey DX",
    .short_name             = "Arcus",

    /* build/arcus-native.bin (tools/build.py --native): CRC32 10650E68, 2 MB file. The size must be
     * the exact file size: the floor only covers addresses below it, and our
     * patch code lives at $100000+. */
#ifdef ARCUS_ROM_CRC32
    .expected_rom_crc32     = ARCUS_ROM_CRC32,     /* another input ROM (e.g. the tall experiment) */
#else
    .expected_rom_crc32     = 0x10650E68u,
#endif
    .expected_rom_size      = 0x200000u,
    .embedded_rom           = arcus_rom_data,
    .embedded_rom_size      = 0x200000u,   /* a static initializer can't use arcus_rom_end;
                                              the size and CRC checks above catch a mismatch */

    /* No disassembly: discovery is heuristic, so keep the interpreter floor
     * on; misses run correctly and are logged for the next regen. */
    .tier3_floor_default    = 1,

    .call_entry_point       = arcus_call_entry_point,
    .call_vblank            = arcus_call_vblank,
    .call_hblank            = arcus_call_hblank,
    .save_resume_pc         = arcus_save_resume_pc,

    /* Controls (SPEC.md §6.1): twin-stick aim, ZR autofire, overlay edges. */
    .load_settings          = arcus_load_settings,
    .input_frame            = arcus_input_frame,
    .input_mask             = arcus_input_mask,

    /* Presentation and settings (track C): 424-px gameplay view + pillarbox,
     * GL/CRT presenter, settings overlay, settings.ini [arcus], game speed. */
    .video                  = &g_arcus_game_video,
    .presenter              = &g_arcus_presenter,
    .main_cpu_divisor       = arcus_main_cpu_divisor,
};
