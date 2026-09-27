// inject_target.c -- a plain Win32 process used to validate the whole probe
// pipeline end to end: it writes probe.ini describing its own tick() function,
// then runs. The harness injects probe.dll from the outside (no loader, no
// game-dir changes) and the probe should log one HIT per tick.
//
// Build: zig cc -target x86_64-windows-gnu -O2 -o inject_target.exe inject_target.c

#include <windows.h>
#include <stdio.h>

volatile int g_sink = 0;

__attribute__((noinline)) void tick(int seq) {
    g_sink += seq;
}

int main(void) {
    char dir[MAX_PATH], self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof(self));
    lstrcpyA(dir, self);
    char *slash = dir;
    for (char *p = dir; *p; ++p) if (*p == '\\') slash = p;
    if (slash) slash[1] = 0;
    char *name = self;
    for (char *p = self; *p; ++p) if (*p == '\\') name = p + 1;

    unsigned long long base = (unsigned long long)(ULONG_PTR)GetModuleHandleA(NULL);
    unsigned long long rva = (unsigned long long)(ULONG_PTR)&tick - base;

    char ini[MAX_PATH * 2];
    lstrcpyA(ini, dir);
    lstrcatA(ini, "probe.ini");
    FILE *f = fopen(ini, "wb");
    if (f) {
        fprintf(f,
                "[Probe]\r\n"
                "Module=%s\r\n"
                "Target1=0x%llX\r\n"
                "MaxHits=200\r\n"
                "Log=probe.log\r\n",
                name, rva);
        fclose(f);
    }

    printf("pid=%lu module=%s base=0x%llX tick_rva=0x%llX\n",
           GetCurrentProcessId(), name, base, rva);
    printf("wrote %s\n", ini);
    printf("running for 10s, calling tick() every 200ms\n");
    fflush(stdout);

    for (int i = 0; i < 50; ++i) {
        tick(i);
        Sleep(200);
    }
    printf("done, sink=%d\n", g_sink);
    return 0;
}
