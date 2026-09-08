#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

#define OUTSIDE_PAD 16
#define MAX_RUNS     6

static void runs_at(int x, int y, int width, char *out, size_t cap, int forward) {
    HDC screen = GetDC(NULL);
    if (!screen) { snprintf(out, cap, "?"); return; }

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -1;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void   *bits = NULL;
    HDC     mem  = CreateCompatibleDC(screen);
    HBITMAP bmp  = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    out[0] = 0;

    if (mem && bmp && bits &&
        (SelectObject(mem, bmp), BitBlt(mem, 0, 0, width, 1, screen, x, y,
                                        SRCCOPY | CAPTUREBLT))) {
        DWORD *px = (DWORD *)bits;
        size_t used = 0;
        int    i    = 0;
        for (int run = 0; run < MAX_RUNS && i < width; run++) {
            DWORD color = px[forward ? i : width - 1 - i] & 0x00FFFFFF;
            int   len   = 0;
            while (i + len < width &&
                   (px[forward ? i + len : width - 1 - i - len] & 0x00FFFFFF)
                   == color) len++;
            int n = snprintf(out + used, cap - used, "%s#%06lX x%d",
                             run ? " " : "", (unsigned long)color, len);
            if (n < 0 || (size_t)n >= cap - used) break;
            used += (size_t)n;
            i    += len;
        }
    }

    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(NULL, screen);
    if (!out[0]) snprintf(out, cap, "?");
}

static RECT frame_rect(HWND hwnd) {
    RECT r;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &r, sizeof r)))
        return r;
    GetWindowRect(hwnd, &r);
    return r;
}

static void report(HWND hwnd, const char *tag) {
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    RECT fr = frame_rect(hwnd);

    POINT o = { 0, 0 };
    ClientToScreen(hwnd, &o);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);

    char left[200], right[200];
    int  y = (int)((fr.top + fr.bottom) / 2);
    runs_at((int)fr.left - OUTSIDE_PAD, y,
            (int)(fr.right - fr.left) + 2 * OUTSIDE_PAD, left, sizeof left, 1);
    runs_at((int)fr.left - OUTSIDE_PAD, y,
            (int)(fr.right - fr.left) + 2 * OUTSIDE_PAD, right, sizeof right, 0);

    printf("%-10s sty=0x%08lX thick=%d\n", tag, (unsigned long)style,
           (style & WS_THICKFRAME) ? 1 : 0);
    printf("           win  %ldx%ld @%ld,%ld\n",
           wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top);
    printf("           dwm  %ldx%ld @%ld,%ld  insets l=%ld t=%ld r=%ld b=%ld\n",
           fr.right - fr.left, fr.bottom - fr.top, fr.left, fr.top,
           fr.left - wr.left, fr.top - wr.top,
           wr.right - fr.right, wr.bottom - fr.bottom);
    printf("           cli  %ldx%ld @%ld,%ld  insets l=%ld t=%ld r=%ld b=%ld\n",
           cr.right - cr.left, cr.bottom - cr.top, o.x, o.y,
           o.x - wr.left, o.y - wr.top,
           wr.right - (o.x + (cr.right - cr.left)),
           wr.bottom - (o.y + (cr.bottom - cr.top)));
    printf("           L: %s\n           R: %s\n", left, right);
    fflush(stdout);
}

int wmain(int argc, wchar_t **argv) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *set_ctx)(HANDLE) = user32
        ? (BOOL (WINAPI *)(HANDLE))(void *)
          GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
    if (set_ctx) set_ctx((HANDLE)-4);

    if (argc < 2) {
        printf("probe_frame <hwnd-hex> [thick on|off] [place x y w h]\n"
               "  thick  add or remove WS_THICKFRAME, then re-measure\n"
               "  place  put the VISIBLE frame on that rect, compensating for\n"
               "         the invisible border the way mshell does\n");
        return 1;
    }

    if (!wcscmp(argv[1], L"cursor")) {
        POINT at;
        GetCursorPos(&at);
        if (argc >= 4) SetCursorPos(_wtol(argv[2]), _wtol(argv[3]));
        printf("%ld %ld\n", at.x, at.y);
        return 0;
    }

    HWND hwnd = (HWND)(ULONG_PTR)wcstoul(argv[1], NULL, 16);
    if (!IsWindow(hwnd)) { printf("no such window\n"); return 1; }

    report(hwnd, "before");

    if (argc >= 4 && !wcscmp(argv[2], L"thick")) {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (!wcscmp(argv[3], L"on")) style |=  WS_THICKFRAME;
        else                         style &= ~(LONG_PTR)WS_THICKFRAME;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
        Sleep(300);
        report(hwnd, "after");
    }

    if (argc >= 7 && !wcscmp(argv[2], L"place")) {
        RECT want = { _wtol(argv[3]), _wtol(argv[4]), 0, 0 };
        want.right  = want.left + _wtol(argv[5]);
        want.bottom = want.top  + _wtol(argv[6]);

        RECT wr = { 0 }, fr = frame_rect(hwnd);
        GetWindowRect(hwnd, &wr);
        want.left   -= fr.left   - wr.left;
        want.top    -= fr.top    - wr.top;
        want.right  += wr.right  - fr.right;
        want.bottom += wr.bottom - fr.bottom;

        SetWindowPos(hwnd, NULL, want.left, want.top,
                     want.right - want.left, want.bottom - want.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                     SWP_NOCOPYBITS);
        Sleep(400);
        report(hwnd, "after");
    }

    return 0;
}
