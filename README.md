<p align="center">
    <img src="docs/logo.png" width="800" alt="AODX Recompiled"/>
</p>

---

AODX Recompiled is an unofficial port of Arcus Odyssey for the Sega Genesis / Mega Drive adding twin-stick controls, wide-screen support, a mini map, and more. The project uses [segagenesisrecomp](https://github.com/mstan/segagenesisrecomp) to statically recompile your own legally acquired copy of the game to build a native port for Switch Homebrew, Mac, or Linux.

**This project does not include any game data. You need your own Arcus Odyssey (USA)
cartridge ROM to build it.** Manuals, reviews and other extras are downloaded at build time
from public archives.

<p align="center">
    <a href="https://www.youtube.com/watch?v=J1Ot9nye7lM"><img src="https://github.com/user-attachments/assets/7e77325c-c143-4173-80b4-1967a3bd292b" width="640" alt="Watch the AODX Recompiled trailer on YouTube"/></a>
    <br/>
    <a href="https://www.youtube.com/watch?v=J1Ot9nye7lM">Watch the trailer on YouTube</a>
</p>

## Table of contents

- [Features](#features)
- [Building](#building)
- [Credits and special thanks](#credits-and-special-thanks)
- [Licence](#licence)
- [Contact](#contact)

## Features

### Plug and play

Bring your own ROM, run the builder, and play on Switch, Mac or Linux.

### Widescreen

The play area has been expanded to support 16:9 displays. You can still switch to 4:3 anytime in the config menu.

### Twin-stick controls

Move with the left stick and aim with the right. Right trigger fires, left trigger uses magic.

### Mini map

Fills in as you explore.

### Save states and rewind

There are 3 save state slots as well as quick save/load. Rewind up to 15 seconds of gameplay.

### Magic UI/UX improvements

Level and spell shown above the HUD. Quick change spells by holding X / S.

### CRT look

Shader, CRT colour and scanlines, each adjustable.

### Options

Autofire speed, slowdown reduction, enemy respawn on or off, and cheats (infinite health, infinite magic).

### Extras

The original manuals, a modern manual, seven 1991–92 magazine reviews, and box and promotional artwork. All are downloaded or made when you build; see [Credits](#credits-and-special-thanks).

The config menu is on **−** (Switch) or **Esc** (Mac and Linux).

## Building

No game data is included, so you build the game yourself from your own ROM. One command does
it all; the steps, and how to install the result, are in **[BUILDING.md](BUILDING.md)**.

## Credits and special thanks

- **[mstan](https://github.com/mstan)**, for [segagenesisrecomp](https://github.com/mstan/segagenesisrecomp),
  which made a native port possible, and the **R.A.I.D. community**
  ([Discord](https://discord.gg/Ad9BwSzctP)).
- **Timothy Lottes**, for [crt-lottes](https://github.com/libretro/glsl-shaders/blob/master/crt/shaders/crt-lottes.glsl)
  (public domain), the model for our CRT shader and scanlines.
- **[JRickey](https://github.com/JRickey)**, for the CRT colour maths from
  [gba-recomp](https://github.com/JRickey/gba-recomp), which segagenesisrecomp uses for CRT COLOUR.
- **[SDL](https://libsdl.org)** (zlib licence) and **[devkitPro](https://devkitpro.org)**, for
  the libraries and the Switch toolchain.

This project stands on the work of people who keep video game history from disappearing.
Every manual page, magazine review and painting in Extras is downloaded when you build, from
the archives that preserve it. Please visit them:

- **[Sega Retro](https://segaretro.org)** and **[retrocdn.net](https://retrocdn.net)**, for
  scanning and hosting whole runs of 1990s magazines, where seven contemporary reviews of
  Arcus Odyssey survive, and for their [Arcus Odyssey article](https://segaretro.org/Arcus_Odyssey).
- **[Games Database](https://www.gamesdatabase.org)**, for the full-colour scan of the original
  Japanese manual.
- **[Evercade](https://evercade.info)**, for keeping the US manual online alongside its
  re-release.
- **[LaunchBox Games Database](https://gamesdb.launchbox-app.com/games/images/4022-arcus-odyssey)**,
  for high-resolution scans of the US and Japanese boxes, the US magazine ad and the logo,
  and a recreation of the US cover painting.
- **[Original Video Game Art](https://www.originalvideogameart.com/index.php?/home/arcus-odyssey-unreleased-super-nintendo-box-cover-art-r457/)**
  and **The Game Curator** collection, for preserving and sharing Julie Bell's painting for
  the Super Nintendo version that never shipped. Thanks also to **Julie Bell** for painting it.

And to Wolf Team, Telenet Japan and Renovation Products, who made the game, and to Motoi
Sakuraba for its music. Arcus Odyssey and all related names, art and music belong to their
respective owners.

The full list of sources, with their addresses, is in [CREDITS.md](CREDITS.md).

## Licence

[PolyForm Noncommercial 1.0.0](LICENSE.md), the same as the engine. You may use, change and
share this code for any non-commercial purpose. The licence covers this project's code only,
not the game.

An unofficial, free and non-commercial fan project, not affiliated with or endorsed by Sega,
Wolf Team, Telenet Japan, Renovation, Nintendo or Apple. All trademarks and copyrights belong
to their respective owners.
