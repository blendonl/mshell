#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>

static void exe_of(HWND h, char *out, size_t cap) {
    DWORD pid = 0; out[0] = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) { snprintf(out, cap, "pid%lu", (unsigned long)pid); return; }
    wchar_t path[MAX_PATH] = {0}; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t *b = wcsrchr(path, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : path, -1, out, (int)cap, NULL, NULL);
    }
    CloseHandle(p);
}

int main(void) {
    int i = 0;
    printf("%-4s %-16s %-10s %-10s %-6s %-22s %-20s %s\n",
           "idx", "hwnd", "style", "exstyle", "flags", "class", "exe", "title");
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT), i++) {
        if (!IsWindowVisible(h)) continue;
        RECT r = {0}; GetWindowRect(h, &r);
        if (r.right - r.left < 120 || r.bottom - r.top < 120) continue;

        wchar_t wcls[128] = {0}, wtitle[80] = {0};
        char cls[256] = {0}, title[160] = {0}, exe[64] = {0};
        GetClassNameW(h, wcls, 127); GetWindowTextW(h, wtitle, 79);
        WideCharToMultiByte(CP_UTF8, 0, wcls, -1, cls, sizeof cls, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, title, sizeof title, NULL, NULL);
        exe_of(h, exe, sizeof exe);

        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        char flags[32] = {0};
        snprintf(flags, sizeof flags, "%s%s%s%s%s",
                 (st & WS_CAPTION) == WS_CAPTION ? "C" : "-",
                 (st & WS_THICKFRAME) ? "T" : "-",
                 (st & WS_SYSMENU) ? "S" : "-",
                 (st & WS_POPUP) ? "P" : "-",
                 (st & WS_OVERLAPPEDWINDOW) ? "O" : "-");
        printf("%-4d %-16p 0x%08lx 0x%08lx %-6s %-22.22s %-20.20s %.40s\n",
               i, (void *)h, (unsigned long)st, (unsigned long)ex, flags, cls, exe, title);
    }
    return 0;
}
