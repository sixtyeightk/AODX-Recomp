#!/usr/bin/env python3
"""Build Arcus Odyssey (native) from your own copy of the game.

    python3 build.py                                   asks for the ROM and the versions
    python3 build.py --rom PATH --target mac switch    or say it up front

Targets: mac (this Mac), linux-x86_64, linux-arm64, switch (.nro). The result lands in this
folder: "Arcus Odyssey DX.app", switch/ (copy it to the SD card's root) or the Linux .tar.gz.
Everything the build makes along the way, and its log, is in build/.
Needs Python 3.10+, internet on the first build, and:
    mac      macOS with the Xcode command line tools and Homebrew cmake, ninja and sdl2
    linux-*  on a Linux PC, for its own architecture: cmake, ninja, a C/C++ compiler and SDL2
             (development files); otherwise Docker (x86_64 on Apple silicon runs emulated: slower)
    switch   Docker
The ROM is checked against the USA release, patched (rom-patch/arcus-odyssey-dx.ips) into build/rom/,
and never leaves this folder. The engine is downloaded at pinned commits and patched
(engine-patches/). A log of every run is kept in build/build.log.
"""
import argparse
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORK = HERE / "build"          # everything generated: engine, ROM copy, extras, build trees, log
CACHE = WORK / ".cache"
ROMDIR = WORK / "rom"
OUT = WORK                     # the build scripts' results, before they are put in place
ENGINE = WORK / "engine"
STAMP = ENGINE / ".made-by-build-py"
VENV = WORK / "venv"
MAC_APP = HERE / "Arcus Odyssey DX.app"
SWITCH_DIR = HERE / "switch"   # mirrors the SD card: switch/arcus-odyssey-dx/arcus-odyssey-dx.nro
PILLOW = "Pillow==12.3.0"
TARGETS = {
    "mac": "macOS app (this Mac)",
    "linux-x86_64": "Linux x86_64: PCs and the Steam Deck",
    "linux-arm64": "Linux ARM64",
    "switch": "Nintendo Switch homebrew .nro (Docker)",
}
ORDER = list(TARGETS)          # desktop builds first: the Switch build reuses their game guide
LOG = None


# ---------------------------------------------------------------- output
def say(msg=""):
    print(msg, flush=True)
    if LOG:
        LOG.write(msg + "\n"); LOG.flush()


def stop(msg):
    say(f"\nerror: {msg}")
    if LOG:
        say(f"(full log: {OUT / 'build.log'})")
    sys.exit(1)


def run(cmd, env=None, what=None, check=True):
    """Run a command, showing and logging its output; stop the build if it fails (check)."""
    say(f"$ {' '.join(str(c) for c in cmd)}")
    # PWD: bash's `pwd` (the scripts' $HERE) then spells this folder exactly as HERE does (the
    # scripts map build/ into containers by stripping that prefix; a case or symlink difference
    # would break it).
    env = dict(env if env is not None else os.environ, PWD=str(HERE))
    p = subprocess.Popen([str(c) for c in cmd], cwd=HERE, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, errors="replace")
    for line in p.stdout:
        sys.stdout.write(line)
        if LOG:
            LOG.write(line)
    if p.wait() and check:
        stop(f"{what or cmd[0]} failed (exit {p.returncode}); the output above says why")
    return p.returncode


# ---------------------------------------------------------------- ROM
def rom_spec():
    return json.loads((HERE / "rom-patch" / "rom.json").read_text())


def read_rom(path):
    """The ROM's bytes: a plain file, or the one ROM inside a .zip."""
    path = Path(path).expanduser()
    if not path.is_file():
        stop(f"no file at {path}")
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if not n.endswith("/") and z.getinfo(n).file_size <= 8 << 20]
            if len(names) != 1:
                stop(f"{path.name} holds {len(names)} files; unzip it and give the ROM itself")
            return z.read(names[0])
    if path.stat().st_size > 8 << 20:
        stop(f"{path.name} is too big to be a Genesis ROM")
    return path.read_bytes()


def ips_apply(src, patch):
    if patch[:5] != b"PATCH":
        stop(f"rom-patch/{rom_spec()['patch']} is not an IPS patch")
    d, p = bytearray(src), 5
    while patch[p:p + 3] != b"EOF":
        if p + 5 > len(patch):
            stop(f"rom-patch/{rom_spec()['patch']} is cut short (no EOF marker); get a fresh copy of the repo")
        off = int.from_bytes(patch[p:p + 3], "big"); size = int.from_bytes(patch[p + 3:p + 5], "big"); p += 5
        if size:
            data = patch[p:p + size]; p += size
        else:
            n = int.from_bytes(patch[p:p + 2], "big"); data = patch[p + 2:p + 3] * n; p += 3
        if off + len(data) > len(d):
            d.extend(b"\0" * (off + len(data) - len(d)))
        d[off:off + len(data)] = data
    return bytes(d)


def deinterleave_smd(b):
    """.smd dumps: a 512-byte header, then 16 KB blocks of odd bytes followed by even bytes."""
    body, out = b[512:], bytearray()
    for i in range(0, len(body), 16384):
        blk = body[i:i + 16384]
        half = len(blk) // 2
        for j in range(half):
            out += bytes([blk[half + j], blk[j]])
    return bytes(out)


def prepare_rom(path):
    spec = rom_spec()
    want = spec["input"]
    b = read_rom(path)
    sha = hashlib.sha1(b).hexdigest()
    if sha != want["sha1"] and len(b) % 16384 == 512:
        for fix, label in ((deinterleave_smd, "an .smd dump, converted"),
                           (lambda x: x[512:], "a 512-byte copier header, removed")):
            fixed = fix(b)
            if hashlib.sha1(fixed).hexdigest() == want["sha1"]:
                b, sha = fixed, want["sha1"]
                say(f"ROM: {label}")
                break
    if sha != want["sha1"]:
        hint = ("it has a 512-byte header" if len(b) == want["size"] + 512 else
                "the size is right, so it may be another region, a revision, a bad dump or a hacked copy"
                if len(b) == want["size"] else f"it is {len(b)} bytes, the game is {want['size']}")
        stop(f"this is not {want['name']} (SHA-1 {sha}; expected {want['sha1']}): {hint}")
    out = ips_apply(b, (HERE / "rom-patch" / spec["patch"]).read_bytes())
    if hashlib.sha1(out).hexdigest() != spec["output"]["sha1"]:
        stop("the patched ROM is not the expected one (is rom-patch/ intact?)")
    ROMDIR.mkdir(exist_ok=True)
    (ROMDIR / "arcus-odyssey-usa.bin").write_bytes(b)            # your ROM as dumped (the guide reads it)
    (ROMDIR / spec["output"]["name"]).write_bytes(out)
    say(f"ROM: {want['name']} ✓ (SHA-1 matches); patched to rom/{spec['output']['name']}")


# ---------------------------------------------------------------- engine
def download(url, sha256, dest):
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha256:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    say(f"download {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "AODX-Recomp-builder/1.0 (+https://github.com/sixtyeightk/AODX-Recomp)"})
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            data = r.read()
    except OSError as e:
        stop(f"couldn't download {url} ({e})")
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        stop(f"{url} is not the expected file (SHA-256 {got}, expected {sha256})")
    dest.write_bytes(data)
    return dest


def extract(tgz, dest):
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tgz) as t:
        for m in t.getmembers():
            parts = m.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            m.name = parts[1]
            if m.name.startswith("/") or ".." in Path(m.name).parts or m.issym() or m.islnk():
                continue                                  # nothing outside dest, no links
            if hasattr(tarfile, "data_filter"):
                t.extract(m, dest, filter="data")
            else:
                t.extract(m, dest)


def prepare_engine():
    sources_file = HERE / "engine-patches" / "sources.json"
    src = json.loads(sources_file.read_text())
    patches = [HERE / "engine-patches" / p for p in src["patches"]]
    h = hashlib.sha256(sources_file.read_bytes())
    for p in patches:
        h.update(p.read_bytes())
    want = h.hexdigest() + "\n"
    if ENGINE.exists():
        if not STAMP.exists():
            stop(f"{ENGINE} exists but was not made by build.py; move it away and run again")
        if STAMP.read_text() == want:
            say("engine: up to date")
            return
        shutil.rmtree(ENGINE)                            # our own output, made again below
    work = CACHE / "engine-new"
    if work.exists():
        shutil.rmtree(work)
    e = src["engine"]
    extract(download(f"https://codeload.github.com/{e['repo']}/tar.gz/{e['commit']}", e["sha256"],
                     CACHE / "downloads" / f"engine-{e['commit'][:12]}.tar.gz"), work)
    for s in src["submodules"]:
        extract(download(f"https://codeload.github.com/{s['repo']}/tar.gz/{s['commit']}", s["sha256"],
                         CACHE / "downloads" / f"{Path(s['path']).name}-{s['commit'][:12]}.tar.gz"),
                work / s["path"])
    if not shutil.which("patch"):
        stop("the 'patch' command is needed (macOS has it; on Linux: apt install patch)")
    for p in patches:
        r = subprocess.run(["patch", "-p1", "-s", "--no-backup-if-mismatch", "-i", str(p)], cwd=work,
                           capture_output=True, text=True)
        if r.returncode:
            stop(f"engine patch {p.name} did not apply:\n{r.stdout}{r.stderr}")
    work.rename(ENGINE)
    STAMP.write_text(want)
    say(f"engine: {e['repo']}@{e['commit'][:7]} + {len(src['submodules'])} submodules + "
        f"{len(patches)} patches ✓")


# ---------------------------------------------------------------- tools
def have(cmd):
    return shutil.which(cmd) is not None


def check_tools(targets):
    missing = []
    if "mac" in targets:
        if sys.platform != "darwin":
            missing.append("mac: only builds on a Mac")
        else:
            if subprocess.run(["xcrun", "--find", "clang"], capture_output=True).returncode:
                missing.append("mac: Xcode command line tools (xcode-select --install)")
            for c in ("cmake", "ninja"):
                if not have(c):
                    missing.append(f"mac: {c} (brew install {c})")
            sdl = have("brew") and subprocess.run(["brew", "--prefix", "--installed", "sdl2"],
                                                  capture_output=True).returncode == 0
            if not sdl and have("pkg-config"):
                sdl = subprocess.run(["pkg-config", "--exists", "sdl2"], capture_output=True).returncode == 0
            if not sdl:
                missing.append("mac: SDL2 (brew install sdl2)")
    native = [t for t in targets if is_native_linux(t)]
    if native:
        for c, pkg in (("cmake", "cmake"), ("ninja", "ninja-build"), ("cc", "build-essential"),
                       ("c++", "build-essential"), ("objdump", "binutils"), ("pkg-config", "pkg-config")):
            if not have(c):
                missing.append(f"linux: {c} (sudo apt install {pkg})")
        if have("pkg-config") and subprocess.run(["pkg-config", "--exists", "sdl2"],
                                                 capture_output=True).returncode:
            missing.append("linux: SDL2 development files (sudo apt install libsdl2-dev)")
    if any(t != "mac" and t not in native for t in targets):
        if not have("docker"):
            missing.append("linux / switch: Docker (https://docs.docker.com/get-docker/)")
        elif subprocess.run(["docker", "info"], capture_output=True).returncode:
            missing.append("linux / switch: Docker is installed but not running; start it")
    if missing:
        stop("missing tools:\n  " + "\n  ".join(missing))
    say("tools: ✓")


def prepare_python():
    py = VENV / "bin" / "python3"
    probe = [str(py), "-c", "import PIL, sys; sys.exit(PIL.__version__ != '" + PILLOW.split("==")[1] + "')"]
    if py.exists() and subprocess.run(probe, capture_output=True).returncode == 0:
        return py
    say(f"python: setting up .venv with {PILLOW} (the icon and the game guide)")
    run([sys.executable, "-m", "venv", str(VENV)], what="making .venv")
    run([str(py), "-m", "pip", "install", "-q", "--disable-pip-version-check", PILLOW], what="installing Pillow")
    return py


# ---------------------------------------------------------------- the builds
def guide_made():
    return (WORK / "extras-cache" / "books" / "02-guide" / "stamp.txt").exists()


def native_linux():
    return "linux-arm64" if platform.machine().lower() in ("arm64", "aarch64") else "linux-x86_64"


def is_native_linux(t):
    """On a Linux PC, its own architecture builds directly, with the system's tools and SDL2.
    Docker is for the other architecture, and for Linux builds made on a Mac."""
    return sys.platform.startswith("linux") and t == native_linux()


def build_linux(t, env, linux_env, what):
    if is_native_linux(t):
        run(["bash", "tools/build-linux.sh"], dict(env, ARCUS_EXTRAS="guide"), what)
    else:
        run(["bash", "tools/build-linux-docker.sh", t.split("-", 1)[1]], linux_env, what)


def build(targets, env):
    made = []
    # The Linux container sees this folder at /src and finds rom/ itself: no host paths.
    linux_env = {k: v for k, v in env.items() if k != "ARCUS_ROM"}
    linux_env["ARCUS_EXTRAS"] = "guide"
    run([env["PYTHON"], "tools/fetch-extras.py"], env, "downloading the extras")
    # The icon (box artwork + logo, downloaded with the extras), for every target: the Linux
    # package takes it from out/ too. Offline, the Mac and Switch builds fall back to a title
    # capture; the Linux package then goes without one.
    if run([env["PYTHON"], "tools/make_icon.py", "--out", str(OUT)], env, check=False):
        say("(no icon yet: the box artwork could not be downloaded)")
    for t in ORDER:
        if t not in targets:
            continue
        say(f"\n=== {TARGETS[t]} ===")
        if t == "mac":
            run(["bash", "tools/build-mac.sh"], dict(env, ARCUS_EXTRAS="cached"), "the Mac build")
            made.append(deliver(OUT / "Arcus Odyssey DX.app", MAC_APP))
        elif t.startswith("linux-"):
            arch = t.split("-", 1)[1]
            build_linux(t, env, linux_env, "the Linux build")
            tgz = OUT / f"arcus-odyssey-dx-{version()}-linux-{'aarch64' if arch == 'arm64' else arch}.tar.gz"
            if not tgz.exists():
                stop(f"the Linux build made no {tgz.name}")
            made.append(deliver(tgz, HERE / tgz.name))
        elif t == "switch":
            mac_game = WORK / "build-mac" / "Arcus Odyssey DX.app" / "Contents" / "MacOS" / "Arcus Odyssey DX"
            if not guide_made() and not mac_game.exists():
                say(f"(the game guide's pictures are captured with a desktop build: making "
                    f"{native_linux()} first, in Docker)")
                run(["bash", "tools/build-linux-docker.sh", native_linux().split("-", 1)[1]],
                    linux_env, "the Linux build (for the guide)")
            e = dict(env)
            if mac_game.exists():
                e["ARCUS_HOST_GAME"] = str(mac_game)
            run(["bash", "tools/build-switch.sh"], dict(e, ARCUS_EXTRAS="cached"), "the Switch build")
            made.append(deliver(OUT / "arcus-odyssey-dx.nro", SWITCH_DIR / "arcus-odyssey-dx" / "arcus-odyssey-dx.nro"))
    return made


def version():
    import re
    m = re.search(r"project\(ArcusOdysseyRecomp VERSION ([0-9.]+)", (HERE / "CMakeLists.txt").read_text())
    return m.group(1) if m else "0"


def deliver(src, dest):
    """Put a result where the player looks for it (replacing the previous build's)."""
    if dest.exists():
        shutil.rmtree(dest) if dest.is_dir() else dest.unlink()     # our own earlier result
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dest))
    return dest


def clean_path(s):
    """A path typed or dragged into the terminal: Terminal escapes spaces, brackets and quotes
    with backslashes, other shells quote the whole path."""
    s = s.strip()
    if Path(s).expanduser().exists():
        return s
    try:
        parts = shlex.split(s)
    except ValueError:
        return s
    return " ".join(parts) if parts else s


def ask(targets, rom):
    if not rom:
        say("Where is your Arcus Odyssey (USA) ROM? (drag the file here, then press Enter)")
        rom = clean_path(input("ROM: "))
    if not targets:
        say("Build which versions? (numbers, separated by spaces)")
        for i, (k, v) in enumerate(TARGETS.items(), 1):
            say(f"  [{i}] {v}")
        picks = input("> ").split()
        keys = list(TARGETS)
        try:
            targets = [keys[int(n) - 1] for n in picks if 1 <= int(n) <= len(keys)]
            if len(targets) != len(picks):
                raise ValueError
        except (ValueError, IndexError):
            stop("pick by number, e.g. 1 4")
    return targets, rom


def main():
    global LOG
    if sys.version_info < (3, 10):
        sys.exit("build.py needs Python 3.10 or newer")
    ap = argparse.ArgumentParser(description="Build Arcus Odyssey (native) from your own copy of the game.")
    ap.add_argument("--rom", help="your Arcus Odyssey (USA) ROM (.md / .bin / .gen / .smd, or a .zip)")
    ap.add_argument("--target", nargs="+", choices=list(TARGETS), help="what to build")
    a = ap.parse_args()
    targets, rom = (a.target, a.rom) if a.target and a.rom else ask(a.target, a.rom)
    if not targets:
        stop("nothing to build")
    OUT.mkdir(exist_ok=True)
    LOG = open(OUT / "build.log", "w")
    say(f"Arcus Odyssey DX build — {time.strftime('%Y-%m-%d %H:%M')}, {platform.system()} {platform.machine()}, "
        f"Python {platform.python_version()}; targets: {' '.join(targets)}")
    prepare_rom(rom)
    check_tools(targets)
    prepare_engine()
    py = prepare_python()
    env = dict(os.environ, PATH=f"{py.parent}{os.pathsep}{os.environ.get('PATH', '')}",
               PYTHON=str(py), ARCUS_ROM=str(ROMDIR / rom_spec()["output"]["name"]),
               ARCUS_WORK=str(WORK), ARCUS_OUT=str(OUT))
    made = build(targets, env)
    say("\nDone:")
    for m in made:
        say(f"  {m.relative_to(HERE)}")
    if "switch" in targets:
        say("\nSwitch: copy the switch folder to the root of the SD card (it holds\n"
            "switch/arcus-odyssey-dx/arcus-odyssey-dx.nro).\n"
            "Run it with full memory: an NSP forwarder (https://nsp-forwarder.n8.io/) pointing at that\n"
            "path, or hold R while starting any game to open the Homebrew Menu. Not from the Album.")
    if any(t.startswith("linux") for t in targets):
        say("\nLinux: unpack the .tar.gz and run ./arcus-odyssey-dx (./install.sh adds a menu entry).")
    say(f"\n(the build's own files and its log are in build/)")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        stop("stopped")
