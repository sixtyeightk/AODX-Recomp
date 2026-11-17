#!/usr/bin/env python3
"""Fetch English-language magazine reviews of Arcus Odyssey (1991-92) for Extras > Reviews.

The scans are Sega Retro's (segaretro.org), hosted on retrocdn.net as whole-magazine PDFs of
37-490 MB. Only each article's pages are read, with HTTP Range requests and pypdf; the pages
are then taken out as JPEGs by tools/pdf_pages.py, like the manual. Nothing is committed or published:
everything lives in extras-cache/ (never versioned), and the build embeds it.

  extras-cache/reviews/<id>-<pages>.pdf    the article's pages only (cache; named by its pages,
                                           so a changed page list extracts again)
  extras-cache/books/2N-review-<id>/       page01.jpg ... and book.txt for CMake

Each article is pinned to its source file's size and page count (both known before reading
any page); a file that changed is skipped with a warning. Never fails the build.

Run by tools/fetch-extras.py with a Python that has pypdf and Pillow (extras-cache/.venv).
usage: fetch-reviews.py
"""
import hashlib
import os
import io
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import pdf_pages  # tools/pdf_pages.py (needs pypdf and Pillow; main() reports if missing)
except ImportError:
    pdf_pages = None

ROOT = Path(__file__).resolve().parent.parent
CACHE = Path(os.environ.get("ARCUS_WORK") or ROOT) / "extras-cache"   # build/ (build.py) or here
HOST = "https://retrocdn.net/images"
UA = {"User-Agent": "AODX-Recomp-builder/1.0 (+https://github.com/sixtyeightk/AODX-Recomp)"}
HEIGHT, QUALITY = 1500, 85

# id, Sega Retro file, its size in bytes and pages, the article's PDF pages, menu title,
# issue, Sega Retro's score out of 100. Spans checked by eye (2026-11-09): Sega Retro lists
# only the first page. VG&CE skips an advert (47) and starts on 46 (45 is another game's
# review). Left out on purpose: Sega Pro (UK) issues 2 and 18 and Console XS 1.
REVIEWS = [
    ("gamepro-26", "GamePro_US_026.pdf", 107219336, 76, [36, 38], "GamePro (US)", "Sep 1991", 96),
    ("egm-26", "EGM_US_026.pdf", 202120010, 148, [20], "EGM (US)", "Sep 1991", 68),
    ("vgce-34", "VG&CE_US_34.pdf", 214076486, 128, [46, 48], "VG&CE (US)", "Nov 1991", 70),
    ("gamesx-30", "GamesX_UK_30.pdf", 108440131, 48, [34], "Games-X (UK)", "Nov 1991", 60),
    ("megatech-1", "MegaTech_UK_01.pdf", 224757662, 84, [53, 54, 55], "MegaTech (UK)", "Xmas 1991", 86),
    ("gpsg-3-1", "GPSG_US_0301.pdf", 128250755, 76, [72], "Game Players (US)", "Feb 1992", 70),
    ("ace-54", "ACE_UK_54.pdf", 36958341, 116, [68], "ACE (UK)", "Mar 1992", 86),
]

# SHA-256 over each chosen page's scan (its largest picture's bytes, in page order), pinned
# like the other extras' files: a review whose pages changed is skipped.
PINS = {
    "gamepro-26": "f496faf9dc10113fc3d75b5d3fe73322127155b1cbab96082169b423347076f7",
    "egm-26": "091718319fc61a9bcf71c700b65acf23cdf4d0cf37641004327e651a3072eb0f",
    "vgce-34": "1f4a139002628679ce2d5544a418baf376015909107a45c0c5fa071c9b797e5b",
    "gamesx-30": "7b904cc121699f2f4802842e447cf2e61d6d8c184318b8b986213c8924f6a640",
    "megatech-1": "78ecabb07ffc079c286667fbc82d763d5de556e3cb16abc48a1735beaaba901f",
    "gpsg-3-1": "2131a93973473eda9b32a55f84a2e0c4eb46abca962446c49b1f35c648ad7310",
    "ace-54": "fd7e6ebc6dcd4557fafd053e4fd2d5a10b57ee8b5e5e759f638d2ad0262faebc",
}


def pages_digest(pages):
    h = hashlib.sha256()
    for p in pages:
        h.update(max(p.images, key=lambda i: i.image.width * i.image.height).data)
    return h.hexdigest()


def warn(msg):
    print(f"[reviews] warning: {msg}", file=sys.stderr)


def url_of(name):
    h = hashlib.md5(name.encode()).hexdigest()           # MediaWiki's hashed upload path
    return f"{HOST}/{h[0]}/{h[:2]}/{urllib.parse.quote(name)}"


class HttpFile(io.RawIOBase):
    """A read-only, seekable view of a remote file, fetched in 256 KB blocks on demand."""
    BLOCK = 256 * 1024

    def __init__(self, url):
        self.url, self.pos, self.cache = url, 0, {}
        req = urllib.request.Request(url, method="HEAD", headers=UA)
        with urllib.request.urlopen(req, timeout=30) as r:
            self.size = int(r.headers["Content-Length"])

    def readable(self): return True
    def seekable(self): return True
    def tell(self): return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def _block(self, i):
        if i not in self.cache:
            a, b = i * self.BLOCK, min(self.size, (i + 1) * self.BLOCK) - 1
            for attempt in range(4):
                try:
                    req = urllib.request.Request(self.url, headers=dict(UA, Range=f"bytes={a}-{b}"))
                    with urllib.request.urlopen(req, timeout=30) as r:
                        if r.status != 206:              # Range ignored: never read a whole magazine
                            raise OSError(f"HTTP {r.status} to a Range request")
                        data = r.read(b - a + 2)
                    if len(data) != b - a + 1:
                        raise OSError(f"{len(data)} bytes for a {b - a + 1}-byte range")
                    self.cache[i] = data
                    break
                except OSError:
                    if attempt == 3:
                        raise
                    time.sleep(2 * (attempt + 1))
        return self.cache[i]

    def read(self, n=-1):
        if n < 0:
            n = self.size - self.pos
        out = bytearray()
        while n > 0 and self.pos < self.size:
            blk = self._block(self.pos // self.BLOCK)
            o = self.pos % self.BLOCK
            chunk = blk[o:o + n]
            out += chunk
            self.pos += len(chunk)
            n -= len(chunk)
        return bytes(out)

    def readinto(self, b):
        d = self.read(len(b))
        b[:len(d)] = d
        return len(d)


def cache_pdf(rid, pages):
    return CACHE / "reviews" / f"{rid}-{'-'.join(map(str, pages))}.pdf"


def extract(rid, name, size, pages_total, pages):
    """The article's pages as a small PDF in the cache, or None."""
    import pypdf
    dest = cache_pdf(rid, pages)
    if dest.exists():
        return dest
    try:
        f = HttpFile(url_of(name))
        if f.size != size:
            warn(f"{rid}: {name} changed ({f.size} bytes); skipped")
            return None
        r = pypdf.PdfReader(f)
        if len(r.pages) != pages_total:
            warn(f"{rid}: {name} changed ({len(r.pages)} pages); skipped")
            return None
        chosen = [r.pages[p - 1] for p in pages]
        if pages_digest(chosen) != PINS[rid]:
            warn(f"{rid}: the pages in {name} changed; skipped")
            return None
        w = pypdf.PdfWriter()
        for pg in chosen:
            w.add_page(pg)
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_suffix(".part")
        with open(tmp, "wb") as fh:
            w.write(fh)
        tmp.replace(dest)
    except Exception as e:                                   # offline, moved, blocked, bad PDF
        warn(f"{rid}: {e}; skipped")
        return None
    print(f"[reviews] {rid}: {len(pages)} page(s)")
    return dest


def render(n, rid, pdf, title, issue, score, count):
    out = CACHE / "books" / f"{20 + n:02d}-review-{rid}"
    want = f"{pdf.name} {HEIGHT} {QUALITY} pdf_pages {title}|{issue}|{score}\n"
    stamp = out / "stamp.txt"
    if stamp.exists() and stamp.read_text() == want and len(list(out.glob("page*.jpg"))) == count:
        return True
    with tempfile.TemporaryDirectory() as tmp:
        try:
            made = pdf_pages.save_pages(pdf, tmp, height=HEIGHT, quality=QUALITY)
        except Exception as e:
            warn(f"{rid}: couldn't take the pages out ({e}); skipped")
            return False
        if len(made) != count:
            warn(f"{rid}: expected {count} pages, got {len(made)}; skipped")
            return False
        if out.exists():
            shutil.rmtree(out)                               # only our own rendered pages
        out.mkdir(parents=True)
        for i, p in enumerate(made, 1):
            shutil.move(str(p), out / f"page{i:02d}.jpg")
    (out / "book.txt").write_text(f"pages|review|{title}|{issue}|{score}\n")
    stamp.write_text(want)
    return True


def main():
    try:
        import pypdf  # noqa: F401
        import PIL  # noqa: F401
    except ImportError:
        warn("pypdf or Pillow missing; reviews skipped")
        return 0
    keep = set()
    for n, (rid, name, size, total, pages, title, issue, score) in enumerate(REVIEWS):
        pdf = extract(rid, name, size, total, pages)
        if pdf and render(n, rid, pdf, title, issue, score, len(pages)):
            keep.add(f"{20 + n:02d}-review-{rid}")
    for d in (CACHE / "books").glob("*-review-*"):           # no stale reviews get embedded
        if d.name not in keep:
            shutil.rmtree(d)
    print(f"[reviews] {len(keep)} of {len(REVIEWS)} reviews")
    return 0


if __name__ == "__main__":
    sys.exit(main())
