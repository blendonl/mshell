/*
 * border.c — focused-window indicator.
 *
 * With title bars stripped, there is no OS cue for which tiled window is
 * active. We draw a thin colored ring around the focused window using a
 * single click-through layered overlay that we reposition on every focus
 * or tiling change. The overlay is a hollow region (outer rect minus inner
 * rect), so its interior is not part of the window at all — clicks and
 * paints land on the app underneath.
 */

#include "mshell.h"
#include "overlay.h"

static const wchar_t *BORDER_CLASS = L"mshell_FocusBorder";

/* Which colour the ring is painting right now. Resolved in border_refresh (main
 * thread) and consumed by WM_PAINT, the same prepared-state discipline the
 * other overlays use. */
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
        return 1;  /* painted in WM_PAINT; skip default erase to avoid flicker */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool border_init(void) {
    if (!overlay_register(BORDER_CLASS, border_wndproc, false)) return false;

    /* Layered + transparent => fully click-through; NOACTIVATE => never steals
     * focus; toolwindow => never enters our own management or Alt+Tab. Not
     * topmost: we re-stack it directly above the focused window each update. */
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

/* Draw the ring around the currently focused window (if any). */
void border_refresh(void) {
    if (!g.border_window) return;

    int bw = g.border_width;
    HWND focus = desktop_get_focused();

    /* Some windows opt out of the ring entirely (games and other rule-matched
     * "hands-off" windows): a ring hugging a fullscreen game just paints a
     * colored line over its edges. Leave those bare. */
    ManagedWindow *fmw = focus ? window_find(focus) : NULL;
    if (fmw && (fmw->no_ring || window_is_screen_fullscreen(fmw))) {
        /* Same reasoning for a fullscreen window: the ring would be a colored
         * line hugging the screen edges, over content that asked for the whole
         * display. A FS_CONTENT window keeps its tile, and keeps its ring. */
        border_hide();
        return;
    }

    RECT r;
    /* Track the DWM visible frame (not the raw window rect, which includes the
     * invisible resize border) so the ring hugs the window the same way the
     * frame-accurate tiler positions it. */
    if (bw <= 0 || !focus || !IsWindow(focus) || IsIconic(focus) ||
        !window_frame_rect(focus, &r)) {
        border_hide();
        return;
    }

    /* Per-state colour: urgent beats floating beats focused, because "this
     * window wants you" is the more urgent fact than how it is laid out. */
    s_color = g.border_color;
    if (fmw && fmw->is_floating) s_color = g.border_color_float;
    if (fmw && fmw->urgent)      s_color = g.border_color_urgent;

    int x = r.left - bw;
    int y = r.top  - bw;
    int w = (r.right  - r.left) + bw * 2;
    int h = (r.bottom - r.top)  + bw * 2;

    /* Hollow region: the overlay is only the ring, never the interior. */
    HRGN outer = CreateRectRgn(0, 0, w, h);
    HRGN inner = CreateRectRgn(bw, bw, w - bw, h - bw);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    DeleteObject(inner);
    SetWindowRgn(g.border_window, outer, FALSE);  /* window now owns `outer` */

    /* HWND_TOP places the ring just above the focused window, which itself
     * was just raised by window_focus(). */
    SetWindowPos(g.border_window, HWND_TOP, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.border_window, NULL, TRUE);
}

void border_shutdown(void) {
    overlay_destroy(&g.border_window, BORDER_CLASS);
}
