"""Confirm the logic the unit tests exercise is the logic the DLL ships.

mod/pg_logic.h is compiled twice: into tools/test_logic.exe (which the tests run)
and into Nioh1PerfectGuard.dll (which the game loads). Both are the same source,
but not the same artefact -- different link modes, possibly different flags -- so
"the tests cover the shipped code" is otherwise an assumption. A codegen
difference (FP contraction, reassociation, a different inlining decision changing
an intermediate rounding) could make the tests pass while the shipped arithmetic
behaves differently.

pg_logic_digest() folds a fixed battery of inputs through the shared functions, so
comparing the two digests answers that question directly.

Usage: python test_logic_digest.py <repo_root>
"""
import ctypes
import os
import re
import subprocess
import sys

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
dll = os.path.join(root, "mod", "Nioh1PerfectGuard.dll")
exe = os.path.join(root, "tools", "test_logic.exe")
src = os.path.join(root, "mod", "pg_logic.h")

if not os.path.exists(dll) or not os.path.exists(exe):
    print("SKIP: need both %s and %s built" % (dll, exe))
    sys.exit(0)

# The test binary prints its digest.
out = subprocess.run([exe, "--digest"], capture_output=True, text=True)
m = re.search(r"logic_digest=([0-9A-Fa-f]{8})", out.stdout)
if not m:
    print("FAIL: test_logic.exe did not report a digest")
    print(out.stdout, out.stderr)
    sys.exit(1)
test_digest = m.group(1).upper()

# The shipped DLL reports its own.
lib = ctypes.CDLL(dll)
lib.PG_GetLogicDigest.restype = ctypes.c_char_p
dll_digest = lib.PG_GetLogicDigest().decode().upper()

print("test_logic.exe          : %s" % test_digest)
print("Nioh1PerfectGuard.dll   : %s" % dll_digest)

# A source change that adds cases will change both digests together; that is fine.
# What must never happen is the two disagreeing.
ok = test_digest == dll_digest
print("\ntested arithmetic == shipped arithmetic: %s" % ok)
if not ok:
    print("FAIL: the two builds of %s disagree -- the unit tests no longer describe"
          % os.path.basename(src))
    print("      the code the game actually runs. Find the codegen difference")
    print("      (try -ffp-contract=off on both) before trusting the test suite.")
sys.exit(0 if ok else 1)
