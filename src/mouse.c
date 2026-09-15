#include "mshell.h"
#include "focus_pick.h"

static POINT s_last_pointer;

static bool has_caption(HWND hwnd) {
    return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
}

static bool foreground_holds_pointer(HWND fg) {
    if (!fg) return false;

    GUITHREADINFO gti = { .cbSize = sizeof gti };
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, NULL), &gti))
        return false;

    const DWORD menu_modes = GUI_INMENUMODE | GUI_POPUPMENUMODE |
                             GUI_SYSTEMMENUMODE;
    return (gti.flags & menu_modes) != 0 || gti.hwndCapture != NULL;
}

static bool drag_in_progress(void) {
    kb_lock();
    bool dragging = (g.drag_hwnd != NULL || g.mod_drag_hwnd != NULL);
    kb_unlock();
    return dragging;
}

void mouse_warp_focus(void) {
    if (!g.cfg.mouse_warp) return;
    if (drag_in_progress()) return;

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
    if (!g.cfg.mouse_follow) return;

    POINT p;
    if (!GetCursorPos(&p)) return;

    if (p.x == s_last_pointer.x && p.y == s_last_pointer.y) return;
    s_last_pointer = p;

    if (drag_in_progress()) return;

    HWND under = WindowFromPoint(p);
    if (!under) return;

    HWND top = GetAncestor(under, GA_ROOT);
    if (!top) return;

    HWND           fg = GetForegroundWindow();
    ManagedWindow *mw = window_find(top);

    PointerTarget target = {
        .managed                  = mw != NULL,
        .tracked_popup            = mw && mw->tracked_only &&
                                    !has_caption(top),
        .on_visible_desktop       = mw && desktop_is_visible(mw->desktop_id),
        .is_foreground            = top == fg,
        .foreground_holds_pointer = foreground_holds_pointer(fg),
    };
    if (!focus_pick_follows_pointer(&target)) return;

    desktop_focus_update(top);
    window_focus(top);
}

static POINT s_grab;
static RECT  s_grab_rect;
static bool  s_resizing;
static int   s_posts_inflight;
static bool  s_drag_ended;

static void drag_post_consumed_locked(void) {
    if (s_posts_inflight > 0) s_posts_inflight--;
    if (s_drag_ended && s_posts_inflight == 0) {
        g.mod_drag_hwnd = NULL;
        s_drag_ended    = false;
    }
}

static bool drag_post_move(POINT pt, bool final) {
    kb_lock();
    if (!g.mod_drag_hwnd || s_drag_ended) { kb_unlock(); return false; }

    int  dx   = pt.x - s_grab.x;
    int  dy   = pt.y - s_grab.y;
    bool post = final || s_posts_inflight == 0;

    if (final) s_drag_ended = true;
    if (post)  s_posts_inflight++;
    kb_unlock();

    if (post && !PostMessageW(g.message_window, WM_MSHELL_MOUSE,
                              (WPARAM)dx, (LPARAM)dy)) {
        kb_lock();
        drag_post_consumed_locked();
        kb_unlock();
    }
    return true;
}

void mouse_mod_drag_apply(int dx, int dy) {
    kb_lock();
    HWND target   = g.mod_drag_hwnd;
    RECT base     = s_grab_rect;
    bool resizing = s_resizing;
    drag_post_consumed_locked();
    kb_unlock();

    ManagedWindow *mw = window_find(target);
    if (!mw || !mw->is_floating) return;

    RECT want = base;
    if (resizing) {
        want.right  += dx;
        want.bottom += dy;
        if (want.right - want.left < g.cfg.min_win_w)
            want.right = want.left + g.cfg.min_win_w;
        if (want.bottom - want.top < g.cfg.min_win_h)
            want.bottom = want.top + g.cfg.min_win_h;
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

        RECT frame;
        if (!window_frame_rect(top, &frame)) return false;

        kb_lock();
        s_grab          = pt;
        s_grab_rect     = frame;
        s_resizing      = (msg == WM_RBUTTONDOWN);
        s_drag_ended    = false;
        g.mod_drag_hwnd = top;
        kb_unlock();
        return true;
    }

    case WM_MOUSEMOVE:
        drag_post_move(pt, false);
        return false;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        return drag_post_move(pt, true);
    }
    return false;
}

static const int PTR_ACCEL_ON[3]  = { 6, 10, 1 };
static const int PTR_ACCEL_OFF[3] = { 0,  0, 0 };

static void pointer_persist_speed(int speed) {
    int current = 0;
    if (SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &current, 0) && current == speed)
        return;
    if (spi_set_persistent(SPI_SETMOUSESPEED, 0, (PVOID)(UINT_PTR)speed))
        log_w(L"mouse: pointer speed %d -> %d, saved to the profile", current, speed);
    else
        log_err(L"mouse: SPI_SETMOUSESPEED(%d) failed: %lu",
                speed, GetLastError());
}

static void pointer_persist_accel(bool on) {
    int current[3] = {0};
    if (SystemParametersInfoW(SPI_GETMOUSE, 0, current, 0) && (current[2] != 0) == on)
        return;
    const int *want = on ? PTR_ACCEL_ON : PTR_ACCEL_OFF;
    int tmp[3] = { want[0], want[1], want[2] };
    if (spi_set_persistent(SPI_SETMOUSE, 0, tmp))
        log_w(L"mouse: pointer precision %ls, saved to the profile", on ? L"on" : L"off");
    else
        log_err(L"mouse: SPI_SETMOUSE failed: %lu", GetLastError());
}

static void pointer_persist_swap(bool swapped) {
    if ((GetSystemMetrics(SM_SWAPBUTTON) != 0) == swapped) return;
    if (spi_set_persistent(SPI_SETMOUSEBUTTONSWAP, swapped ? 1 : 0, NULL))
        log_w(L"mouse: primary button %ls, saved to the profile",
              swapped ? L"right" : L"left");
    else
        log_err(L"mouse: SPI_SETMOUSEBUTTONSWAP(%d) failed: %lu",
                (int)swapped, GetLastError());
}

void mouse_sync_pointer(void) {
    if (g.cfg.mouse_speed > 0) pointer_persist_speed(g.cfg.mouse_speed);
    if (g.cfg.mouse_accel >= 0) pointer_persist_accel(g.cfg.mouse_accel != 0);
    if (g.cfg.mouse_swap  >= 0) pointer_persist_swap(g.cfg.mouse_swap != 0);
}

void mouse_drag_begin(HWND hwnd) {
    if (!g.cfg.mouse_enabled) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw || mw->is_floating) return;

    g.drag_hwnd = hwnd;
    GetCursorPos(&g.drag_start);
}

void mouse_drag_end(HWND hwnd) {
    if (!g.cfg.mouse_enabled || g.drag_hwnd != hwnd) { g.drag_hwnd = NULL; return; }
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
