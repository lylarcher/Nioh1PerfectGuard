"""Exercise the mod's INI reader across encodings and malformed inputs.

The mod used to rely on GetPrivateProfile*, which silently mis-reads UTF-8 files.
It now normalises the file itself, so this harness checks the encodings that a
real user's editor might produce.

Usage: python test_ini_encodings.py <mod_dir>
"""
import ctypes
import os
import shutil
import sys

mod = sys.argv[1]
ini = os.path.join(mod, "Nioh1PerfectGuard.ini")
bak = ini + ".orig"

shutil.copy(ini, bak)

CASES = [
    ("utf-8 no BOM", "utf-8", False),
    ("utf-8 with BOM", "utf-8-sig", False),
    ("utf-16 LE with BOM", "utf-16", False),
    ("ansi/cp1252", "cp1252", False),
]

BODY = """[PerfectGuard]
Enabled=1
WindowMs=333
KiDamageReductionPercent=42
KiRecoveryMode=2
FixedRecovery=77.5
RequireTimelyGuard=1
GuardButtonMask=0x0200
GuardKeyVK=160
SoundEnabled=0
SoundVolume=0.25
SoundFile=my_parry.wav
; a comment with a fake KiDamageReductionPercent=999
[OtherSection]
KiDamageReductionPercent=999
"""

def run_case(label, enc, expect_ok):
    with open(ini, "wb") as fh:
        fh.write(BODY.encode(enc))
    d = ctypes.CDLL(os.path.join(mod, "Nioh1PerfectGuard.dll"))
    d.PG_SelfTest.restype = ctypes.c_char_p
    out = d.PG_SelfTest().decode()
    good = ("window=333" in out and "reduction=42" in out and
            "recovery=2" in out and "vol=0.25" in out and
            "file=my_parry.wav" in out)
    print("%-22s %s" % (label, "OK " if good else "FAIL"))
    print("    %s" % out)
    return good

results = []
for label, enc, expect in CASES:
    try:
        results.append(run_case(label, enc, True))
    except Exception as exc:  # noqa: BLE001
        print("%-22s EXCEPTION %s" % (label, exc))
        results.append(False)

# malformed values must be rejected and fall back to defaults
print("\n--- malformed values ---")
with open(ini, "wb") as fh:
    fh.write(b"[PerfectGuard]\nWindowMs=abc\nKiDamageReductionPercent=999\n"
             b"SoundFile=\n")
d = ctypes.CDLL(os.path.join(mod, "Nioh1PerfectGuard.dll"))
d.PG_SelfTest.restype = ctypes.c_char_p
out = d.PG_SelfTest().decode()
print("   ", out)
results.append("window=250" in out and "reduction=100" in out)


# The compiled defaults are what the mod runs on when the INI is missing or
# unreadable, so they must agree with the shipped INI. A gate_timely defaulting
# to 0, for instance, silently turns *every* block into a perfect guard.
print("\n--- compiled defaults must match the shipped INI ---")


def fields(text):
    got = {}
    for part in text.split("|")[0].split():
        if "=" in part:
            key, _, value = part.partition("=")
            got[key] = value
    return got


dll = ctypes.CDLL(os.path.join(mod, "Nioh1PerfectGuard.dll"))
dll.PG_SelfTest.restype = ctypes.c_char_p
with_ini = fields(dll.PG_SelfTest().decode())

absent = ini + ".hidden"
os.rename(ini, absent)
try:
    no_ini = fields(dll.PG_SelfTest().decode())
finally:
    os.rename(absent, ini)

print("    with INI   :", with_ini)
print("    no INI     :", no_ini)
mismatch = {k: (with_ini.get(k), no_ini.get(k))
            for k in with_ini if k != "config_rc" and with_ini.get(k) != no_ini.get(k)}
if mismatch:
    print("    MISMATCH   :", mismatch)
results.append(not mismatch)

shutil.copy(bak, ini)
os.remove(bak)
print("\nall passed: %s (%d/%d)" % (all(results), sum(results), len(results)))
sys.exit(0 if all(results) else 1)
