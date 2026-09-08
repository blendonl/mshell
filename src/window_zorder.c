#include "mshell.h"
#include "window_internal.h"
#include "overlay.h"

static bool zorder_wants_topmost(const ManagedWindow *mw) {
    return window_is_screen_fullscreen(mw) || mw->always_on_top ||
           (g.cfg.float_on_top && window_is_float_tier(mw));
}

bool window_set_band(HWND hwnd, HWND after, bool topmost) {
    if (SetWindowPos(hwnd, after, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        return true;

    DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        log_w(L"SetWindowPos(%p, z-order) failed: %lu", (void *)hwnd, err);
        return false;
    }
    return helper_set_topmost(hwnd, topmost);
}

static void zorder_mark_promoted(ManagedWindow *mw) {
    if (!mw->made_topmost &&
        !(GetWindowLongPtrW(mw->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        mw->made_topmost = true;
}

static void zorder_raise_over_floats(void) {
    for (int m = 0; m < g.monitor_count; m++) {
        Desktop *dt = desktop_by_id(desktop_on_monitor(m));
        if (!dt) continue;

        for (int i = 0; i < dt->count; i++) {
            ManagedWindow *mw = window_find(dt->windows[i]);
            if (!mw || !IsWindow(mw->hwnd)) continue;
            if (!window_is_screen_fullscreen(mw) && !mw->always_on_top) continue;
            if (!window_on_screen(mw)) continue;

            zorder_mark_promoted(mw);
            window_set_band(mw->hwnd, HWND_TOPMOST, true);
        }
    }
}

void window_raise_floats(void) {
    if (!g.cfg.float_on_top) { zorder_raise_over_floats(); return; }

    HWND floats[MAX_WINDOWS_PER_DESKTOP];
    int  n = 0;

    int wanted = 0;
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!window_is_float_tier(mw)) continue;
        if (!desktop_is_visible(mw->desktop_id)) continue;
        if (!window_on_screen(mw) || IsIconic(mw->hwnd)) continue;
        wanted++;
    }
    if (wanted == 0) { zorder_raise_over_floats(); return; }
    if (wanted > MAX_WINDOWS_PER_DESKTOP) wanted = MAX_WINDOWS_PER_DESKTOP;

    int steps = 0;
    for (HWND h = GetTopWindow(NULL);
         h && n < wanted && steps < ZORDER_WALK_MAX;
         h = GetWindow(h, GW_HWNDNEXT), steps++) {
        ManagedWindow *mw = window_find(h);
        if (!window_is_float_tier(mw)) continue;
        if (!desktop_is_visible(mw->desktop_id)) continue;
        if (!window_on_screen(mw) || IsIconic(h)) continue;
        floats[n++] = h;
    }
    if (n == 0) { zorder_raise_over_floats(); return; }

    HWND focused = desktop_get_focused();
    for (int i = 1; i < n; i++) {
        if (floats[i] != focused) continue;
        memmove(&floats[1], &floats[0], (size_t)i * sizeof floats[0]);
        floats[0] = focused;
        break;
    }

    HWND after = HWND_TOPMOST;
    for (int i = 0; i < n; i++) {
        ManagedWindow *mw = window_find(floats[i]);
        if (mw) zorder_mark_promoted(mw);
        if (!window_set_band(floats[i], after, true)) continue;
        after = floats[i];
    }

    overlay_raise_all();
    zorder_raise_over_floats();
}

int window_sunk_count(void) {
    int n = 0;
    for (int i = 0; i < g.managed_count; i++)
        if (g.managed[i].sunk && IsWindow(g.managed[i].hwnd)) n++;
    return n;
}

void window_resink(void) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg)) return;

    int sunk = window_sunk_count();

    if (sunk == 0) {
        SetWindowPos(bg, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (!window_sink_intact()) {
        for (int i = 0; i < g.managed_count; i++) {
            ManagedWindow *mw = &g.managed[i];
            if (!mw->sunk || !IsWindow(mw->hwnd)) continue;
            SetWindowPos(mw->hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    HWND top      = GetTopWindow(NULL);
    HWND top_sunk = NULL;
    int  steps    = 0;
    for (HWND h = top ? GetWindow(top, GW_HWNDLAST) : NULL;
         h && steps < SINK_WALK_MAX;
         h = GetWindow(h, GW_HWNDPREV), steps++) {
        if (h == bg) continue;
        ManagedWindow *mw = window_find(h);
        if (mw && mw->sunk) { top_sunk = h; continue; }
        break;
    }
    if (!top_sunk) return;

    HWND anchor = GetWindow(top_sunk, GW_HWNDPREV);
    if (anchor && anchor != bg)
        SetWindowPos(bg, anchor, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool window_sink_intact(void) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg)) return true;

    int sunk = window_sunk_count();
    if (sunk == 0) return true;

    HWND top = GetTopWindow(NULL);
    if (!top) return true;

    int seen = 0, steps = 0;
    for (HWND h = GetWindow(top, GW_HWNDLAST);
         h && steps < SINK_WALK_MAX;
         h = GetWindow(h, GW_HWNDPREV), steps++) {
        if (h == bg) return seen >= sunk;
        ManagedWindow *mw = window_find(h);
        if (mw && mw->sunk) seen++;
    }

    return true;
}

void window_enforce_zorder(void) {
    window_resink();

    window_raise_floats();

    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!mw->made_topmost || !IsWindow(mw->hwnd)) continue;
        if (zorder_wants_topmost(mw) && window_on_screen(mw)) continue;

        mw->made_topmost = false;
        window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
    }
}

static void claim_foreground_rights(void) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type           = INPUT_KEYBOARD;
    in.ki.wVk         = 0;
    in.ki.wScan       = 0;
    in.ki.dwExtraInfo = MSHELL_INPUT_TAG;
    SendInput(1, &in, sizeof(in));
}

void window_focus(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    claim_foreground_rights();
    SetForegroundWindow(hwnd);

    if (GetForegroundWindow() != hwnd) {
        spi_set_broadcast(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)(UINT_PTR)0);
        claim_foreground_rights();
        SetForegroundWindow(hwnd);

        if (GetForegroundWindow() != hwnd)
            SwitchToThisWindow(hwnd, TRUE);
    }

    SetFocus(hwnd);

    {
        ManagedWindow *mw = window_find(hwnd);
        int mon = mw ? desktop_monitor_of_window(mw) : monitor_of_window(hwnd);
        if (mon >= 0 && mon < g.monitor_count) {
            bool crossed = (mon != g.focused_monitor);
            g.focused_monitor = mon;

            if (crossed) desktop_sync_current();
        }

        if (mw) mw->urgent = false;
    }

    window_raise_floats();

    border_refresh();
    bar_refresh();

    {
        static HWND s_last_fired = NULL;
        if (hwnd != s_last_fired) {
            s_last_fired = hwnd;
            lua_fire(LUA_EVENT_FOCUS, hwnd, NULL);
        }
    }

    HWND fg = GetForegroundWindow();
    if (fg == hwnd) {
        log_w(L"focus -> %p ok", (void *)hwnd);
    } else {
        wchar_t cls[128] = {0};
        if (fg) GetClassNameW(fg, cls, 128);
        log_w(L"focus -> %p FAILED — foreground is still %p [%ls]",
              (void *)hwnd, (void *)fg, cls);
    }
}

void window_focus_none(void) {
    border_hide();

    HWND sink = g.background_window;
    if (!sink) return;

    HWND fg = GetForegroundWindow();
    if (fg == sink) return;
    if (fg && !window_find(fg)) return;

    LONG_PTR ex = GetWindowLongPtrW(sink, GWL_EXSTYLE);
    SetWindowLongPtrW(sink, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_NOACTIVATE);

    claim_foreground_rights();
    SetForegroundWindow(sink);

    SetWindowLongPtrW(sink, GWL_EXSTYLE, ex);
}
