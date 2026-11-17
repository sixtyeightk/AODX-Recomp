#!/usr/bin/env python3
"""Make Extras > Modern manual: one tall page of short notes on the characters, their magic
and the items, with pictures captured from the player's own copy of the game.

The pictures come from running this build's game itself, headless, with input scripts (the
same runner options the tests use): the character select screen for the portraits, one cast
of every spell in Act 1 with an enemy placed nearby (the homing ones turn to it), the item
window for the icons, and a Power Jewel placed in a boss area (only those load its tiles).
Every capture is taken at 3x with the CRT filter on (scanlines + CRT colour, the defaults),
and pasted at that size so the scanlines stay even. Nothing from the game is ever committed: the result lives in
extras-cache/ (never versioned) and the build embeds it.

  extras-cache/books/02-guide/page01.jpg ...   1200 px wide strips, top to bottom
  extras-cache/books/02-guide/book.txt         "scroll|guide|Modern manual|"
  extras-cache/books/03-guide-frame/page01.jpg the red Power Jewel; book.txt
                                               "frame|guide-frame|Power Jewel|x|y|90": the viewer
                                               swaps it over the blue one at page (x, y) every
                                               90 frames, as the game does

The text is ours, checked against the game (2026-11-09: every spell cast in the original
ROM; item effects read from the item code at $E87A and tried; the names are the ROM's own
item strings and the manual's). Some descriptions started from Sega Retro (CC BY 4.0),
credited in the project's README (GitHub), not on the page. The font is Pillow's bundled
Aileron (CC0).

Remade only when this script or the ROM changes (stamp.txt). Never fails the build: without
a game binary, Pillow or a working capture the guide is simply left out.

usage: tools/make-guide.py --exe PATH_TO_GAME_BINARY [--rom build/rom/arcus-odyssey-usa.bin]
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = Path(os.environ.get("ARCUS_WORK") or ROOT)    # build/ (build.py) or here
OUT = WORK / "extras-cache" / "books" / "02-guide"
FRAME_OUT = WORK / "extras-cache" / "books" / "03-guide-frame"   # the jewel's other colour
JEWEL_PERIOD = 90     # frames per colour: the jewel's own timer (+$09, reset to $5A at $15ACA)

# ---- what the guide says ----------------------------------------------------------------

CHARACTERS = [
    ("Jedda Chaf", "Swordsman, 25",
     "His sword generates a vacuum wave when swung. Balanced reach and power.",
     "Starts with 6 hit points. Up to Lv4 magic."),
    ("Erin Gashuna", "Warrior, 26",
     "A wandering fighter with a chain whip: the shortest reach, the hardest hits.",
     "Starts with 7 hit points. Up to Lv3 magic."),
    ("Diana Fireya", "Archer, age unknown",
     "Her arrows fly far and bounce off walls, but hit lightly.",
     "Starts with 6 hit points. Up to Lv4 magic."),
    ("Bead Shia", "Sorcerer, 59",
     "Fires bolts of energy: long reach and strong, but the least health.",
     "Starts with 5 hit points. Up to Lv5 magic."),
]

# (name, casts, what it does) per level; casts from the game's table at $1E556.
SPELLS = [
    [("Stoneskin", 1, "A shield of light: you take less damage for a while. A partner standing close gets it too."),
     ("Fireball", 20, "A fireball, straight ahead."),
     ("Firebird", 10, "A bird of flame flies straight ahead."),
     ("Flame cross", 5, "Firebirds fly out ahead, behind and to both sides.")],
    [("Anti-poison", 2, "Cures poison."),
     ("Gust", 15, "A gust of wind straight ahead."),
     ("Whirlwind", 8, "A bigger, swirling gust.")],
    [("Healing light", 1, "Restores your health to full."),
     ("Guiding light", 15, "A ball of light that homes in on an enemy."),
     ("Light fracture", 10, "Homes in too, and shatters into shards when it hits."),
     ("Light rain", 3, "An arrow into the sky, then light rains down all around you.")],
    [("Cure silence", 2, "Lifts silence, the curse that stops you casting."),
     ("Crux fissure", 15, "Lightning waves spread out along the ground all around you."),
     ("Void vortex", 10, "A swirl of scattering dark energy."),
     ("Arc lightning", 3, "Lightning that leaps to an enemy."),
     ("Dark lord's lightning", 1, "Summons a winged dark lord: lightning strikes across the screen.")],
]

# (icon key, name, what it does); item ids are the game's ($823B + slot).
ITEMS = [
    ("crystal", "Magic Power Crystal", "Carry up to 6. Spend them in the item window to set your magic level."),
    ("jewel", "Power Jewel", "Take it while blue for more health, red for more attack power."),
    (1, "Lamp of Life", "Restores your health to full."),
    (2, "Herb of Purification", "Cures poison."),
    (3, "Anti-poison Herb of Wizardry", "Cures silence, the curse that stops you casting."),
    (4, "Doll of Life", "When your life runs out, it automatically brings you back at full health."),
    (6, "Potion of Invincibility", "No damage for about five seconds."),
    (5, "Magic of Elemental Summoning", "Spirits gather around you, then strike every enemy on the screen."),
    (7, "Magical Fire-ring", "Fireballs circle you for a while and burn what they touch."),
]

MAGIC_HOWTO = [
    "Spend crystals in the item window to set your magic level. M on the panel shows the casts remaining.",
    "Quick magic: hold X (S on a keyboard), pick with Up / Down, let go.",
]


# ---- capture ----------------------------------------------------------------------------

BOOT_SELECT = ["WAIT 1200", "PRESS START 4", "WAIT 90", "PRESS START 4", "WAIT 90"]
TO_ACT1 = ["PRESS START 4", "WAIT_RAM8 FF8082 7", "WAIT 30", "PRESS START 4", "WAIT_RAM8 FF8292 1", "WAIT 60"]
COURTYARD = ["HOLD RIGHT", "WAIT 150", "RELEASE", "HOLD UP", "WAIT 100", "RELEASE", "WAIT 150",
             "PRESS RIGHT 6", "WAIT 10"]
ENEMY_X, ENEMY_Y = 128 + 200, 128 + 125          # enemy 0 placed at raster (200, 125)
SHOTS = [8, 12, 16, 20, 24, 32, 40, 48, 56]      # frames after B to look at
K = 3                                            # capture scale: the game view at 3x, CRT on
SPELL_BOX = (200, 120)                           # raster px around each spell's movement
# Per spell (character, level), when the automatic pick needs help: frames to choose from,
# and "blobs": n = the first of them where n separate things moved away from the player.
# Light fracture: the ball bursts into three shards when it hits (about frame 23 after B);
# by 27-29 the three show apart, then fly off in three directions.
SHOT_PICK = {(2, 3): {"frames": list(range(18, 35)), "blobs": 3}}
# A Power Jewel object (the record the game's spawner at $12A2A writes): screen position + 128,
# pickup flags $60, colour state +$0A (1 red, 0 blue: the game turns it blue a frame later and
# keeps cycling), type $33. Area $20 (a boss area) has its tiles loaded; slot 40 is free.
JEWEL_AREA, JEWEL_SLOT = 0x20, 40
JEWEL = bytes.fromhex("014800fd" "0001" "0060" "0001" "01" "33" "0c110c080c11" "0000" "00000000" "ff" "000000")


def key(k):
    return [f"PRESS {k} 4", "WAIT 20"]


def run_game(exe, workdir, script, settings):
    """Run the game with an input script in its own folder; True on a clean exit."""
    d = Path(workdir)
    d.mkdir(parents=True, exist_ok=True)
    app = d / "sdmc" / "switch" / "arcus-odyssey-dx"
    app.mkdir(parents=True, exist_ok=True)
    ini = "[arcus]\n" + "".join(f"{k} = {v}\n" for k, v in settings.items())
    for base in (d, app):
        (base / "settings.ini").write_text(ini)
    if script and script[-1].startswith("EXIT"):
        script = script[:-1] + ["WAIT 2", script[-1]]   # a SCREENSHOT lands on the next present
    (d / "run.input").write_text("\n".join(script).replace("@OUT@", str(d)) + "\n")
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software",
               GENESIS_FAKE_SDMC=str(d / "sdmc"), GENESIS_DATA_DIR=str(d),
               SDL_JOYSTICK_HIDAPI="0", SDL_JOYSTICK_MFI="0", SDL_JOYSTICK_IOKIT="0")
    for k in ("ARCUS_PRESENT_DUMP", "ARCUS_NAV_SCRIPT", "ARCUS_VIEW", "ARCUS_NO_EXTRAS"):
        env.pop(k, None)
    env["ARCUS_WINDOW"] = f"{426 * K}x{240 * K}"    # the 426 x 240 view at K x in every build
    env["ARCUS_SCREENSHOT_PRESENTED"] = "1"         # SCREENSHOT = what the player sees (CRT on)
    try:
        p = subprocess.run([str(exe), "--no-launcher", "--turbo", "--input-script", str(d / "run.input")],
                           cwd=d, env=env, capture_output=True, text=True, timeout=300)
    except (subprocess.TimeoutExpired, OSError) as e:
        print(f"[guide] warning: capture run failed ({e})", file=sys.stderr)
        return False
    (d / "log.txt").write_text(p.stdout + p.stderr)
    return p.returncode == 0


SETTINGS = {"settings_version": 6, "crt_preset": 1, "color_lut": 1, "minimap": 0, "magic_info": 0,
            "cheat_health": 1}


def capture_spell(exe, tmp, c, lv):
    """One cast of character c's level lv spell, picked in the item window like a player (the
    pick also loads the spell's graphics; writing the level alone would not)."""
    d = Path(tmp) / f"spell_{c}_{lv}"
    s = BOOT_SELECT + ["PRESS RIGHT 4", "WAIT 30"] * c + TO_ACT1 + COURTYARD + ["WRITE_RAM8 FF8238 6", "WAIT 20"]
    s += key("A") + key("UP") + key("RIGHT") * (lv - 1) + key("A") + key("B") + ["WAIT 30"]
    s += [f"WRITE_RAM16 FF8500 {ENEMY_X:X}", f"WRITE_RAM16 FF8502 {ENEMY_Y:X}",
          "WAIT 4", "DUMP_RAM @OUT@/ram.bin", "SCREENSHOT @OUT@/s00.png", "PRESS B 4"]
    t = 4
    for k in sorted(set(SHOTS) | set(SHOT_PICK.get((c, lv), {}).get("frames", []))):
        s += [f"WAIT {k - t}", f"SCREENSHOT @OUT@/s{k:02d}.png"]
        t = k
    s += ["EXIT 0"]
    return run_game(exe, d, s, SETTINGS), d


def capture_select(exe, tmp):
    d = Path(tmp) / "select"
    s = BOOT_SELECT + ["SCREENSHOT @OUT@/sel0.png", "PRESS RIGHT 4", "WAIT 30", "SCREENSHOT @OUT@/sel1.png", "EXIT 0"]
    return run_game(exe, d, s, SETTINGS), d


def capture_items(exe, tmp, ids, name):
    """The item window with the given item ids in slots 1.. and 6 crystals."""
    d = Path(tmp) / name
    s = BOOT_SELECT + TO_ACT1 + ["WAIT 60", "WRITE_RAM8 FF8238 6"]
    for slot, it in enumerate(ids, 1):
        s += [f"WRITE_RAM8 FF{0x823B + slot:04X} {it:X}", f"WRITE_RAM8 FF{0x8240 + slot:04X} 3"]
    s += ["WAIT 4", "PRESS A 4", "WAIT 40", "SCREENSHOT @OUT@/win.png", "EXIT 0"]
    return run_game(exe, d, s, SETTINGS), d


def capture_jewel(exe, tmp):
    """A Power Jewel on the floor of a boss area: the floor alone, then the jewel red and blue."""
    d = Path(tmp) / "jewel"
    base = 0x8500 + 0x1C * JEWEL_SLOT
    s = BOOT_SELECT + ["PRESS START 4", "WAIT_RAM8 FF8082 7", f"WRITE_RAM8 FF828E {JEWEL_AREA:X}", "WAIT 30",
                       "PRESS START 4", "WAIT_RAM8 FF8292 1", "WAIT 60", "SCREENSHOT @OUT@/floor.png"]
    s += [f"WRITE_RAM8 FF{base + i:04X} {b:X}" for i, b in enumerate(JEWEL)]
    s += [f"WRITE_RAM16 FF8138 {JEWEL_SLOT + 1:X}", "WAIT 3", "SCREENSHOT @OUT@/red.png",
          "WAIT 30", "SCREENSHOT @OUT@/blue.png", "EXIT 0"]
    return run_game(exe, d, s, SETTINGS), d


# ---- picture helpers ---------------------------------------------------------------------

def raster_origin(im):
    """Where the 320 x 224 game raster (at K x) sits in a presented frame (centred)."""
    return (im.width - 320 * K) // 2, (im.height - 224 * K) // 2


def crop_raster(im, x0, y0, x1, y1):
    """A crop given in raster pixels, at the capture's K x."""
    ox, oy = raster_origin(im)
    return im.crop((ox + x0 * K, oy + y0 * K, ox + x1 * K, oy + y1 * K))


def blobs(mask, keep_out):
    """Bounding boxes of the 8-connected groups of set pixels in a raster-size 0/255 mask, at
    least 3 x 3 (smaller ones are edge noise), leaving out those touching a keep_out box
    (x0, y0, x1, y1)."""
    w, h = mask.size
    px = mask.load()
    seen, out = set(), []
    for y in range(h):
        for x in range(w):
            if not px[x, y] or (x, y) in seen:
                continue
            stack, box = [(x, y)], [x, y, x, y]
            seen.add((x, y))
            while stack:
                a, b = stack.pop()
                box = [min(box[0], a), min(box[1], b), max(box[2], a), max(box[3], b)]
                for da in (-1, 0, 1):
                    for db in (-1, 0, 1):
                        q = (a + da, b + db)
                        if 0 <= q[0] < w and 0 <= q[1] < h and q not in seen and px[q]:
                            seen.add(q)
                            stack.append(q)
            touches = any(box[2] >= k[0] and box[0] <= k[2] and box[3] >= k[1] and box[1] <= k[3]
                          for k in keep_out)
            if not touches and box[2] - box[0] >= 2 and box[3] - box[1] >= 2:
                out.append(box)
    return out


def best_shot(d, Image, ImageChops, pick=None):
    """The frame after B that differs most from the one before it (not a full-screen flash),
    cropped to SPELL_BOX around what moved."""
    from PIL import ImageStat
    pick = pick or {}
    base = crop_raster(Image.open(d / "s00.png").convert("RGB"), 0, 0, 320, 184)   # the playfield
    best, score, bbox = None, -1, None
    if pick.get("blobs"):
        import struct
        ram = (d / "ram.bin").read_bytes()
        px, py = struct.unpack(">H", ram[0x8118:0x811A])[0] - 128, struct.unpack(">H", ram[0x811C:0x811E])[0] - 128
        ex, ey = ENEMY_X - 128, ENEMY_Y - 128
        keep_out = [(px - 24, py - 40, px + 24, py + 36),        # raster: the caster, the placed enemy
                    (ex - 6, ey - 6, ex + 16, ey + 32)]
        for k in pick["frames"]:
            f = d / f"s{k:02d}.png"
            if not f.exists():
                continue
            im = crop_raster(Image.open(f).convert("RGB"), 0, 0, 320, 184)
            diff = ImageChops.difference(im, base).convert("L").point(lambda v: 255 if v > 40 else 0)
            got = blobs(diff.resize((320, 184), Image.NEAREST), keep_out)
            if len(got) >= pick["blobs"]:
                best, score = im, sum((b[2] - b[0] + 1) * (b[3] - b[1] + 1) for b in got) * K * K
                bbox = (min(b[0] for b in got) * K, min(b[1] for b in got) * K,
                        (max(b[2] for b in got) + 1) * K, (max(b[3] for b in got) + 1) * K)
                break
    for k in ([] if best is not None else pick.get("frames", SHOTS)):
        f = d / f"s{k:02d}.png"
        if not f.exists():
            continue
        im = crop_raster(Image.open(f).convert("RGB"), 0, 0, 320, 184)
        if ImageStat.Stat(im.convert("L")).mean[0] > 150:
            continue                                         # a flash
        diff = ImageChops.difference(im, base).convert("L").point(lambda v: 255 if v > 40 else 0)
        n = diff.histogram()[255]
        if n > score:
            best, score, bbox = im, n, diff.getbbox()
    if best is None:
        return None, 0
    bw, bh = SPELL_BOX
    cx, cy = (160 * K, 92 * K) if not bbox else ((bbox[0] + bbox[2]) // 2, (bbox[1] + bbox[3]) // 2)
    x0 = min(max(0, cx // K - bw // 2), 320 - bw) * K
    y0 = min(max(0, cy // K - bh // 2), 184 - bh) * K
    return best.crop((x0, y0, x0 + bw * K, y0 + bh * K)), score // (K * K)


def portraits(d, Image):
    """The four select-screen portraits (cursor elsewhere for each)."""
    sel0 = Image.open(d / "sel0.png").convert("RGB")          # cursor on Jedda
    sel1 = Image.open(d / "sel1.png").convert("RGB")          # cursor on Erin
    boxes = [(9, 57, 71, 205), (89, 33, 151, 181), (169, 33, 231, 181), (249, 57, 311, 205)]   # raster
    return [crop_raster(sel1 if i == 0 else sel0, *box) for i, box in enumerate(boxes)]


def icons(d, Image, count):
    """Item icons from the window's third row (16 x 16 cells from raster (40, 96), the M first)
    and the first crystal of the row above."""
    win = Image.open(d / "win.png").convert("RGB")
    got = [crop_raster(win, 40 + 16 * i, 96, 56 + 16 * i, 112) for i in range(1, count + 1)]
    return got, crop_raster(win, 40, 72, 56, 88)


def jewels(d, Image, ImageChops):
    """The Power Jewel, blue and red: 16 x 16 raster cells around what the jewel changed."""
    floor = Image.open(d / "floor.png").convert("RGB")
    out = []
    for name in ("blue", "red"):
        im = Image.open(d / f"{name}.png").convert("RGB")
        bbox = ImageChops.difference(im, floor).convert("L").point(lambda v: 255 if v > 40 else 0).getbbox()
        if not bbox:
            return None
        ox, oy = raster_origin(im)
        cx, cy = ((bbox[0] + bbox[2]) // 2 - ox) // K, ((bbox[1] + bbox[3]) // 2 - oy) // K
        cell = (cx - 8, cy - 8, cx + 8, cy + 8)
        # The jewel alone, on black like the item icons: keep what differs from the floor.
        mask = ImageChops.difference(crop_raster(im, *cell), crop_raster(floor, *cell)).convert("L") \
            .point(lambda v: 255 if v > 24 else 0)
        out.append(Image.composite(crop_raster(im, *cell), Image.new("RGB", mask.size), mask))
    return out


# ---- page -------------------------------------------------------------------------------

W = 1200
BG, INK, DIM, GOLD, RULE = (0, 0, 0), (232, 228, 216), (150, 146, 136), (240, 216, 144), (64, 64, 72)


def compose(port, spells, item_icons, crystal, jewel, Image, ImageDraw, ImageFont):
    """The page and where the jewel's picture is on it (or None). Captures are pasted at their
    captured size (K x), never resampled."""
    font = lambda n: ImageFont.load_default(size=n)
    F_TITLE, F_H1, F_H2, F_BODY, F_SMALL = font(60), font(42), font(33), font(29), font(24)
    page = Image.new("RGB", (W, 16000), BG)
    dr = ImageDraw.Draw(page)
    y = 48

    def wrap(text, f, width):
        words, lines, cur = text.split(), [], ""
        for w in words:
            t = (cur + " " + w).strip()
            if dr.textlength(t, font=f) <= width:
                cur = t
            else:
                lines.append(cur)
                cur = w
        if cur:
            lines.append(cur)
        return lines

    def para(x, y, text, f, col, width, gap=6):
        for line in wrap(text, f, width):
            dr.text((x, y), line, font=f, fill=col)
            y += f.size + gap
        return y

    def heading(y, text):
        y += 26
        dr.text((60, y), text, font=F_H1, fill=GOLD)
        y += F_H1.size + 12
        dr.line((60, y, W - 60, y), fill=RULE, width=2)
        return y + 24

    dr.text((60, y), "Arcus Odyssey", font=F_TITLE, fill=GOLD)
    y += F_TITLE.size + 8
    dr.text((62, y), "Modern manual: characters, magic and items", font=F_H2, fill=DIM)
    y += F_H2.size + 10

    y = heading(y, "Characters")
    for (name, sub, l1, l2), im in zip(CHARACTERS, port):
        page.paste(im, (60, y))
        tx = 60 + im.width + 36
        ty = y + im.height // 4
        dr.text((tx, ty), name, font=F_H2, fill=INK)
        dr.text((tx, ty + F_H2.size + 6), sub, font=F_SMALL, fill=DIM)
        ty = para(tx, ty + F_H2.size + F_SMALL.size + 26, l1, F_BODY, INK, W - 60 - tx)
        para(tx, ty + 8, l2, F_BODY, DIM, W - 60 - tx)
        y += im.height + 20

    y = heading(y, "Magic")
    for text in MAGIC_HOWTO:
        y = para(60, y, text, F_BODY, INK, W - 120) + 10
    for (name, _, _, _), levels, shots in zip(CHARACTERS, SPELLS, spells):
        y += 24
        dr.text((60, y), name.split()[0], font=F_H2, fill=GOLD)
        y += F_H2.size + 16
        for i, ((spell, casts, what), im) in enumerate(zip(levels, shots)):
            tx, iy = 60, y
            if im is not None:
                page.paste(im, (60, y))
                tx = 60 + im.width + 32
            dr.text((tx, iy + 8), f"Lv{i + 1}  {spell}", font=F_H2, fill=INK)
            dr.text((tx, iy + 8 + F_H2.size + 6), f"{casts} cast{'s' if casts != 1 else ''}", font=F_SMALL, fill=DIM)
            ty = para(tx, iy + 8 + F_H2.size + F_SMALL.size + 22, what, F_BODY, DIM, W - 60 - tx)
            y = max(ty, y + (im.height if im is not None else 0)) + 24

    y = heading(y, "Items")
    jewel_at = None
    for key_, name, what in ITEMS:
        icon = crystal if key_ == "crystal" else jewel[0] if key_ == "jewel" and jewel else item_icons.get(key_)
        if icon is not None:
            page.paste(icon, (60, y + 2))
            if key_ == "jewel":
                jewel_at = (60, y + 2)                     # blue here; the viewer swaps in red
        tx = 60 + 16 * K + 28
        dr.text((tx, y), name, font=F_H2, fill=INK)
        y = max(para(tx, y + F_H2.size + 6, what, F_BODY, DIM, W - 60 - tx), y + 16 * K + 8) + 22

    return page.crop((0, 0, W, y + 40)), jewel_at


# ---- main -------------------------------------------------------------------------------

class _keep:
    """ARCUS_GUIDE_KEEP=dir: capture into a kept folder (to look at the raw captures)."""
    def __init__(self, d): self.d = d
    def __enter__(self):
        Path(self.d).mkdir(parents=True, exist_ok=True)
        return self.d
    def __exit__(self, *a): return False


def stamp_for(rom):
    h = hashlib.sha256(Path(__file__).read_bytes())
    if rom and Path(rom).exists():
        h.update(Path(rom).read_bytes())
    return h.hexdigest() + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    # rom/arcus-odyssey-usa.bin: the public repo (build.py); ../rom/: the development tree
    rom = WORK / "rom" / "arcus-odyssey-usa.bin"
    ap.add_argument("--rom", default=str(rom if rom.exists() else ROOT.parent / "rom" / "arcus.bin"))
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    want = stamp_for(a.rom)
    if not a.force and (OUT / "stamp.txt").exists() and (OUT / "stamp.txt").read_text() == want \
            and any(OUT.glob("page*.jpg")):
        print("[guide] up to date")
        return 0
    try:
        from PIL import Image, ImageChops, ImageDraw, ImageFont
        ImageFont.load_default(size=20)                      # Pillow >= 10.1 (FreeType Aileron)
    except Exception as e:
        print(f"[guide] warning: Pillow >= 10.1 needed ({e}); building without the guide", file=sys.stderr)
        return 0
    exe = Path(a.exe).resolve()
    if not exe.exists():
        print(f"[guide] warning: no game binary at {exe}; building without the guide", file=sys.stderr)
        return 0
    keep = os.environ.get("ARCUS_GUIDE_KEEP")
    with (tempfile.TemporaryDirectory(prefix="arcus-guide-") if not keep else _keep(keep)) as tmp:
        jobs = [("select", None, None), ("items", (1, 2, 3, 4, 6), "items_0"), ("items", (5, 7), "items_1"),
                ("jewel", None, None)]
        jobs += [("spell", c, lv) for c, levels in enumerate(SPELLS) for lv in range(1, len(levels) + 1)]

        def do(job):
            kind, x, y = job
            if kind == "select":
                return job, capture_select(exe, tmp)
            if kind == "items":
                return job, capture_items(exe, tmp, x, y)
            if kind == "jewel":
                return job, capture_jewel(exe, tmp)
            return job, capture_spell(exe, tmp, x, y)

        with ThreadPoolExecutor(max_workers=max(2, min(8, (os.cpu_count() or 4) - 1))) as pool:
            results = list(pool.map(do, jobs))
        if not all(ok for _, (ok, _) in results):
            print("[guide] warning: a capture failed; building without the guide", file=sys.stderr)
            return 0
        dirs = {job: d for job, (ok, d) in results}
        port = portraits(dirs[("select", None, None)], Image)
        item_icons, crystal = {}, None
        for job in jobs[1:3]:
            got, crys = icons(dirs[job], Image, len(job[1]))
            item_icons.update(zip(job[1], got))
            crystal = crystal or crys
        jewel = jewels(dirs[("jewel", None, None)], Image, ImageChops)
        if not jewel:
            print("[guide] note: the Power Jewel wasn't captured; shown without a picture", file=sys.stderr)
        spells, weak = [], []
        for c, levels in enumerate(SPELLS):
            row = []
            for lv in range(1, len(levels) + 1):
                ram = (dirs[("spell", c, lv)] / "ram.bin").read_bytes()
                if (ram[0x8218], ram[0x8236]) != (c, lv):            # character, magic level picked
                    print(f"[guide] warning: spell {c}/{lv}: got character {ram[0x8218]} level {ram[0x8236]}; "
                          "building without the guide", file=sys.stderr)
                    return 0
                im, score = best_shot(dirs[("spell", c, lv)], Image, ImageChops, SHOT_PICK.get((c, lv)))
                row.append(im)
                if score < 40:
                    weak.append(f"{c}/{lv}:{score}")
            spells.append(row)
        if weak:
            print(f"[guide] note: little movement captured for {', '.join(weak)}", file=sys.stderr)
        page, jewel_at = compose(port, spells, item_icons, crystal, jewel, Image, ImageDraw, ImageFont)
    for d in (OUT, FRAME_OUT):
        if d.exists():
            shutil.rmtree(d)                                 # only our own generated pictures
    OUT.mkdir(parents=True)
    if jewel_at:
        FRAME_OUT.mkdir(parents=True)
        jewel[1].save(FRAME_OUT / "page01.jpg", quality=95)
        (FRAME_OUT / "book.txt").write_text(f"frame|guide-frame|Power Jewel|{jewel_at[0]}|{jewel_at[1]}|{JEWEL_PERIOD}\n")
    strip = 1000
    for i, top in enumerate(range(0, page.height, strip), 1):
        page.crop((0, top, W, min(page.height, top + strip))).save(OUT / f"page{i:02d}.jpg", quality=88)
    (OUT / "book.txt").write_text("scroll|guide|Modern manual|\n")
    (OUT / "stamp.txt").write_text(want)
    print(f"[guide] {W} x {page.height} px in {len(list(OUT.glob('page*.jpg')))} strips")
    return 0


if __name__ == "__main__":
    sys.exit(main())
