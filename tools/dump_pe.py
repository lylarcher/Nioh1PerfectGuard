"""Minimal PE inspector: sections, imports, exports, strings (ASCII + UTF-16LE).

Usage: python dump_pe.py <file.dll> [--strings-min N] [--grep REGEX]
"""
import re
import struct
import sys


def load(path):
    with open(path, "rb") as f:
        return f.read()


class PE:
    def __init__(self, data):
        self.d = data
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE"
        coff = e_lfanew + 4
        (self.machine, self.nsec, _ts, _sym, _nsym, self.opt_size, self.chars) = struct.unpack_from(
            "<HHIIIHH", data, coff
        )
        self.opt_off = coff + 20
        magic = struct.unpack_from("<H", data, self.opt_off)[0]
        self.pe32plus = magic == 0x20B
        self.sections = []
        sec_off = self.opt_off + self.opt_size
        for i in range(self.nsec):
            o = sec_off + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
            self.sections.append(
                dict(name=name, va=vaddr, vsize=vsize, raw=rawptr, rawsize=rawsize)
            )
        # data directories
        dd_off = self.opt_off + (112 if self.pe32plus else 96)
        self.dirs = []
        for i in range(16):
            rva, size = struct.unpack_from("<II", data, dd_off + i * 8)
            self.dirs.append((rva, size))
        self.image_base = struct.unpack_from("<Q" if self.pe32plus else "<I", data, self.opt_off + 24 if self.pe32plus else self.opt_off + 28)[0]

    def rva2off(self, rva):
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
                return s["raw"] + (rva - s["va"])
        return None

    def cstr(self, rva):
        o = self.rva2off(rva)
        if o is None:
            return None
        end = self.d.find(b"\0", o)
        return self.d[o:end].decode("latin1", "replace")

    def imports(self):
        out = []
        rva, _ = self.dirs[1]
        if not rva:
            return out
        o = self.rva2off(rva)
        idx = 0
        while True:
            ent = self.d[o + idx * 20: o + idx * 20 + 20]
            if len(ent) < 20 or ent == b"\0" * 20:
                break
            oft, _t, _fc, name_rva, first = struct.unpack("<IIIII", ent)
            dll = self.cstr(name_rva)
            funcs = []
            thunk_rva = oft or first
            to = self.rva2off(thunk_rva)
            j = 0
            while to is not None:
                v = struct.unpack_from("<Q" if self.pe32plus else "<I", self.d, to + j * (8 if self.pe32plus else 4))[0]
                if v == 0:
                    break
                if v & (1 << 63 if self.pe32plus else 1 << 31):
                    funcs.append("#%d" % (v & 0xFFFF))
                else:
                    funcs.append(self.cstr(v + 2) or "?")
                j += 1
            out.append((dll, funcs))
            idx += 1
        return out

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return []
        o = self.rva2off(rva)
        (flags, _ts, _maj, _min, name_rva, base, nfun, nname, afun, aname, aord) = struct.unpack_from(
            "<IIHHIIIIIII", self.d, o
        )
        names = []
        ano = self.rva2off(aname)
        aoo = self.rva2off(aord)
        for i in range(nname):
            nr = struct.unpack_from("<I", self.d, ano + i * 4)[0]
            ordi = struct.unpack_from("<H", self.d, aoo + i * 2)[0]
            names.append((self.cstr(nr), ordi + base))
        return names

    def sections_summary(self):
        return self.sections


def strings(data, minlen=6):
    pat = re.compile(rb"[\x20-\x7e]{%d,}" % minlen)
    for m in pat.finditer(data):
        yield m.start(), "A", m.group().decode("latin1")
    pat16 = re.compile((rb"(?:[\x20-\x7e]\x00){%d,}" % minlen))
    for m in pat16.finditer(data):
        yield m.start(), "W", m.group().decode("utf-16-le", "replace")


def main():
    path = sys.argv[1]
    minlen = 6
    grep = None
    if "--strings-min" in sys.argv:
        minlen = int(sys.argv[sys.argv.index("--strings-min") + 1])
    if "--grep" in sys.argv:
        grep = re.compile(sys.argv[sys.argv.index("--grep") + 1], re.I)
    data = load(path)
    pe = PE(data)
    print("machine=%04X pe32plus=%s imagebase=%X sections=%d" % (pe.machine, pe.pe32plus, pe.image_base, pe.nsec))
    for s in pe.sections_summary():
        print("  sec %-8s va=%08X vsize=%08X raw=%08X rawsize=%08X" % (s["name"], s["va"], s["vsize"], s["raw"], s["rawsize"]))
    print("\n-- EXPORTS --")
    for n, o in pe.exports():
        print("  %s @%d" % (n, o))
    print("\n-- IMPORTS --")
    for dll, funcs in pe.imports():
        print("  %s (%d): %s" % (dll, len(funcs), ", ".join(funcs)))
    print("\n-- STRINGS --")
    for off, kind, s in strings(data, minlen):
        if grep and not grep.search(s):
            continue
        print("  %08X %s %s" % (off, kind, s))


if __name__ == "__main__":
    main()