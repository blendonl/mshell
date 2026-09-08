#include "mshell.h"
#include "overlay.h"

static const wchar_t *BG_CLASS = L"mshell_Background";

static void virtual_screen_rect(int *x, int *y, int *w, int *h) {
    *x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    *y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

static LRESULT CALLBACK bg_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        overlay_fill(dc, &rc, g.cfg.background_color);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool background_init(void) {
    if (!overlay_register(BG_CLASS, bg_wndproc, true)) return false;

    g.background_window = overlay_create(BG_CLASS,
                                         WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    if (!g.background_window) return false;

    background_update();
    return true;
}

void background_update(void) {
    if (!g.background_window) return;

    int x, y, w, h;
    virtual_screen_rect(&x, &y, &w, &h);

    bool visible = IsWindowVisible(g.background_window) != 0;
    RECT cur;
    bool placed = visible && GetWindowRect(g.background_window, &cur) &&
                  cur.left == x && cur.top == y &&
                  cur.right - cur.left == w && cur.bottom - cur.top == h;

    if (!placed) {
        bool sunk = window_sunk_count() > 0;
        SetWindowPos(g.background_window, sunk ? NULL : HWND_BOTTOM, x, y, w, h,
                     SWP_NOACTIVATE | (visible ? 0u : SWP_SHOWWINDOW) |
                     (sunk ? SWP_NOZORDER : 0u));
    }

    window_resink();
    InvalidateRect(g.background_window, NULL, TRUE);
}

void background_shutdown(void) {
    overlay_destroy(&g.background_window, BG_CLASS);
}
