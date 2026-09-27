// selftest.c -- validates the probe DLL's hardware-breakpoint engine locally,
// with no game involved. It prints the RVA of a dummy target, points probe.ini
// at it, loads probe.dll, calls the target repeatedly, then reports how many
// hits the probe logged.
//
// Build: zig cc -target x86_64-windows-gnu -O2 -o selftest.exe selftest.c

#include <windows.h>
#include <stdio.h>

volatile int g_sink = 0;

__attribute__((noinline)) int dummy_target(int a, int b, int c, int d) {
    g_sink += a + b + c + d;
    return g_sink;
}

// a second, never-called target to prove the probe distinguishes addresses
__attribute__((noinline)) int never_called(int a) {
    g_sink += a;
    return g_sink;
}

// context-modification target: the probe sets rcx=100 at entry, so calling
// compute(5) must return 200 instead of 10.
__attribute__((noinline)) int compute(int a) {
    return a * 2;
}

static void dir_of_self(char *out, size_t cap) {
    GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *slash = out;
    for (char *p = out; *p; ++p) if (*p == '\\') slash = p;
    if (slash) slash[1] = 0;
}

int main(void) {
    char dir[MAX_PATH];
    dir_of_self(dir, sizeof(dir));

    char exe_name[MAX_PATH];
    GetModuleFileNameA(NULL, exe_name, sizeof(exe_name));
    char *base = exe_name;
    for (char *p = exe_name; *p; ++p) if (*p == '\\') base = p + 1;

    unsigned long long mod_base = (unsigned long long)(ULONG_PTR)GetModuleHandleA(NULL);
    unsigned long long rva_hit = (unsigned long long)(ULONG_PTR)&dummy_target - mod_base;
    unsigned long long rva_miss = (unsigned long long)(ULONG_PTR)&never_called - mod_base;
    unsigned long long rva_comp = (unsigned long long)(ULONG_PTR)&compute - mod_base;

    printf("module      = %s\n", base);
    printf("base        = 0x%llX\n", mod_base);
    printf("dummy_target rva = 0x%llX\n", rva_hit);
    printf("never_called rva = 0x%llX\n", rva_miss);
    printf("compute      rva = 0x%llX\n", rva_comp);

    // probe.ini goes next to the DLL we are about to load
    char ini[MAX_PATH * 2];
    lstrcpyA(ini, dir);
    lstrcatA(ini, "probe.ini");
    FILE *f = fopen(ini, "wb");
    if (!f) { printf("cannot write %s\n", ini); return 2; }
    fprintf(f,
            "[Probe]\r\n"
            "Module=%s\r\n"
            "Target1=0x%llX\r\n"
            "Target2=0x%llX\r\n"
            "; on every entry to compute(), force rcx (arg a) to 100\r\n"
            "Action2=rcx=100\r\n"
            "Target3=0x%llX\r\n"
            "MaxHits=100\r\n"
            "Log=probe.log\r\n",
            base, rva_hit, rva_comp, rva_miss);
    fclose(f);
    printf("wrote %s\n", ini);

    char dll[MAX_PATH * 2];
    lstrcpyA(dll, dir);
    lstrcatA(dll, "probe.dll");
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 3; }
    printf("probe.dll loaded at 0x%p\n", (void *)h);

    printf("waiting for probe to arm...\n");
    Sleep(3000);

    printf("calling dummy_target 10 times\n");
    for (int i = 0; i < 10; ++i) {
        dummy_target(i, i + 1, i + 2, i + 3);
        Sleep(60);
    }
    Sleep(300);

    typedef long (*hitfn)(void);
    hitfn hc = (hitfn)GetProcAddress(h, "probe_hit_count");
    printf("probe_hit_count() = %ld  (expect 10)\n", hc ? hc() : -1);

    printf("\n--- context-modification test ---\n");
    int before = compute(5);
    printf("compute(5) = %d   (expect 200: the probe forces rcx=100 at entry)\n", before);
    if (before != 200)
        printf("  !! context action did NOT take effect\n");
    else
        printf("  OK: behaviour changed without patching a single code byte\n");

    char logf[MAX_PATH * 2];
    lstrcpyA(logf, dir);
    lstrcatA(logf, "probe.log");
    printf("\n---- probe.log ----\n");
    FILE *lf = fopen(logf, "rb");
    if (lf) {
        char line[1024];
        int shown = 0;
        while (fgets(line, sizeof(line), lf) && shown < 40) {
            printf("%s", line);
            ++shown;
        }
        fclose(lf);
    } else {
        printf("(no log written)\n");
    }
    return 0;
}
