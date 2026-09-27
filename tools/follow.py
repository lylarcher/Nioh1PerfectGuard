"""Follow runtime pointers inside a memory-dumped image.

The dumped image contains runtime-*initialised* .data (singletons are constructed
at startup), so absolute pointers in it are meaningful. This lets us walk object
graphs statically -- something the on-disk image cannot do.

Usage:
    python follow.py <image> --at 0x<rva> [--qwords N] [--depth D]
    python follow.py <image> --strptr "IsAttackGuard"
    python follow.py <image> --scan-obj <rva> [--qwords N]
"""
from __future__ import annotations

import argparse
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402


class Img:
    def __init__(self, path):
        self.data = load(path)
        self.pe = PE(self.data)
        self.ib = self.pe.image_base
        sec = {s["name"].strip(): s for s in self.pe.sections}
        self.text = sec[".text"]
        self.tlo, self.thi = self.text["va"], self.text["va"] + self.text["vsize"]
        self.secs = sec

    def in_image(self, va):
        return self.ib <= va < self.ib + self.pe.dirs[0][0] + 0x40000000

    def read(self, rva, size):
        off = self.pe.rva2off(rva)
        if off is None or off + size > len(self.data):
            return None
        return self.data[off:off + size]

    def q(self, rva):
        b = self.read(rva, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def d(self, rva):
        b = self.read(rva, 4)
        return struct.unpack("<I", b)[0] if b else None

    def cstr(self, rva, limit=200):
        off = self.pe.rva2off(rva)
        if off is None:
            return None
        end = self.data.find(b"\0", off, off + limit)
        if end == -1:
            return None
        raw = self.data[off:end]
        if len(raw) < 3 or not all(0x20 <= c < 0x7F for c in raw):
            return None
        return raw.decode("latin1")

    def rva_of(self, va):
        r = va - self.ib
        return r if 0 <= r < 0x8000000 else None

    def is_text(self, va):
        return self.tlo <= va - self.ib < self.thi


def annotate(img, rva, va):
    """Describe a pointer value."""
    if va == 0:
        return "NULL"
    r = img.rva_of(va)
    if r is None:
        return "0x%016X (out of image)" % va
    tag = ""
    if img.is_text(va):
        tag = "  .text fn"
    s = img.cstr(r)
    if s and len(s) >= 4:
        tag += '  "%s"' % s[:70]
    return "0x%016X -> rva 0x%X%s" % (va, r, tag)


def cmd_at(args):
    img = Img(args.image)
    rva = int(args.at, 16)
    print("image base 0x%X   target rva 0x%X" % (img.ib, rva))
    for depth in range(args.depth):
        print("\n--- depth %d: object at rva 0x%X ---" % (depth, rva))
        blob = img.read(rva, args.qwords * 8)
        if blob is None:
            print("  unreadable")
            return 1
        first_fn = None
        for i in range(args.qwords):
            va = struct.unpack_from("<Q", blob, i * 8)[0]
            info = annotate(img, rva + i * 8, va)
            print("  +0x%03X  %s" % (i * 8, info))
            if first_fn is None and img.is_text(va):
                first_fn = va
        if args.only_one:
            break
        # follow the first pointer that looks like an object (not text, not string)
        nxt = None
        for i in range(args.qwords):
            va = struct.unpack_from("<Q", blob, i * 8)[0]
            r = img.rva_of(va)
            if r is None or img.is_text(va):
                continue
            s = img.cstr(r)
            if s:
                continue
            # must look like an object: its first qword is a .text pointer
            f0 = img.q(r)
            if f0 and img.is_text(f0):
                nxt = r
                break
        if nxt is None:
            print("  (no further object pointer found)")
            break
        print("  -> following +0x%X object pointer" % 0)
        rva = nxt
    return 0


def cmd_strptr(args):
    """Find absolute pointers to a string, then show the structures holding them."""
    img = Img(args.image)
    pat = re.compile(args.strptr)
    targets = {}
    for m in re.finditer(rb"[\x20-\x7e]{4,200}", img.data):
        s = m.group().decode("latin1")
        if not pat.search(s):
            continue
        for s_i in img.pe.sections:
            if s_i["raw"] <= m.start() < s_i["raw"] + s_i["rawsize"]:
                targets[s_i["va"] + (m.start() - s_i["raw"])] = s
                break
    print("matching strings: %d" % len(targets))
    if not targets:
        return 1
    for rva, s in list(targets.items())[: args.limit]:
        va = img.ib + rva
        # which data sections point at this string?
        print("\n\"%s\"  (rva 0x%X)" % (s[:70], rva))
        for sec in img.pe.sections:
            if sec["name"].strip() not in (".rdata", ".data", "_RDATA"):
                continue
            blob = img.data[sec["raw"]:sec["raw"] + sec["rawsize"]]
            for i in range(0, len(blob) - 8, 8):
                v = struct.unpack_from("<Q", blob, i)[0]
                if v == va:
                    holder = sec["va"] + i
                    print("   held at rva 0x%X; context:" % holder)
                    base = holder - args.window * 8
                    for k in range(args.window * 2 + 1):
                        o = img.pe.rva2off(base + k * 8)
                        if o is None or o + 8 > len(img.data):
                            continue
                        vv = struct.unpack_from("<Q", img.data, o)[0]
                        print("      %+4d  %s" % ((k - args.window) * 8,
                                                  annotate(img, base + k * 8, vv)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("at"); p.add_argument("image"); p.add_argument("--at", required=True)
    p.add_argument("--qwords", type=int, default=16); p.add_argument("--depth", type=int, default=1)
    p.add_argument("--only-one", action="store_true")
    p.set_defaults(func=cmd_at)

    p = sub.add_parser("strptr"); p.add_argument("image"); p.add_argument("strptr")
    p.add_argument("--limit", type=int, default=5); p.add_argument("--window", type=int, default=4)
    p.set_defaults(func=cmd_strptr)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
