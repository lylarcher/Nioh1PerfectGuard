"""Check that the numbers the documents state are the numbers the project has.

Two rounds have now been lost to stale counts: the CHANGELOG claimed 11 exports
after a twelfth was added, and it claimed 47 unit-test assertions long after there
were 85. Both are the kind of claim a reader trusts precisely because it looks
specific, and both were only caught by hand.

So measure the real values and compare them against what the *current-status*
documents actually say. RE_NOTES.md is deliberately excluded: it is a chronological
notebook, and its per-round entries ("47 passed" recorded in round 20) are correct
as history.

Usage: python test_doc_counts.py <repo_root>
"""
import ctypes
import io
import os
import re
import subprocess
import sys

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
STATUS_DOCS = ["CHANGELOG.md", "交接摘要.md"]
dll = os.path.join(root, "mod", "Nioh1PerfectGuard.dll")
exe = os.path.join(root, "tools", "test_logic.exe")

# --- measure -----------------------------------------------------------------
# exports
lib = ctypes.CDLL(dll)
n_exports = None
try:
    import struct
    sys.path.insert(0, os.path.join(root, "tools"))
    from dump_pe import PE
    data = open(dll, "rb").read()
    pe = PE(data)
    eva, esize = pe.dirs[0]
    off = None
    for s in pe.sections:
        if s["va"] <= eva < s["va"] + max(s["vsize"], s["rawsize"]):
            off = s["raw"] + (eva - s["va"])
    n_exports = struct.unpack_from("<I", data, off + 24)[0]
except Exception as exc:                                    # noqa: BLE001
    print("WARN: could not read the export table (%s)" % exc)

# unit-test assertions
out = subprocess.run([exe], capture_output=True, text=True)
m = re.search(r"(\d+) passed, (\d+) failed", out.stdout)
n_assertions = int(m.group(1)) + int(m.group(2)) if m else None

# log lines and documented markers
src = io.open(os.path.join(root, "mod", "Nioh1PerfectGuard.c"), encoding="utf-8").read()
n_loglines = len(re.findall(r"\blog_line\s*\(", src))


def log_literals(text):
    out_ = []
    for mm in re.finditer(r"\blog_line\s*\(", text):
        i, depth, start = mm.end(), 1, mm.end()
        while i < len(text) and depth:
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == '"':
                i += 1
                while i < len(text) and text[i] != '"':
                    i += 2 if text[i] == "\\" else 1
            i += 1
        joined = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text[start:i]))
        if joined:
            out_.append(joined)
    return out_


n_literals = len(log_literals(src))

# documented markers: from the acceptance doc's marker table
acc = io.open(os.path.join(root, "验收测试说明.md"), encoding="utf-8").read()
marker_lines, on = [], False
for line in acc.splitlines():
    if line.startswith("#"):
        if on:
            break
        on = "日志标记对照表" in line
        continue
    if on:
        marker_lines.append(line)
n_markers = 0
for line in marker_lines:
    if line.startswith("|") and line.strip().strip("|").split("|")[0].strip().startswith("`"):
        n_markers += 1

measured = {
    "exports": n_exports,
    "assertions": n_assertions,
    "markers": n_markers,
    "loglines": n_literals,
    "logline_calls": n_loglines,
}
print("measured:")
for k, v in measured.items():
    print("   %-14s %s" % (k, v))

# --- compare -----------------------------------------------------------------
# Each pattern captures the number a document states for a measured quantity.
CLAIMS = [
    (r"(\d+)\s*个\s*`PG_\*`", "exports"),
    (r"\*\*(\d+)\s*条断言全过\*\*", "assertions"),
    (r"\*\*(\d+)/(\d+)\s*标记[；;]\s*(\d+)\s*条日志", "markers3"),
]

fails = []
for doc in STATUS_DOCS:
    path = os.path.join(root, doc)
    if not os.path.exists(path):
        # CHANGELOG ships inside the package; the source copy is authoritative.
        path = os.path.join(root, "dist", "Nioh1PerfectGuard", doc)
    if not os.path.exists(path):
        fails.append("%s not found" % doc)
        continue
    text = io.open(path, encoding="utf-8").read()
    print("\n--- %s ---" % doc)
    seen = 0
    for pat, kind in CLAIMS:
        for mm in re.finditer(pat, text):
            seen += 1
            if kind == "exports":
                ok = int(mm.group(1)) == n_exports
                stated = "%s exports" % mm.group(1)
                actual = n_exports
            elif kind == "assertions":
                ok = n_assertions is not None and int(mm.group(1)) == n_assertions
                stated = "%s assertions" % mm.group(1)
                actual = n_assertions
            else:
                ok = (int(mm.group(1)) == n_markers and int(mm.group(2)) == n_markers
                      and int(mm.group(3)) == n_literals)
                stated = "%s/%s markers, %s log lines" % mm.groups()
                actual = "%s/%s markers, %s log lines" % (n_markers, n_markers, n_literals)
            print("    %-6s %s" % ("OK" if ok else "STALE", stated))
            if not ok:
                fails.append("%s states %s, measured %s" % (doc, stated, actual))
    if not seen:
        print("    (no numeric claims found)")

print("\nstated counts match measured counts: %s" % (not fails))
for f in fails:
    print("  FAIL %s" % f)
sys.exit(1 if fails else 0)
