#!/usr/bin/env python3
"""Make the app icon: out/icon.jpg (256x256 JPEG without EXIF, for the .nro and the NSP) and
out/icon-1024.png (the Mac iconset, up to 512@2x).

Source, first available:
  1. composed from public art, downloaded and pinned by tools/fetch-extras.py (same cache,
     extras-cache/art/): the US cover painting (EXTRAS > ARTWORK > BOX ARTWORK, a 1280 x 720
     recreation from LaunchBox's fan art), its full-height 720 x 720 square at x 320
     (the old icon's framing), with LaunchBox's US clear logo over the bottom (88% wide, its
     bottom at 96.5%);
  2. a title-screen capture of the native build (fallback: offline, or a file changed).
Both are game art, so they stay out of git. The capture is cached as
out/title.png (taken once, headless, with the Mac executable: boot, press START once in
the intro, capture at ~frame 1300) and turned into out/icon.jpg by padding the 256x224
H32 title to 256x256 with black (no resampling, so the pixel art stays sharp).

usage: make_icon.py [--exe build/ArcusOdyssey] [--out out]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def capture(exe, png):
    work = tempfile.mkdtemp(prefix="arcus-icon-")   # input-script paths stop at spaces
    if " " in work:
        sys.exit(f"temp dir {work} contains a space; set TMPDIR")
    script = Path(work) / "icon.input"
    script.write_text(f"WAIT 1200\nPRESS START 4\nWAIT 96\nSCREENSHOT {work}/title.png\nWAIT 2\nEXIT 0\n")
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_RENDER_DRIVER="software", SDL_AUDIODRIVER="dummy")
    subprocess.run([str(exe), "--no-launcher", "--turbo", "--input-script", str(script)],
                   cwd=work, env=env, check=True, capture_output=True, timeout=300)
    shutil.copyfile(Path(work) / "title.png", png)
    shutil.rmtree(work, ignore_errors=True)


def to_icon(png, jpg):
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image:
        im = Image.open(png).convert("RGB")
        if im.width > 256:                      # H40 capture: centre 256 columns
            x = (im.width - 256) // 2
            im = im.crop((x, 0, x + 256, im.height))
        canvas = Image.new("RGB", (256, 256))
        canvas.paste(im, ((256 - im.width) // 2, (256 - im.height) // 2))
        canvas.save(jpg, "JPEG", quality=92)
    else:                                       # macOS fallback
        subprocess.run(["sips", "-s", "format", "jpeg", "--cropToHeightWidth", "224", "256",
                        "--padToHeightWidth", "256", "256", "--padColor", "000000",
                        str(png), "--out", str(jpg)], check=True, capture_output=True)


ICON_CROP = (320, 0, 1040, 720)       # in the 1280 x 720 painting
LOGO_W, LOGO_BOTTOM = 0.88, 0.965


def composed(out):
    """out/icon-1024.png and out/icon.jpg from the painting and the logo; False if unavailable."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("fetch_extras", ROOT / "tools" / "fetch-extras.py")
    fx = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fx)
    art = next(i for i in fx.ART if i["name"] == "art-box-artwork")
    paint = fx.fetch_art_file(art["name"], 1, *art["files"][0])
    logo = fx.fetch(fx.ICON_LOGO, fx.CACHE / "art" / "art-logo.png")
    if not (paint and logo):
        return False
    from PIL import Image
    S = 1024
    im = Image.open(paint).convert("RGB").crop(ICON_CROP).resize((S, S), Image.LANCZOS)
    lg = Image.open(logo).convert("RGBA")
    w = int(S * LOGO_W)
    h = round(w * lg.height / lg.width)
    lg = lg.resize((w, h), Image.LANCZOS)
    im.paste(lg, ((S - w) // 2, int(S * LOGO_BOTTOM) - h), lg)
    im.save(out / "icon-1024.png")
    im.resize((256, 256), Image.LANCZOS).save(out / "icon.jpg", "JPEG", quality=92)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "build" / "ArcusOdyssey"))
    ap.add_argument("--out", default=str(ROOT / "out"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    png, jpg = out / "title.png", out / "icon.jpg"
    try:
        if composed(out):
            print(f"wrote {jpg} and {out / 'icon-1024.png'} (box artwork + logo)")
            return
    except ImportError:
        print("make_icon: no Pillow; using the title capture", file=sys.stderr)
    (out / "icon-1024.png").unlink(missing_ok=True)     # our own output: no stale large icon
    if not png.exists():
        if not Path(a.exe).exists():
            sys.exit(f"no cached {png} and no Mac build at {a.exe} to capture it")
        capture(Path(a.exe).resolve(), png)
        print(f"captured {png}")
    to_icon(png, jpg)
    print(f"wrote {jpg}")


if __name__ == "__main__":
    main()
