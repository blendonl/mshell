#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

#define MAX_TARGETS 16
#define MAX_RUNS     6
#define OUTSIDE_PAD 12

typedef struct {
    HWND  hwnd;
    char  exe[64];
    char  line[512];
} Target;

static UINT    (WINAPI *p_GetDpiForWindow)(HWND);
static HRESULT (WINAPI *p_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
static BOOL    (WINAPI *p_SetProcessDpiAwarenessContext)(HANDLE);

static void dpi_api_init(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        p_GetDpiForWindow = (UINT (WINAPI *)(HWND))
            (void *)GetProcAddress(user32, "GetDpiForWindow");
        p_SetProcessDpiAwarenessContext = (BOOL (WINAPI *)(HANDLE))
            (void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore)
        p_GetDpiForMonitor = (HRESULT (WINAPI *)(HMONITOR, int, UINT *, UINT *))
            (void *)GetProcAddress(shcore, "GetDpiForMonitor");

    if (p_SetProcessDpiAwarenessContext)
        p_SetProcessDpiAwarenessContext((HANDLE)-4);
}

static UINT dpi_of_window(HWND hwnd) {
    return p_GetDpiForWindow ? p_GetDpiForWindow(hwnd) : 96;
}

static UINT dpi_of_monitor(HMONITOR mon) {
    UINT x = 96, y = 96;
    if (p_GetDpiForMonitor && SUCCEEDED(p_GetDpiForMonitor(mon, 0, &x, &y)))
        return x;
    return 96;
}

static int metric_for_dpi(int index, UINT dpi) {
    static int (WINAPI *p_GetSystemMetricsForDpi)(int, UINT);
    static int looked_up;
    if (!looked_up) {
        looked_up = 1;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32)
            p_GetSystemMetricsForDpi = (int (WINAPI *)(int, UINT))
                (void *)GetProcAddress(user32, "GetSystemMetricsForDpi");
    }
    return p_GetSystemMetricsForDpi ? p_GetSystemMetricsForDpi(index, dpi)
                                    : GetSystemMetrics(index);
}

static void exe_of(HWND hwnd, char *out, size_t cap) {
    DWORD pid = 0;
    out[0] = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { snprintf(out, cap, "pid%lu", (unsigned long)pid); return; }

    wchar_t path[MAX_PATH] = { 0 };
    DWORD   n = MAX_PATH;
    if (QueryFullProcessImageNameW(proc, 0, path, &n)) {
        const wchar_t *base = wcsrchr(path, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : path, -1,
                            out, (int)cap, NULL, NULL);
    }
    CloseHandle(proc);
}

static int contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               (char)tolower((unsigned char)p[i]) ==
               (char)tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

typedef struct {
    Target *list;
    int     count;
    const char *match;
} Collect;

static BOOL CALLBACK collect_proc(HWND hwnd, LPARAM lp) {
    Collect *c = (Collect *)lp;
    if (c->count >= MAX_TARGETS) return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    RECT r;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right - r.left < 300 || r.bottom - r.top < 200) return TRUE;

    char exe[64];
    exe_of(hwnd, exe, sizeof exe);
    if (!contains_ci(exe, c->match)) return TRUE;

    Target *t = &c->list[c->count++];
    t->hwnd = hwnd;
    memcpy(t->exe, exe, sizeof t->exe);
    t->line[0] = 0;
    return TRUE;
}

static int capture_row(int x, int y, int width, DWORD *out) {
    HDC screen = GetDC(NULL);
    if (!screen) return 0;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -1;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void   *bits = NULL;
    HDC     mem  = CreateCompatibleDC(screen);
    HBITMAP bmp  = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int     ok   = 0;

    if (mem && bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, width, 1, screen, x, y, SRCCOPY | CAPTUREBLT)) {
            memcpy(out, bits, (size_t)width * sizeof(DWORD));
            ok = 1;
        }
        SelectObject(mem, old);
    }

    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok;
}

static int format_runs(const DWORD *px, int count, int step, char *out, size_t cap) {
    size_t used = 0;
    int    i    = 0;
    for (int run = 0; run < MAX_RUNS && i < count; run++) {
        DWORD color = px[step > 0 ? i : count - 1 - i] & 0x00FFFFFF;
        int   len   = 0;
        while (i + len < count) {
            DWORD c = px[step > 0 ? i + len : count - 1 - i - len] & 0x00FFFFFF;
            if (c != color) break;
            len++;
        }
        int n = snprintf(out + used, cap - used, "%s#%06lX x%d",
                         run ? " " : "", (unsigned long)color, len);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
        i    += len;
    }
    return (int)used;
}

static void describe(Target *t, char *out, size_t cap) {
    RECT wr = { 0 }, fr = { 0 }, cr = { 0 };
    GetWindowRect(t->hwnd, &wr);
    if (FAILED(DwmGetWindowAttribute(t->hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &fr, sizeof fr)))
        fr = wr;

    GetClientRect(t->hwnd, &cr);
    POINT origin = { cr.left, cr.top };
    ClientToScreen(t->hwnd, &origin);
    RECT cs = { origin.x, origin.y,
                origin.x + (cr.right - cr.left),
                origin.y + (cr.bottom - cr.top) };

    HMONITOR mon = MonitorFromWindow(t->hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);

    LONG_PTR style = GetWindowLongPtrW(t->hwnd, GWL_STYLE);

    int width = (int)(fr.right - fr.left) + 2 * OUTSIDE_PAD;
    int y     = (int)((fr.top + fr.bottom) / 2);

    char left[160]  = "?";
    char right[160] = "?";

    if (width > 0 && width < 20000) {
        DWORD *px = (DWORD *)malloc((size_t)width * sizeof(DWORD));
        if (px) {
            if (capture_row((int)fr.left - OUTSIDE_PAD, y, width, px)) {
                format_runs(px, width, +1, left, sizeof left);
                format_runs(px, width, -1, right, sizeof right);
            }
            free(px);
        }
    }

    snprintf(out, cap,
             "%-12s %p win=%ldx%ld@%ld,%ld dwm=%ldx%ld@%ld,%ld "
             "cli=%ldx%ld@%ld,%ld dpi=%u/%u mon=%ldx%ld@%ld,%ld "
             "sty=0x%08lX zoom=%d max=%d cap=%d thick=%d bord=%d "
             "sizeframe=%d padded=%d | L: %s | R: %s",
             t->exe, (void *)t->hwnd,
             wr.right - wr.left, wr.bottom - wr.top, wr.left, wr.top,
             fr.right - fr.left, fr.bottom - fr.top, fr.left, fr.top,
             cs.right - cs.left, cs.bottom - cs.top, cs.left, cs.top,
             dpi_of_window(t->hwnd), dpi_of_monitor(mon),
             mi.rcMonitor.right - mi.rcMonitor.left,
             mi.rcMonitor.bottom - mi.rcMonitor.top,
             mi.rcMonitor.left, mi.rcMonitor.top,
             (unsigned long)style,
             IsZoomed(t->hwnd) ? 1 : 0,
             (style & WS_MAXIMIZE) ? 1 : 0,
             (style & WS_CAPTION) == WS_CAPTION ? 1 : 0,
             (style & WS_THICKFRAME) ? 1 : 0,
             (style & WS_BORDER) ? 1 : 0,
             metric_for_dpi(SM_CXSIZEFRAME, dpi_of_window(t->hwnd)),
             metric_for_dpi(SM_CXPADDEDBORDER, dpi_of_window(t->hwnd)),
             left, right);
}

int wmain(int argc, wchar_t **argv) {
    char match[64] = "chrome";
    int  seconds   = 0;

    if (argc > 1) {
        if (!wcscmp(argv[1], L"-h") || !wcscmp(argv[1], L"--help")) {
            printf(
"probe_dpiband [exe-substring] [seconds]\n"
"\n"
"Prints, for every visible window whose exe matches (default \"chrome\"):\n"
"  win  GetWindowRect            dwm  DWMWA_EXTENDED_FRAME_BOUNDS\n"
"  cli  client area, screen px   dpi  window/monitor DPI\n"
"  L:/R: the colour runs across a horizontal line through the middle of the\n"
"        window, starting %d px OUTSIDE the frame and scanning inwards.\n"
"\n"
"The colour runs say who owns a band at the window's edge: the mshell backdrop\n"
"(config's appearance.background) means the window is placed short of its tile;\n"
"a Chromium frame colour means the app is insetting its own content.\n"
"\n"
"With a seconds argument it polls 4x/second and prints a line whenever\n"
"something changes, so a monitor switch can be captured live.\n", OUTSIDE_PAD);
            return 0;
        }
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, match, sizeof match,
                            NULL, NULL);
    }
    if (argc > 2) seconds = _wtoi(argv[2]);

    dpi_api_init();

    Target  targets[MAX_TARGETS];
    Collect c = { targets, 0, match };
    EnumWindows(collect_proc, (LPARAM)&c);

    if (!c.count) {
        printf("no visible window matching \"%s\"\n", match);
        return 1;
    }

    if (seconds <= 0) {
        for (int i = 0; i < c.count; i++) {
            char line[512];
            describe(&targets[i], line, sizeof line);
            printf("%s\n", line);
        }
        return 0;
    }

    printf("watching %d window(s) for %ds — switch the monitor now\n",
           c.count, seconds);

    ULONGLONG start = GetTickCount64();
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if ((int)((now - start) / 1000) >= seconds) break;

        for (int i = 0; i < c.count; i++) {
            if (!IsWindow(targets[i].hwnd)) continue;
            char line[512];
            describe(&targets[i], line, sizeof line);
            if (strcmp(line, targets[i].line) == 0) continue;
            memcpy(targets[i].line, line, sizeof targets[i].line);
            printf("%6.1fs %s\n", (double)(now - start) / 1000.0, line);
            fflush(stdout);
        }
        Sleep(250);
    }
    return 0;
}
