"""Verify the mod's anchor bytes against the decrypted game image.

The anchors are the mod's safety mechanism: it arms nothing unless the bytes at
each RVA match what it expects, so a wrong build never gets patched. That also
makes a single typo in an expected byte catastrophic in a *silent* way -- the mod
would refuse to install on a perfectly correct game and the log would just say
ANCHOR MISS.

So re-derive every anchor from the dumped image and compare. Also checks the
input-manager slot decoded from the getter's own displacement against the
independently known value.

Usage: python test_anchors.py <repo_root>
"""
import os
import re
import struct
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
src = open(os.path.join(root, "mod", "Nioh1PerfectGuard.c"), encoding="utf-8").read()
image = os.path.join(root, "_work", "nioh1.mem.exe")

# The input-manager singleton, known independently of the anchor table.
EXPECTED_INPUT_SLOT_RVA = 0x1B78658

if not os.path.exists(image):
    print("SKIP: %s not present (the decrypted image is a build-time artefact)" % image)
    sys.exit(0)

sys.path.insert(0, os.path.join(root, "tools"))
from dump_pe import PE  # noqa: E402

data = open(image, "rb").read()
pe = PE(data)


def rva_to_raw(rva):
    for s in pe.sections:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
            return s["raw"] + (rva - s["va"])
    return None


arrays = {}
for m in re.finditer(r"static const unsigned char (BYTES_\w+)\s*\[\s*(\d+)\s*\]\s*=\s*\{([^}]*)\}",
                     src):
    name, size, body = m.group(1), int(m.group(2)), m.group(3)
    vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{1,2})", body)]
    arrays[name] = vals
    if len(vals) != size:
        print("  WARN %s declared [%d] but has %d values" % (name, size, len(vals)))

entries = []
for m in re.finditer(
        r'\{\s*"(\w+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(BYTES_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}',
        src):
    entries.append({
        "name": m.group(1),
        "rva": int(m.group(2), 16),
        "arr": m.group(3),
        "len": int(m.group(4)),
        "enabled": int(m.group(5)),
        "armable": int(m.group(6)),
    })

print("byte arrays in source : %d" % len(arrays))
print("anchor entries        : %d" % len(entries))

fails = []
used = set()
for a in entries:
    want = arrays.get(a["arr"])
    used.add(a["arr"])
    if want is None:
        fails.append("%s references unknown array %s" % (a["name"], a["arr"]))
        continue
    if len(want) != a["len"]:
        fails.append("%s declares len=%d but %s has %d bytes"
                     % (a["name"], a["len"], a["arr"], len(want)))
    off = rva_to_raw(a["rva"])
    if off is None:
        fails.append("%s rva 0x%X is not inside any section" % (a["name"], a["rva"]))
        continue
    got = list(data[off:off + a["len"]])
    hexw = " ".join("%02X" % b for b in want)
    hexg = " ".join("%02X" % b for b in got)
    if got != want:
        fails.append("%s @0x%X MISMATCH\n      expect %s\n      image  %s"
                     % (a["name"], a["rva"], hexw, hexg))
        print("  MISS %-18s rva=0x%-8X\n       expect %s\n       image  %s"
              % (a["name"], a["rva"], hexw, hexg))
    else:
        print("  OK   %-18s rva=0x%-8X armable=%d  %s"
              % (a["name"], a["rva"], a["armable"], hexw))

# how many anchors can actually take a DR slot
armable = sum(1 for a in entries if a["armable"])
print("\narmable anchors (need a DR0..DR3 slot): %d of 4 available" % armable)
if armable > 4:
    fails.append("%d armable anchors exceed the 4 hardware breakpoint slots" % armable)

# the non-armable getter must be present and must decode to the known slot
getter = next((a for a in entries if a["name"] == "inputmgr_getter"), None)
if getter is None:
    fails.append("inputmgr_getter anchor is missing")
elif getter["armable"]:
    fails.append("inputmgr_getter must not be armable (it runs every frame)")
else:
    want = arrays[getter["arr"]]
    if want[:3] != [0x48, 0x8B, 0x05]:
        fails.append("inputmgr_getter is not `mov rax, [rip+disp32]`")
    else:
        disp = struct.unpack_from("<i", bytes(want), 3)[0]
        slot = getter["rva"] + 7 + disp
        print("  getter decodes to input-manager slot rva 0x%X (expected 0x%X)"
              % (slot, EXPECTED_INPUT_SLOT_RVA))
        if slot != EXPECTED_INPUT_SLOT_RVA:
            fails.append("input-manager slot decodes to 0x%X, expected 0x%X"
                         % (slot, EXPECTED_INPUT_SLOT_RVA))

# Expected-byte arrays that are deliberately not wired to an anchor, but whose
# address we still want pinned so the fallback stays verified rather than
# becoming folklore.
SPARES = [
    ("BYTES_SUB", 0x7B4C40, "ki_subtract_prim: movss xmm2,[rcx+0xC] - the float "
                            "subtract primitive the guard cost flows into"),
]

# unused expected-byte arrays are dead weight and usually mean a stale anchor
unused = sorted(set(arrays) - used - {n for n, _, _ in SPARES})
if unused:
    print("\n  NOTE unused byte array(s): %s" % ", ".join(unused))

print()
for name, rva, why in SPARES:
    want = arrays.get(name)
    if want is None:
        fails.append("spare %s is missing from the source" % name)
        continue
    off = rva_to_raw(rva)
    if off is None:
        fails.append("spare %s rva 0x%X is not inside any section" % (name, rva))
        continue
    got = list(data[off:off + len(want)])
    if got != want:
        fails.append("spare %s @0x%X MISMATCH: expect %s, image %s"
                     % (name, rva,
                        " ".join("%02X" % b for b in want),
                        " ".join("%02X" % b for b in got)))
        print("  MISS %-14s rva=0x%-8X (spare)" % (name, rva))
    else:
        print("  OK   %-14s rva=0x%-8X (spare) %s" % (name, rva, why))

print("\nanchors verified against the image: %s" % (not fails))
for f in fails:
    print("  FAIL %s" % f)
sys.exit(1 if fails else 0)
