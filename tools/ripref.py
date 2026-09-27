"""Raw RIP-relative reference scanner.

The cross-reference database only disassembles functions listed in .pdata, so a
leaf function without unwind info can be missed. This scanner instead walks the
whole .text looking for the machine encodings that take a RIP-relative operand
and reports the ones that resolve to a requested target.

Usage:
    python ripref.py <image> 0x119B038 [0x119BBD8 ...]
"""
from __future__ import annotations

import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402

# (prefix bytes, description) for encodings whose last 4 bytes are a RIP disp32.
PATTERNS = [
    (bytes([0xFF, 0x15]), "call  [rip+d]"),
    (bytes([0xFF, 0x25]), "jmp   [rip+d]"),
    (bytes([0xFF, 0xD0 + 0]), None),
]
# mov/lea with modrm mod=00 rm=101 (RIP-relative) for reg 0..15
MOVRX = []
for rex in (0x48, 0x4C):
    for op in (0x8B, 0x8D, 0x89, 0x3B):
        for reg in range(8):
            modrm = (reg << 3) | 0x05
            MOVRX.append((bytes([rex, op, modrm]), "op%02X r%d [rip+d]" % (op, reg)))
for reg in range(8):
    MOVRX.append((bytes([0x8B, (reg << 3) | 0x05]), "mov32 r%d [rip+d]" % reg))
    MOVRX.append((bytes([0x8D, (reg << 3) | 0x05]), "lea32 r%d [rip+d]" % reg))


def callers(data, pe, code, tva, targets):
    """Raw scan for E8/E9 rel32 whose target is one of `targets`.

    The .pdata-driven cross-reference database misses code that has no unwind
    info, so this byte-level scan is the fallback that never misses a direct call.
    """
    out = {t: [] for t in targets}
    for t in targets:
        for opcode, name in ((0xE8, "call"), (0xE9, "jmp")):
            start = 0
            pat = bytes([opcode])
            while True:
                i = code.find(pat, start)
                if i == -1:
                    break
                start = i + 1
                if i + 5 > len(code):
                    continue
                rel = struct.unpack_from("<i", code, i + 1)[0]
                if tva + i + 5 + rel == t:
                    out[t].append((tva + i, name))
    return out


def main():
    image = sys.argv[1]
    args = sys.argv[2:]
    mode = "refs"
    if args and args[0] in ("refs", "callers"):
        mode = args[0]
        args = args[1:]
    targets = {int(v, 16) for v in args}
    data = load(image)
    pe = PE(data)
    sec = {s["name"].strip(): s for s in pe.sections}
    text = sec[".text"]
    code = data[text["raw"]:text["raw"] + text["vsize"]]
    tva = text["va"]

    if mode == "callers":
        res = callers(data, pe, code, tva, targets)
        for t in sorted(targets):
            print("target rva 0x%X : %d direct caller(s)" % (t, len(res[t])))
            for rva, kind in res[t][:40]:
                print("    at rva 0x%-8X  %s" % (rva, kind))
        return 0

    hits = {t: [] for t in targets}
    allpat = [(bytes([0xFF, 0x15]), "call [rip+d]"), (bytes([0xFF, 0x25]), "jmp [rip+d]")] + [p for p in MOVRX]
    for pat, desc in allpat:
        start = 0
        while True:
            i = code.find(pat, start)
            if i == -1:
                break
            start = i + 1
            d = i + len(pat)
            if d + 4 > len(code):
                continue
            disp = struct.unpack_from("<i", code, d)[0]
            tgt = tva + d + 4 + disp
            if tgt in targets:
                hits[tgt].append((tva + i, desc))

    for t in sorted(targets):
        print("target rva 0x%X : %d reference(s)" % (t, len(hits[t])))
        for rva, desc in hits[t][:20]:
            print("    at rva 0x%-8X  %s" % (rva, desc))
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
