#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>

static const wchar_t *ignore_classes[] = {
    L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
    L"TrayNotifyWnd", L"NotifyIconOverflowWindow", L"Windows.UI.Core.CoreWindow",
    L"ForegroundStaging", L"MultitaskingViewFrame", L"XamlExplorerHostIslandWindow",
    L"mshell_Background", L"mshell_FocusBorder", L"mshell_MessageWindow",
    L"mshell_Bar", L"mshell_WhichKey", L"mshell_Notify", L"mshell_Launcher",
    L"mrun_Window", L"mshell_Dim", NULL };

static const wchar_t *verdict(HWND h, int min_w, int min_h) {
    if (!IsWindowVisible(h))                  return L"NO   invisible";
    if (!IsWindowEnabled(h))                  return L"TRACK disabled";
    if (GetAncestor(h, GA_ROOT) != h)         return L"NO   not-root";
    if (GetWindow(h, GW_OWNER))               return L"TRACK owned";
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)                return L"NO   toolwindow";
    if (st & WS_CHILD)                        return L"NO   child";
    wchar_t cls[128] = {0};
    GetClassNameW(h, cls, 127);
    for (const wchar_t **p = ignore_classes; *p; p++)
        if (!_wcsicmp(cls, *p))               return L"NO   ignore-class";
    if (!IsIconic(h)) {
        RECT r; if (!GetWindowRect(h, &r))    return L"TRACK no-rect";
        if (r.right - r.left < min_w || r.bottom - r.top < min_h)
            return L"TRACK too-small";
    }
    int cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked)                              return L"NO   cloaked";
    if ((st & WS_POPUP) && !(st & WS_CAPTION) && !(st & WS_SIZEBOX))
        return L"NO   popup-no-caption(rules may rescue)";
    return L"FULL";
}

static void exe_of(HWND h, wchar_t *out, size_t cap) {
    DWORD pid = 0; out[0] = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) { _snwprintf(out, cap, L"pid%lu", (unsigned long)pid); return; }
    wchar_t path[MAX_PATH] = {0}; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t *b = wcsrchr(path, L'\\');
        _snwprintf(out, cap, L"%ls", b ? b + 1 : path);
    }
    CloseHandle(p);
}

int wmain(void) {
    int i = 0, adoptable = 0;
    wprintf(L"idx  hwnd             %-20ls %-30ls %-22ls verdict\n", L"exe", L"title", L"class");
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT), i++) {
        if (!IsWindowVisible(h)) continue;
        RECT r = {0}; GetWindowRect(h, &r);
        if (r.right - r.left < 120 || r.bottom - r.top < 120) continue;
        wchar_t cls[128] = {0}, title[80] = {0}, exe[64] = {0};
        GetClassNameW(h, cls, 127); GetWindowTextW(h, title, 79); exe_of(h, exe, 63);
        const wchar_t *v = verdict(h, 100, 100);
        if (!wcsncmp(v, L"FULL", 4) || !wcsncmp(v, L"TRACK", 5)) adoptable++;
        wprintf(L"%-4d %p %-20.20ls %-30.30ls %-22.22ls %ls\n",
                i, (void *)h, exe, title, cls, v);
    }
    wprintf(L"\n%d adoptable windows on screen\n", adoptable);
    return 0;
}
