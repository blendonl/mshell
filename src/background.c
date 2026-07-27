/*
 * background.c — desktop backdrop.
 *
 * When mshell is the shell there is no Explorer painting the desktop, so an
 * empty workspace would be pure black. We host one bottom-most, non-activating
 * window that fills the whole virtual screen and paints a solid color. It sits
 * below every managed window and never participates in tiling or focus.
 */

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
        overlay_fill(dc, &rc, g.background_color);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool background_init(void) {
    /* The arrow cursor matters here and nowhere else: the backdrop is the one
     * overlay the pointer can come to rest over. */
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
    SetWindowPos(g.background_window, HWND_BOTTOM, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.background_window, NULL, TRUE);
}

void background_shutdown(void) {
    overlay_destroy(&g.background_window, BG_CLASS);
}
