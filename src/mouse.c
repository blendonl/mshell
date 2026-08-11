/* ===========================================================================
 * mouse.c — focus-follows-mouse, Mod+drag for floating windows, and Windows'
 *           own pointer settings.
 *
 * The first two are features with deliberately different mechanisms, because
 * their costs are not the same. The third is at the bottom of the file and is a
 * different kind of thing entirely — settings mshell borrows from the machine
 * rather than owns.
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

    window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
    border_refresh();   /* the ring follows the dragged/resized window */
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

/* ---------------------------------------------------------------------------
 * Windows' own pointer settings
 *
 * Pointer speed, "enhance pointer precision" and the left/right button swap are
 * not mshell's to keep. They are per-user Windows settings that every other
 * application on the machine sees, and a shell that has replaced Explorer is
 * precisely the program that has taken away the Settings page you would undo
 * them from.
 *
 * So they are BORROWED, on the same terms as the foreground lock timeout in
 * main.c: the machine's values are snapshotted before the first write, every
 * SystemParametersInfo call omits SPIF_UPDATEINIFILE — which would write the
 * change into the user profile, where it would outlive the process — and the
 * originals go back at shutdown. Drop a field from the config and save, and it
 * is handed back on the reload rather than at exit.
 *
 * The snapshot is taken ONCE, for the same reason tweaks.c never overwrites a
 * backup: after the first apply the "current" value is mshell's own, and
 * re-reading it would record that as the thing to restore.
 * --------------------------------------------------------------------------- */

/* SPI_SETMOUSE takes {threshold1, threshold2, acceleration} as one array. These
 * are the two triples Windows' own "enhance pointer precision" checkbox writes,
 * so `accel = true` lands on the same setting the Control Panel would. */
static const int PTR_ACCEL_ON[3]  = { 6, 10, 1 };
static const int PTR_ACCEL_OFF[3] = { 0,  0, 0 };

static bool s_ptr_saved;            /* the snapshot below has been taken */
static int  s_ptr_prev_speed;
static int  s_ptr_prev_accel[3];
static bool s_ptr_prev_swap;

/* Which of the three mshell is currently holding. Tracked per field so a reload
 * that stops asking for one gives back that one and leaves the others alone. */
static bool s_ptr_own_speed, s_ptr_own_accel, s_ptr_own_swap;

static void pointer_snapshot(void) {
    if (s_ptr_saved) return;
    s_ptr_saved = true;

    if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &s_ptr_prev_speed, 0))
        s_ptr_prev_speed = 10;                  /* the slider's middle notch */
    if (!SystemParametersInfoW(SPI_GETMOUSE, 0, s_ptr_prev_accel, 0))
        memcpy(s_ptr_prev_accel, PTR_ACCEL_ON, sizeof s_ptr_prev_accel);

    /* There is no SPI_GETMOUSEBUTTONSWAP; the swap is readable only as a
     * system metric. */
    s_ptr_prev_swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
}

static void pointer_set_speed(int speed) {
    if (!SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (PVOID)(UINT_PTR)speed,
                               SPIF_SENDCHANGE))
        log_err(L"mouse: SPI_SETMOUSESPEED(%d) failed: %lu",
                speed, GetLastError());
}

static void pointer_set_accel(const int v[3]) {
    /* pvParam is not const in the SDK and SPI_SETMOUSE does not write through
     * it — copy rather than cast the const off a static. */
    int tmp[3] = { v[0], v[1], v[2] };
    if (!SystemParametersInfoW(SPI_SETMOUSE, 0, tmp, SPIF_SENDCHANGE))
        log_err(L"mouse: SPI_SETMOUSE failed: %lu", GetLastError());
}

static void pointer_set_swap(bool swapped) {
    if (!SystemParametersInfoW(SPI_SETMOUSEBUTTONSWAP, swapped ? 1 : 0, NULL,
                               SPIF_SENDCHANGE))
        log_err(L"mouse: SPI_SETMOUSEBUTTONSWAP(%d) failed: %lu",
                (int)swapped, GetLastError());
}

void mouse_sync_pointer(void) {
    bool want_speed = g.mouse_speed > 0;
    bool want_accel = g.mouse_accel >= 0;
    bool want_swap  = g.mouse_swap  >= 0;

    /* Nothing asked for and nothing held: return before the snapshot, so a
     * config that says nothing about the pointer never touches it at all. */
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

/* Shutdown, and the crash handler. Restores only what mshell actually changed,
 * so a machine whose pointer the config never mentioned is left untouched even
 * here. */
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
