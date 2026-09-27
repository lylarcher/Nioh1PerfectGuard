"""Print import IAT slot RVAs so cross-references can find the calling code.

A call through the import table appears as `call qword ptr [rip+X]`, which the
xref database records as a data reference to the *IAT slot*. Knowing the slot RVA
is therefore enough to find every caller of an imported API.

Usage:
    python iat.py <image> [--dll DINPUT8.dll] [--name DirectInput8Create] [--all]
"""
from __future__ import annotations

import argparse
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402


def imports_with_slots(data, pe):
    """Yield (dll, func_name_or_ordinal, iat_slot_rva, iat_slot_va)."""
    rva, _ = pe.dirs[1]
    if not rva:
        return
    o = pe.rva2off(rva)
    idx = 0
    while True:
        ent = data[o + idx * 20: o + idx * 20 + 20]
        if len(ent) < 20 or ent == b"\0" * 20:
            break
        oft, _t, _fc, name_rva, first = struct.unpack("<IIIII", ent)
        dll = pe.cstr(name_rva) or "?"
        # The IAT is the FirstThunk array. In a memory-dumped image the entries
        # are resolved addresses, but the slot *location* is still FirstThunk.
        step = 8 if pe.pe32plus else 4
        thunk_rva = oft or first
        to = pe.rva2off(thunk_rva)
        k = 0
        while to is not None:
            v = struct.unpack_from("<Q" if pe.pe32plus else "<I", data,
                                   to + k * step)[0]
            if v == 0:
                break
            if v & (1 << 63 if pe.pe32plus else 1 << 31):
                name = "#%d" % (v & 0xFFFF)
            else:
                name = pe.cstr(v + 2) or "?"
            slot_rva = first + k * step
            yield dll, name, slot_rva, pe.image_base + slot_rva
            k += 1
        idx += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--dll")
    ap.add_argument("--name")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    data = load(args.image)
    pe = PE(data)
    needle_dll = args.dll.lower() if args.dll else None
    needle_name = args.name if args.name else None

    shown = 0
    for dll, name, slot_rva, slot_va in imports_with_slots(data, pe):
        if needle_dll and needle_dll not in dll.lower():
            continue
        if needle_name and needle_name != name:
            continue
        print("%-26s %-34s IAT slot rva=0x%-8X va=0x%X" % (dll, name, slot_rva, slot_va))
        shown += 1
    print("\n%d entr%s shown" % (shown, "y" if shown == 1 else "ies"))


if __name__ == "__main__":
    main()
