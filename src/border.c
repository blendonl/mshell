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

/* ===========================================================================
 * Smart borders — how many windows can you actually SEE on `mon` right now?
 *
 * Counted over the current desktop's window list, the same list the tiler
 * walks, so a window on another desktop or another display never keeps the
 * ring alive. Anything off the screen is skipped whichever way it went away:
 * the app hid itself (app_hidden), mshell hid it for a desktop switch or a
 * stowed scratchpad (wm_hidden, which covers cloaked/sunk/stashed), the layout
 * held it back (layout_hidden — monocle shows one window, and that is exactly
 * the case with nothing to disambiguate), or the user minimized it.
 *
 * Floats and tracked windows COUNT: they are windows you can see, and picking
 * the focused one out of a float sitting over a lone tile is the ring's job.
 *
 * Stops at 2 — "more than one" is all the caller ever asks.
 * =========================================================================== */
static int monitor_visible_count(int mon) {
    Desktop *dt = desktop_current();
    if (!dt) return 0;

    int n = 0;
    for (int i = 0; i < dt->count && n < 2; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw) continue;
        if (mw->app_hidden || mw->wm_hidden || mw->layout_hidden) continue;
        if (IsIconic(dt->windows[i])) continue;

        int wmon = mw->monitor;
        if (wmon < 0 || wmon >= g.monitor_count) wmon = 0;   /* defensive */
        if (wmon != mon) continue;
        n++;
    }
    return n;
}

/* Draw the ring around the currently focused window (if any). */
void border_refresh(void) {
    /* The dim scrim is placed relative to the focused window, so it moves at
     * exactly the moments the ring does — one call site rather than two sets
     * that can drift apart. */
    anim_dim_refresh();

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

    /* Smart borders: nothing to point at when the focused window is the only
     * thing on its display. Needs a ManagedWindow to know WHICH display that
     * is — an unmanaged focused window keeps its ring, as it always did. */
    if (g.smart_borders && fmw) {
        int mon = fmw->monitor;
        if (mon < 0 || mon >= g.monitor_count) mon = 0;
        if (monitor_visible_count(mon) <= 1) {
            border_hide();
            return;
        }
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
    /* The tier, not the exemption: a tracked window is not a float the user
     * chose, and colouring it as one says the layout is doing something with it
     * that it is not. */
    if (window_is_float_tier(fmw))  s_color = g.border_color_float;
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

    /* Directly above the window it belongs to, rather than HWND_TOP: with
     * floats kept over the tiled grid, a ring pinned to the very top would draw
     * itself across the float covering the focused window — a coloured line
     * over unrelated content. Sitting on the window instead means whatever is
     * legitimately above that window covers the ring too.
     *
     * A topmost `focus` (fullscreen, always-on-top) pulls the ring into the
     * topmost band with it, which is exactly where it is wanted. */
    SetWindowPos(g.border_window, focus, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.border_window, NULL, TRUE);
}

void border_shutdown(void) {
    overlay_destroy(&g.border_window, BORDER_CLASS);
}
