#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>

static void exe_of(HWND h, wchar_t *out, size_t cap) {
    out[0] = 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) { _snwprintf(out, cap, L"pid%lu", (unsigned long)pid); return; }
    wchar_t path[MAX_PATH] = {0};
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t *b = wcsrchr(path, L'\\');
        _snwprintf(out, cap, L"%ls", b ? b + 1 : path);
    } else {
        _snwprintf(out, cap, L"pid%lu", (unsigned long)pid);
    }
    CloseHandle(p);
}

int wmain(int argc, wchar_t **argv) {
    int all = (argc > 1 && !wcscmp(argv[1], L"-a"));
    _setmode(_fileno(stdout), 0x00020000);

    int i = 0, bg = -1;
    wprintf(L"idx  hwnd             vis cloak icon top tool noact  %-24ls %-22ls %-30ls rect\n",
            L"class", L"exe", L"title");
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT), i++) {
        wchar_t cls[64] = {0}, title[64] = {0}, exe[64] = {0};
        GetClassNameW(h, cls, 63);
        GetWindowTextW(h, title, 63);
        RECT r = {0};
        GetWindowRect(h, &r);
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        int vis = IsWindowVisible(h) != 0;
        int cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        int w = r.right - r.left, ht = r.bottom - r.top;

        if (!wcscmp(cls, L"mshell_Background")) bg = i;
        if (!all) {
            if (!vis) continue;
            if (w < 120 || ht < 120) continue;
        }
        exe_of(h, exe, 63);
        wprintf(L"%-4d %p %3d %5d %4d %3d %4d %5d  %-24.24ls %-22.22ls %-30.30ls %ld,%ld %dx%d\n",
                i, (void *)h, vis, cloaked, IsIconic(h) != 0,
                (ex & WS_EX_TOPMOST) != 0, (ex & WS_EX_TOOLWINDOW) != 0,
                (ex & WS_EX_NOACTIVATE) != 0,
                cls, exe, title, r.left, r.top, w, ht);
    }
    wprintf(L"\ntotal %d top-level windows; backdrop at z-index %d\n", i, bg);
    return 0;
}
