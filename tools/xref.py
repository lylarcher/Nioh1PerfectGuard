"""Build a whole-image cross-reference database with capstone, then query it.

This is the main reverse-engineering workhorse: function boundaries come from the
.pdata exception directory (exact, no heuristics), and every function is
disassembled to record outgoing direct calls and RIP-relative data references.

Build (one-off, cached):
    python xref.py build <image> --db xref.pkl

Query:
    python xref.py callers   <db> <rva>          who calls this function
    python xref.py whorefs   <db> <rva>          which functions reference this data address
    python xref.py dumpfn    <image> <rva>       disassemble the function at rva
    python xref.py findfn    <db> <image> <rva>  locate which function contains rva
    python xref.py strings   <image> <regex>     strings with rvas (quick helper)
    python xref.py whorefs-str <db> <image> <regex>
"""
from __future__ import annotations

import argparse
import json
import pickle
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from dump_pe import PE, load  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM  # noqa: E402

try:
    from capstone.x86 import X86_REG_RIP  # noqa: E402
except Exception:  # pragma: no cover
    X86_REG_RIP = 41


def sections_of(pe):
    return {s["name"].strip(): s for s in pe.sections}


def read_functions(data, pe):
    sec = sections_of(pe)
    pd = sec.get(".pdata")
    blob = data[pd["raw"]:pd["raw"] + pd["rawsize"]]
    fns = []
    for i in range(0, len(blob) - 12 + 1, 12):
        begin, end, unwind = struct.unpack_from("<III", blob, i)
        if begin and end > begin:
            fns.append((begin, end))
    fns.sort()
    return fns


def cmd_build(args):
    data = load(args.image)
    pe = PE(data)
    sec = sections_of(pe)
    ib = pe.image_base
    text = sec[".text"]
    tlo, thi = text["va"], text["va"] + text["vsize"]
    code = data[text["raw"]:text["raw"] + text["vsize"]]

    fns = read_functions(data, pe)
    print("functions: %d" % len(fns))

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    # rva -> sorted list of caller function rvas
    callers = {}
    # data target rva -> list of (fn_rva, insn_rva)
    datarefs = {}
    n_insn = 0
    # structural indexes for pattern-shaped queries
    fn_calls = {}   # fn_rva -> set of direct call targets (any)
    disps = {}      # memory displacement -> set of fn_rva using it
    for begin, end in fns:
        off = begin - text["va"]
        if off < 0 or off >= len(code):
            continue
        chunk = code[off: off + (end - begin)]
        for ins in md.disasm(chunk, ib + begin):
            n_insn += 1
            ins_rva = ins.address - ib
            m = ins.mnemonic
            if m in ("call", "jmp") and ins.operands and ins.operands[0].type == CS_OP_IMM:
                tgt = ins.operands[0].imm - ib
                if tlo <= tgt < thi:
                    callers.setdefault(tgt, set()).add(begin)
                fn_calls.setdefault(begin, set()).add(tgt)
            # RIP-relative memory operands
            for op in ins.operands:
                if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                    tgt_rva = (ins.address + ins.size + op.mem.disp) - ib
                    datarefs.setdefault(tgt_rva, []).append((begin, ins_rva))
                elif op.type == CS_OP_MEM and op.mem.disp:
                    disps.setdefault(op.mem.disp & 0xFFFFFFFF, set()).add(begin)
    print("instructions: %d   call targets: %d   data refs: %d   disps: %d" % (
        n_insn, len(callers), len(datarefs), len(disps)))

    db = dict(image=args.image, image_base=ib,
              callers={k: sorted(v) for k, v in callers.items()},
              datarefs={k: v for k, v in datarefs.items()},
              fn_calls={k: sorted(v) for k, v in fn_calls.items()},
              disps={k: sorted(v) for k, v in disps.items()},
              fns=fns)
    with open(args.db, "wb") as fh:
        pickle.dump(db, fh, protocol=4)
    print("wrote %s (%.1f MB)" % (args.db, __import__("os").path.getsize(args.db) / 1e6))
    return 0


def load_db(path):
    with open(path, "rb") as fh:
        return pickle.load(fh)


def cmd_find(args):
    """Find functions by structural signature: must call X and/or use displacement D."""
    db = load_db(args.db)
    cands = None
    detail = []
    if args.calls is not None:
        tgt = int(args.calls, 16)
        roots = set(db["callers"].get(tgt, []))
        detail.append("calls 0x%X (%d fns)" % (tgt, len(roots)))
        cands = roots
    for d in (args.disp or []):
        dv = int(d, 16)
        users = set(db["disps"].get(dv, []))
        detail.append("uses disp 0x%X (%d fns)" % (dv, len(users)))
        cands = users if cands is None else (cands & users)
    if args.calls2 is not None:
        tgt = int(args.calls2, 16)
        roots = set(db["callers"].get(tgt, []))
        detail.append("also calls 0x%X (%d fns)" % (tgt, len(roots)))
        cands = roots if cands is None else (cands & roots)
    if cands is None:
        print("give at least one of --calls/--calls2/--disp")
        return 1
    print("filters: " + "; ".join(detail))
    print("candidates: %d" % len(cands))
    fns = db["fns"]
    starts = [b for b, _ in fns]
    import bisect
    for c in sorted(cands)[: args.limit]:
        i = bisect.bisect_right(starts, c) - 1
        size = (fns[i][1] - fns[i][0]) if i >= 0 else 0
        print("   fn 0x%-8X size=%d" % (c, size))
    return 0


def cmd_ops(args):
    """Find instructions that write an immediate into a given struct displacement.

    This is how a known field (e.g. the byte flag the guard query reads) is traced
    back to the code that sets it.
    """
    data = load(args.image)
    pe = PE(data)
    sec = sections_of(pe)
    ib = pe.image_base
    text = sec[".text"]
    code = data[text["raw"]:text["raw"] + text["vsize"]]
    fns = read_functions(data, pe)

    want_disp = int(args.disp, 16)
    want_imm = args.imm
    want_size = args.size

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    hits = []
    for begin, end in fns:
        off = begin - text["va"]
        if off < 0 or off >= len(code):
            continue
        for ins in md.disasm(code[off: off + (end - begin)], ib + begin):
            if ins.mnemonic not in ("mov", "and", "or", "xor", "inc", "dec", "not"):
                continue
            ops = ins.operands
            if not ops or ops[0].type != CS_OP_MEM:
                continue
            mem = ops[0].mem
            if mem.base == X86_REG_RIP:
                continue
            if (mem.disp & 0xFFFFFFFF) != (want_disp & 0xFFFFFFFF):
                continue
            if want_size and ops[0].size != want_size:
                continue
            if want_imm is not None:
                if len(ops) < 2 or ops[1].type != CS_OP_IMM:
                    continue
                if ops[1].imm != want_imm:
                    continue
            hits.append((begin, ins.address - ib, ins.mnemonic, ins.op_str, ins.bytes.hex()))

    print("matches: %d" % len(hits))
    fns_of = sorted({h[0] for h in hits})
    print("distinct functions: %d" % len(fns_of))
    for begin, irva, mn, op, by in hits[: args.limit]:
        print("  fn 0x%-8X +0x%-5X  %s %s   [%s]" % (begin, irva - begin, mn, op, by))
    if len(fns_of) <= 40:
        print("\nfunctions:")
        for f in fns_of:
            print("   0x%X" % f)
    return 0


def cmd_callers(args):
    db = load_db(args.db)
    rva = int(args.rva, 16)
    cs = db["callers"].get(rva, [])
    print("rva 0x%X has %d caller function(s) (showing %d):" % (
        rva, len(cs), min(len(cs), args.limit)))
    for c in cs[: args.limit]:
        print("   fn 0x%X" % c)
    return 0


def cmd_whorefs(args):
    db = load_db(args.db)
    rva = int(args.rva, 16)
    rs = db["datarefs"].get(rva, [])
    fns = sorted({f for f, _ in rs})
    print("data rva 0x%X referenced %d time(s) from %d function(s):" % (rva, len(rs), len(fns)))
    for f in fns[: args.limit]:
        sites = [i for ff, i in rs if ff == f]
        print("   fn 0x%X  at %s" % (f, ", ".join("0x%X" % s for s in sites)))
    return 0


def cmd_dumpfn(args):
    data = load(args.image)
    pe = PE(data)
    sec = sections_of(pe)
    ib = pe.image_base
    text = sec[".text"]
    code = data[text["raw"]:text["raw"] + text["vsize"]]
    tlo, thi = text["va"], text["va"] + text["vsize"]
    fns = read_functions(data, pe)
    rva = int(args.rva, 16)
    match = None
    if not args.exact:
        for b, e in fns:
            if b <= rva < e:
                match = (b, e)
                break
    if args.exact or not match:
        if not args.exact:
            print("; 0x%X is not inside any .pdata function; raw window follows" % rva)
        off = rva - text["va"]
        if off < 0 or off >= len(code):
            return 1
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        md.detail = True
        n = 0
        for ins in md.disasm(code[off: off + args.limit * 8], ib + rva):
            ins_rva = ins.address - ib
            note = ""
            for op in ins.operands:
                if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                    note += "   ; -> 0x%X" % ((ins.address + ins.size + op.mem.disp) - ib)
            print("%08X  %-30s %s %s%s" % (ins_rva, ins.bytes.hex(), ins.mnemonic, ins.op_str, note))
            n += 1
            if n >= args.limit:
                break
        return 0
    begin, end = match
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    chunk = code[begin - text["va"]: end - text["va"]]
    print("; function 0x%X - 0x%X  (%d bytes)" % (begin, end, end - begin))
    n = 0
    for ins in md.disasm(chunk, ib + begin):
        ins_rva = ins.address - ib
        note = ""
        for op in ins.operands:
            if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                tgt = (ins.address + ins.size + op.mem.disp) - ib
                note += "   ; -> rva 0x%X" % tgt
            elif op.type == CS_OP_IMM and ins.mnemonic in ("call", "jmp"):
                # printing the RVA here avoids hand-subtracting the image base,
                # which is an easy and very confusing mistake to make
                t = op.imm - ib
                where = "rva" if tlo <= t < thi else "abs"
                note += "   ; %s 0x%X" % (where, t if where == "rva" else op.imm)
        print("%08X  %-30s %s %s%s" % (ins_rva, ins.bytes.hex(), ins.mnemonic, ins.op_str, note))
        n += 1
        if n >= args.limit:
            print("... (%d+ instructions)" % n)
            break
    return 0


def cmd_strings(args):
    data = load(args.image)
    pe = PE(data)
    pat = re.compile(args.pattern)
    for m in re.finditer(rb"[\x20-\x7e]{4,300}", data):
        s = m.group().decode("latin1")
        if not pat.search(s):
            continue
        for s_i in pe.sections:
            if s_i["raw"] <= m.start() < s_i["raw"] + s_i["rawsize"]:
                print("%s\t%08X\t%s" % (s_i["name"].strip(), s_i["va"] + (m.start() - s_i["raw"]), s))
                break
    return 0


def cmd_whorefs_str(args):
    db = load_db(args.db)
    data = load(args.image)
    pe = PE(data)
    pat = re.compile(args.pattern)
    seen = 0
    for m in re.finditer(rb"[\x20-\x7e]{4,300}", data):
        s = m.group().decode("latin1")
        if not pat.search(s):
            continue
        for s_i in pe.sections:
            if s_i["raw"] <= m.start() < s_i["raw"] + s_i["rawsize"]:
                rva = s_i["va"] + (m.start() - s_i["raw"])
                rs = db["datarefs"].get(rva, [])
                fns = sorted({f for f, _ in rs})
                print("%s  (rva 0x%X) refs=%d fns=%s" % (
                    s[:70], rva, len(rs), ",".join("0x%X" % f for f in fns[:8])))
                seen += 1
                break
        if seen >= args.limit:
            break
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("build"); p.add_argument("image"); p.add_argument("--db", required=True)
    p.set_defaults(func=cmd_build)

    p = sub.add_parser("callers"); p.add_argument("db"); p.add_argument("rva")
    p.add_argument("--limit", type=int, default=60)
    p.set_defaults(func=cmd_callers)

    p = sub.add_parser("find"); p.add_argument("db")
    p.add_argument("--calls", help="must directly call this rva")
    p.add_argument("--calls2", help="must also call this rva")
    p.add_argument("--disp", action="append",
                   help="must use this memory displacement (repeatable)")
    p.add_argument("--limit", type=int, default=60)
    p.set_defaults(func=cmd_find)

    p = sub.add_parser("whorefs"); p.add_argument("db"); p.add_argument("rva")
    p.add_argument("--limit", type=int, default=20); p.set_defaults(func=cmd_whorefs)

    p = sub.add_parser("dumpfn"); p.add_argument("image"); p.add_argument("rva")
    p.add_argument("--limit", type=int, default=200)
    p.add_argument("--exact", action="store_true",
                   help="disassemble from exactly this rva even if it is inside a .pdata function")
    p.set_defaults(func=cmd_dumpfn)

    p = sub.add_parser("strings"); p.add_argument("image"); p.add_argument("pattern")
    p.set_defaults(func=cmd_strings)

    p = sub.add_parser("ops")
    p.add_argument("image")
    p.add_argument("--disp", required=True, help="struct displacement, e.g. 0xC8")
    p.add_argument("--imm", type=lambda v: int(v, 0), default=None,
                   help="immediate written, e.g. 1")
    p.add_argument("--size", type=int, default=0, help="operand size in bytes, e.g. 1")
    p.add_argument("--limit", type=int, default=60)
    p.set_defaults(func=cmd_ops)

    p = sub.add_parser("whorefs-str"); p.add_argument("db"); p.add_argument("image")
    p.add_argument("pattern"); p.add_argument("--limit", type=int, default=40)
    p.set_defaults(func=cmd_whorefs_str)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
