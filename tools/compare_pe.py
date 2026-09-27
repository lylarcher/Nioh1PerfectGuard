"""Compare a memory-dumped PE against the on-disk original, section by section."""
import struct
import sys


def sections(path):
    with open(path, "rb") as fh:
        data = fh.read()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    machine, nsec, _ts, _s, _n, opt_size, _c = struct.unpack_from("<HHIIIHH", data, coff)
    opt = coff + 20
    pe32plus = struct.unpack_from("<H", data, opt)[0] == 0x20B
    sec_off = opt + opt_size
    out = {}
    for i in range(nsec):
        o = sec_off + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        out[name] = dict(va=vaddr, vsize=vsize, rawsize=rawsize, rawptr=rawptr)
    return data, out, pe32plus, machine, nsec


dump_path, orig_path = sys.argv[1], sys.argv[2]
dd, ds, dplus, dmach, dn = sections(dump_path)
od, os_, oplus, omach, on = sections(orig_path)
print("dump : machine=%04X sections=%d" % (dmach, dn))
print("orig : machine=%04X sections=%d" % (omach, on))
print()
print("%-10s %10s %10s %8s" % ("section", "dump_size", "orig_size", "match%"))
for name in sorted(set(ds) | set(os_)):
    s = ds.get(name)
    o = os_.get(name)
    if not s or not o:
        print("%-10s %10s %10s   (missing on one side)" % (
            name, s["vsize"] if s else "-", o["vsize"] if o else "-"))
        continue
    # dumped image is identity-mapped: offset == RVA. original uses rawptr.
    a = dd[s["va"]: s["va"] + s["vsize"]]
    b = od[o["rawptr"]: o["rawptr"] + min(o["rawsize"], len(a))]
    n = min(len(a), len(b))
    same = sum(1 for i in range(n) if a[i] == b[i])
    print("%-10s %10d %10d %7.2f%%  (compared %d bytes)" % (
        name, s["vsize"], o["vsize"], (100.0 * same / n) if n else 0.0, n))
