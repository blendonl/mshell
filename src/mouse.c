/* ===========================================================================
 * mouse.c — focus-follows-mouse, and Mod+drag for floating windows.
 *
 * Two features with deliberately different mechanisms, because their costs are
 * not the same.
 *
 * FOCUS-FOLLOWS-MOUSE polls on a timer rather than hooking the mouse. A
 * WH_MOUSE_LL hook fires on every pixel of every mouse movement, on the same
 * thread that has to answer WH_KEYBOARD_LL inside LowLevelHooksTimeout — the
 * exact budget the dedicated hook thread exists to protect. Focus following a
 * cursor is a human-speed effect; 120 ms of granularity is imperceptible and
 * costs one WindowFromPoint per tick instead of one per mouse event.
 *
 * MOD+DRAG genuinely needs the hook: it has to swallow the button-down that
 * starts the drag, and nothing else can. So it is OPT-IN, the hook is installed
 * only while it is enabled, and the proc's first act is to return for anything
 * that is not the modifier being held.
 *
 * Tiled windows are not draggable and that is not an omission — the layout owns
 * their geometry. Dragging one still swaps tiles (events.c); this is for
 * floating windows, and especially for the borderless ones a `decorate = false`
 * rule produces, which have no title bar to grab.
 * =========================================================================== */
#include "mshell.h"

/* ---------------------------------------------------------------------------
 * Focus follows mouse
 * --------------------------------------------------------------------------- */
void mouse_poll_focus(void) {
    if (!g.mouse_follow) return;

    POINT p;
    if (!GetCursorPos(&p)) return;

    /* Nothing to do until the pointer actually moves. Without this, focus
     * would be re-asserted every tick, which fights any focus the user set
     * with the keyboard while the cursor sits still over another window. */
    static POINT last;
    if (p.x == last.x && p.y == last.y) return;
    last = p;

    /* A drag in progress owns the pointer; stealing focus mid-drag would drop
     * whatever is being dragged. */
    if (g.drag_hwnd || g.mod_drag_hwnd) return;

    HWND under = WindowFromPoint(p);
    if (!under) return;

    /* WindowFromPoint gives the deepest child; the manager only knows
     * top-levels. */
    HWND top = GetAncestor(under, GA_ROOT);
    if (!top || top == GetForegroundWindow()) return;

    ManagedWindow *mw = window_find(top);
    if (!mw || mw->desktop_id != g.current_desktop_id) return;

    desktop_focus_update(top);
    window_focus(top);
}

/* ---------------------------------------------------------------------------
 * Mod+drag
 *
 * State lives in g so the poll above can see a drag is in progress. The hook
 * thread writes it and the main thread reads it; both are single writers of
 * their own fields, and a torn read would at worst skip one focus poll.
 * --------------------------------------------------------------------------- */
static POINT s_grab;        /* cursor position when the drag started  */
static RECT  s_grab_rect;   /* the window's frame at that moment      */
static bool  s_resizing;

/* Applied on the main thread — SetWindowPos from the hook thread would run
 * inside the input timeout. */
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

    RECT adj = window_adjust_for_frame(mw->hwnd, want);
    window_set_pos(mw->hwnd, adj.left, adj.top,
                   adj.right - adj.left, adj.bottom - adj.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    mw->applied_rect = want;
    mw->has_applied  = true;
}

/* Called from the hook thread. Returns true when the event was consumed. */
bool mouse_mod_drag_event(WPARAM msg, POINT pt, bool mod_held) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        if (!mod_held) return false;

        HWND top = GetAncestor(WindowFromPoint(pt), GA_ROOT);
        if (!top) return false;

        ManagedWindow *mw = window_find(top);
        /* Floating only. A tiled window's drag is already interpreted as a
         * tile swap by the MOVESIZE handler, and two meanings for one gesture
         * is worse than one. */
        if (!mw || !mw->is_floating) return false;
        if (!window_frame_rect(top, &s_grab_rect)) return false;

        s_grab           = pt;
        s_resizing       = (msg == WM_RBUTTONDOWN);
        g.mod_drag_hwnd  = top;
        return true;    /* swallow: the app must not see this click */
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
