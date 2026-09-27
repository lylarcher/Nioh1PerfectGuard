"""Find every place a given value (typically a vftable RVA) is referenced as data.

Used to locate registration tables: a state-object class vftable is referenced by
the factory/registry entry that owns it, and the neighbouring fields usually name
the node. Works on the memory-dumped image.

Usage:
    python findrefs.py <image> 0x11A9450 [--window 6] [--limit 10]
    python findrefs.py <image> 0x11A9450 0x11A82D0 ...   (several values)
"""
from __future__ import annotations

import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402


def describe(img, pe, ib, rva, va, targets):
    if va == 0:
        return "NULL"
    r = va - ib
    if not (0 <= r < 0x8000000):
        return "0x%016X (out of image)" % va
    off = pe.rva2off(r)
    if off is None:
        return "0x%016X -> rva 0x%X (unmapped)" % (va, r)
    end = img.find(b"\0", off, off + 200)
    tag = ""
    if end != -1 and end > off + 3:
        raw = img[off:end]
        if all(0x20 <= c < 0x7F for c in raw):
            tag = '  "%s"' % raw.decode("latin1")[:70]
    if r in targets:
        tag += "   <-- TARGET vftable"
    return "0x%016X -> rva 0x%X%s" % (va, r, tag)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("values", nargs="+")
    ap.add_argument("--window", type=int, default=6)
    ap.add_argument("--limit", type=int, default=8)
    args = ap.parse_args()

    img = load(args.image)
    pe = PE(img)
    ib = pe.image_base
    targets = {int(v, 16) for v in args.values}
    print("image base 0x%X   looking for %d value(s)" % (ib, len(targets)))

    for tv in sorted(targets):
        va = ib + tv
        print("\n" + "=" * 74)
        print("value 0x%X  (as VA 0x%X)" % (tv, va))
        print("=" * 74)
        hits = []
        for s in pe.sections:
            if s["name"].strip() not in (".rdata", ".data", "_RDATA"):
                continue
            blob = img[s["raw"]:s["raw"] + s["rawsize"]]
            for i in range(0, len(blob) - 8, 8):
                if struct.unpack_from("<Q", blob, i)[0] == va:
                    hits.append((s["name"].strip(), s["va"] + i, 8))
            for i in range(0, len(blob) - 4, 4):
                if struct.unpack_from("<I", blob, i)[0] == tv:
                    hits.append((s["name"].strip(), s["va"] + i, 4))
        if not hits:
            print("  no data references found")
            continue
        print("  %d data reference(s); first %d shown with context:" % (len(hits), args.limit))
        for name, rva, width in hits[: args.limit]:
            print("\n  -- %s @ rva 0x%X (%d-byte ref)" % (name, rva, width))
            base = rva - args.window * 8
            for k in range(args.window * 2 + 1):
                off = pe.rva2off(base + k * 8)
                if off is None or off + 8 > len(img):
                    continue
                v = struct.unpack_from("<Q", img, off)[0]
                mark = "  <== here" if base + k * 8 == rva else ""
                print("     %+4d  %s%s" % ((k - args.window) * 8,
                                           describe(img, pe, ib, base + k * 8, v, targets),
                                           mark))
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
