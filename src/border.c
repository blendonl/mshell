#include "mshell.h"
#include "overlay.h"

static const wchar_t *BORDER_CLASS = L"mshell_FocusBorder";

static COLORREF s_color;

static LRESULT CALLBACK border_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        overlay_fill(dc, &rc, s_color);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool border_init(void) {
    if (!overlay_register(BORDER_CLASS, border_wndproc, false)) return false;

    g.border_window = overlay_create(
        BORDER_CLASS,
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    if (!g.border_window) return false;

    SetLayeredWindowAttributes(g.border_window, 0, 255, LWA_ALPHA);
    return true;
}

void border_hide(void) {
    if (g.border_window) ShowWindow(g.border_window, SW_HIDE);
}

static int monitor_visible_count(int mon) {
    Desktop *dt = desktop_by_id(desktop_on_monitor(mon));
    if (!dt) dt = desktop_current();
    if (!dt) return 0;

    int n = 0;
    for (int i = 0; i < dt->count && n < 2; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw) continue;
        if (mw->app_hidden || mw->wm_hidden || mw->layout_hidden) continue;
        if (IsIconic(dt->windows[i])) continue;

        int wmon = mw->monitor;
        if (wmon < 0 || wmon >= g.monitor_count) wmon = 0;
        if (wmon != mon) continue;
        n++;
    }
    return n;
}

static BorderRect border_rect(const RECT *r) {
    BorderRect out = { r->left, r->top, r->right, r->bottom };
    return out;
}

static HRGN border_region(const BorderGeometry *geo, int w, int h) {
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (!rgn) return NULL;

    if (geo->has_ring) {
        HRGN outer = CreateRectRgn(0, 0, w, h);
        HRGN inner = CreateRectRgn(geo->hole.left, geo->hole.top,
                                   geo->hole.right, geo->hole.bottom);
        CombineRgn(outer, outer, inner, RGN_DIFF);
        CombineRgn(rgn, rgn, outer, RGN_OR);
        DeleteObject(inner);
        DeleteObject(outer);
    }

    if (geo->has_accent) {
        HRGN bar = CreateRectRgn(geo->accent.left, geo->accent.top,
                                 geo->accent.right, geo->accent.bottom);
        CombineRgn(rgn, rgn, bar, RGN_OR);
        DeleteObject(bar);
    }

    return rgn;
}

void border_refresh(void) {
    anim_dim_refresh();

    if (!g.border_window) return;

    HWND focus = desktop_get_focused();

    ManagedWindow *fmw = focus ? window_find(focus) : NULL;
    if (fmw && (fmw->no_ring || window_is_screen_fullscreen(fmw))) {
        border_hide();
        return;
    }

    if (g.cfg.smart_borders && fmw) {
        int mon = fmw->monitor;
        if (mon < 0 || mon >= g.monitor_count) mon = 0;
        if (monitor_visible_count(mon) <= 1) {
            border_hide();
            return;
        }
    }

    RECT r;
    if (!focus || !IsWindow(focus) || IsIconic(focus) ||
        !window_frame_rect(focus, &r)) {
        border_hide();
        return;
    }

    BorderGeometry geo = border_geometry(
        border_rect(&r),
        border_rect(&g.monitors[monitor_of_window(focus)].work_area),
        g.cfg.border_width, g.cfg.border_accent, g.cfg.border_accent_width);

    if (!geo.visible) {
        border_hide();
        return;
    }

    s_color = g.cfg.border_color;
    if (window_is_float_tier(fmw))  s_color = g.cfg.border_color_float;
    if (fmw && fmw->urgent)      s_color = g.cfg.border_color_urgent;

    int w = geo.bounds.right  - geo.bounds.left;
    int h = geo.bounds.bottom - geo.bounds.top;

    HRGN rgn = border_region(&geo, w, h);
    if (!rgn) {
        border_hide();
        return;
    }
    SetWindowRgn(g.border_window, rgn, FALSE);

    SetWindowPos(g.border_window, focus, geo.bounds.left, geo.bounds.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.border_window, NULL, TRUE);
}

void border_shutdown(void) {
    overlay_destroy(&g.border_window, BORDER_CLASS);
}
