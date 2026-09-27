// Probe how XAudio2 can be obtained on this machine, since that determines how
// the mod will play its custom parry sound. Tries both routes:
//   (a) LoadLibrary + XAudio2Create from the versioned DLLs
//   (b) CoCreateInstance with the documented class ids
#include <windows.h>
#include <xaudio2.h>
#include <cstdio>

static const GUID kClsid28 = {
    0x8bcf1f58, 0x9f7e, 0x4583, {0x8e, 0x3d, 0xe5, 0x5c, 0x4d, 0x4c, 0x5a, 0x6e}};
static const GUID kClsid27 = {
    0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID kClsid29 = {
    0x3d10575d, 0x1e6b, 0x4bf3, {0x8d, 0x9d, 0x7e, 0x0d, 0x0d, 0x8f, 0x0f, 0x0f}};

typedef HRESULT(WINAPI* pfnXAudio2Create)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

static void try_voice(IXAudio2* xa, const char* how) {
    if (!xa) return;
    IXAudio2MasteringVoice* mv = nullptr;
    HRESULT hr = xa->CreateMasteringVoice(&mv, 2, 48000);
    printf("  %-28s CreateMasteringVoice hr=0x%08lX mv=%p\n",
           how, (unsigned long)hr, (void*)mv);
    if (SUCCEEDED(hr)) {
        hr = xa->StartEngine();
        printf("  %-28s StartEngine          hr=0x%08lX\n", how, (unsigned long)hr);
        xa->StopEngine();
        if (mv) mv->DestroyVoice();
    }
    xa->Release();
}

int main() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    printf("CoInitializeEx hr=0x%08lX\n\n", (unsigned long)hr);

    const char* dlls[] = {"xaudio2_9.dll", "xaudio2_8.dll", "xaudio2_7.dll"};
    for (const char* d : dlls) {
        HMODULE m = LoadLibraryA(d);
        printf("%-14s LoadLibrary -> %p\n", d, (void*)m);
        if (!m) continue;
        pfnXAudio2Create fn = (pfnXAudio2Create)(void*)GetProcAddress(m, "XAudio2Create");
        printf("  XAudio2Create -> %p\n", (void*)fn);
        if (fn) {
            IXAudio2* xa = nullptr;
            HRESULT h2 = fn(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
            printf("  call -> hr=0x%08lX xa=%p\n", (unsigned long)h2, (void*)xa);
            char label[64];
            wsprintfA(label, "%s", d);
            try_voice(xa, label);
        }
    }

    struct { const GUID* g; const char* n; } cl[] = {
        {&kClsid28, "CoCreateInstance 8bcf1f58"},
        {&kClsid27, "CoCreateInstance 5a508685"},
        {&kClsid29, "CoCreateInstance 3d10575d"},
    };
    printf("\n");
    for (auto& c : cl) {
        IXAudio2* xa = nullptr;
        HRESULT h2 = CoCreateInstance(*c.g, nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IXAudio2), (void**)&xa);
        printf("%-28s hr=0x%08lX xa=%p\n", c.n, (unsigned long)h2, (void*)xa);
        if (SUCCEEDED(h2) && xa) try_voice(xa, c.n);
    }

    CoUninitialize();
    return 0;
}
