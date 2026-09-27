"""List every log_line call site together with the guard that bounds it.

A repeating log line with no cap is a slow leak: fine for a 60-second test, fatal
for a three-hour session that then has to be sent back. The audit is mechanical --
show each call site and the lines immediately above it, so the guard (or its
absence) is visible without reading 2000 lines.

Usage: python audit_log_sites.py <repo_root>
"""
import io
import os
import re
import sys

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
src = io.open(os.path.join(root, "mod", "Nioh1PerfectGuard.c"), encoding="utf-8").read()
lines = src.splitlines()

# Which functions are called once, or are inherently rare by construction.
ONCE_CONTEXT = (
    "INSTALL", "ANCHOR", "INPUT manager", "SOUND", "CONFIG", "STATUS",
    "SELFTEST", "MANUAL EXPORT", "STATE ", "NOTICE", "WARNING",
    "===", "base(nioh", "KIV", "LAYOUT", "GUARD FLAG", "PERFECT GUARD",
)

uncapped = []
rows = []
for i, line in enumerate(lines):
    if "log_line(" not in line:
        continue
    # skip the definition of log_line itself
    if "static void log_line" in line:
        continue
    # gather the whole call (it may span lines) to show the format text
    text, j = "", i
    while j < len(lines) and (j == i or lines[j].strip()):
        text += lines[j]
        j += 1
    m = re.search(r'"((?:[^"\\]|\\.)*)"', text)
    fmt = m.group(1) if m else "?"
    # The guard can sit a fair way above the call (the KIV and LAYOUT sites have
    # their cap at the top of the enclosing function), so look back further.
    above = "\n".join(lines[max(0, i - 26):i])
    has_cap = bool(re.search(r"<\s*\d+|<=\s*\d+|>=\s*\d+|% *\d+ *==|CAP|MAX_LINES|"
                             r"_log\b|_logged|cap|first_|g_first|return;", above))
    once = any(k in fmt for k in ONCE_CONTEXT)
    verdict = "once/rare" if once else ("capped" if has_cap else "**UNBOUNDED?**")
    rows.append((i + 1, verdict, fmt))
    if verdict == "**UNBOUNDED?**":
        uncapped.append((i + 1, fmt))

print("%-6s %-14s %s" % ("line", "guard", "format"))
for ln, verdict, fmt in rows:
    print("%-6d %-14s %s" % (ln, verdict, fmt[:88]))

print("\n%d log_line call site(s); %d look unbounded" % (len(rows), len(uncapped)))
for ln, fmt in uncapped:
    print("   line %-5d %s" % (ln, fmt[:88]))
