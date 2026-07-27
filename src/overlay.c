/* ===========================================================================
 * overlay.c — see overlay.h for what this is and why.
 * =========================================================================== */
#include "mshell.h"
#include "overlay.h"

bool overlay_register(const wchar_t *cls, WNDPROC proc, bool arrow_cursor) {
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = proc;
    wc.hInstance     = g.hinst;
    wc.lpszClassName = cls;
    if (arrow_cursor) wc.hCursor = LoadCursorW(NULL, IDC_ARROW);

    /* No background brush on purpose: every overlay returns 1 from
     * WM_ERASEBKGND and paints its whole client area, which is what keeps them
     * from flickering. */
    if (RegisterClassExW(&wc)) return true;

    /* Already registered is success — the status bar registers once and then
     * creates a window per monitor. */
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND overlay_create(const wchar_t *cls, DWORD exstyle) {
    HWND h = CreateWindowExW(exstyle, cls, L"", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g.hinst, NULL);
    if (!h)
        log_err(L"overlay: CreateWindowEx(%ls) failed: %lu", cls, GetLastError());
    return h;
}

void overlay_destroy(HWND *hwnd, const wchar_t *cls) {
    if (hwnd && *hwnd) {
        DestroyWindow(*hwnd);
        *hwnd = NULL;
    }
    if (cls) UnregisterClassW(cls, g.hinst);
}

int overlay_scale(int px, UINT dpi) {
    if (dpi == 0) dpi = 96;
    return MulDiv(px, (int)dpi, 96);
}

HFONT overlay_font(OverlayFont *of, UINT dpi, int px) {
    if (!of) return NULL;
    if (px < 1) px = 1;

    if (of->font && of->dpi == dpi && of->px == px) return of->font;
    if (of->font) DeleteObject(of->font);

    /* Negative height means "this many pixels", as opposed to points — which
     * is what makes the caller's own DPI scaling the only scaling applied. */
    of->font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    of->dpi = dpi;
    of->px  = px;
    return of->font;
}

void overlay_font_free(OverlayFont *of) {
    if (!of || !of->font) return;
    DeleteObject(of->font);
    of->font = NULL;
    of->dpi  = 0;
    of->px   = 0;
}

HDC overlay_paint_begin(OverlayPaint *p, HWND hwnd) {
    memset(p, 0, sizeof(*p));
    p->hwnd = hwnd;
    p->hdc  = BeginPaint(hwnd, &p->ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    p->w = rc.right;
    p->h = rc.bottom;

    if (p->w <= 0 || p->h <= 0) {
        EndPaint(hwnd, &p->ps);
        p->hdc = NULL;
        return NULL;
    }

    p->mem = CreateCompatibleDC(p->hdc);
    p->bmp = CreateCompatibleBitmap(p->hdc, p->w, p->h);
    if (!p->mem || !p->bmp) {
        if (p->bmp) DeleteObject(p->bmp);
        if (p->mem) DeleteDC(p->mem);
        EndPaint(hwnd, &p->ps);
        p->hdc = NULL;
        return NULL;
    }
    p->obm = (HBITMAP)SelectObject(p->mem, p->bmp);
    return p->mem;
}

void overlay_paint_end(OverlayPaint *p) {
    if (!p || !p->hdc) return;

    BitBlt(p->hdc, 0, 0, p->w, p->h, p->mem, 0, 0, SRCCOPY);

    SelectObject(p->mem, p->obm);
    DeleteObject(p->bmp);
    DeleteDC(p->mem);
    EndPaint(p->hwnd, &p->ps);
    p->hdc = NULL;
}

void overlay_fill(HDC dc, const RECT *rc, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, rc, br);
    DeleteObject(br);
}
