import os, struct, sys

# Import dump_pe from this file's own directory rather than relying on the caller
# having set PYTHONPATH -- build.ps1 does not set it, and the check failed with
# "No module named 'tools'" until this was made self-locating.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dump_pe import PE  # noqa: E402

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
