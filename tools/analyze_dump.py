"""Read the interesting parts out of a Windows minidump.

When nioh.exe faults, the fastest way to tell "did the mod do this?" from "the game
did this to itself" is the exception stream: the faulting address, and -- if the
dump contains the thread context -- the debug registers. If DR0..DR3 still hold
this mod's anchor addresses and DR6 shows a breakpoint condition, the trap was
involved; if they are empty or point elsewhere, it was not.

Usage: python analyze_dump.py <dump.dmp> [rva ...]
"""
import struct
import sys

path = sys.argv[1]
anchor_rvas = [int(a, 16) for a in sys.argv[2:]]


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


f = open(path, "rb")
header = f.read(32)
if header[:4] != b"MDMP":
    print("not a minidump")
    sys.exit(1)
nstreams = u32(header, 8)
dir_rva = u32(header, 12)
f.seek(dir_rva)
streams = {}
for _ in range(nstreams):
    entry = f.read(12)
    stype, size, rva = struct.unpack("<III", entry)
    streams.setdefault(stype, []).append((size, rva))
print("streams: %s" % sorted(streams))

# --- modules (stream 4) -------------------------------------------------------
# MINIDUMP_MODULE is 108 bytes; ModuleNameRva sits at offset 20 (after
# BaseOfImage(8) SizeOfImage(4) CheckSum(4) TimeDateStamp(4)).
modules = []
for size, rva in streams.get(4, []):
    f.seek(rva)
    count = u32(f.read(4), 0)
    for _ in range(count):
        m = f.read(108)
        base = u64(m, 0)
        size_of_image = u32(m, 8)
        name_rva = u32(m, 20)
        cur = f.tell()
        f.seek(name_rva)
        ln = u32(f.read(4), 0)
        name = f.read(ln).decode("utf-16-le", "replace")
        f.seek(cur)
        modules.append((base, size_of_image, name))
print("\nmodules: %d" % len(modules))
nioh = None
for base, size, name in modules:
    if name.lower().endswith("nioh.exe"):
        nioh = (base, size, name)
print("nioh.exe: %s" % (("base=0x%X size=0x%X %s" % nioh) if nioh else "NOT FOUND"))
for base, size, name in modules[:8]:
    print("  0x%016X 0x%08X %s" % (base, size, name))

# --- exception (stream 6) -----------------------------------------------------
ex = streams.get(6)
if not ex:
    print("\nno exception stream")
    sys.exit(0)
size, rva = ex[0]
f.seek(rva)
rec = f.read(size)
thread_id = u32(rec, 0)
exc_code = u32(rec, 8)
exc_addr = u64(rec, 8 + 16)
nparams = u32(rec, 8 + 24)
print("\nexception:")
print("  thread id      : %d" % thread_id)
print("  exception code : 0x%08X" % exc_code)
print("  fault address  : 0x%016X" % exc_addr)
if nioh:
    print("  -> nioh.exe+0x%X" % (exc_addr - nioh[0]) if nioh[0] <= exc_addr
          else "  -> outside nioh.exe")
for i in range(min(nparams, 15)):
    print("  param[%d]       : 0x%016X" % (i, u64(rec, 8 + 32 + i * 8)))

ctx_size = u32(rec, 8 + 32 + 15 * 8)
ctx_rva = u32(rec, 8 + 32 + 15 * 8 + 4)
if not ctx_rva:
    print("\nno thread context in the dump")
    sys.exit(0)
f.seek(ctx_rva)
ctx = f.read(ctx_size)
print("\ncontext (%d bytes):" % len(ctx))
flags = u32(ctx, 48)
print("  ContextFlags   : 0x%08X" % flags)
for i in range(4):
    print("  Dr%d            : 0x%016X" % (i, u64(ctx, 72 + i * 8)))
dr6 = u64(ctx, 104)
dr7 = u64(ctx, 112)
print("  Dr6            : 0x%016X%s" % (dr6, "   <-- breakpoint condition bits set!"
                                       if (dr6 & 0xF) else ""))
print("  Dr7            : 0x%016X%s" % (dr7, "   <-- local enables: %s"
                                       % bin(dr7 & 0xFF)))
rip = u64(ctx, 248)
print("  Rip            : 0x%016X" % rip)
if nioh:
    inside = nioh[0] <= rip < nioh[0] + nioh[1]
    print("  -> %s" % ("nioh.exe+0x%X" % (rip - nioh[0]) if inside else "outside nioh.exe"))
    for r in anchor_rvas:
        tgt = nioh[0] + r
        if dr6 and any(u64(ctx, 72 + i * 8) == tgt for i in range(4)):
            print("     DR holds anchor rva 0x%X  <-- this mod's breakpoint" % r)
print("  Rsp            : 0x%016X" % u64(ctx, 152))
print("  Rcx            : 0x%016X" % u64(ctx, 128))
