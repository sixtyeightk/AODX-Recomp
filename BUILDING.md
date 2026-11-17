# Building

AODX Recompiled is built from your own copy of Arcus Odyssey: the USA cartridge ROM. No game data is included or downloaded.

## 1. Clone the repository

Clone AODX Recompiled using [Git](https://git-scm.com/).

```bash
git clone https://github.com/sixtyeightk/AODX-Recomp.git
```

## 2. Install dependencies

The Switch version also needs Docker, which supplies the Switch toolchain (devkitPro) so
everyone builds with the same tested tools.

### On a Mac

Install [Homebrew](https://brew.sh) if you don't have it, then run:

```bash
xcode-select --install
brew install python cmake ninja sdl2
```

To build the Switch or Linux version, also install Docker and start it:

```bash
brew install --cask docker-desktop
open -a Docker
```

### On Linux

These commands are for Ubuntu and Debian. On other distros, install the same packages with
your package manager.

```bash
# For Ubuntu and Debian, run:
sudo apt install git python3 python3-venv build-essential cmake ninja-build pkg-config libsdl2-dev
```

To build the Switch version, also install Docker:

```bash
sudo apt install docker.io
sudo usermod -aG docker $USER      # lets you run docker without sudo; log out and back in
```

## 3. Build the project

Run the line for the version you want:

| Version                         | Build on               | Command                                  |
| ------------------------------- | ---------------------- | ---------------------------------------- |
| Switch Homebrew                 | Mac or Linux           | `python3 build.py --target switch`       |
| Mac (Apple silicon tested)      | the Mac you'll play on | `python3 build.py --target mac`          |
| Linux PC or Steam Deck (x86_64) | Mac or Linux           | `python3 build.py --target linux-x86_64` |
| Linux ARM64                     | Mac or Linux           | `python3 build.py --target linux-arm64`  |

It asks where your ROM is: drag the file into the terminal. It accepts `.md`, `.bin`, `.gen`,
`.smd`, or a `.zip` with the ROM inside. The result appears in the repository folder:
`Arcus Odyssey DX.app`, a `switch` folder, or the Linux `.tar.gz`. Everything the build makes
along the way, and its log, goes in `build/`. The first Switch build takes a while: Docker downloads the toolchain.

On a Linux PC, the Linux version is built with your system's tools and SDL2, for that PC. On
a Mac, it is built in Docker instead. Building x86_64 Linux on an Apple silicon Mac runs
emulated and is slow.

## 4. Install and play

### Switch Homebrew

1. Copy the `switch` folder to the root of your SD card. It holds
   `switch/arcus-odyssey-dx/arcus-odyssey-dx.nro`, so the game ends up at
   `sdmc:/switch/arcus-odyssey-dx/arcus-odyssey-dx.nro`.
2. Start it **with full memory**. The extras need it, and the game uses about 155 MB.
   Either:
   - **Recommended: an NSP forwarder.** Make one at <https://nsp-forwarder.n8.io/> pointing
     at that path, and install it. The game then gets its own icon on the HOME menu and
     launches with full memory.
   - Or hold **R** while starting any game to open the Homebrew Menu (title override), then
     start Arcus Odyssey DX there.

   Don't start it from the Album (applet mode): it has too little memory, and the reviews and
   other extras come up blank.

### Mac

Open `Arcus Odyssey DX.app`, or move it to Applications first. It is built on your Mac, so
macOS doesn't ask about an unknown developer.

### Linux

```bash
tar -xzf arcus-odyssey-dx-1.0.0-linux-x86_64.tar.gz
cd arcus-odyssey-dx
./arcus-odyssey-dx       # or ./install.sh to add it to your applications menu
```

Built on a Linux PC, it runs on that PC and needs its SDL2 installed. Built on a Mac, it
has SDL2 inside and runs on most distros: glibc 2.35 or newer (Ubuntu 22.04, Debian 12,
SteamOS 3 and later).

Tested on Ubuntu 24.04 (x86_64 and ARM64). Controllers and the Steam Deck haven't been tested
on real hardware yet; if something doesn't work, please open an issue with `build/build.log`.

## Windows

Not tested. WSL2 with Docker Desktop should work for the Switch and Linux versions: follow the
Linux steps inside your WSL2 distro.

## What the builder does

1. Checks your ROM against the USA release (SHA-1). Your ROM is copied into `build/rom/`
   (`arcus-odyssey-usa.bin`) and never leaves this folder.
2. Applies this project's patch (`rom-patch/arcus-odyssey-dx.ips`, our own code only) to make the
   full-screen version of the game (`build/rom/arcus-odyssey-usa-dx.bin`).
3. Downloads the engine, [segagenesisrecomp](https://github.com/mstan/segagenesisrecomp), at a
   pinned commit, checks it, and applies our changes (`engine-patches/`).
4. Downloads the extras from their archives, each checked against a pinned checksum. An extra
   that can't be downloaded is left out, and the build goes on without it.
5. Builds the version you asked for. A log of everything is in `build/build.log`.

Run `python3 build.py` with no options to choose interactively, or give several targets at
once (`--target switch mac`).

## How the repository is laid out

- `rom-patch/`: the game patch. `arcus-odyssey-dx.ips` holds only our own code (about 2 KB of new
  68000 routines and the jumps to them), never the game's. The `asm/` sources are there to read.
- `engine-patches/`: our changes to segagenesisrecomp, and `sources.json`, which pins the
  exact upstream commits.
- `game/`: the port. It covers the full-screen view, the menu, the extras viewer, input,
  saves, the mini map and the magic display.
- `platform/`, `shaders/`, `tools/`: packaging for each platform, the CRT shader and the
  build scripts.

AI coding tools were used in making it.

## Troubleshooting

- **Something else failed.** The last lines of output say which step stopped. If you open an
  issue, attach `build/build.log`.
- **An extra is missing from the menu.** Its download failed or its source changed. The build
  tells you which one, and the rest of the game is not affected.
- **The game misbehaves while playing.** Open an issue and attach the `.toml` files from the
  game's save folder (`~/Library/Application Support/Arcus Odyssey DX/` on a Mac,
  `~/.local/share/Arcus Odyssey DX/` on Linux, `sdmc:/switch/arcus-odyssey-dx/` on the
  Switch). They
  list any game code the recompile missed, which pinpoints the bug.
