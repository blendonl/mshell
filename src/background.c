/*
 * background.c — desktop backdrop.
 *
 * When mshell is the shell there is no Explorer painting the desktop, so an
 * empty workspace would be pure black. We host one bottom-most, non-activating
 * window that fills the whole virtual screen and paints a solid color. It sits
 * below every managed window and never participates in tiling or focus.
 */

#include "mshell.h"

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
        HBRUSH br = CreateSolidBrush(g.background_color);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool background_init(void) {
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = bg_wndproc;
    wc.hInstance     = g.hinst;
    wc.lpszClassName = BG_CLASS;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    int x, y, w, h;
    virtual_screen_rect(&x, &y, &w, &h);

    g.background_window = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        BG_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g.hinst, NULL);

    if (!g.background_window) {
        log_w(L"background: CreateWindowEx failed: %lu", GetLastError());
        return false;
    }

    SetWindowPos(g.background_window, HWND_BOTTOM, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
    if (g.background_window) {
        DestroyWindow(g.background_window);
        g.background_window = NULL;
    }
    UnregisterClassW(BG_CLASS, g.hinst);
}
