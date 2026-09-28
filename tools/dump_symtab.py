"""Dump the engine's own {name, function} table out of the runtime image.

Found while hunting the 99 gauge (RE_NOTES 4.55): .rdata holds a table of 16-byte
records -- a pointer to a name string, then a pointer into .text. The names are real
member functions ("Player::SetTsukumoWeaponActiveFlag", "Character::ForceDropAmurita",
"Gadget::UseAbosrbAmurita", "Refer::Hp", ...), so this turns "which function does X"
from a guess into a lookup, for this and every later investigation.

The image is the decrypted runtime module (`_work/nioh1.mem.exe`, file offset == RVA,
image base 0x140000000); nothing here needs the game running.

Usage:
    python dump_symtab.py <image> [--out symtab.txt] [--grep REGEX] [--all]

    --out    where to write "RVA NAME" lines (default: stdout)
    --grep   only print symbols whose name matches this regex (case-insensitive)
    --all    print every record, not just the ones that look like C++ names
"""
from __future__ import annotations

import argparse
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402

# A name record is "Some::Member", "Some::Member<T>", "operator=", "~Dtor", ...
NAME_RE = re.compile(rb"^[A-Za-z_~][\w:<>,~ *&\[\]\.@-]{3,119}$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--out")
    ap.add_argument("--grep")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    data = load(args.image)
    pe = PE(data)
    ib = pe.image_base
    sec = {s["name"].strip(): s for s in pe.sections}
    text = sec[".text"]
    tlo, thi = text["va"], text["va"] + text["vsize"]

    def cstr(rva):
        for s in pe.sections:
            if s["va"] <= rva < s["va"] + s["rawsize"]:
                off = s["raw"] + (rva - s["va"])
                end = data.find(b"\x00", off, off + 200)
                return None if end == -1 else data[off:end]
        return None

    records = []
    for s in pe.sections:
        if s["name"].strip() not in (".rdata", ".data", "_RDATA"):
            continue
        blob = data[s["raw"]:s["raw"] + s["rawsize"]]
        for i in range(0, len(blob) - 16, 8):
            # record = { qword name_ptr, qword func_ptr } -- name first, function second
            # (verified against analyze.py namerefs, which printed the function at +8
            # of the string pointer). Reading them the other way round silently
            # pairs every name with the *previous* function.
            name_ptr, fn = struct.unpack_from("<QQ", blob, i)
            if not (ib + tlo <= fn < ib + thi):
                continue
            raw = cstr(name_ptr - ib)
            if not raw:
                continue
            if not args.all and not NAME_RE.match(raw):
                continue
            try:
                name = raw.decode("ascii")
            except UnicodeDecodeError:
                continue
            records.append((fn - ib, name))

    records.sort(key=lambda r: r[1])
    if args.grep:
        pat = re.compile(args.grep, re.IGNORECASE)
        records = [r for r in records if pat.search(r[1])]

    lines = ["%08X %s" % (rva, name) for rva, name in records]
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines) + "\n")
        print("wrote %s (%d symbol(s))" % (args.out, len(lines)))
    else:
        print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
