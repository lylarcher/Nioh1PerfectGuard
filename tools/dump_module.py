"""Dump a running process's module image from memory and rebuild it as a PE file.

Why: nioh.exe's .text is encrypted on disk by Steam DRM (SteamStub) and only
decrypted in memory at runtime. To build AOB signatures we need the *runtime*
bytes, so we read the loaded module over ReadProcessMemory and reassemble a PE
whose file offsets equal the section RVAs (identity mapping), which IDA/Ghidra
and our own capstone tooling can consume directly.

No injection, no code in the game folder, no DRM circumvention.

Usage:
    python dump_module.py --list
    python dump_module.py --name nioh.exe --out out\nioh1.mem.exe
    python dump_module.py --pid 1234 --module nioh.exe --out out.exe
"""
from __future__ import annotations

import argparse
import ctypes
import math
import struct
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010

TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010

MAX_PATH = 260
MAX_MODULE_NAME32 = 255

MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
WRITABLE = 0x04 | 0x08 | 0x40 | 0x80  # RW, WRITECOPY, EXECUTE_READWRITE, EXECUTE_WRITECOPY
READABLE = 0x02 | 0x04 | 0x20 | 0x40 | 0x80


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * MAX_PATH),
    ]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", ctypes.c_void_p),
        ("szModule", wintypes.WCHAR * (MAX_MODULE_NAME32 + 1)),
        ("szExePath", wintypes.WCHAR * MAX_PATH),
    ]


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__alignment1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__alignment2", wintypes.DWORD),
    ]


k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
k32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
k32.VirtualQueryEx.argtypes = [
    wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION64),
    ctypes.c_size_t,
]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def iter_processes():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateToolhelp32Snapshot(processes)")
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = k32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            # copy: the API reuses the same buffer on every iteration
            yield entry.th32ProcessID, entry.szExeFile
            ok = k32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snap)


def iter_modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "CreateToolhelp32Snapshot(modules)")
    try:
        entry = MODULEENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = k32.Module32FirstW(snap, ctypes.byref(entry))
        while ok:
            # copy: the API reuses the same buffer on every iteration
            yield MODULEENTRY32W.from_buffer_copy(entry)
            ok = k32.Module32NextW(snap, ctypes.byref(entry))
    finally:
        k32.CloseHandle(snap)


def find_pid(name):
    want = name.lower()
    for pid, exe in iter_processes():
        if exe.lower() == want:
            return pid
    return None


def open_process(pid):
    access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
    handle = k32.OpenProcess(access, False, pid)
    if not handle:
        access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
        handle = k32.OpenProcess(access, False, pid)
    if not handle:
        raise OSError(ctypes.get_last_error(), "OpenProcess(pid=%d)" % pid)
    return handle


def read_mem(handle, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[: got.value]


def dump_image(handle, base, size):
    """Page-by-page dump of [base, base+size). Unreadable pages become zeros."""
    out = bytearray(size)
    addr = base
    end = base + size
    page = 0x1000
    unreadable = 0
    while addr < end:
        mbi = MEMORY_BASIC_INFORMATION64()
        if not k32.VirtualQueryEx(handle, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            addr += page
            unreadable += page
            continue
        region_end = mbi.BaseAddress + mbi.RegionSize
        if mbi.State != MEM_COMMIT or (mbi.Protect & PAGE_NOACCESS) or (mbi.Protect & PAGE_GUARD):
            new_addr = min(region_end, end)
            unreadable += new_addr - addr
            addr = new_addr
            continue
        chunk_end = min(region_end, end)
        chunk = chunk_end - addr
        data = read_mem(handle, addr, chunk)
        if data is None:
            # fall back to page granularity
            pos = addr
            while pos < chunk_end:
                n = min(page, chunk_end - pos)
                d = read_mem(handle, pos, n)
                if d is not None:
                    out[pos - base: pos - base + len(d)] = d
                    if len(d) < n:
                        break
                else:
                    unreadable += n
                pos += n
        else:
            out[addr - base: addr - base + len(data)] = data
        addr = chunk_end
    return bytes(out), unreadable


def parse_pe_headers(img):
    if img[:2] != b"MZ":
        raise ValueError("not an MZ image")
    e_lfanew = struct.unpack_from("<I", img, 0x3C)[0]
    if img[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    coff = e_lfanew + 4
    machine, nsec, ts, _, _, opt_size, chars = struct.unpack_from("<HHIIIHH", img, coff)
    opt = coff + 20
    magic = struct.unpack_from("<H", img, opt)[0]
    pe32plus = magic == 0x20B
    size_of_image = struct.unpack_from("<I", img, opt + 56)[0]
    size_of_headers = struct.unpack_from("<I", img, opt + 60)[0]
    checksum_off = opt + 64
    dd_off = opt + (112 if pe32plus else 96)
    sec_off = opt + opt_size
    sections = []
    for i in range(nsec):
        o = sec_off + i * 40
        name = img[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", img, o + 8)
        ch = struct.unpack_from("<I", img, o + 36)[0]
        sections.append(dict(idx=i, hdr_off=o, name=name, va=vaddr, vsize=vsize,
                             rawsize=rawsize, rawptr=rawptr, chars=ch))
    return dict(e_lfanew=e_lfanew, coff=coff, machine=machine, nsec=nsec,
                opt=opt, opt_size=opt_size, pe32plus=pe32plus,
                size_of_image=size_of_image, size_of_headers=size_of_headers,
                checksum_off=checksum_off, dd_off=dd_off, sec_off=sec_off,
                sections=sections)


def rebuild_pe(img, hdrs, rva_equals_offset=True):
    """Rewrite section headers so file offset == RVA (identity mapping)."""
    out = bytearray(img[: hdrs["size_of_image"]])
    for s in hdrs["sections"]:
        o = s["hdr_off"]
        virtual = s["vsize"] if rva_equals_offset else s["rawsize"]
        # PointerToRawData @ +20, SizeOfRawData @ +16
        struct.pack_into("<I", out, o + 16, virtual)
        struct.pack_into("<I", out, o + 20, s["va"])
        # clear IMAGE_SCN_CNT_UNINITIALIZED_DATA so tools read the raw bytes
        ch = s["chars"] & ~0x00000080
        struct.pack_into("<I", out, o + 36, ch)
    # checksum must be recomputed by the consumer; zero it to avoid false warnings
    struct.pack_into("<I", out, hdrs["checksum_off"], 0)
    return bytes(out)


def entropy(blob):
    if not blob:
        return 0.0
    counts = [0] * 256
    for b in blob:
        counts[b] += 1
    n = len(blob)
    e = 0.0
    for c in counts:
        if c:
            p = c / n
            e -= p * math.log2(p)
    return e


def verify(img, hdrs):
    print("\n=== verification of rebuilt image ===")
    for s in hdrs["sections"]:
        blob = img[s["va"]: s["va"] + s["vsize"]]
        if not blob:
            continue
        ent = entropy(blob)
        note = ""
        if s["name"].strip() == ".text":
            # fraction of E8 rel32 whose target lands inside this section
            inside = total = 0
            lo, hi = s["va"], s["va"] + s["vsize"]
            pos = blob.find(b"\xe8")
            n = len(blob)
            while pos != -1 and pos + 5 <= n:
                rel = struct.unpack_from("<i", blob, pos + 1)[0]
                tgt = lo + pos + 5 + rel
                total += 1
                if lo <= tgt < hi:
                    inside += 1
                pos = blob.find(b"\xe8", pos + 1)
            pct = (100.0 * inside / total) if total else 0.0
            note = "  E8=%d in-section=%.2f%%" % (total, pct)
        print("  %-8s va=%08X size=%9d entropy=%.3f%s" % (s["name"], s["va"], len(blob), ent, note))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="list processes matching --name and exit")
    ap.add_argument("--list-modules", action="store_true")
    ap.add_argument("--name", help="process image name, e.g. nioh.exe")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--module", help="module name to dump (default: main module)")
    ap.add_argument("--out", help="output PE path")
    ap.add_argument("--raw", help="also write the raw memory image here")
    ap.add_argument("--wait-decrypted", action="store_true",
                    help="poll until the code section no longer looks like ciphertext")
    ap.add_argument("--wait-timeout", type=float, default=180.0)
    args = ap.parse_args()

    if args.list:
        needle = (args.name or "").lower()
        for pid, exe in sorted(iter_processes(), key=lambda t: t[1].lower()):
            if not needle or needle in exe.lower():
                print("%8d  %s" % (pid, exe))
        return 0

    pid = args.pid or (find_pid(args.name) if args.name else None)
    if not pid:
        print("process not found: %s" % (args.name or args.pid), file=sys.stderr)
        return 2

    mods = list(iter_modules(pid))
    if args.list_modules:
        for m in mods:
            print("%016X  %9d  %s\n                        %s" % (
                m.modBaseAddr or 0, m.modBaseSize, m.szModule, m.szExePath))
        return 0

    target = None
    if args.module:
        want = args.module.lower()
        for m in mods:
            if m.szModule.lower() == want:
                target = m
                break
    else:
        # Toolhelp does not guarantee the main module is first: match the
        # process image name, else fall back to the lowest base address.
        proc_name = None
        for p_pid, p_exe in iter_processes():
            if p_pid == pid:
                proc_name = p_exe.lower()
                break
        if proc_name:
            for m in mods:
                if m.szModule.lower() == proc_name:
                    target = m
                    break
        if target is None and mods:
            target = min(mods, key=lambda m: m.modBaseAddr or 0)
    if target is None:
        print("module not found; available: %s" % ", ".join(m.szModule for m in mods[:20]), file=sys.stderr)
        return 2

    base = target.modBaseAddr or 0
    print("pid=%d module=%s base=0x%X size=%d" % (pid, target.szModule, base, target.modBaseSize))

    handle = open_process(pid)
    try:
        hdr = read_mem(handle, base, 0x1000)
        if not hdr:
            print("cannot read PE headers", file=sys.stderr)
            return 3
        hdrs = parse_pe_headers(hdr + b"\0" * 0x1000)
        size_of_image = hdrs["size_of_image"]
        print("machine=%04X sections=%d sizeOfImage=%d" % (hdrs["machine"], hdrs["nsec"], size_of_image))

        code = next((s for s in hdrs["sections"] if s["name"].strip() in (".text", ".code")), None)
        if args.wait_decrypted and code:
            deadline = time.time() + args.wait_timeout
            probe = min(0x40000, code["vsize"])
            attempt = 0
            while True:
                attempt += 1
                sample = read_mem(handle, base + code["va"], probe)
                ent = entropy(sample) if sample else 0.0
                print("  [wait] attempt %d: %s entropy=%.3f (threshold 7.000)" % (
                    attempt, code["name"], ent))
                if ent and ent < 7.0:
                    print("  [wait] code section looks decrypted")
                    break
                if time.time() >= deadline:
                    print("  [wait] TIMEOUT after %.0fs; dumping anyway" % args.wait_timeout,
                          file=sys.stderr)
                    break
                time.sleep(3.0)

        t0 = time.time()
        img, unreadable = dump_image(handle, base, size_of_image)
        print("dumped %d bytes in %.1fs (unreadable bytes: %d)" % (len(img), time.time() - t0, unreadable))

        rebuilt = rebuild_pe(img, hdrs)
        if args.raw:
            with open(args.raw, "wb") as fh:
                fh.write(img)
            print("raw image -> %s" % args.raw)
        if args.out:
            with open(args.out, "wb") as fh:
                fh.write(rebuilt)
            print("rebuilt PE -> %s (%d bytes)" % (args.out, len(rebuilt)))
            # verify from the file so we test what a consumer would see
            verify(rebuilt, parse_pe_headers(rebuilt))
        else:
            verify(img, hdrs)
    finally:
        k32.CloseHandle(handle)
    return 0


if __name__ == "__main__":
    sys.exit(main())
