import struct, sys
from tools.dump_pe import PE
data = open(sys.argv[1], "rb").read()
pe = PE(data)
secs = pe.sections
def rva2off(rva):
    for s in secs:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
            return s["raw"] + (rva - s["va"])
    return None
# export directory is data dir 0
edir = pe.dirs[0]
if not edir[1]:
    print("no exports"); raise SystemExit
off = rva2off(edir[0])
nfunc, nname = struct.unpack_from("<II", data, off + 20)[0], struct.unpack_from("<I", data, off + 24)[0]
aof, aon, aoo = struct.unpack_from("<III", data, off + 28)
names = []
for i in range(nname):
    noff = rva2off(struct.unpack_from("<I", data, rva2off(aon) + 4*i)[0])
    end = data.index(b"\0", noff)
    names.append(data[noff:end].decode())
print("exports (%d):" % len(names))
for n in sorted(names): print("  ", n)
