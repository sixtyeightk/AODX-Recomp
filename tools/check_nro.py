#!/usr/bin/env python3
"""Sanity-check a Switch .nro: NRO0 header and segments, ASET asset section with a NACP
(name/author/version) and a 256x256 JPEG icon, and the overall size.

usage: check_nro.py out/arcus-odyssey-dx.nro [--name N --author A --version V]
"""
import argparse
import struct
import sys
from pathlib import Path


def jpeg_size(data):
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            return None
        marker, seglen = data[i + 1], struct.unpack(">H", data[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + seglen
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nro")
    ap.add_argument("--name", default="Arcus Odyssey DX")
    ap.add_argument("--author", default="sixtyeightk")
    import re
    cm = (Path(__file__).resolve().parent.parent / "CMakeLists.txt").read_text()
    ver = re.search(r"project\(\S+ VERSION ([0-9.]+)", cm).group(1)   # the NACP gets PROJECT_VERSION
    ap.add_argument("--version", default=ver)
    a = ap.parse_args()
    b = Path(a.nro).read_bytes()
    errs = []

    def need(cond, msg):
        if not cond:
            errs.append(msg)
        return cond

    need(b[0x10:0x14] == b"NRO0", "missing NRO0 magic at 0x10")
    nro_size = struct.unpack_from("<I", b, 0x18)[0]
    segs = [struct.unpack_from("<II", b, 0x20 + 8 * i) for i in range(3)]
    bss = struct.unpack_from("<I", b, 0x38)[0]
    need(nro_size <= len(b), f"header size {nro_size} > file size {len(b)}")
    need(all(off + size <= nro_size for off, size in segs), "segment outside the NRO image")
    need(segs[0][0] == 0 and segs[0][1] > 0, "bad .text segment")
    print(f"NRO0: image {nro_size} bytes; text {segs[0][1]}, rodata {segs[1][1]}, data {segs[2][1]}, bss {bss}")

    aset = nro_size
    if need(b[aset:aset + 4] == b"ASET", "missing ASET asset header after the NRO image"):
        icon_off, icon_sz, nacp_off, nacp_sz, romfs_off, romfs_sz = struct.unpack_from("<6Q", b, aset + 8)
        icon = b[aset + icon_off:aset + icon_off + icon_sz]
        nacp = b[aset + nacp_off:aset + nacp_off + nacp_sz]
        dims = jpeg_size(icon)
        need(dims == (256, 256), f"icon is not a 256x256 JPEG ({dims}, {icon_sz} bytes)")
        if need(nacp_sz == 0x4000, f"NACP size {nacp_sz} != 0x4000"):
            name = nacp[0:0x200].split(b"\0")[0].decode()
            author = nacp[0x200:0x300].split(b"\0")[0].decode()
            ver = nacp[0x3060:0x3070].split(b"\0")[0].decode()
            print(f"NACP: name {name!r}, author {author!r}, version {ver!r}")
            need(name == a.name, f"NACP name {name!r} != {a.name!r}")
            need(author == a.author, f"NACP author {author!r} != {a.author!r}")
            need(ver == a.version, f"NACP version {ver!r} != {a.version!r}")
        print(f"ASET: icon {icon_sz} bytes {dims}, nacp {nacp_sz} bytes, romfs {romfs_sz} bytes")
    mib = len(b) / (1 << 20)
    print(f"file: {len(b)} bytes ({mib:.1f} MiB)")
    # The program image: the 2 MB ROM is embedded and the recompiled code is several MB;
    # outside this range means a broken link (missing code) or a debug/unstripped blow-up.
    img = nro_size / (1 << 20)
    need(4 <= img <= 64, f"program image {img:.1f} MiB outside the expected 4-64 MiB")
    for e in errs:
        print("FAIL:", e)
    print("OK" if not errs else f"{len(errs)} problem(s)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
