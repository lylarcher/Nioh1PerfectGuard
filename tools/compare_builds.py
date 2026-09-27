"""Compare two builds of the mod section by section.

Why the file hash alone is not enough: the final DLL hash is *not* independent of
where you build it. Measured with Zig 0.16.0 / lld on this source:

  * same source, same absolute output path  -> byte-identical (3 runs, 1 hash)
  * same source, same basename, other dir   -> exactly 20 bytes differ:
        .buildid          16 bytes   Zig/lld build id
        PE header +0x80    4 bytes   TimeDateStamp, derived from the same
                                     build inputs, NOT the wall clock
                                     (a fixed constant for a fixed path)
  * different output basename, same length   -> plus 1 byte in .rdata: the
                                     embedded export module name
                                     (`pg_a.dll` -> `pg_b.dll`)
  * different output basename, other length -> the embedded name shifts the
                                     .rdata layout, so every rip-relative
                                     displacement into .rdata/.data shifts too
                                     and .text + .pdata differ as well
                                     (`Nioh1PerfectGuard.dll` -> `pg_renamed.dll`:
                                     7757 bytes, .text/.rdata/.pdata)
  * no directory path is ever embedded in the file (verified by searching for
    the build directory string: absent)

So "did my edit actually change the code?" is unanswerable from the hash alone,
and rebuilding a release in another directory is *expected* to produce a
different hash. That is why a shipped SHA256SUMS.txt verifies the shipped copy,
not a future rebuild. This tool answers the question the hash cannot: which
sections changed, and where inside them.

Usage: python compare_builds.py <old.dll> <new.dll>
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dump_pe import PE  # noqa: E402

# Sections that are build metadata rather than program content.
METADATA = {".buildid", ".comment", ".debug", ".pdb"}

CONTEXT = 16          # bytes of context shown around a differing region
GAP = 8               # merge differing bytes separated by <= GAP equal bytes
MAX_REGIONS = 6       # cap per section; the rest is summarised as a count


def diff_regions(a, b, gap=GAP, cap=MAX_REGIONS):
    """Contiguous-ish runs of differing bytes. Returns (regions, total_diff_bytes)."""
    n = min(len(a), len(b))
    idx = [i for i in range(n) if a[i] != b[i]]
    if not idx:
        return [], 0
    regions, start, prev = [], idx[0], idx[0]
    for i in idx[1:]:
        if i - prev <= gap:
            prev = i
        else:
            regions.append((start, prev + 1))
            start = prev = i
    regions.append((start, prev + 1))
    return regions[:cap], len(idx)


def render(data, lo, hi):
    """Printable ASCII view of a byte range, dots for anything else."""
    body = data[lo:hi]
    return "".join(chr(c) if 32 <= c < 127 else "." for c in body)


def region_kind(ba, bb, lo, hi):
    """'string' when both sides are genuinely printable ASCII, else 'binary'.

    Note the check must run on the raw bytes: the rendered view replaces
    non-printable bytes with '.', so testing the rendered text would classify
    every hash and every header field as a string.
    """
    a, b = ba[lo:hi], bb[lo:hi]
    printable = all(32 <= c < 127 for c in a + b)
    has_letter = any(65 <= c <= 90 or 97 <= c <= 122 for c in a + b)
    return "string" if (printable and has_letter and hi - lo >= 4) else "binary"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pa, pb = sys.argv[1], sys.argv[2]
    da, db = open(pa, "rb").read(), open(pb, "rb").read()

    pe_a, pe_b = PE(da), PE(db)
    sec_a = {s["name"].strip(): s for s in pe_a.sections}
    sec_b = {s["name"].strip(): s for s in pe_b.sections}

    def section_hash(data, s):
        return hashlib.sha256(data[s["raw"]:s["raw"] + s["rawsize"]]).hexdigest()

    lfanew = struct.unpack_from("<I", da, 0x3C)[0]
    stamp_off = lfanew + 8
    stamp = struct.unpack_from("<I", da, stamp_off)[0]

    print("old: %s  %d bytes  sha256 %s" % (pa, len(da), hashlib.sha256(da).hexdigest()[:16]))
    print("new: %s  %d bytes  sha256 %s" % (pb, len(db), hashlib.sha256(db).hexdigest()[:16]))
    print()

    code_diff, meta_diff, only = [], [], []
    for name in sorted(set(sec_a) | set(sec_b)):
        sa, sb = sec_a.get(name), sec_b.get(name)
        ha = section_hash(da, sa) if sa else None
        hb = section_hash(db, sb) if sb else None
        if ha == hb:
            state = "same"
        elif name in METADATA:
            state = "metadata"
            meta_diff.append(name)
        else:
            state = "DIFF"
            code_diff.append(name)
        if sa is None or sb is None:
            only.append(name)
        print("  %-10s %-9s old=%s new=%s"
              % (name, state, (ha or "-")[:16], (hb or "-")[:16]))

    # Ranges that live outside every section (PE headers). The previous version of
    # this tool never looked here, so a TimeDateStamp change was invisible.
    covered = [(s["raw"], s["raw"] + s["rawsize"]) for s in pe_a.sections]

    def locate(off):
        for s in pe_a.sections:
            if s["raw"] <= off < s["raw"] + s["rawsize"]:
                return s["name"].strip(), s["va"] + (off - s["raw"])
        return None, off

    print()
    if code_diff or meta_diff:
        regions, total = diff_regions(da, db)
        print("differing bytes: %d in %d region(s)%s"
              % (total, len(regions), " (showing first %d)" % MAX_REGIONS
                 if total > MAX_REGIONS else ""))
        for lo, hi in regions:
            name, where = locate(lo)
            stamp_here = lo <= stamp_off < hi or stamp_off <= lo < stamp_off + 4
            if name is None:
                label = "PE header TimeDateStamp" if stamp_here else "PE header"
                print("  %-28s file 0x%-6X %2d B  [%s]"
                      % (label, lo, hi - lo, region_kind(da, db, lo, hi)))
            else:
                print("  %-10s rva 0x%-6X %2d B (file 0x%X)  [%s]"
                      % (name, where, hi - lo, lo, region_kind(da, db, lo, hi)))
            if stamp_here:
                continue          # reported once, precisely, below
            clo, chi = max(0, lo - CONTEXT), min(len(da), hi + CONTEXT)
            if region_kind(da, db, lo, hi) == "string":
                print("      old |%s|" % render(da, clo, chi))
                print("      new |%s|" % render(db, clo, chi))
                print("      -> embedded string: a changed module name, or a changed")
                print("         string constant (check INI defaults and log text)")
            else:
                print("      old %s" % da[clo:chi].hex(" "))
                print("      new %s" % db[clo:chi].hex(" "))
    else:
        print("differing bytes: %d (headers and sections)"
              % sum(1 for i in range(min(len(da), len(db))) if da[i] != db[i]))
    if stamp_off + 4 <= len(da) and da[stamp_off:stamp_off + 4] != db[stamp_off:stamp_off + 4]:
        print("  PE TimeDateStamp: 0x%08X -> 0x%08X (path-derived, not a clock)"
              % (struct.unpack_from("<I", da, stamp_off)[0],
                 struct.unpack_from("<I", db, stamp_off)[0]))

    print()
    if code_diff:
        print("content sections changed: %s" % ", ".join(code_diff))
    if meta_diff:
        print("metadata sections changed: %s" % ", ".join(meta_diff))
    if only:
        print("section present in only one build: %s" % ", ".join(only))

    if da == db:
        print("\nRESULT: byte-identical")
    elif not code_diff and not only:
        print("\nRESULT: code-identical (only build id / timestamp metadata differs)")
    else:
        print("\nRESULT: content sections changed - inspect the regions above.")
        print("        .text differing in a few rip-relative displacements only means the")
        print("        .rdata/.data layout shifted, which renaming the output does on its")
        print("        own (an embedded name changes length). Rebuild both sides under the")
        print("        same output name to separate a rename from a real edit.")
    return 0 if (not code_diff and not only) else 1


if __name__ == "__main__":
    sys.exit(main())
