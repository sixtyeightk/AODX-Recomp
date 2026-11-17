# Credits and sources

This file records where every non-code asset comes from.

Nothing in this list is committed to the repo. Each file is downloaded at build time from
the address below and checked against a pinned SHA-256 (the reviews against size and page
count). A file that has moved or changed is left out of that build, and its menu entry
disappears.

The code side of this is in `tools/fetch-extras.py`: every item there carries `credit` and
`source` fields. Keep the two in step when adding an extra.

## Game and engine

| What                          | Source                                                                                                                                                                                                                                                                                                             |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| The game                      | Your own Arcus Odyssey (USA) ROM. Never included or downloaded. Arcus Odyssey © 1991 Wolf Team / Telenet Japan; published in North America by Renovation Products.                                                                                                                                                 |
| Music                         | Motoi Sakuraba (composer), played by the game itself.                                                                                                                                                                                                                                                              |
| Recompiler / runtime          | segagenesisrecomp by mstan, https://github.com/mstan/segagenesisrecomp (PolyForm Noncommercial 1.0.0), with its m68k-recomp-core, z80-recomp-core and rbengine submodules. `build.py` downloads them at the commits pinned in `engine-patches/sources.json` and applies our changes (`engine-patches/`).           |
| SDL2 (Linux package)          | Simple DirectMedia Layer 2.32.10, https://libsdl.org (zlib licence). The Linux build image (`platform/linux/Dockerfile`) downloads its source release from GitHub, https://github.com/libsdl-org/SDL/releases, checks the pinned SHA-256 and links it in statically. The Mac app bundles Homebrew's build of SDL2. |
| CRT shader and scanlines (`shaders/crt.glsl`) | Written for this project, modelled on crt-lottes by Timothy Lottes (public domain), https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-lottes.glsl. No code copied; see `shaders/THIRD-PARTY.md`. |
| CRT COLOUR (the colour table) | Part of segagenesisrecomp (`runner/video/color_lut.c`): its colorimetry is ported from JRickey's gba-recomp, https://github.com/JRickey/gba-recomp (MIT OR Apache-2.0), used with permission. |

## EXTRAS

| Extra                                                                                                              | Hosted / preserved by                                                                                                                                                                                                                                      | Address                                                                                                                                                                                              |
| ------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Original manual (US, Renovation)                                                                                   | Evercade (Blaze Entertainment), hosting it for the Evercade re-release                                                                                                                                                                                     | https://evercade.info/wp-content/uploads/2021/07/Arcusodyssey_md_us_manual.pdf                                                                                                                       |
| Japanese manual (Wolf Team, T-32053, full colour)                                                                  | Games Database, gamesdatabase.org                                                                                                                                                                                                                          | https://www.gamesdatabase.org/Media/SYSTEM/Sega_Genesis//Manual/formated/Arcus_Odyssey_-_1991_-_Renovation_Products.pdf                                                                              |
| Modern manual | Made by this project from your own ROM, partly based on Sega Retro's article (CC BY 4.0). Font: Aileron by Sora Sagano (CC0). | https://segaretro.org/Arcus_Odyssey |
| Reviews (7 magazines, 1991–92): GamePro #26, EGM #26, VG&CE #34, Games-X #30, MegaTech #1, Game Players 3.1, ACE #54 | Sega Retro's magazine scans, hosted on retrocdn.net. Only each review's pages are read from the whole-magazine PDFs. | https://segaretro.org/Arcus_Odyssey, https://retrocdn.net |
| ARTWORK: US box and Japanese box (front and back), magazine ad (Renovation, "It's time to learn a thing or two about fun.") | LaunchBox Games Database | https://gamesdb.launchbox-app.com/games/images/4022-arcus-odyssey |
| ARTWORK: Box artwork (an AI-made recreation of the US cover painting, expanded to a wide picture; not official art) | LaunchBox Games Database (Fanart - Background) | https://gamesdb.launchbox-app.com/games/images/4022-arcus-odyssey |
| ARTWORK: Unreleased SNES cover (Julie Bell, 1992, oil on board, for Renovation's cancelled Super Nintendo version) | Original Video Game Art (OVGA); the painting is in The Game Curator collection                                                                                                                                                                             | https://www.originalvideogameart.com/index.php?/home/arcus-odyssey-unreleased-super-nintendo-box-cover-art-r457/                                                                                     |

## App icon (Switch, Mac and Linux)

Composed at build time by `tools/make_icon.py` from two files:

- the box artwork recreation above, cropped square;
- the US clear logo from LaunchBox Games Database (same page as the box art).
