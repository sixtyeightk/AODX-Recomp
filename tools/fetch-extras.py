#!/usr/bin/env python3
"""Fetch the extras from their public URLs and prepare them for embedding.

The repository carries only each extra's URL and SHA-256, never the file. The build scripts
(build-mac.sh, build-switch.sh) run this first:
  1. download into extras-cache/ (once; never published, like the build trees);
  2. check the SHA-256: a changed or unreachable file is skipped with a warning, and the
     build goes on without it (its menu entry is hidden);
  3. pages: each scanned page's picture is taken out of the PDF as a JPEG (tools/pdf_pages.py:
     pypdf and Pillow, no rendering) into a "book" folder,
     extras-cache/books/<NN-name>/page01.jpg ... with book.txt ("kind|group|title|detail").
     CMake embeds whatever books it finds there (see CMakeLists.txt).

The PDF work runs in a Python with pypdf and Pillow (a venv in extras-cache/.venv, made on
first use); the magazine reviews come from tools/fetch-reviews.py, run with it too.

Every item names where it came from ("source": the page it was found on, "credit": who
hosts or preserves it); CREDITS.md lists them all for the README's special thanks.

Never fails the build: exit status 0 unless --strict and something was skipped.

usage: tools/fetch-extras.py [--strict]
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = Path(os.environ.get("ARCUS_WORK") or ROOT) / "extras-cache"   # build/ (build.py) or here

MANUAL = {
    "name": "manual",
    "book": "01-manual",
    "credit": "Evercade (Blaze Entertainment), hosting the manual for its re-release",
    "source": "https://evercade.info/",
    "book_txt": "pages|manual|Original manual|",
    # The original Renovation US manual (1991), as hosted for the Evercade re-release.
    "url": "https://evercade.info/wp-content/uploads/2021/07/Arcusodyssey_md_us_manual.pdf",
    "sha256": "8340d04d56e6c078a4e64798c7d98f2ba97c5921af424a34c59b11d48415209c",
    "pages": 32,
    "height": 1500,      # px per page: readable at handheld 1280 x 720 when zoomed to width
    "quality": 85,
}

MANUAL_JP = {
    "name": "manual-jp",
    "book": "04-manual-jp",
    "credit": "Games Database (gamesdatabase.org)",
    "source": "https://www.gamesdatabase.org/game/sega-genesis/arcus-odyssey",
    "book_txt": "pages|manual-jp|Japanese manual|",
    # The original Wolf Team manual (1991, T-32053), full colour, as scanned by gamesdatabase.org
    # (the file name says Renovation; the booklet is the Japanese one).
    "url": "https://www.gamesdatabase.org/Media/SYSTEM/Sega_Genesis//Manual/formated/Arcus_Odyssey_-_1991_-_Renovation_Products.pdf",
    "sha256": "f8326c3312b2ce592981b3367b52639ea21fc318b1c1ca7c2ab958b59d31c0a9",
    "pages": 32,
    "dpi": 150,          # the scans' own resolution (about 1120 x 968 a spread); no resampling
    "quality": 85,
    # Each PDF page is a two-page spread, cut at its fold (px at 150 dpi, measured per spread)
    # into single pages so the viewer pairs them like the US manual. The first spread is the
    # back cover (left) beside the front cover (right): the front opens the book, the back ends it.
    "split": [560, 551, 555, 552, 553, 555, 558, 558, 567, 565, 564, 565, 565, 563, 567, 565],
}


LAUNCHBOX = "LaunchBox Games Database"
LAUNCHBOX_PAGE = "https://gamesdb.launchbox-app.com/games/images/4022-arcus-odyssey"
LB = "https://images.launchbox-app.com/"

# Pictures (EXTRAS > ARTWORK): each book's pages are the downloaded JPEGs as they are
# (baseline JPEG, which the viewer's stb_image decodes). book.txt's detail is a caption the
# viewer shows in its hint strip.
ART = [
    {"name": "art-us-box", "book": "05-art-us-box", "book_txt": "pages|art|US box|",
     "credit": LAUNCHBOX, "source": LAUNCHBOX_PAGE,
     "files": [(LB + "80fccbb4-ec9b-4b0d-b341-94d691474edc.jpg", "b524dda504f5856d9ad6bce3a9854fcbba0c55a67978a7b231f04c787a89878f"),
               (LB + "83cda5ad-868b-432a-844e-8a8dcca772ce.jpg", "92a9e753b8f901eaffd8c888b0e359f98111bb060334184b4e0d1b6cfed641a0")]},
    {"name": "art-jp-box", "book": "06-art-jp-box", "book_txt": "pages|art|Japanese box|",
     "credit": LAUNCHBOX, "source": LAUNCHBOX_PAGE,
     "files": [(LB + "fd589574-fd15-4f62-85b5-f1845a3c2bbf.jpg", "155a1da665ad2daf1462f3c4f606dc51ad7829409b46cc018c8edcd648246d71"),
               (LB + "d73fb153-03de-4c91-845a-f71af1173bf6.jpg", "8f4095933c131e770a69d805305b738d29ea29d4336812fd29dedaa545ebb87a")]},
    # The US cover painting without the box: an AI-made recreation expanded to 1280 x 720,
    # first seen as a YouTube thumbnail, now on LaunchBox as Fanart - Background (the same
    # bytes). The app icon is cut from it too.
    {"name": "art-box-artwork", "book": "07-art-box-artwork", "book_txt": "pages|art|Box artwork|",
     "credit": LAUNCHBOX + " (fan art: an AI-made recreation of the US cover painting)",
     "source": LAUNCHBOX_PAGE,
     "files": [(LB + "c6298eda-1a42-4437-86cc-459c4b11fee3.jpg", "108a1c75ed7b4f0af1979a7d1a3f449d8c3830caa4f66c10c3da843e4086ccb0")]},
    {"name": "art-ad", "book": "08-art-ad", "book_txt": "pages|art|Magazine ad|",
     "credit": LAUNCHBOX, "source": LAUNCHBOX_PAGE,
     "files": [(LB + "18c6d3c0-481f-418c-b2d4-9993a8493eda.jpg", "b3d685586c51db1869260510021cdc8b39c6e7c15820fc2b7c288f212b11b8ec")]},
    # Julie Bell's painting for Renovation's cancelled Super Nintendo version (1992, oil on
    # board; printed in EGM #51 and the 1993 Super NES Game Guide). The Game Curator collection.
    {"name": "art-snes", "book": "09-art-snes", "book_txt": "pages|art|Unreleased SNES cover|Julie Bell, 1992: cancelled SNES version",
     "credit": "Original Video Game Art (OVGA), The Game Curator collection; painting by Julie Bell",
     "source": "https://www.originalvideogameart.com/index.php?/home/arcus-odyssey-unreleased-super-nintendo-box-cover-art-r457/",
     "files": [("https://www.originalvideogameart.com/uploads/monthly_2024_08/J-131.jpg.c90e14610fd2753dc95e748fc72adb3e.jpg",
                "a908c1bc9d9a2195c0cb3a343fc456ec7de959a58100d24fd5cbe4ec6a92e9d4")]},
]

# The app icon's two layers (tools/make_icon.py): the box artwork above and the US logo.
ICON_LOGO = {"name": "art-logo", "credit": LAUNCHBOX, "source": LAUNCHBOX_PAGE,
             "url": LB + "be0a126d-a567-4095-b502-d70bb7e05b54.png",
             "sha256": "84e0c6504b8a27630dd2af1fd842c5ff706b1541ef9b01c81005858dc3152444"}


def warn(msg):
    print(f"[extras] warning: {msg}", file=sys.stderr)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(item, dest=None):
    """The verified download in the cache (item: name, url, sha256), or None."""
    dest = dest or CACHE / f"{item['name']}.pdf"
    if dest.exists() and sha256(dest) == item["sha256"]:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(".part")
    try:
        req = urllib.request.Request(item["url"], headers={"User-Agent": "AODX-Recomp-builder/1.0 (+https://github.com/sixtyeightk/AODX-Recomp)"})
        with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
    except Exception as e:                                   # offline, moved, blocked
        warn(f"{item['name']}: download failed ({e}); building without it")
        tmp.unlink(missing_ok=True)
        return None
    got = sha256(tmp)
    if got != item["sha256"]:
        warn(f"{item['name']}: the file at {item['url']} changed (sha256 {got[:16]}...); building without it")
        tmp.unlink(missing_ok=True)
        return None
    tmp.replace(dest)
    print(f"[extras] {item['name']}: downloaded {dest.stat().st_size // 1024} KB")
    return dest


def render(item, pdf):
    """extras-cache/<name>/pageNN.jpg for every page; True when they are in place."""
    out = CACHE / "books" / item["book"]
    stamp = out / "stamp.txt"
    want = f"{item['sha256']} {item.get('height')} {item.get('split')} {item['quality']} pdf_pages\n"
    pages = sorted(out.glob("page*.jpg"))
    if len(pages) == item["pages"] and stamp.exists() and stamp.read_text() == want:
        return True
    py = pdf_python()
    if not py:
        return False
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [str(py), str(ROOT / "tools" / "pdf_pages.py"), str(pdf), tmp, "--quality", str(item["quality"])]
        if "split" in item:
            cmd += ["--split", ",".join(map(str, item["split"]))]
        else:
            cmd += ["--height", str(item["height"])]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            warn(f"{item['name']}: couldn't take the pages out ({r.stderr.strip().splitlines()[-1:]}); building without it")
            return False
        made = sorted(Path(tmp).glob("p-*.jpg"))
        if len(made) != item["pages"]:
            warn(f"{item['name']}: expected {item['pages']} pages, got {len(made)}; building without it")
            return False
        if out.exists():
            shutil.rmtree(out)                               # only our own rendered pages
        out.mkdir(parents=True)
        for i, p in enumerate(made, 1):
            shutil.move(str(p), out / f"page{i:02d}.jpg")
    (out / "book.txt").write_text(item["book_txt"] + "\n")
    stamp.write_text(want)
    size = sum(p.stat().st_size for p in out.glob("page*.jpg"))
    print(f"[extras] {item['name']}: {item['pages']} pages, {size // 1024} KB")
    return True


PDF_PACKAGES = ["pypdf==7.0.0", "Pillow==12.3.0"]   # both pip packages: no system libraries
_PY = []


def pdf_python():
    """A Python with pypdf and Pillow for the PDF work: extras-cache/.venv, made once."""
    if _PY:
        return _PY[0]
    venv = CACHE / ".venv"
    py = venv / "bin" / "python3"
    probe = [str(py), "-c", "import pypdf, PIL"]
    if not (py.exists() and subprocess.run(probe, capture_output=True).returncode == 0):
        try:
            CACHE.mkdir(parents=True, exist_ok=True)
            subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True, capture_output=True)
            subprocess.run([str(py), "-m", "pip", "install", "-q", "--disable-pip-version-check",
                            *PDF_PACKAGES], check=True, capture_output=True)
        except (subprocess.CalledProcessError, OSError) as e:
            warn(f"couldn't set up pypdf and Pillow ({e}); building without the manuals and reviews")
            _PY.append(None)
            return None
    _PY.append(py)
    return py


def fetch_art_file(name, i, url, digest):
    """One picture of an ART item, verified, in extras-cache/art/ (or None)."""
    ext = Path(url.split("?")[0]).suffix or ".jpg"
    return fetch({"name": f"{name}-{i}", "url": url, "sha256": digest}, CACHE / "art" / f"{name}-{i}{ext}")


def art_book(item):
    """extras-cache/books/<book>/pageNN.jpg: the item's pictures as they are."""
    out = CACHE / "books" / item["book"]
    want = " ".join(d for _, d in item["files"]) + "\n" + item["book_txt"] + "\n"
    stamp = out / "stamp.txt"
    if stamp.exists() and stamp.read_text() == want and len(list(out.glob("page*.jpg"))) == len(item["files"]):
        return True
    got = [fetch_art_file(item["name"], i, u, d) for i, (u, d) in enumerate(item["files"], 1)]
    if not all(got):
        return False
    if out.exists():
        shutil.rmtree(out)                                   # only our own pages
    out.mkdir(parents=True)
    for i, f in enumerate(got, 1):
        shutil.copyfile(f, out / f"page{i:02d}.jpg")
    (out / "book.txt").write_text(item["book_txt"] + "\n")
    stamp.write_text(want)
    print(f"[extras] {item['name']}: {len(got)} picture(s)")
    return True


def main():
    ok = True
    for item in ART:
        if not art_book(item):
            ok = False
            shutil.rmtree(CACHE / "books" / item["book"], ignore_errors=True)
    for item in (MANUAL, MANUAL_JP):
        pdf = fetch(item)
        if not (pdf and render(item, pdf)):
            ok = False
            shutil.rmtree(CACHE / "books" / item["book"], ignore_errors=True)   # no stale pages get embedded
    py = pdf_python()
    if py:
        r = subprocess.run([str(py), str(ROOT / "tools" / "fetch-reviews.py")])
        ok = ok and r.returncode == 0
    else:
        ok = False
    return 0 if ok or "--strict" not in sys.argv else 1


if __name__ == "__main__":
    sys.exit(main())
