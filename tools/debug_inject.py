"""Pinpoint why CreateRemoteThread+LoadLibraryW fails: verify the remote string,
and compare kernel32's base address between this process and the target."""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import inject_dll as inj  # noqa: E402

WORK = os.path.join(os.path.dirname(HERE), "_work")
target = os.path.join(WORK, "inject_target.exe")
dll = os.path.join(WORK, "probe_selftest.dll")

proc = subprocess.Popen([target], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        cwd=WORK, text=True)
time.sleep(1.5)
proc.stdout.readline()

k32 = inj.k32
k32.ReadProcessMemory.restype = wintypes.BOOL
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

ACCESS = (inj.PROCESS_CREATE_THREAD | inj.PROCESS_QUERY_INFORMATION |
          inj.PROCESS_VM_OPERATION | inj.PROCESS_VM_WRITE | inj.PROCESS_VM_READ)
h = k32.OpenProcess(ACCESS, False, proc.pid)
print("handle = 0x%X" % h)

# --- 1. kernel32 base in the target vs here ---
their_k32 = inj.list_modules(proc.pid, "kernel32.dll")
my_k32 = k32.GetModuleHandleW("kernel32.dll")
print("\nlocal  kernel32 base = 0x%X" % my_k32)
for name, base, size in their_k32:
    print("target kernel32 base = 0x%X  (%s, size=%d)" % (base, name, size))
if their_k32:
    delta = their_k32[0][1] - my_k32
    print("delta = %+d (0 means same base, safe to reuse the local LoadLibraryW address)"
          % delta)

# --- 2. write + read back the path string ---
path_bytes = dll.encode("mbcs") + b"\0"
remote = k32.VirtualAllocEx(h, None, len(path_bytes),
                            inj.MEM_COMMIT | inj.MEM_RESERVE, inj.PAGE_READWRITE)
written = ctypes.c_size_t(0)
ok = k32.WriteProcessMemory(h, ctypes.c_void_p(remote), path_bytes,
                            len(path_bytes), ctypes.byref(written))
print("\nWriteProcessMemory ok=%s written=%d remote=0x%X" % (ok, written.value, remote))

buf = ctypes.create_string_buffer(len(path_bytes) + 16)
got = ctypes.c_size_t(0)
ok = k32.ReadProcessMemory(h, ctypes.c_void_p(remote), buf, len(path_bytes),
                           ctypes.byref(got))
print("ReadProcessMemory ok=%s got=%d" % (ok, got.value))
print("read back = %r" % buf.raw[:max(got.value, 1)])
print("expected  = %r" % path_bytes)

# --- 3. try the thread again, reporting the raw exit code ---
load_lib = k32.GetProcAddress(ctypes.c_void_p(my_k32), b"LoadLibraryW")
th = k32.CreateRemoteThread(h, None, 0, ctypes.c_void_p(load_lib),
                            ctypes.c_void_p(remote), 0, None)
print("\nCreateRemoteThread -> 0x%X  (last error %d)" % (th, ctypes.get_last_error()))
if th:
    r = k32.WaitForSingleObject(th, 10000)
    code = wintypes.DWORD(0)
    k32.GetExitCodeThread(th, ctypes.byref(code))
    print("wait=%d exitcode=0x%X" % (r, code.value))
    k32.CloseHandle(th)

mods = inj.list_modules(proc.pid, "probe_selftest")
print("probe_selftest loaded? %s" % (mods if mods else "NO"))
k32.CloseHandle(h)
proc.terminate()
