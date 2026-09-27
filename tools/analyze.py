"""Static analysis toolkit for a PE image (works on the on-disk nioh.exe for
data sections, and on the memory-dumped image for code).

Subcommands:
    rtti      reconstruct class name -> vftable -> method RVA map via MSVC RTTI
    funcs     list function boundaries from the .pdata exception directory
    stats     section/pointer diagnostics

Usage:
    python analyze.py rtti  <image> --out rtti.json [--min-methods 1]
    python analyze.py funcs <image> --out funcs.txt
    python analyze.py stats <image>
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402


def image_base(pe):
    return pe.image_base


def read_rva(data, pe, rva, size):
    off = pe.rva2off(rva)
    if off is None or off + size > len(data):
        return None
    return data[off:off + size]


def cstr_at(data, pe, rva, limit=512):
    off = pe.rva2off(rva)
    if off is None:
        return None
    end = data.find(b"\0", off, off + limit)
    if end == -1:
        return None
    raw = data[off:end]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        return None


def demangle(name: str) -> str:
    """Very small MSVC RTTI name prettifier: .?AVFoo@Bar@@ -> Bar::Foo."""
    if not name.startswith(".?A"):
        return name
    body = name[4:]
    if body.endswith("@@"):
        body = body[:-2]
    body = re.sub(r"@@.*$", "", body)
    parts = body.split("@")
    parts = [p for p in parts if p]
    if not parts:
        return name
    cls = parts[0]
    ns = parts[1:]
    # strip template/parameter encoding tails for readability
    cls = re.sub(r"\$+", "", cls)
    return "::".join(reversed(ns + [cls])) if ns else cls


def cmd_stats(args):
    data = load(args.image)
    pe = PE(data)
    print("imagebase=0x%X sections=%d" % (pe.image_base, len(pe.sections)))
    ib = pe.image_base
    sec = {s["name"].strip(): s for s in pe.sections}
    text = sec.get(".text")
    tlo, thi = text["va"], text["va"] + text["vsize"]
    print(".text rva range: 0x%X - 0x%X" % (tlo, thi))
    for name in (".rdata", ".data", "_RDATA"):
        s = sec.get(name)
        if not s:
            continue
        blob = data[s["raw"]:s["raw"] + s["rawsize"]]
        ptr_in_text = 0
        for i in range(0, len(blob) - 8, 8):
            v = struct.unpack_from("<Q", blob, i)[0]
            if tlo <= v - ib < thi:
                ptr_in_text += 1
        print("section %-8s size=%9d qwords pointing into .text: %d" % (name, len(blob), ptr_in_text))


def cmd_rtti(args):
    data = load(args.image)
    pe = PE(data)
    ib = pe.image_base
    sec = {s["name"].strip(): s for s in pe.sections}
    text = sec.get(".text")
    tlo, thi = text["va"], text["va"] + text["vsize"]

    # ---- 1. collect type descriptors ------------------------------------
    # _TypeDescriptor: { void* pVFTable; void* spare; char name[]; }
    # names are emitted as ".?AV<Class>@<Namespace>@@"
    type_by_rva = {}
    for m in re.finditer(rb"\.\?A[VU][^\x00]{2,400}@@", data):
        name = m.group().decode("latin1")
        name_rva = None
        for s in pe.sections:
            if s["raw"] <= m.start() < s["raw"] + s["rawsize"]:
                name_rva = s["va"] + (m.start() - s["raw"])
                break
        if name_rva is None:
            continue
        type_by_rva[name_rva - 0x10] = name  # TypeDescriptor start = name - 0x10

    print("type descriptors: %d" % len(type_by_rva))

    # ---- 2. collect complete object locators ----------------------------
    # _RTTICompleteObjectLocator (x64):
    #  +0 signature, +4 offset, +8 cdOffset, +0xC pTypeDescriptor(RVA),
    #  +0x10 pClassDescriptor(RVA), +0x14 pSelf(RVA, only when signature==1)
    col_by_rva = {}
    for s in pe.sections:
        blob = data[s["raw"]:s["raw"] + s["rawsize"]]
        for i in range(0, len(blob) - 24, 4):
            sig, offset, cd, ptype, pclass = struct.unpack_from("<IIIII", blob, i)
            if sig > 1:
                continue
            if offset > 0x10000 or cd > 0x10000:
                continue
            if ptype not in type_by_rva:
                continue
            col_rva = s["va"] + i
            col_by_rva[col_rva] = dict(type=type_by_rva[ptype], offset=offset,
                                       cd=cd, ptype=ptype, pclass=pclass)

    print("complete object locators: %d" % len(col_by_rva))

    # ---- 3. locate vftables: a qword pointing at a COL, vftable = qword+8 --
    classes = {}
    for s in pe.sections:
        if s["name"].strip() not in (".rdata", ".data", "_RDATA"):
            continue
        blob = data[s["raw"]:s["raw"] + s["rawsize"]]
        for i in range(0, len(blob) - 8, 8):
            v = struct.unpack_from("<Q", blob, i)[0]
            col_rva = v - ib
            if col_rva not in col_by_rva:
                continue
            vft_rva = s["va"] + i + 8
            # read method pointers until we leave .text
            methods = []
            j = i + 8
            while j + 8 <= len(blob):
                m = struct.unpack_from("<Q", blob, j)[0]
                if tlo <= m - ib < thi:
                    methods.append(m - ib)
                    j += 8
                else:
                    break
            if len(methods) < args.min_methods:
                continue
            info = col_by_rva[col_rva]
            key = info["type"]
            entry = classes.setdefault(key, dict(name=key, pretty=demangle(key),
                                                 vftables=[]))
            entry["vftables"].append(dict(vftable_rva=vft_rva,
                                          col_rva=col_rva,
                                          offset=info["offset"],
                                          methods=methods))

    total_vft = sum(len(c["vftables"]) for c in classes.values())
    total_m = sum(len(v["methods"]) for c in classes.values() for v in c["vftables"])
    print("classes with vftables: %d   vftables: %d   methods: %d" % (
        len(classes), total_vft, total_m))

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(sorted(classes.values(), key=lambda c: c["pretty"]), fh,
                      ensure_ascii=False, indent=1)
        print("wrote %s" % args.out)

    if args.grep:
        pat = re.compile(args.grep, re.I)
        for c in sorted(classes.values(), key=lambda c: c["pretty"]):
            if pat.search(c["pretty"]):
                print("%-70s vft=%s methods=%d" % (
                    c["pretty"],
                    ",".join("0x%X" % v["vftable_rva"] for v in c["vftables"]),
                    sum(len(v["methods"]) for v in c["vftables"])))
    return 0


def cmd_namerefs(args):
    """Find data references (as full VA or as RVA) to strings matching a pattern.

    The code section may be encrypted, but pointer tables live in .rdata/.data and
    are readable. This finds the node/handler tables that reference engine script
    node names such as "Refer::IsAttackGuard", and shows the neighbouring qwords
    so function pointers into .text become visible without disassembly.
    """
    data = load(args.image)
    pe = PE(data)
    ib = pe.image_base
    sec = {s["name"].strip(): s for s in pe.sections}
    text = sec.get(".text")
    tlo, thi = text["va"], text["va"] + text["vsize"]
    pat = re.compile(args.pattern)

    # collect matching strings with their RVAs
    targets = {}
    for m in re.finditer(rb"[\x20-\x7e]{4,200}", data):
        try:
            s = m.group().decode("latin1")
        except UnicodeDecodeError:
            continue
        if not pat.search(s):
            continue
        for sec_i in pe.sections:
            if sec_i["raw"] <= m.start() < sec_i["raw"] + sec_i["rawsize"]:
                targets[sec_i["va"] + (m.start() - sec_i["raw"])] = s
                break
    print("matching strings: %d" % len(targets))

    # build a reverse index of 8-byte values and 4-byte values per data section
    def scan(kind):
        out = {}
        for s in pe.sections:
            if s["name"].strip() not in (".rdata", ".data", "_RDATA"):
                continue
            blob = data[s["raw"]:s["raw"] + s["rawsize"]]
            step = 8 if kind == 8 else 4
            for i in range(0, len(blob) - step + 1, step):
                v = struct.unpack_from("<Q" if kind == 8 else "<I", blob, i)[0]
                rva = v - ib if kind == 8 else v
                if rva in targets:
                    out.setdefault(rva, []).append(s["va"] + i)
        return out

    refs_va = scan(8)
    refs_rva = scan(4)

    for rva in sorted(targets, key=lambda r: targets[r]):
        name = targets[rva]
        hits = []
        for loc in refs_va.get(rva, []):
            hits.append(("VA", loc))
        for loc in refs_rva.get(rva, []):
            hits.append(("RVA", loc))
        if not hits:
            continue
        print("\n%s   (string rva 0x%X)  %d data ref(s)" % (name, rva, len(hits)))
        for kind, loc in hits[: args.max_refs]:
            off = pe.rva2off(loc - args.window * 8)
            if off is None:
                continue
            n = args.window * 2 + 1
            print("   %s ref @0x%X  context:" % (kind, loc))
            for k in range(n):
                o = off + k * 8
                if o + 8 > len(data):
                    break
                v = struct.unpack_from("<Q", data, o)[0]
                mark = ""
                if tlo <= v - ib < thi:
                    mark = "  <-- .text fn rva 0x%X" % (v - ib)
                elif v - ib in targets:
                    mark = "  <-- string \"%s\"" % targets[v - ib][:60]
                print("       %+4d  0x%016X%s" % ((k - args.window) * 8, v, mark))
    return 0


def cmd_nodes(args):
    """Enumerate the KTGL script-node registry: 16-byte {const char* name; void* fn} records.

    Found by observing that data tables reference engine node names such as
    "Refer::IsAttackGuard" as {name, handler} pairs. Walking the array gives every
    node name together with its handler function RVA -- a direct anchor source.
    """
    data = load(args.image)
    pe = PE(data)
    ib = pe.image_base
    sec = {s["name"].strip(): s for s in pe.sections}
    text = sec[".text"]
    tlo, thi = text["va"], text["va"] + text["vsize"]

    def valid_string(va):
        rva = va - ib
        off = pe.rva2off(rva)
        if off is None or off + 4 > len(data):
            return None
        end = data.find(b"\0", off, off + 300)
        if end == -1 or end == off:
            return None
        raw = data[off:end]
        if not all(0x20 <= c < 0x7F for c in raw):
            return None
        return raw.decode("latin1")

    def valid_fn(va):
        return tlo <= (va - ib) < thi

    records = []
    for s in pe.sections:
        if s["name"].strip() not in (".rdata", ".data", "_RDATA"):
            continue
        blob = data[s["raw"]:s["raw"] + s["rawsize"]]
        i = 0
        run = None
        while i + 16 <= len(blob):
            name_va, fn_va = struct.unpack_from("<QQ", blob, i)
            nm = valid_string(name_va) if name_va > ib else None
            if nm is not None and valid_fn(fn_va):
                if run is None:
                    run = [s["va"] + i, []]
                run[1].append((nm, fn_va - ib))
            else:
                if run and len(run[1]) >= args.min_entries:
                    records.append(tuple(run))
                run = None
            i += 16
        if run and len(run[1]) >= args.min_entries:
            records.append(tuple(run))

    total = sum(len(r[1]) for r in records)
    print("node registry arrays: %d   total entries: %d" % (len(records), total))

    pat = re.compile(args.grep) if args.grep else None
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            for base_rva, entries in records:
                for k, (nm, fn) in enumerate(entries):
                    fh.write("%08X\t%08X\t%s\n" % (base_rva + k * 16, fn, nm))
        print("wrote %s" % args.out)

    if pat:
        for base_rva, entries in records:
            for k, (nm, fn) in enumerate(entries):
                if pat.search(nm):
                    print("  %s\tfn=0x%X\ttable=0x%X[%d]" % (nm, fn, base_rva, k))
    return 0


def cmd_funcs(args):
    data = load(args.image)
    pe = PE(data)
    sec = {s["name"].strip(): s for s in pe.sections}
    pd = sec.get(".pdata")
    if not pd:
        print(".pdata not found", file=sys.stderr)
        return 1
    blob = data[pd["raw"]:pd["raw"] + pd["rawsize"]]
    ents = []
    for i in range(0, len(blob) - 12 + 1, 12):
        begin, end, unwind = struct.unpack_from("<III", blob, i)
        if begin == 0 and end == 0:
            continue
        ents.append((begin, end, unwind))
    ents.sort()
    print("RUNTIME_FUNCTION entries: %d" % len(ents))
    if ents:
        sizes = [e - b for b, e, _ in ents if e > b]
        import statistics
        print("function size: min=%d median=%d mean=%.1f max=%d" % (
            min(sizes), statistics.median(sizes),
            sum(sizes) / len(sizes), max(sizes)))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            for b, e, u in ents:
                fh.write("%08X %08X %08X\n" % (b, e, u))
        print("wrote %s" % args.out)
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("rtti")
    p.add_argument("image")
    p.add_argument("--out")
    p.add_argument("--grep")
    p.add_argument("--min-methods", type=int, default=1)
    p.set_defaults(func=cmd_rtti)

    p = sub.add_parser("funcs")
    p.add_argument("image")
    p.add_argument("--out")
    p.set_defaults(func=cmd_funcs)

    p = sub.add_parser("stats")
    p.add_argument("image")
    p.set_defaults(func=cmd_stats)

    p = sub.add_parser("namerefs")
    p.add_argument("image")
    p.add_argument("pattern", help="regex over engine node-name strings")
    p.add_argument("--refs", dest="max_refs", type=int, default=6)
    p.add_argument("--window", type=int, default=6,
                   help="qwords of context on each side of the reference")
    p.set_defaults(func=cmd_namerefs)

    p = sub.add_parser("nodes")
    p.add_argument("image")
    p.add_argument("--grep")
    p.add_argument("--out")
    p.add_argument("--min-entries", type=int, default=4)
    p.set_defaults(func=cmd_nodes)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
