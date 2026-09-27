// Does an x64 execution breakpoint in DR0 actually fire in this process, and
// does the DR7 encoding matter? Three variants, one throwaway thread each.
#include <windows.h>
#include <stdio.h>

static volatile LONG g_hits = 0;
static volatile LONG g_calls = 0;
static unsigned long long g_target = 0;

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if ((unsigned long long)ep->ContextRecord->Rip == g_target) {
        ep->ContextRecord->EFlags |= 0x10000;   // resume flag
        InterlockedIncrement(&g_hits);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((noinline)) static void probe(void) { InterlockedIncrement(&g_calls); }

static DWORD WINAPI body(LPVOID p) {
    (void)p;
    for (int i = 0; i < 5; ++i) probe();
    return 0;
}

static DWORD64 run(const char *label, int variant) {
    InterlockedExchange(&g_hits, 0);
    InterlockedExchange(&g_calls, 0);

    DWORD tid = 0;
    HANDLE th = CreateThread(NULL, 0, body, NULL, CREATE_SUSPENDED, &tid);
    if (!th) { printf("%-28s CreateThread failed %lu\n", label, GetLastError()); return 0; }

    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) {
        printf("%-28s GetThreadContext failed %lu\n", label, GetLastError());
        CloseHandle(th);
        return 0;
    }
    DWORD64 initial = c.Dr7;

    c.Dr0 = g_target;
    c.Dr1 = c.Dr2 = c.Dr3 = 0;
    if (variant == 0) {
        c.Dr7 = 0x1;                                   // naive: wipe everything else
    } else if (variant == 1) {
        DWORD64 d = c.Dr7;
        d &= ~(DWORD64)0xFF;          // clear L0..G3
        d &= ~(DWORD64)0xFFFF0000ULL; // clear the RW/LEN fields
        c.Dr7 = d | 0x1;              // keep reserved bits, enable L0 exec 1 byte
    } else {
        c.Dr7 = 0x401;                                 // bit 10 + L0
    }

    if (!SetThreadContext(th, &c)) {
        printf("%-28s SetThreadContext failed %lu\n", label, GetLastError());
        ResumeThread(th);
        CloseHandle(th);
        return 0;
    }
    DWORD64 written = c.Dr7;
    ResumeThread(th);
    WaitForSingleObject(th, 3000);
    CloseHandle(th);

    printf("%-28s initial Dr7=0x%-5llX wrote Dr7=0x%-5llX -> hits=%ld/5 calls=%ld\n",
           label, (unsigned long long)initial, (unsigned long long)written,
           (long)g_hits, (long)g_calls);
    return (DWORD64)g_hits;
}

int main(void) {
    AddVectoredExceptionHandler(1, veh);
    g_target = (unsigned long long)(ULONG_PTR)&probe;
    printf("probe at %p\n", (void *)(ULONG_PTR)g_target);

    run("A: Dr7=0x1 (wipe)", 0);
    run("B: preserve reserved|L0", 1);
    run("C: Dr7=0x401", 2);

    // And confirm the breakpoint actually suppresses execution free of the VEH:
    // detach the handler effect by pointing the target elsewhere.
    g_target = 0;
    run("D: target=0 (no match)", 0);
    return 0;
}
