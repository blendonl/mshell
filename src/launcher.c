#include "mshell.h"
#include "overlay.h"

#include <shlobj.h>

static const wchar_t *LAUNCHER_CLASS = L"mshell_Launcher";

#define LAUNCH_MAX_ENTRIES 512
#define LAUNCH_MAX_SHOWN   9
#define LAUNCH_TIMER_ID    1
#define LAUNCH_SANITY_MS   250

#define L_PAD    14
#define L_ROW    28
#define L_WIDTH  520

typedef struct {
    wchar_t name[128];
    wchar_t target[MAX_PATH];
} Entry;

static Entry       s_index[LAUNCH_MAX_ENTRIES];
static int         s_index_n;
static bool        s_indexed;

static wchar_t     s_query[128];
static int         s_query_n;
static int         s_hits[LAUNCH_MAX_ENTRIES];
static int         s_hit_n;
static int         s_sel;
static OverlayFont s_font;

static void index_dir(const wchar_t *dir, int depth) {
    if (depth > 3 || s_index_n >= LAUNCH_MAX_ENTRIES) return;

    wchar_t pat[MAX_PATH];
    if (_snwprintf(pat, MAX_PATH, L"%ls\\*", dir) <= 0) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.cFileName[0] == L'.') continue;

        wchar_t full[MAX_PATH];
        if (_snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) <= 0)
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            index_dir(full, depth + 1);
            continue;
        }

        const wchar_t *ext = wcsrchr(fd.cFileName, L'.');
        if (!ext || _wcsicmp(ext, L".lnk") != 0) continue;
        if (s_index_n >= LAUNCH_MAX_ENTRIES) break;

        Entry *e = &s_index[s_index_n];
        wcsncpy(e->target, full, MAX_PATH - 1);
        e->target[MAX_PATH - 1] = L'\0';

        wcsncpy(e->name, fd.cFileName, 127);
        e->name[127] = L'\0';
        wchar_t *dot = wcsrchr(e->name, L'.');
        if (dot) *dot = L'\0';

        bool dup = false;
        for (int i = 0; i < s_index_n; i++)
            if (_wcsicmp(s_index[i].name, e->name) == 0) { dup = true; break; }
        if (!dup) s_index_n++;
    } while (FindNextFileW(h, &fd) && s_index_n < LAUNCH_MAX_ENTRIES);

    FindClose(h);
}

static void index_known(REFKNOWNFOLDERID id) {
    PWSTR p = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p))) {
        index_dir(p, 0);
        CoTaskMemFree(p);
    }
}

static void launcher_build_index(void) {
    if (s_indexed) return;
    s_index_n = 0;
    index_known(&FOLDERID_CommonPrograms);
    index_known(&FOLDERID_Programs);
    s_indexed = true;
    log_msg(LOG_INFO, L"launcher: indexed %d entries", s_index_n);
}

static bool subseq(const wchar_t *needle, const wchar_t *hay) {
    if (!*needle) return true;
    for (; *hay; hay++) {
        if (towlower(*hay) == towlower(*needle)) {
            needle++;
            if (!*needle) return true;
        }
    }
    return false;
}

static void launcher_filter(void) {
    s_hit_n = 0;
    s_sel   = 0;

    for (int pass = 0; pass < 2 && s_hit_n < LAUNCH_MAX_ENTRIES; pass++) {
        for (int i = 0; i < s_index_n && s_hit_n < LAUNCH_MAX_ENTRIES; i++) {
            bool prefix = (s_query_n > 0) &&
                          (_wcsnicmp(s_index[i].name, s_query, s_query_n) == 0);
            if (pass == 0 && !prefix) continue;
            if (pass == 1 && prefix)  continue;
            if (!subseq(s_query, s_index[i].name)) continue;
            s_hits[s_hit_n++] = i;
        }
    }
}

static void launcher_relayout(void) {
    if (!g.launcher_window) return;

    int mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
             ? g.focused_monitor : g.primary_monitor;
    UINT dpi = monitor_dpi(mi);

    overlay_font(&s_font, dpi, overlay_scale(17, dpi));

    int pad = overlay_scale(L_PAD,   dpi);
    int row = overlay_scale(L_ROW,   dpi);
    int w   = overlay_scale(L_WIDTH, dpi);

    int shown = s_hit_n < LAUNCH_MAX_SHOWN ? s_hit_n : LAUNCH_MAX_SHOWN;
    int h     = pad * 2 + row + shown * row;

    RECT mon = (mi >= 0 && mi < g.monitor_count) ? g.monitors[mi].work_area
                                                 : g.work_area;
    int x = mon.left + ((mon.right - mon.left) - w) / 2;
    int y = mon.top  + (mon.bottom - mon.top) / 5;

    SetWindowPos(g.launcher_window, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.launcher_window, NULL, TRUE);
}

static LRESULT CALLBACK launcher_wndproc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == LAUNCH_TIMER_ID) {
            if (!g.launcher_open) { KillTimer(hwnd, LAUNCH_TIMER_ID); }
            else if (!IsWindowVisible(hwnd)) launcher_close();
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        OverlayPaint op;
        HDC mdc = overlay_paint_begin(&op, hwnd);
        if (!mdc) return 0;

        int mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
                 ? g.focused_monitor : g.primary_monitor;
        UINT dpi = monitor_dpi(mi);
        int pad = overlay_scale(L_PAD, dpi);
        int row = overlay_scale(L_ROW, dpi);

        RECT all = { 0, 0, op.w, op.h };
        overlay_fill(mdc, &all, g.whichkey_bg);

        HPEN pen = CreatePen(PS_SOLID, 1, g.whichkey_border);
        HPEN opn = (HPEN)SelectObject(mdc, pen);
        HBRUSH obr = (HBRUSH)SelectObject(mdc, GetStockObject(NULL_BRUSH));
        Rectangle(mdc, 0, 0, op.w, op.h);
        SelectObject(mdc, obr);
        SelectObject(mdc, opn);
        DeleteObject(pen);

        HFONT of = (HFONT)SelectObject(mdc, s_font.font);
        SetBkMode(mdc, TRANSPARENT);

        wchar_t line[160];
        _snwprintf(line, 159, L"> %ls_", s_query);
        line[159] = L'\0';
        SetTextColor(mdc, g.whichkey_key_fg);
        TextOutW(mdc, pad, pad, line, (int)wcslen(line));

        int shown = s_hit_n < LAUNCH_MAX_SHOWN ? s_hit_n : LAUNCH_MAX_SHOWN;
        for (int i = 0; i < shown; i++) {
            int  y   = pad + row + i * row;
            bool sel = (i == s_sel);

            if (sel) {
                RECT hl = { pad / 2, y, op.w - pad / 2, y + row };
                overlay_fill(mdc, &hl, g.whichkey_key_fg);
            }
            SetTextColor(mdc, sel ? g.whichkey_bg : g.whichkey_fg);
            const wchar_t *nm = s_index[s_hits[i]].name;
            TextOutW(mdc, pad, y + 2, nm, (int)wcslen(nm));
        }

        if (s_hit_n == 0 && s_query_n > 0) {
            SetTextColor(mdc, g.whichkey_fg);
            const wchar_t *none = L"(no match)";
            TextOutW(mdc, pad, pad + row, none, (int)wcslen(none));
        }

        SelectObject(mdc, of);
        overlay_paint_end(&op);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void launcher_open(void) {
    if (!g.launcher_window || g.launcher_open) return;

    launcher_build_index();
    s_query[0] = L'\0';
    s_query_n  = 0;
    launcher_filter();

    g.launcher_open = true;
    SetTimer(g.launcher_window, LAUNCH_TIMER_ID, LAUNCH_SANITY_MS, NULL);
    launcher_relayout();
}

void launcher_close(void) {
    if (!g.launcher_window) return;
    g.launcher_open = false;
    KillTimer(g.launcher_window, LAUNCH_TIMER_ID);
    ShowWindow(g.launcher_window, SW_HIDE);
}

static void launcher_run_selected(void) {
    if (s_sel >= 0 && s_sel < s_hit_n) {
        const Entry *e = &s_index[s_hits[s_sel]];
        spawn_command(e->target, NULL, NULL, L"launcher");
    } else if (s_query_n > 0) {
        spawn_command(s_query, NULL, NULL, L"launcher");
    }
    launcher_close();
}

void launcher_key(DWORD vk, wchar_t ch) {
    if (!g.launcher_open) return;

    switch (vk) {
    case VK_ESCAPE:  launcher_close();        return;
    case VK_RETURN:  launcher_run_selected(); return;

    case VK_UP:
        if (s_sel > 0) s_sel--;
        launcher_relayout();
        return;

    case VK_DOWN: {
        int shown = s_hit_n < LAUNCH_MAX_SHOWN ? s_hit_n : LAUNCH_MAX_SHOWN;
        if (s_sel + 1 < shown) s_sel++;
        launcher_relayout();
        return;
    }

    case VK_BACK:
        if (s_query_n > 0) s_query[--s_query_n] = L'\0';
        launcher_filter();
        launcher_relayout();
        return;
    }

    if (ch >= L' ' && s_query_n < 127) {
        s_query[s_query_n++] = ch;
        s_query[s_query_n]   = L'\0';
        launcher_filter();
        launcher_relayout();
    }
}

static bool mrun_beside_us(wchar_t *out, size_t cap) {
    wchar_t dir[MAX_PATH];
    DWORD   n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return false;
    *slash = L'\0';

    int w = _snwprintf(out, cap, L"%ls\\mrun.exe", dir);
    if (w <= 0 || (size_t)w >= cap) return false;

    DWORD attr = GetFileAttributesW(out);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool mrun_on_path(wchar_t *out, size_t cap) {
    DWORD n = SearchPathW(NULL, L"mrun.exe", NULL, (DWORD)cap, out, NULL);
    return n > 0 && n < cap;
}

bool launcher_spawn_mrun(void) {
    wchar_t path[MAX_PATH];

    if (!mrun_beside_us(path, MAX_PATH) && !mrun_on_path(path, MAX_PATH))
        return false;

    return spawn_command(path, NULL, NULL, L"launcher");
}

bool launcher_init(void) {
    if (!overlay_register(LAUNCHER_CLASS, launcher_wndproc, false)) return false;

    g.launcher_window = overlay_create(
        LAUNCHER_CLASS,
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!g.launcher_window) return false;

    SetLayeredWindowAttributes(g.launcher_window, 0, 245, LWA_ALPHA);
    return true;
}

void launcher_shutdown(void) {
    if (g.launcher_window) KillTimer(g.launcher_window, LAUNCH_TIMER_ID);
    g.launcher_open = false;
    overlay_destroy(&g.launcher_window, LAUNCHER_CLASS);
    overlay_font_free(&s_font);
}
