#include "mshell.h"
#include "window_internal.h"

bool window_set_monitor(ManagedWindow *mw, int mon) {
    if (!mw) return false;
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;
    if (mon < 0 || mon >= g.monitor_count) return false;

    bool changed = (mw->monitor != mon);
    mw->monitor = mon;
    if (changed) mw->has_applied = false;

    wcsncpy(mw->monitor_device, g.monitors[mon].device, CCHDEVICENAME - 1);
    mw->monitor_device[CCHDEVICENAME - 1] = L'\0';
    return changed;
}

ManagedWindow *window_find(HWND hwnd) {
    int idx = window_index_of(hwnd);
    return (idx >= 0) ? &g.managed[idx] : NULL;
}

void window_reassert_rule(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    if (mw->no_decor) window_strip_decorations(hwnd);

    if (!mw->fullscreen || !mw->is_floating) return;

    if (mw->has_applied) {
        RECT cur, a = mw->applied_rect;
        if (window_frame_rect(hwnd, &cur)) {
            const int EPS = 4;
            if (abs((int)(cur.left - a.left)) <= EPS &&
                abs((int)(cur.top  - a.top))  <= EPS &&
                abs((int)((cur.right - cur.left) - (a.right - a.left))) <= EPS &&
                abs((int)((cur.bottom - cur.top) - (a.bottom - a.top))) <= EPS)
                return;
        }
    }

    window_apply_fullscreen(hwnd);
}

void window_set_floating(HWND hwnd, bool floating) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    mw->is_floating = floating;
    mw->has_applied = false;

    mw->place_refused = false;

    if (mw->layout_hidden) {
        mw->layout_hidden = false;
        if (floating && desktop_is_visible(mw->desktop_id)) {
            events_suppress_begin();
            window_show(mw);
            events_suppress_end();
        }
    }
    if (floating) {
        if (mw->no_decor) window_strip_decorations(hwnd);
        else              window_restore_decorations(hwnd);
        window_place_float(mw);
    } else {
        window_strip_decorations(hwnd);

        fs_forget_prev(mw);

        if (mw->made_topmost && !window_is_screen_fullscreen(mw) &&
            !mw->always_on_top) {
            mw->made_topmost = false;
            window_set_band(hwnd, HWND_NOTOPMOST, false);
        }
    }
}

void window_close(HWND hwnd) {
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) return;

    if (GetLastError() == ERROR_ACCESS_DENIED) helper_close_window(hwnd);
    else log_w(L"close: PostMessage(WM_CLOSE) on %p failed: %lu",
               (void *)hwnd, GetLastError());
}

void window_kill(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;

    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hp) return;

    TerminateProcess(hp, 1);
    CloseHandle(hp);
}
