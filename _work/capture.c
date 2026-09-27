// capture.cs -- build with:
//   <zig>/zig cc -target x86_64-windows-gnu -O2 -o capture.exe capture.c -lgdi32 -luser32
// Captures a window via PrintWindow (works for DirectX windows with
// PW_RENDERFULLCONTENT) and writes a BMP. Also reports whether the calling
// process can see the interactive desktop, and can send a test key so we can
// tell whether synthetic input actually reaches the game.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HWND find_game(void) {
    // match by window title containing "Nioh"
    HWND found = NULL;
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
        char title[256];
        if (!GetWindowTextA(h, title, sizeof(title))) continue;
        if (strstr(title, "Nioh") || strstr(title, "NIOH")) {
            if (IsWindowVisible(h)) { found = h; break; }
        }
    }
    return found;
}

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "shot.bmp";
    int send_key = (argc > 2) ? atoi(argv[2]) : 0;

    printf("desktop check:\n");
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    printf("  OpenInputDesktop -> %s\n", d ? "OK (interactive desktop reachable)" : "FAILED");
    if (d) CloseDesktop(d);
    printf("  GetForegroundWindow -> 0x%p\n", (void *)GetForegroundWindow());

    HWND h = find_game();
    if (!h) { printf("game window not found\n"); return 2; }
    char title[256];
    GetWindowTextA(h, title, sizeof(title));
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    printf("window '%s' hwnd=0x%p rect=%d,%d %dx%d iconic=%d\n",
           title, (void *)h, r.left, r.top, w, ht, IsIconic(h));

    if (send_key) {
        printf("sending test key VK=0x%X to the game window...\n", send_key);
        SetForegroundWindow(h);
        Sleep(300);
        keybd_event((BYTE)send_key, 0, 0, 0);
        Sleep(60);
        keybd_event((BYTE)send_key, 0, KEYEVENTF_KEYUP, 0);
        printf("  foreground after send: 0x%p (game hwnd 0x%p) same=%d\n",
               (void *)GetForegroundWindow(), (void *)h,
               GetForegroundWindow() == h);
    }

    HDC hdcWin = GetWindowDC(h);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP bmp = CreateCompatibleBitmap(hdcWin, w, ht);
    HGDIOBJ old = SelectObject(hdcMem, bmp);
    BOOL ok = PrintWindow(h, hdcMem, 2 /* PW_RENDERFULLCONTENT */);
    printf("PrintWindow -> %d (err %lu)\n", ok, GetLastError());

    // read back and measure how non-black it is, so we can tell a real capture
    // from a black frame without needing to view it
    BITMAPINFOHEADER ih;
    memset(&ih, 0, sizeof(ih));
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -ht;   // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    int stride = w * 4;
    unsigned char *buf = (unsigned char *)malloc((size_t)stride * ht);
    int got = GetDIBits(hdcMem, bmp, 0, ht, buf, (BITMAPINFO *)&ih, DIB_RGB_COLORS);
    long nonzero = 0, sum = 0;
    if (got) {
        for (long i = 0; i < (long)stride * ht; i += 4) {
            unsigned v = buf[i] + buf[i + 1] + buf[i + 2];
            sum += v;
            if (v > 12) nonzero++;
        }
    }
    long total = (long)w * ht;
    printf("pixels: nonblack=%ld/%ld (%.1f%%) avg=%.1f\n",
           nonzero, total, total ? 100.0 * nonzero / total : 0.0,
           total ? (double)sum / total : 0.0);

    if (got) {
        // write a 24-bit BMP
        BITMAPFILEHEADER fh;
        memset(&fh, 0, sizeof(fh));
        BITMAPINFOHEADER oh;
        memset(&oh, 0, sizeof(oh));
        int ostride = ((w * 3 + 3) / 4) * 4;
        int imgsize = ostride * ht;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(oh);
        fh.bfSize = fh.bfOffBits + imgsize;
        oh.biSize = sizeof(oh);
        oh.biWidth = w;
        oh.biHeight = ht;   // bottom-up for BMP
        oh.biPlanes = 1;
        oh.biBitCount = 24;
        oh.biCompression = BI_RGB;
        oh.biSizeImage = imgsize;
        FILE *f = fopen(out, "wb");
        if (f) {
            fwrite(&fh, sizeof(fh), 1, f);
            fwrite(&oh, sizeof(oh), 1, f);
            unsigned char *row = (unsigned char *)malloc(ostride);
            for (int y = ht - 1; y >= 0; --y) {
                unsigned char *src = buf + (size_t)y * stride;
                for (int x = 0; x < w; ++x) {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(row, ostride, 1, f);
            }
            free(row);
            fclose(f);
            printf("wrote %s\n", out);
        }
    }
    free(buf);
    SelectObject(hdcMem, old);
    DeleteObject(bmp);
    DeleteDC(hdcMem);
    ReleaseDC(h, hdcWin);
    return 0;
}
