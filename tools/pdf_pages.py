#!/usr/bin/env python3
"""Turn a scanned PDF's pages into JPEGs, without rendering it.

Every PDF the extras use (the manuals, the magazine reviews) is a scan: each page is one
JPEG with no text. So the page is taken as its largest picture, straight out of the file
(pypdf), and only resized or cut with Pillow. Small overlays (a hosting site's watermark)
are left behind. No Poppler, no system libraries: pypdf and Pillow are both pip packages.

usage: pdf_pages.py PDF OUTDIR [--height PX] [--split X1,X2,...] [--quality Q]
  --height  scale each page to this height (keeps the aspect ratio)
  --split   each page is a two-page spread, cut at these x positions (px of the scan, one
            per spread) into single pages in reading order: the front cover (the first
            spread's right half), the inside spreads' halves, the back cover (its left half)
Writes OUTDIR/p-01.jpg ... and prints the number of pages.
"""
import argparse
import io
import sys
from pathlib import Path

from PIL import Image
import pypdf


def page_images(pdf):
    """One Pillow image per page: the page's largest picture, turned by the page's /Rotate."""
    out = []
    for n, page in enumerate(pypdf.PdfReader(str(pdf)).pages, 1):
        pics = list(page.images)
        if not pics:
            raise ValueError(f"page {n} has no picture (not a scan?)")
        best = max(pics, key=lambda p: p.image.width * p.image.height)
        im = Image.open(io.BytesIO(best.data)) if best.data[:2] == b"\xff\xd8" else best.image
        im = im.convert("RGB")
        rot = int(page.get("/Rotate", 0) or 0) % 360
        if rot:
            im = im.rotate(-rot, expand=True)
        out.append(im)
    return out


def split_spreads(spreads, cuts):
    if len(cuts) != len(spreads):
        raise ValueError(f"{len(spreads)} spreads but {len(cuts)} cut positions")

    def half(n, side):
        im, x = spreads[n - 1], cuts[n - 1]
        return im.crop((0, 0, x, im.height) if side == "L" else (x, 0, im.width, im.height))
    order = [(1, "R")] + [(n, s) for n in range(2, len(spreads) + 1) for s in "LR"] + [(1, "L")]
    return [half(n, s) for n, s in order]


def save_pages(pdf, outdir, height=None, split=None, quality=85):
    pages = page_images(pdf)
    if split:
        pages = split_spreads(pages, split)
    if height:
        pages = [im if im.height == height else
                 im.resize((max(1, round(im.width * height / im.height)), height), Image.LANCZOS)
                 for im in pages]
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    made = []
    for i, im in enumerate(pages, 1):
        p = outdir / f"p-{i:02d}.jpg"
        im.save(p, "JPEG", quality=quality, optimize=True)
        made.append(p)
    return made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf")
    ap.add_argument("outdir")
    ap.add_argument("--height", type=int)
    ap.add_argument("--split", type=lambda s: [int(x) for x in s.split(",")])
    ap.add_argument("--quality", type=int, default=85)
    a = ap.parse_args()
    print(len(save_pages(a.pdf, a.outdir, a.height, a.split, a.quality)))


if __name__ == "__main__":
    sys.exit(main())
