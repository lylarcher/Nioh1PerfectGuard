"""Compare two builds of the mod section by section.

Zig stamps a `.buildid` section, so a rebuild is never byte-identical even when
nothing functional changed. That makes "did my edit actually change the code?"
unanswerable from the file hash alone, and it is a question worth being able to
answer precisely: a comment-only rebuild should be code-identical, and a real
change should show up in `.text` (or `.rdata`/`.data` for constants).

Usage: python compare_builds.py <old.dll> <new.dll>
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dump_pe import PE  # noqa: E402

# Sections that are build metadata rather than program content.
METADATA = {".buildid", ".comment", ".debug", ".pdb"}


def describe(path):
    data = open(path, "rb").read()
    pe = PE(data)
    sections = {}
    for s in pe.sections:
        name = s["name"].strip()
        body = data[s["raw"]:s["raw"] + s["rawsize"]]
        sections[name] = hashlib.sha256(body).hexdigest()
    return {
        "size": len(data),
        "sha": hashlib.sha256(data).hexdigest(),
        "sections": sections,
    }


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    old, new = describe(sys.argv[1]), describe(sys.argv[2])
    print("old: %s  %d bytes  sha256 %s" % (sys.argv[1], old["size"], old["sha"][:16]))
    print("new: %s  %d bytes  sha256 %s" % (sys.argv[2], new["size"], new["sha"][:16]))
    print()

    code_diff, meta_diff, only = [], [], []
    for name in sorted(set(old["sections"]) | set(new["sections"])):
        a, b = old["sections"].get(name), new["sections"].get(name)
        if a == b:
            state = "same"
        elif name in METADATA:
            state = "metadata"
            meta_diff.append(name)
        else:
            state = "DIFF"
            code_diff.append(name)
        if a is None or b is None:
            only.append(name)
        print("  %-10s %-9s old=%s new=%s"
              % (name, state, (a or "-")[:16], (b or "-")[:16]))

    print()
    if code_diff:
        print("content sections changed: %s" % ", ".join(code_diff))
    if meta_diff:
        print("metadata sections changed: %s" % ", ".join(meta_diff))
    if only:
        print("section present in only one build: %s" % ", ".join(only))

    if old["sha"] == new["sha"]:
        print("\nRESULT: byte-identical")
    elif not code_diff and not only:
        print("\nRESULT: code-identical (only build metadata differs)")
    else:
        print("\nRESULT: the compiled content changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
