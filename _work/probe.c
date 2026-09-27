// probe.c -- hardware-breakpoint tracer DLL for locating gameplay anchors.
//
// Why hardware breakpoints instead of inline hooks: this probe only answers
// "is function X executed, and with what arguments", and it must not modify any
// game code byte while anchors are still being hunted. Debug registers give
// execution breakpoints with zero code patching.
//
// Configuration is read from probe.ini next to the DLL:
//
//   [Probe]
//   Module=nioh.exe
//   Target1=0x749640
//   Target2=0x70ECE0
//   MaxHits=2000
//   Log=probe.log
//
// Up to 4 targets can be armed at once (x64 exposes DR0..DR3). DR registers are
// per-thread, so every thread of the process is armed once and new threads are
// picked up by the poll loop.
//
// Build:
//   zig cc -target x86_64-windows-gnu -shared -O2 -o probe.dll probe.c

#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TARGETS 4
#define MAX_ARMED 8192

static char g_dir[MAX_PATH] = {0};
static char g_log_path[MAX_PATH * 2] = {0};
static char g_module_name[MAX_PATH] = "nioh.exe";
static unsigned long long g_target_rva[MAX_TARGETS] = {0};
static unsigned long long g_target_va[MAX_TARGETS] = {0};
static int g_target_count = 0;
static long g_max_hits = 2000;
static volatile long g_hits = 0;
static volatile long g_target_hits[MAX_TARGETS] = {0};
static volatile LONG g_target_off[MAX_TARGETS] = {0};  // 1 = stop trapping this target
static volatile LONG g_need_rearm = 0;
static long g_per_target_cap = 0;   // 0 = only the global cap applies

// Per-target context action applied at the breakpoint. This is what turns the
// tracer into a mod: the handler edits the *saved thread context* (argument
// registers, SSE registers, or RIP) instead of patching game code.
// ini syntax:  ActionN=rcx=0x64 | rdx=0x1 | r8=.. | r9=.. | xmm1=0.0 | skip=5
#define ACT_NONE 0
#define ACT_INT  1
#define ACT_F32  2
#define ACT_SKIP 3
typedef struct {
    int kind;
    int reg;              // 0=rcx 1=rdx 2=r8 3=r9 for ACT_INT
    unsigned long long ival;
    float fval;
    int skip;
} Action;

static Action g_action[MAX_TARGETS];
static unsigned long long g_base = 0;
static PVOID g_veh = NULL;
static HANDLE g_log_mutex = NULL;
static volatile LONG g_started = 0;
static volatile LONG g_stop = 0;

static DWORD g_armed[MAX_ARMED];
static volatile LONG g_armed_n = 0;

static unsigned long long g_text_lo = 0, g_text_hi = 0;
static unsigned long long g_data_lo = 0, g_data_hi = 0;
static unsigned long long g_img_lo = 0, g_img_hi = 0;
static int g_hunt_inputmgr = 0;

static void log_line(const char *fmt, ...);

// ------------------------------------------------------------- watches ------
// A watch follows a pointer chain and logs the bytes whenever they change, so a
// single game session shows e.g. the player's stamina curve going up and down.
// ini syntax:  WatchN=label;0xBASE_RVA;+off;+off;...;size
//   +off  = add offset then DEREFERENCE (follow a pointer)
//   #off  = add offset only (inline sub-structure within the same object)
// The final step is always a read of `size` bytes.
#define MAX_WATCH 4
typedef struct {
    char label[40];
    unsigned long long base_rva;
    unsigned long long off[6];
    int inline_off[6];
    int chain_len;
    int size;
} Watch;

static Watch g_watch[MAX_WATCH];
static int g_watch_count = 0;
static unsigned char g_watch_last[MAX_WATCH][32];
static int g_watch_valid[MAX_WATCH] = {0};
static unsigned long long g_watch_ptr[MAX_WATCH] = {0};

static void parse_watch(const char *val) {
    if (g_watch_count >= MAX_WATCH) return;
    char buf[300];
    lstrcpynA(buf, val, sizeof(buf));

    char *parts[10];
    int np = 0;
    char *p = buf;
    while (p && np < 10) {
        parts[np++] = p;
        char *semi = strchr(p, ';');
        if (semi) { *semi = 0; p = semi + 1; } else p = NULL;
    }
    if (np < 3) return;

    Watch *w = &g_watch[g_watch_count];
    memset(w, 0, sizeof(*w));
    lstrcpynA(w->label, parts[0], sizeof(w->label));
    w->base_rva = strtoull(parts[1], NULL, 16);
    w->chain_len = 0;
    for (int i = 2; i < np - 1 && w->chain_len < 6; ++i) {
        const char *t = parts[i];
        int inl = 0;
        if (*t == '#') { inl = 1; ++t; }
        else if (*t == '+') { ++t; }
        w->off[w->chain_len] = strtoull(t, NULL, 16);
        w->inline_off[w->chain_len] = inl;
        w->chain_len++;
    }
    w->size = atoi(parts[np - 1]);
    if (w->size < 1) w->size = 4;
    if (w->size > 32) w->size = 32;
    if (w->chain_len == 0 || w->base_rva == 0) return;
    g_watch_count++;
}

static void sample_watches(void) {
    for (int i = 0; i < g_watch_count; ++i) {
        Watch *w = &g_watch[i];
        SIZE_T got = 0;
        unsigned long long p = 0;
        unsigned long long addr = g_base + w->base_rva;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, &p, 8, &got) || !p) {
            if (g_watch_valid[i]) {
                g_watch_valid[i] = 0;
                log_line("WATCH %s lost (chain head NULL)", w->label);
            }
            continue;
        }
        g_watch_ptr[i] = p;
        for (int k = 0; k < w->chain_len; ++k) {
            if (k == w->chain_len - 1) {
                // final step: read the payload at p + off
                addr = p + w->off[k];
                unsigned char buf[32];
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, buf,
                                       (SIZE_T)w->size, &got) ||
                    got != (SIZE_T)w->size) {
                    break;
                }
                if (!g_watch_valid[i] || memcmp(buf, g_watch_last[i], w->size) != 0) {
                    memcpy(g_watch_last[i], buf, w->size);
                    char hex[3 * 32 + 1];
                    int n = 0;
                    for (int b = 0; b < w->size && n < (int)sizeof(hex) - 3; ++b)
                        n += snprintf(hex + n, sizeof(hex) - n, "%02X", buf[b]);
                    char fl[160];
                    int m = 0;
                    fl[0] = 0;
                    for (int f = 0; f + 4 <= w->size && f / 4 < 4; f += 4) {
                        float fv;
                        memcpy(&fv, buf + f, 4);
                        m += snprintf(fl + m, sizeof(fl) - m, "%s%.5g", f ? "," : "", fv);
                    }
                    log_line("WATCH %s obj=0x%llX at=0x%llX hex=%s floats=[%s]",
                             w->label, g_watch_ptr[i], addr, hex, fl);
                    g_watch_valid[i] = 1;
                }
                break;
            }
            if (w->inline_off[k]) {
                p += w->off[k];          // inline sub-structure: no dereference
            } else {
                unsigned long long nxt = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(p + w->off[k]),
                                       &nxt, 8, &got) || !nxt) {
                    break;
                }
                p = nxt;
            }
        }
    }
}

// Read the module's own PE headers to learn the .text range, so stack scanning
// can tell code addresses apart from data.
static void compute_text_range(unsigned long long base) {
    if (!base) return;
    unsigned char *p = (unsigned char *)(ULONG_PTR)base;
    if (p[0] != 'M' || p[1] != 'Z') return;
    unsigned int e_lfanew = *(unsigned int *)(p + 0x3C);
    unsigned char *nt = p + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E') return;
    unsigned short nsec = *(unsigned short *)(nt + 6);
    unsigned short opt_size = *(unsigned short *)(nt + 20);
    // SizeOfImage lives at optional-header offset 56 in both PE32 and PE32+
    g_img_lo = base;
    g_img_hi = base + *(unsigned int *)(nt + 24 + 56);
    unsigned char *sec = nt + 24 + opt_size;
    for (int i = 0; i < nsec; ++i, sec += 40) {
        const char *name = (const char *)sec;
        unsigned int vsize = *(unsigned int *)(sec + 8);
        unsigned int vaddr = *(unsigned int *)(sec + 12);
        if (name[0] == '.' && name[1] == 't' && name[2] == 'e' &&
            name[3] == 'x' && name[4] == 't') {
            g_text_lo = base + vaddr;
            g_text_hi = g_text_lo + vsize;
        } else if (name[0] == '.' && name[1] == 'd' && name[2] == 'a' &&
                   name[3] == 't' && name[4] == 'a') {
            g_data_lo = base + vaddr;
            g_data_hi = g_data_lo + vsize;
        }
    }
}

// Hunt for the input manager object.
//
// The per-frame pad poll at rva 0xE6AF34 reads XINPUT_STATE out of
// [manager + 0x49E08 + slot*20], with a per-slot "connected" byte at
// [manager + 0x49E04 + slot*20]. The manager pointer is not reachable
// statically (the poll is called through a table with no RTTI), so look for a
// pointer in .data whose target matches that layout.
static int plausible_ptr(unsigned long long v) {
    if (v < 0x10000ULL || v > 0x00007FFFFFFFFFFFULL) return 0;
    if (v & 7) return 0;
    // the input manager is a heap object: anything inside the game image is a
    // false positive (e.g. pointers into .rdata that happen to look right)
    if (v >= g_img_lo && v < g_img_hi) return 0;
    // cache the last region: VirtualQuery per candidate would make this scan crawl
    static unsigned long long cached_lo = 0, cached_hi = 0;
    static int cached_ok = 0;
    if (v >= cached_lo && v < cached_hi) return cached_ok;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)(ULONG_PTR)v, &mbi, sizeof(mbi)) == 0) {
        cached_lo = cached_hi = 0;
        cached_ok = 0;
        return 0;
    }
    cached_lo = (unsigned long long)mbi.BaseAddress;
    cached_hi = cached_lo + mbi.RegionSize;
    cached_ok = (mbi.State == MEM_COMMIT) &&
                ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_WRITECOPY)) != 0);
    return cached_ok;
}

static int safe_read(const void *addr, void *out, SIZE_T n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, n, &got) &&
           got == n;
}

static void hunt_inputmgr(void) {
    if (!g_data_lo) {
        log_line("HUNT inputmgr: .data range unknown");
        return;
    }
    log_line("HUNT inputmgr: scanning .data 0x%llX-0x%llX", g_data_lo, g_data_hi);
    int found = 0, scanned = 0;
    for (unsigned long long a = g_data_lo; a + 8 <= g_data_hi; a += 8) {
        unsigned long long v = *(unsigned long long *)(ULONG_PTR)a;
        if (!plausible_ptr(v)) continue;
        scanned++;
        // read the whole per-slot block (4 slots: flag at +0x04+i*20, state at
        // +0x08+i*20); ReadProcessMemory keeps this safe on non-resident pages
        unsigned char blk[96];
        if (!safe_read((const void *)(ULONG_PTR)(v + 0x49E00), blk, sizeof(blk)))
            continue;
        int connected = 0, ok = 1;
        unsigned short btns[4];
        unsigned int packets[4];
        for (int i = 0; i < 4; ++i) {
            unsigned char flag = blk[0x04 + i * 20];
            if (flag > 1) { ok = 0; break; }
            if (flag) connected++;
            memcpy(&btns[i], blk + 0x0C + i * 20, 2);
            memcpy(&packets[i], blk + 0x08 + i * 20, 4);
            if (btns[i] > 0x8000) { ok = 0; break; }
            if (packets[i] > 0x10000000u) { ok = 0; break; }
        }
        if (!ok || connected == 0) continue;
        float timer = 0.0f;
        if (!safe_read((const void *)(ULONG_PTR)(v + 0x49E54), &timer, 4))
            continue;
        if (!((timer >= 0.0f) && (timer <= 4.0f))) continue;
        log_line("HUNT inputmgr CANDIDATE ptr=0x%llX (from .data 0x%llX) conn=%d "
                 "buttons=[0x%04X,0x%04X,0x%04X,0x%04X] packets=[%u,%u,%u,%u] timer=%.4f",
                 v, a, connected, btns[0], btns[1], btns[2], btns[3],
                 packets[0], packets[1], packets[2], packets[3], timer);
        found++;
        if (found >= 8) break;
    }
    log_line("HUNT inputmgr: done, %d candidate(s) from %d checked pointers",
             found, scanned);
}

// ---------------------------------------------------------------- logging ---

static void log_line(const char *fmt, ...) {
    if (!g_log_path[0]) return;
    if (g_log_mutex) WaitForSingleObject(g_log_mutex, 2000);

    HANDLE f = CreateFileA(g_log_path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        // wsprintfA does not understand %llX; use the real CRT formatter.
        char line[1200];
        int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ",
                         st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(line + n, sizeof(line) - n, fmt, ap);
        va_end(ap);
        if (m < 0) m = 0;
        n += m;
        if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
        line[n++] = '\r';
        line[n++] = '\n';

        DWORD w = 0;
        WriteFile(f, line, (DWORD)n, &w, NULL);
        CloseHandle(f);
    }
    if (g_log_mutex) ReleaseMutex(g_log_mutex);
}

static void module_dir_of(HMODULE mod, char *out, size_t cap) {
    GetModuleFileNameA(mod, out, (DWORD)cap);
    char *slash = out;
    for (char *p = out; *p; ++p) if (*p == '\\') slash = p;
    if (slash) slash[1] = 0;
}

// ------------------------------------------------------------------- ini ----

static void read_config(void) {
    char ini[MAX_PATH * 2];
    lstrcpyA(ini, g_dir);
    lstrcatA(ini, "probe.ini");

    GetPrivateProfileStringA("Probe", "Module", "nioh.exe", g_module_name,
                             sizeof(g_module_name), ini);
    g_max_hits = GetPrivateProfileIntA("Probe", "MaxHits", 2000, ini);
    g_per_target_cap = GetPrivateProfileIntA("Probe", "PerTargetCap", 400, ini);
    g_hunt_inputmgr = GetPrivateProfileIntA("Probe", "HuntInputMgr", 0, ini);

    char logname[260];
    GetPrivateProfileStringA("Probe", "Log", "probe.log", logname, sizeof(logname), ini);
    lstrcpyA(g_log_path, g_dir);
    lstrcatA(g_log_path, logname);

    g_target_count = 0;
    for (int i = 0; i < MAX_TARGETS; ++i) {
        char key[32], val[64];
        wsprintfA(key, "Target%d", i + 1);
        GetPrivateProfileStringA("Probe", key, "", val, sizeof(val), ini);
        if (!val[0]) continue;
        g_target_rva[g_target_count++] = strtoull(val, NULL, 16);
    }

    g_watch_count = 0;
    for (int i = 0; i < MAX_WATCH; ++i) {
        char key[32], val[300];
        wsprintfA(key, "Watch%d", i + 1);
        GetPrivateProfileStringA("Probe", key, "", val, sizeof(val), ini);
        if (!val[0]) continue;
        parse_watch(val);
    }

    memset(g_action, 0, sizeof(g_action));
    for (int i = 0; i < MAX_TARGETS; ++i) {
        char key[32], val[96];
        wsprintfA(key, "Action%d", i + 1);
        GetPrivateProfileStringA("Probe", key, "", val, sizeof(val), ini);
        if (!val[0]) continue;
        char *eq = strchr(val, '=');
        if (!eq) continue;
        *eq = 0;
        const char *name = val;
        const char *v = eq + 1;
        Action *a = &g_action[i];
        if (!lstrcmpiA(name, "rcx")) { a->kind = ACT_INT; a->reg = 0; a->ival = strtoull(v, NULL, 0); }
        else if (!lstrcmpiA(name, "rdx")) { a->kind = ACT_INT; a->reg = 1; a->ival = strtoull(v, NULL, 0); }
        else if (!lstrcmpiA(name, "r8")) { a->kind = ACT_INT; a->reg = 2; a->ival = strtoull(v, NULL, 0); }
        else if (!lstrcmpiA(name, "r9")) { a->kind = ACT_INT; a->reg = 3; a->ival = strtoull(v, NULL, 0); }
        else if (!lstrcmpiA(name, "xmm1")) { a->kind = ACT_F32; a->fval = (float)atof(v); }
        else if (!lstrcmpiA(name, "skip")) { a->kind = ACT_SKIP; a->skip = atoi(v); }
    }
}

// ------------------------------------------------------ debug registers -----

static BOOL arm_thread(HANDLE thread) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    if (SuspendThread(thread) == (DWORD)-1) return FALSE;
    BOOL ok = FALSE;
    if (GetThreadContext(thread, &ctx)) {
        ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
        DWORD64 dr7 = 0;
        for (int i = 0; i < g_target_count && i < 4; ++i) {
            if (g_target_off[i]) continue;   // budget exhausted: stop trapping it
            (&ctx.Dr0)[i] = g_target_va[i];
            dr7 |= (DWORD64)1 << (i * 2);    // L0..L3 enable, RW=00 LEN=00 (execute)
        }
        ctx.Dr7 = dr7;
        ok = SetThreadContext(thread, &ctx);
    }
    ResumeThread(thread);
    return ok;
}

static int is_armed(DWORD tid) {
    LONG n = g_armed_n;
    for (LONG i = 0; i < n; ++i) if (g_armed[i] == tid) return 1;
    return 0;
}

static void remember_armed(DWORD tid) {
    LONG n = InterlockedIncrement(&g_armed_n) - 1;
    if (n >= 0 && n < MAX_ARMED) g_armed[n] = tid;
    else InterlockedDecrement(&g_armed_n);
}

static void arm_new_threads(void) {
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            // Suspending ourselves would deadlock: this thread would never reach
            // the matching ResumeThread.
            if (te.th32ThreadID == self) continue;
            if (is_armed(te.th32ThreadID)) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (arm_thread(th)) remember_armed(te.th32ThreadID);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// ------------------------------------------------------------- VE handler ---

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *c = ep->ContextRecord;
    unsigned long long rip = c->Rip;
    int idx = -1;
    for (int i = 0; i < g_target_count; ++i)
        if (rip == g_target_va[i]) { idx = i; break; }
    if (idx < 0) return EXCEPTION_CONTINUE_SEARCH;

    long th_hits = InterlockedIncrement(&g_target_hits[idx]);
    // A target past its budget is unarmed entirely: continuing to trap a hot
    // function would stutter the game and distort the very timings we measure.
    if (g_per_target_cap > 0 && th_hits > g_per_target_cap) {
        if (th_hits == g_per_target_cap + 1) {
            log_line("PROBE target%d budget exhausted (%ld hits); unarming",
                     idx, g_per_target_cap);
            InterlockedExchange(&g_target_off[idx], 1);
            InterlockedExchange(&g_need_rearm, 1);
        }
        c->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    long h = InterlockedIncrement(&g_hits);
    if (h <= g_max_hits) {
        // Stack scan: collect qwords that point into .text -- a cheap backtrace,
        // so a single hit reveals the whole call chain.
        unsigned long long stack[96];
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c->Rsp, stack, sizeof(stack), &got);
        char frames[512];
        int fn = 0;
        frames[0] = 0;
        SIZE_T n = got / sizeof(unsigned long long);
        for (SIZE_T i = 0; i < n && fn < 6; ++i) {
            unsigned long long v = stack[i];
            if (g_text_lo && v >= g_text_lo && v < g_text_hi) {
                fn += snprintf(frames + lstrlenA(frames),
                               sizeof(frames) - lstrlenA(frames),
                               "%s0x%llX", fn ? "," : "", v - g_base);
            }
        }
        log_line("HIT#%ld target=%d n=%ld rva=0x%llX tid=%lu rcx=%016llX rdx=%016llX "
                 "r8=%016llX r9=%016llX rsp=%016llX chain=[%s]",
                 h, idx, th_hits, rip - g_base, GetCurrentThreadId(),
                 (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
                 (unsigned long long)c->R8, (unsigned long long)c->R9,
                 (unsigned long long)c->Rsp, frames);
    }

    // Resume Flag: without it the execution breakpoint fires again immediately.
    c->EFlags |= 0x10000;

    // Apply the configured action to the *saved context*. The game's code bytes
    // stay untouched; we only change what this thread sees when it resumes.
    const Action *a = &g_action[idx];
    switch (a->kind) {
    case ACT_INT:
        switch (a->reg) {
        case 0: c->Rcx = a->ival; break;
        case 1: c->Rdx = a->ival; break;
        case 2: c->R8 = a->ival; break;
        case 3: c->R9 = a->ival; break;
        }
        break;
    case ACT_F32: {
        float f = a->fval;
        // xmm1 lives in the 16-byte-aligned XMM save area of the context
        memcpy(&c->Xmm1, &f, 4);
        break;
    }
    case ACT_SKIP:
        if (a->skip > 0 && a->skip < 16) c->Rip += a->skip;
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ------------------------------------------------------------------ thread --

static DWORD WINAPI worker(LPVOID param) {
    (void)param;
    Sleep(1500);

    read_config();
    log_line("PROBE begin pid=%lu module=%s targets=%d maxhits=%ld cap=%ld",
             GetCurrentProcessId(), g_module_name, g_target_count, g_max_hits,
             g_per_target_cap);

    g_base = (unsigned long long)(ULONG_PTR)GetModuleHandleA(g_module_name);
    if (!g_base) {
        g_base = (unsigned long long)(ULONG_PTR)GetModuleHandleA(NULL);
        log_line("PROBE module '%s' NOT found; using main module base=0x%llX",
                 g_module_name, g_base);
    } else {
        log_line("PROBE base(0x%s)=0x%llX", g_module_name, g_base);
    }

    for (int i = 0; i < g_target_count; ++i) {
        g_target_va[i] = g_base + g_target_rva[i];
        log_line("PROBE armed target%d rva=0x%llX va=0x%llX", i,
                 g_target_rva[i], g_target_va[i]);
    }
    compute_text_range(g_base);
    log_line("PROBE text range 0x%llX - 0x%llX", g_text_lo, g_text_hi);
    log_line("PROBE data range 0x%llX - 0x%llX", g_data_lo, g_data_hi);
    if (g_hunt_inputmgr) {
        // the manager may not exist yet at the loading screen, so retry a few times
        for (int attempt = 0; attempt < 6; ++attempt) {
            hunt_inputmgr();
            Sleep(4000);
        }
    }
    if (g_target_count == 0 && g_watch_count == 0) {
        log_line("PROBE nothing configured; idle");
        return 0;
    }

    if (g_target_count > 0) {
        g_veh = AddVectoredExceptionHandler(1, veh_handler);
        log_line("PROBE veh=%p", g_veh);
    }
    for (int i = 0; i < g_watch_count; ++i)
        log_line("PROBE watch%d %s base=0x%llX chain=%d size=%d", i,
                 g_watch[i].label, g_watch[i].base_rva, g_watch[i].chain_len,
                 g_watch[i].size);

    int pass = 0;
    while (!g_stop) {
        // Watches want a fast cadence; enumerating ~400 threads does not, so the
        // two run on different periods.
        sample_watches();
        if (++pass % 4 == 0) arm_new_threads();
        if (InterlockedCompareExchange(&g_need_rearm, 0, 1) == 1)
            InterlockedExchange(&g_armed_n, 0);
        if (pass % 80 == 0) InterlockedExchange(&g_armed_n, 0);
        if (pass % 160 == 0) {
            char buf[320];
            int n = snprintf(buf, sizeof(buf), "PROBE totals total=%ld", g_hits);
            for (int i = 0; i < g_target_count && n < (int)sizeof(buf) - 32; ++i)
                n += snprintf(buf + n, sizeof(buf) - n, " t%d=%ld%s", i,
                              g_target_hits[i], g_target_off[i] ? "(off)" : "");
            log_line("%s", buf);
        }
        Sleep(150);
    }
    return 0;
}

// ------------------------------------------------------------------ entry ---

__declspec(dllexport) long probe_hit_count(void) { return g_hits; }

__declspec(dllexport) int probe_reload(void) {
    read_config();
    return g_target_count;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        module_dir_of(inst, g_dir, sizeof(g_dir));
        g_log_mutex = CreateMutexA(NULL, FALSE, NULL);
        if (InterlockedCompareExchange(&g_started, 1, 0) == 0) {
            HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            if (th) CloseHandle(th);
        }
    }
    return TRUE;
}
