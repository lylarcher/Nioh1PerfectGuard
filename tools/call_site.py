"""Find an E8 call to a target inside a function range and disassemble the lead-up.

Used to see what base pointer a caller passes to a table walker.

Usage: python call_site.py <image> <fn_start_hex> <fn_end_hex> <target_hex> [--before 30]
"""
import struct
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402

image = sys.argv[1]
fn_start = int(sys.argv[2], 16)
fn_end = int(sys.argv[3], 16)
target = int(sys.argv[4], 16)
before = 30
if "--before" in sys.argv:
    before = int(sys.argv[sys.argv.index("--before") + 1])

data = load(image)
pe = PE(data)
sec = {s["name"].strip(): s for s in pe.sections}
text = sec[".text"]
code = data[text["raw"]:text["raw"] + text["vsize"]]
tva = text["va"]
ib = pe.image_base

base = fn_start - tva
end = fn_end - tva
hits = []
i = base
while i < end - 5:
    if code[i] == 0xE8:
        rel = struct.unpack_from("<i", code, i + 1)[0]
        if tva + i + 5 + rel == target:
            hits.append(i)
    i += 1

print("call sites to 0x%X inside 0x%X-0x%X: %d" % (target, fn_start, fn_end, len(hits)))
for h in hits:
    print("\n--- call at rva 0x%X ---" % (tva + h))
    start = tva + max(base, h - before)
    print("(disassembling from rva 0x%X)" % start)
    print("copy these bytes to disassemble: not needed, see output below")


def disasm_at(rva, count):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    off = rva - tva
    for ins in md.disasm(code[off:off + count * 8], ib + rva):
        note = ""
        for op in ins.operands:
            if op.type == 3 and op.mem.base == 41:  # MEM, RIP
                t = (ins.address + ins.size + op.mem.disp) - ib
                note += "   ; -> rva 0x%X" % t
        print("  %08X  %-26s %s %s%s" % (ins.address - ib, ins.bytes.hex(),
                                         ins.mnemonic, ins.op_str, note))
        count -= 1
        if count <= 0:
            break


for h in hits:
    lo = tva + max(base, h - before)
    # step back a little so the first decode is aligned to something sensible
    disasm_at(lo, before + 6)
