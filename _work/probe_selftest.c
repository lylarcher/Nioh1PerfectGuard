// Toolchain smoke test: a minimal x64 Windows DLL.
#include <windows.h>
#include <stdio.h>

static void write_log(const char *msg) {
    char path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&write_log, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char *slash = path;
    for (char *p = path; *p; ++p) if (*p == '\\') slash = p;
    if (slash) slash[1] = 0;
    lstrcatA(path, "probe_selftest.log");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    DWORD n = 0;
    WriteFile(f, msg, (DWORD)lstrlenA(msg), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        write_log("probe attached ok");
    }
    return TRUE;
}

__declspec(dllexport) int probe_selftest(void) { return 42; }
