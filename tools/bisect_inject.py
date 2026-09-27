"""Bisect the injection failure: try several DLLs against the same target."""
import ctypes
import subprocess
import sys
import time
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import inject_dll as inj  # noqa: E402

WORK = os.path.join(os.path.dirname(HERE), "_work")
target = os.path.join(WORK, "inject_target.exe")

proc = subprocess.Popen([target], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        cwd=WORK, text=True)
time.sleep(1.5)
print(proc.stdout.readline().strip())
print(proc.stdout.readline().strip())

candidates = [
    r"C:\Windows\System32\psapi.dll",
    os.path.join(WORK, "probe_selftest.dll"),
    os.path.join(WORK, "probe.dll"),
]

for c in candidates:
    print("\n=== injecting %s ===" % c)
    try:
        inj.inject(proc.pid, c, verbose=False)
    except SystemExit as e:
        print("  inject raised:", e)

print("\n=== final module list check ===")
for name in ("probe.dll", "probe_selftest.dll", "psapi.dll"):
    m = inj.list_modules(proc.pid, name)
    print("  %-20s %s" % (name, m if m else "NOT LOADED"))

proc.terminate()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
print("\n--- probe.log ---")
lp = os.path.join(WORK, "probe.log")
if os.path.exists(lp):
    with open(lp, "r", errors="replace") as fh:
        print(fh.read()[:2000])
else:
    print("(none)")
