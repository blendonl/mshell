#include "mshell.h"

static POINT s_last_pointer;

void mouse_warp_focus(void) {
    if (!g.mouse_warp) return;
    if (g.drag_hwnd || g.mod_drag_hwnd) return;

    int mon = g.focused_monitor;
    if (mon < 0 || mon >= g.monitor_count) return;

    POINT p;
    if (!GetCursorPos(&p)) return;

    RECT mr = g.monitors[mon].full;
    if (PtInRect(&mr, p)) return;

    RECT target = g.monitors[mon].work_area;
    HWND focus  = desktop_get_focused();
    if (focus && IsWindow(focus)) {
        ManagedWindow *mw = window_find(focus);
        RECT wr;
        if (mw && window_on_screen(mw) && GetWindowRect(focus, &wr))
            target = wr;
    }

    POINT c = { (target.left + target.right) / 2,
                (target.top + target.bottom) / 2 };

    if (!PtInRect(&mr, c)) {
        c.x = (mr.left + mr.right) / 2;
        c.y = (mr.top + mr.bottom) / 2;
    }

    if (SetCursorPos(c.x, c.y)) s_last_pointer = c;
}

void mouse_poll_focus(void) {
    if (!g.mouse_follow) return;

    POINT p;
    if (!GetCursorPos(&p)) return;

    if (p.x == s_last_pointer.x && p.y == s_last_pointer.y) return;
    s_last_pointer = p;

    if (g.drag_hwnd || g.mod_drag_hwnd) return;

    HWND under = WindowFromPoint(p);
    if (!under) return;

    HWND top = GetAncestor(under, GA_ROOT);
    if (!top || top == GetForegroundWindow()) return;

    ManagedWindow *mw = window_find(top);
    if (!mw || !desktop_is_visible(mw->desktop_id)) return;

    desktop_focus_update(top);
    window_focus(top);
}

static POINT s_grab;
static RECT  s_grab_rect;
static bool  s_resizing;

void mouse_mod_drag_apply(int dx, int dy) {
    ManagedWindow *mw = window_find(g.mod_drag_hwnd);
    if (!mw || !mw->is_floating) return;

    RECT want = s_grab_rect;
    if (s_resizing) {
        want.right  += dx;
        want.bottom += dy;
        if (want.right - want.left < g.min_win_w)
            want.right = want.left + g.min_win_w;
        if (want.bottom - want.top < g.min_win_h)
            want.bottom = want.top + g.min_win_h;
    } else {
        want.left += dx; want.right  += dx;
        want.top  += dy; want.bottom += dy;
    }

    window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
    border_refresh();
}

bool mouse_mod_drag_event(WPARAM msg, POINT pt, bool mod_held) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        if (!mod_held) return false;

        HWND top = GetAncestor(WindowFromPoint(pt), GA_ROOT);
        if (!top) return false;

        ManagedWindow *mw = window_find(top);
        if (!mw || !mw->is_floating) return false;
        if (!window_frame_rect(top, &s_grab_rect)) return false;

        s_grab           = pt;
        s_resizing       = (msg == WM_RBUTTONDOWN);
        g.mod_drag_hwnd  = top;
        return true;
    }

    case WM_MOUSEMOVE:
        if (!g.mod_drag_hwnd) return false;
        PostMessageW(g.message_window, WM_MSHELL_MOUSE,
                     (WPARAM)(pt.x - s_grab.x), (LPARAM)(pt.y - s_grab.y));
        return true;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (!g.mod_drag_hwnd) return false;
        g.mod_drag_hwnd = NULL;
        return true;
    }
    return false;
}

static const int PTR_ACCEL_ON[3]  = { 6, 10, 1 };
static const int PTR_ACCEL_OFF[3] = { 0,  0, 0 };

static bool s_ptr_saved;
static int  s_ptr_prev_speed;
static int  s_ptr_prev_accel[3];
static bool s_ptr_prev_swap;

static bool s_ptr_own_speed, s_ptr_own_accel, s_ptr_own_swap;

static void pointer_snapshot(void) {
    if (s_ptr_saved) return;
    s_ptr_saved = true;

    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &s_ptr_prev_speed, 0))
        s_ptr_prev_speed = 10;
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, s_ptr_prev_accel, 0))
        memcpy(s_ptr_prev_accel, PTR_ACCEL_ON, sizeof s_ptr_prev_accel);

    s_ptr_prev_swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
}

static void pointer_set_speed(int speed) {
    if (!spi_set_broadcast(SPI_SETMOUSESPEED, 0, (PVOID)(UINT_PTR)speed))
        log_err(L"mouse: SPI_SETMOUSESPEED(%d) failed: %lu",
                speed, GetLastError());
}

static void pointer_set_accel(const int v[3]) {
    int tmp[3] = { v[0], v[1], v[2] };
    if (!spi_set_broadcast(SPI_SETMOUSE, 0, tmp))
        log_err(L"mouse: SPI_SETMOUSE failed: %lu", GetLastError());
}

static void pointer_set_swap(bool swapped) {
    if (!spi_set_broadcast(SPI_SETMOUSEBUTTONSWAP, swapped ? 1 : 0, NULL))
        log_err(L"mouse: SPI_SETMOUSEBUTTONSWAP(%d) failed: %lu",
                (int)swapped, GetLastError());
}

void mouse_sync_pointer(void) {
    bool want_speed = g.mouse_speed > 0;
    bool want_accel = g.mouse_accel >= 0;
    bool want_swap  = g.mouse_swap  >= 0;

    if (!want_speed && !want_accel && !want_swap &&
        !s_ptr_own_speed && !s_ptr_own_accel && !s_ptr_own_swap)
        return;

    pointer_snapshot();

    if (want_speed) {
        pointer_set_speed(g.mouse_speed);
        s_ptr_own_speed = true;
    } else if (s_ptr_own_speed) {
        pointer_set_speed(s_ptr_prev_speed);
        s_ptr_own_speed = false;
    }

    if (want_accel) {
        pointer_set_accel(g.mouse_accel ? PTR_ACCEL_ON : PTR_ACCEL_OFF);
        s_ptr_own_accel = true;
    } else if (s_ptr_own_accel) {
        pointer_set_accel(s_ptr_prev_accel);
        s_ptr_own_accel = false;
    }

    if (want_swap) {
        pointer_set_swap(g.mouse_swap != 0);
        s_ptr_own_swap = true;
    } else if (s_ptr_own_swap) {
        pointer_set_swap(s_ptr_prev_swap);
        s_ptr_own_swap = false;
    }
}

void mouse_restore_pointer(void) {
    if (!s_ptr_saved) return;

    if (s_ptr_own_speed) {
        pointer_set_speed(s_ptr_prev_speed);
        s_ptr_own_speed = false;
    }
    if (s_ptr_own_accel) {
        pointer_set_accel(s_ptr_prev_accel);
        s_ptr_own_accel = false;
    }
    if (s_ptr_own_swap) {
        pointer_set_swap(s_ptr_prev_swap);
        s_ptr_own_swap = false;
    }
}

void mouse_drag_begin(HWND hwnd) {
    if (!g.mouse_enabled) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw || mw->is_floating) return;

    g.drag_hwnd = hwnd;
    GetCursorPos(&g.drag_start);
}

void mouse_drag_end(HWND hwnd) {
    if (!g.mouse_enabled || g.drag_hwnd != hwnd) { g.drag_hwnd = NULL; return; }
    g.drag_hwnd = NULL;

    POINT drop;
    if (!GetCursorPos(&drop)) { tile_current(); return; }

    Desktop *dt = desktop_current();

    int from = -1, to = -1;
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw || mw->is_floating || !mw->has_applied) continue;

        if (dt->windows[i] == hwnd) { from = i; continue; }

        RECT r = mw->applied_rect;
        if (drop.x >= r.left && drop.x < r.right &&
            drop.y >= r.top  && drop.y < r.bottom)
            to = i;
    }

    if (from >= 0 && to >= 0 && from != to) {
        hwnd_swap(&dt->windows[from], &dt->windows[to]);
        dt->focused = to;
        log_w(L"mouse: swapped tiles %d <-> %d", from, to);
    }

    tile_current();
}
