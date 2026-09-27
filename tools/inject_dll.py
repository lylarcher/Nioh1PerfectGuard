"""Inject a DLL into a running process with ctypes (no compiler, no game-dir changes).

Why this exists: it removes the need to place the dinput8 proxy loader in the game
folder during the analysis phase. The probe DLL can live in the workspace, be
injected on demand while the game is already running, and be re-injected after
each rebuild.

Usage:
    python inject_dll.py --name nioh.exe --dll <path\to\probe.dll>
    python inject_dll.py --pid 1234  --dll <path>
"""
from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_CREATE_THREAD = 0x0002
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_WRITE = 0x0020
PROCESS_VM_READ = 0x0010

MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04

WAIT_TIMEOUT = 0x00000102


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
        ("szExeFile", wintypes.WCHAR * 260),
    ]


k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                               wintypes.DWORD, wintypes.DWORD]
k32.VirtualFreeEx.restype = wintypes.BOOL
k32.VirtualFreeEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                              wintypes.DWORD]
k32.WriteProcessMemory.restype = wintypes.BOOL
k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.GetModuleHandleW.restype = ctypes.c_void_p
k32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
k32.CreateRemoteThread.restype = wintypes.HANDLE
k32.CreateRemoteThread.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                                   ctypes.c_void_p]
k32.WaitForSingleObject.restype = wintypes.DWORD
k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k32.GetExitCodeThread.restype = wintypes.BOOL
k32.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]


def list_modules(pid, needle=None):
    """Enumerate loaded modules of a process (validates that a DLL really loaded)."""
    TH32CS_SNAPMODULE = 0x8
    TH32CS_SNAPMODULE32 = 0x10

    class MODULEENTRY32W(ctypes.Structure):
        _fields_ = [
            ("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
            ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
            ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
            ("modBaseSize", wintypes.DWORD), ("hModule", ctypes.c_void_p),
            ("szModule", wintypes.WCHAR * 256), ("szExePath", wintypes.WCHAR * 260),
        ]

    if not hasattr(k32, "Module32FirstW"):
        pass
    k32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    k32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == wintypes.HANDLE(-1).value:
        return []
    out = []
    try:
        e = MODULEENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Module32FirstW(snap, ctypes.byref(e))
        while ok:
            if not needle or needle.lower() in e.szModule.lower():
                out.append((e.szModule, e.modBaseAddr or 0, e.modBaseSize))
            ok = k32.Module32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return out


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    if snap == wintypes.HANDLE(-1).value:
        raise OSError(ctypes.get_last_error(), "snapshot")
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.lower() == name.lower():
                return e.th32ProcessID
            ok = k32.Process32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return None


def wait_thread(handle, ms=15000):
    r = k32.WaitForSingleObject(handle, ms)
    if r == WAIT_TIMEOUT:
        return None
    code = wintypes.DWORD(0)
    k32.GetExitCodeThread(handle, ctypes.byref(code))
    return code.value


def inject(pid, dll_path, verbose=True):
    dll_path = os.path.abspath(dll_path)
    if not os.path.isfile(dll_path):
        raise SystemExit("dll not found: %s" % dll_path)

    access = (PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ)
    h = k32.OpenProcess(access, False, pid)
    if not h:
        raise SystemExit("OpenProcess failed: %d (try running elevated)"
                         % ctypes.get_last_error())

    try:
        # LoadLibraryW wants UTF-16LE; handing it ANSI bytes silently produces a
        # garbage path and the load fails. Encode accordingly.
        path_bytes = dll_path.encode("utf-16-le") + b"\0\0"
        size = len(path_bytes)
        remote = k32.VirtualAllocEx(h, None, size, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE)
        if not remote:
            raise SystemExit("VirtualAllocEx failed: %d" % ctypes.get_last_error())

        written = ctypes.c_size_t(0)
        if not k32.WriteProcessMemory(h, ctypes.c_void_p(remote), path_bytes, size,
                                      ctypes.byref(written)):
            raise SystemExit("WriteProcessMemory failed: %d" % ctypes.get_last_error())

        # kernel32 is normally mapped at the same base in every process of a
        # session, but mandatory ASLR can break that. Resolve it defensively.
        local_k32 = k32.GetModuleHandleW("kernel32.dll")
        load_lib = k32.GetProcAddress(ctypes.c_void_p(local_k32), b"LoadLibraryW")
        if not load_lib:
            raise SystemExit("GetProcAddress(LoadLibraryW) failed")

        if verbose:
            print("  remote buffer = 0x%X (%d bytes written)" % (remote, written.value))
            print("  local kernel32= 0x%X  LoadLibraryW= 0x%X" % (local_k32, load_lib))

        th = k32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(load_lib),
                                    ctypes.c_void_p(remote), 0, None)
        if not th:
            raise SystemExit("CreateRemoteThread failed: %d" % ctypes.get_last_error())

        rc = wait_thread(th, 20000)
        k32.CloseHandle(th)
        k32.VirtualFreeEx(h, ctypes.c_void_p(remote), 0, MEM_RELEASE)

        if rc is None:
            print("warning: remote thread still running after 20s")
        elif rc:
            print("injected OK: %s -> pid %d (HMODULE=0x%X)" % (dll_path, pid, rc))
            return 0
        else:
            print("LoadLibraryW returned NULL inside the target "
                  "(bitness mismatch, sandboxed target, or DllMain failed)")

        name = os.path.basename(dll_path)
        mods = list_modules(pid, name)
        if mods:
            for m, base, sz in mods:
                print("  target already has %s at 0x%X size=%d" % (m, base, sz))
            return 0
        print("  %s is NOT present in the target's module list" % name)
        return 1
    finally:
        k32.CloseHandle(h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", help="process image name, e.g. nioh.exe")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--dll", required=True)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        needle = (args.name or "").lower()
        snap = k32.CreateToolhelp32Snapshot(0x2, 0)
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            if not needle or needle in e.szExeFile.lower():
                print("%8d  %s" % (e.th32ProcessID, e.szExeFile))
            ok = k32.Process32NextW(snap, ctypes.byref(e))
        k32.CloseHandle(snap)
        return 0

    pid = args.pid or (find_pid(args.name) if args.name else None)
    if not pid:
        print("process not found", file=sys.stderr)
        return 2
    return inject(pid, args.dll)


if __name__ == "__main__":
    sys.exit(main())
