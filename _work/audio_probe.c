// Probe what the toolchain gives us for audio: does mingw ship xaudio2.h, and
// can we activate XAudio2 by COM without linking an import library?
#include <windows.h>
#include <stdio.h>

#ifdef HAVE_XAUDIO2
#include <xaudio2.h>
#endif

int main(void) {
#ifdef HAVE_XAUDIO2
    printf("xaudio2.h included; IXAudio2=%p\n", (void *)sizeof(IXAudio2));
#else
    printf("xaudio2.h NOT used\n");
#endif
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    printf("CoInitializeEx -> 0x%08lX\n", (unsigned long)hr);

    static const GUID CLSID_XAudio2_28 = {
        0x8bcf1f58, 0x9f7e, 0x4583, {0x8e, 0x3d, 0xe5, 0x5c, 0x4d, 0x4c, 0x5a, 0x6e}};
    static const GUID CLSID_XAudio2_27 = {
        0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
    static const GUID IID_IXAudio2_any = {
        0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

    const GUID *clsid[2] = {&CLSID_XAudio2_28, &CLSID_XAudio2_27};
    const char *names[2] = {"2.8", "2.7"};
    for (int i = 0; i < 2; ++i) {
        void *obj = NULL;
        HRESULT h = CoCreateInstance(clsid[i], NULL, CLSCTX_INPROC_SERVER,
                                     &IID_IXAudio2_any, &obj);
        printf("CoCreateInstance XAudio2 %s -> hr=0x%08lX obj=%p\n",
               names[i], (unsigned long)h, obj);
        if (obj) {
            // vtable slot 0..2 are IUnknown; check we can find CreateMasteringVoice
            void **vt = *(void ***)obj;
            printf("   vtable=%p  [0]=%p [1]=%p [2]=%p\n", (void *)vt,
                   vt ? vt[0] : NULL, vt ? vt[1] : NULL, vt ? vt[2] : NULL);
            ((IUnknown *)obj)->Release();
        }
    }
    CoUninitialize();
    return 0;
}
