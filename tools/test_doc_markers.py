"""Cross-check the acceptance document against what the DLL really accepts/prints.

Two independent drift checks:

  1. log markers -- every marker the doc tells the user to look for must exist in
     the source. If it does not, the user waits for something that cannot happen.
  2. config keys -- every INI key the doc's default table names must exist in the
     shipped INI, and vice versa for the keys that matter.

Both have drifted before in this project, and both are cheap to check mechanically
instead of by eye.

Usage: python test_doc_markers.py <repo_root>
"""
import os
import re
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
src = os.path.join(root, "mod", "Nioh1PerfectGuard.c")
doc = os.path.join(root, "docs", "验收测试说明.md")
ini_path = os.path.join(root, "mod", "Nioh1PerfectGuard.ini")

# All user-facing prose: a log line may be explained in any of these.
DOCS = [
    doc,
    os.path.join(root, "mod", "README_CN.md"),
    os.path.join(root, "mod", "README_EN.md"),
    os.path.join(root, "CHANGELOG.md"),
]

code = open(src, encoding="utf-8").read()
text = open(doc, encoding="utf-8").read()
ini = open(ini_path, encoding="utf-8").read()
all_text = "\n".join(open(p, encoding="utf-8").read() for p in DOCS if os.path.exists(p))

ALL_LITERALS = re.findall(r'"((?:[^"\\]|\\.)*)"', code)
CODE_BLOB = "\n".join(ALL_LITERALS)


def log_literals(src_text):
    """Only the strings that reach log_line, for the code->doc direction."""
    out = []
    for m in re.finditer(r"\blog_line\s*\(", src_text):
        i, depth, start = m.end(), 1, m.end()
        while i < len(src_text) and depth:
            c = src_text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c == '"':
                i += 1
                while i < len(src_text) and src_text[i] != '"':
                    i += 2 if src_text[i] == "\\" else 1
            i += 1
        joined = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', src_text[start:i]))
        if joined:
            out.append(joined)
    return out


def section(text, heading):
    """Lines of the section starting at `heading`, up to the next heading."""
    lines = text.splitlines()
    out, on = [], False
    for line in lines:
        if line.startswith("#"):
            if on:
                break
            on = heading in line
            continue
        if on:
            out.append(line)
    return out


def first_column_backticks(lines):
    found = []
    for line in lines:
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if cells and cells[0].startswith("`"):
            found.extend(re.findall(r"`([^`]+)`", cells[0]))
    return found


def tokens(s):
    out = []
    for p in re.split(r"[^0-9A-Za-z_:\-=]+", s):
        p = p.strip(":-=")
        # drop pure values (250ms, 0x0100, 4/4) and single-letter placeholders
        # with a unit suffix (Xms, Ys), which are prose, not literal text
        if len(p) < 3 or p[0].isdigit() or not any(c.isalpha() for c in p):
            continue
        if re.fullmatch(r"[A-Za-z]{1,2}(ms|s|f|%)?", p):
            continue
        out.append(p)
    return out


fails = []

# ---------------------------------------------------------------- log markers --
marker_lines = section(text, "日志标记对照表")
MARKERS = first_column_backticks(marker_lines)
# Report the row count as well: tools/test_doc_counts.py counts *rows*, while a
# row may name more than one marker. The two numbers are both useful (spans are
# what gets verified, rows are what a doc claims), and printing only one of them
# left the two checkers looking like they disagreed for no reason.
ROWS = sum(1 for line in marker_lines
           if line.startswith("|")
           and line.strip().strip("|").split("|")[0].strip().startswith("`"))
print("log markers claimed by the doc : %d in %d row(s)" % (len(MARKERS), ROWS))
missing = []
for mk in MARKERS:
    toks = tokens(mk)
    if not toks:
        continue
    absent = [t for t in toks if t not in CODE_BLOB]
    if absent:
        missing.append((mk, absent))
print("  verified present in source   : %d" % (len(MARKERS) - len(missing)))
for mk, absent in missing:
    print("  MISS %s" % mk)
    print("       tokens nowhere in source: %s" % ", ".join(absent))
if missing:
    fails.append("%d documented log marker(s) do not exist in the source" % len(missing))

# ---------------------------------------------------------------- config keys --
cfg_lines = section(text, "默认配置")
DOC_KEYS = first_column_backticks(cfg_lines)
INI_KEYS = set(re.findall(r"(?m)^\s*([A-Za-z][A-Za-z0-9]*)\s*=", ini))
print("\nconfig keys in the doc table  : %d" % len(DOC_KEYS))
print("keys defined in the shipped INI: %d" % len(INI_KEYS))

# the doc writes "A / B" for a pair of keys
flat = []
for k in DOC_KEYS:
    flat.extend([p.strip() for p in k.split("/")])
doc_missing_keys = [k for k in flat if k and k not in INI_KEYS]
for k in doc_missing_keys:
    print("  MISS doc names a key absent from the INI: %s" % k)
if doc_missing_keys:
    fails.append("%d documented INI key(s) missing from the shipped INI" % len(doc_missing_keys))

undocumented = sorted(k for k in INI_KEYS if k not in flat)
print("  INI keys not named in the doc : %s" % (", ".join(undocumented) or "(none)"))

# Every shipped knob must be documented somewhere the user will actually look.
# Three keys were added over the project's life without either README ever naming
# them, which leaves a working setting that nobody can discover.
QUICKSTART = os.path.join(root, "QUICKSTART.md")
doc_texts = [all_text]
if os.path.exists(QUICKSTART):
    doc_texts.append(open(QUICKSTART, encoding="utf-8").read())
missing_anywhere = [k for k in sorted(INI_KEYS)
                    if not any(k in t for t in doc_texts)]
print("  INI keys named in no shipped doc: %s"
      % (", ".join(missing_anywhere) or "(none)"))
if missing_anywhere:
    fails.append("%d shipped INI key(s) are documented nowhere: %s"
                 % (len(missing_anywhere), ", ".join(missing_anywhere)))

# ------------------------------------------------- code lines nobody documented --
LITERALS = log_literals(code)
print("\nlog_line literals in source    : %d" % len(LITERALS))
undoc = []
for lit in LITERALS:
    toks = tokens(lit)
    if toks and not any(t in all_text for t in toks[:2]):
        undoc.append(lit)
undoc = sorted(set(undoc))

# A log line a player can see should be explained somewhere. Lines that are
# genuinely internal-only must be listed here explicitly, so that adding a new
# user-visible line is a deliberate decision rather than silent drift.
INTERNAL_ONLY = set()
unexplained = [lit for lit in undoc if lit not in INTERNAL_ONLY]

print("  not referenced in any doc    : %d" % len(undoc))
for lit in undoc:
    tag = "internal" if lit in INTERNAL_ONLY else "UNEXPLAINED"
    print("    %-11s %s" % (tag, lit[:90]))
if unexplained:
    fails.append("%d user-visible log line(s) are not explained in any doc" % len(unexplained))

print("\nall documented markers and keys exist: %s" % (not fails))
for f in fails:
    print("  FAIL %s" % f)
sys.exit(1 if fails else 0)
