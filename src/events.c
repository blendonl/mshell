/*
 * events.c — WinEvent hooks for window tracking.
 *
 * We use out-of-context hooks so everything fires in our message-pump
 * thread — no DLL injection, no threading headaches.
 */

#include "mshell.h"

/* ===========================================================================
 * WinEvent callback
 * =========================================================================== */
void CALLBACK events_win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD idEventThread, DWORD dwmsEventTime) {
    (void)hook;
    (void)idEventThread;
    (void)dwmsEventTime;

    /* Bail out if we're in the middle of a tiling pass or desktop switch —
     * otherwise SetWindowPos / ShowWindow calls would re-enter endlessly. */
    if (events_suppressed()) return;

    /* Only interested in top-level windows, not child controls */
    if (idObject != OBJID_WINDOW) return;
    if (idChild  != CHILDID_SELF) return;

    switch (event) {

    case EVENT_OBJECT_CREATE:
        /* A new window was created. Try to manage it.
         * We defer slightly — the window may not be fully initialised yet.
         * A quick IsWindowVisible check filters out windows still being set up. */
        if (IsWindow(hwnd) && IsWindowVisible(hwnd)) {
            window_manage(hwnd);
        }
        break;

    case EVENT_OBJECT_DESTROY:
        /* Window being destroyed; clean up */
        window_unmanage(hwnd);
        break;

    case EVENT_OBJECT_SHOW:
        /* Window was hidden and is now shown. Either something we already
         * manage came back, or it's a window we've never seen. */
        if (IsWindow(hwnd)) {
            ManagedWindow *mw = window_find(hwnd);
            if (!mw) {
                window_manage(hwnd);
            } else if (mw->app_hidden) {
                /* The app put it back (tray icon clicked). It rejoins the
                 * layout. */
                mw->app_hidden  = false;
                mw->has_applied = false;
                if (mw->desktop_id == g.current_desktop_id) tile_current();
            }
        }
        break;

    case EVENT_OBJECT_HIDE:
        /* A managed window was hidden and it wasn't us.
         *
         * Every hide mshell performs — desktop switches, monocle, moving a
         * window to another desktop — runs inside events_suppress_begin(), and
         * this callback returns early while suppressed. So reaching here means
         * the APP hid its own window: minimise-to-tray, which Discord, Slack,
         * Telegram and Steam all do.
         *
         * That has to be recorded, not just re-tiled around. The window stays
         * managed (it is still the app's window, on this desktop, and will come
         * back), but it leaves the layout — otherwise collect_clients keeps
         * handing it a tile and flush_placements, which shows anything in the
         * placement list that isn't visible, drags it straight back onto the
         * screen. */
        {
            ManagedWindow *mw = window_find(hwnd);
            if (mw && !mw->app_hidden) {
                mw->app_hidden  = true;
                mw->has_applied = false;
                log_w(L"app hid its own window: %p — leaving the layout",
                      (void *)hwnd);
                if (mw->desktop_id == g.current_desktop_id) tile_current();
            }
        }
        break;

    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
        /* A window minimized or came back. Minimizing does NOT clear WS_VISIBLE
         * — it is not a hide — so nothing above notices it, and without this a
         * minimized window keeps its tile: SetWindowPos on an iconic window only
         * edits the rect it will restore to, so the cell just sits empty.
         *
         * collect_clients skips iconic windows, so both directions are simply a
         * re-tile. Floating windows are not in the layout either way. */
        {
            ManagedWindow *mw = window_find(hwnd);
            if (mw && !mw->is_floating &&
                mw->desktop_id == g.current_desktop_id) {
                mw->has_applied = false;
                tile_current();
            }
        }
        break;

    case EVENT_SYSTEM_FOREGROUND:
        /* Focus changed behind our back — the user clicked a window, or an app
         * activated itself on startup. Re-sync our idea of who is focused:
         * every focus keybind computes its target *relative to* that index, so
         * a stale one makes Win+h/j/k/l walk from the wrong window — usually
         * landing on the window that already has focus, which looks exactly
         * like the keybind doing nothing. */
        if (IsWindow(hwnd) && window_index_of(hwnd) >= 0) {
            ManagedWindow *mw = window_find(hwnd);
            desktop_focus_update(hwnd);
            if (mw && mw->monitor >= 0 && mw->monitor < g.monitor_count)
                g.focused_monitor = mw->monitor;
            border_refresh();   /* ring follows mouse/app focus too */
        }
        break;

    case EVENT_OBJECT_LOCATIONCHANGE:
        /* A tiled window was moved or resized. Snap it back — but only if it
         * genuinely drifted from where we last put it, so our own placements
         * (delivered here asynchronously by the out-of-context hook, after the
         * suppression counter has already been released) don't cause a re-tile
         * storm. Floating windows are exempt — bar the rule re-assert below. */
        {
            ManagedWindow *mw = window_find(hwnd);
            if (!mw || mw->desktop_id != g.current_desktop_id) break;

            /* Floating means "you keep whatever geometry you like" — with one
             * exception. A window whose rule asked to be borderless or
             * fullscreen (a game) rebuilds itself once its graphics device is
             * up, re-adding the frame we stripped and resizing away from the
             * monitor we parked it on. Re-assert; it no-ops when nothing moved
             * and when the frame is already bare. */
            if (mw->is_floating) {
                if (mw->no_decor || mw->fullscreen) window_reassert_rule(hwnd);
                break;
            }

            /* ---- fullscreen: who owns this window's geometry? ----
             * FS_WINDOW  mshell does — put it back over the monitor if the app
             *            moved it.
             * FS_BOTH    the app does — that is the whole point of the mode, so
             *            never snap it back.
             * FS_CONTENT mshell does, at the window's tile: falling through to
             *            the ordinary snap-back below is exactly the pinning
             *            that keeps an app's fullscreen inside the window. */
            if (mw->fs_mode == FS_WINDOW) {
                if (!window_covers_monitor(hwnd)) {
                    mw->has_applied = false;
                    window_park_over_monitor(hwnd);
                }
                break;
            }
            if (mw->fs_mode == FS_BOTH) break;

            /* An app that fullscreened itself — a video going fullscreen, F11
             * in a browser — resizes to cover the display. With the "monitor"
             * policy it may keep it: drop it out of the layout while it lasts,
             * and put it back the moment it returns to a smaller rect. Windows
             * with an explicit mode are excluded: that mode already decided. */
            if (g.fullscreen_policy == FS_BOTH || mw->app_fullscreen) {
                bool covers = (g.fullscreen_policy == FS_BOTH) &&
                              mw->fs_mode == FS_OFF &&
                              window_covers_monitor(hwnd);
                if (covers != mw->app_fullscreen) {
                    log_w(L"app fullscreen %ls: %p",
                          covers ? L"entered" : L"left", (void *)hwnd);
                    mw->app_fullscreen = covers;
                    mw->has_applied    = false;
                    tile_current();
                    break;
                }
                if (covers) break;      /* hands off while the app is fullscreen */
            }

            /* A tiled app that maximized itself has escaped the grid. */
            if (IsZoomed(hwnd)) {
                events_suppress_begin();
                ShowWindow(hwnd, SW_RESTORE);
                events_suppress_end();
                mw->has_applied = false;
                tile_current();
                break;
            }

            if (mw->has_applied) {
                RECT cur;
                RECT a = mw->applied_rect;
                if (window_frame_rect(hwnd, &cur)) {
                    const int EPS = 4;   /* tolerate sub-pixel/rounding noise */
                    int dx = abs((int)(cur.left - a.left));
                    int dy = abs((int)(cur.top  - a.top));
                    int dw = abs((int)((cur.right - cur.left) - (a.right - a.left)));
                    int dh = abs((int)((cur.bottom - cur.top) - (a.bottom - a.top)));
                    if (dx <= EPS && dy <= EPS && dw <= EPS && dh <= EPS)
                        break;           /* within tolerance — that was us */
                }
                mw->has_applied = false; /* real drift: force reposition */
            }
            tile_current();
        }
        break;

    default:
        break;
    }
}

/* ===========================================================================
 * Set up WinEvent hooks
 * =========================================================================== */
bool events_init(void) {
    /* We care about object creation/destruction, show/hide, focus,
     * and location changes for ALL top-level windows, out-of-context.  */
    g.win_event_hook = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_LOCATIONCHANGE,
        NULL,                              /* our own module           */
        events_win_event_proc,             /* callback                 */
        0, 0,                              /* all processes, all threads */
        WINEVENT_OUTOFCONTEXT              /* fire in our thread       */
    );

    if (!g.win_event_hook) {
        log_w(L"SetWinEventHook failed: %lu", GetLastError());
        return false;
    }

    /* Focus changes need a SECOND hook. EVENT_SYSTEM_FOREGROUND is 0x0003 —
     * it lives in the system-event id range, far below EVENT_OBJECT_CREATE
     * (0x8000), so the object-event range above never delivers it no matter
     * what the callback does with it. Hooking the whole 0x0003..0x800B span
     * instead is not an option: it would firehose us with NAMECHANGE /
     * VALUECHANGE / STATECHANGE traffic from every window on the system.
     *
     * Not fatal if it fails: without it the WM still tiles and its own focus
     * moves still work, we just stop noticing focus the user gives with the
     * mouse. Refusing to start would strand the machine with no shell. */
    g.foreground_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        NULL,
        events_win_event_proc,
        0, 0,
        WINEVENT_OUTOFCONTEXT
    );

    if (!g.foreground_hook)
        log_w(L"SetWinEventHook(FOREGROUND) failed: %lu — focus tracking is "
              L"degraded (mouse-driven focus won't be seen)", GetLastError());

    /* Minimize/restore need a THIRD hook, for the same reason the foreground
     * one is separate: EVENT_SYSTEM_MINIMIZESTART/END are 0x0016/0x0017, in the
     * system-event range, nowhere near the object range above. They are
     * adjacent to each other, so one hook covers both.
     *
     * Also not fatal: without it a minimized window keeps an empty tile, which
     * is untidy rather than broken. */
    g.minimize_hook = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART,
        EVENT_SYSTEM_MINIMIZEEND,
        NULL,
        events_win_event_proc,
        0, 0,
        WINEVENT_OUTOFCONTEXT
    );

    if (!g.minimize_hook)
        log_w(L"SetWinEventHook(MINIMIZE) failed: %lu — minimized windows will "
              L"keep an empty tile", GetLastError());

    return true;
}

/* ===========================================================================
 * Tear down WinEvent hooks
 * =========================================================================== */
void events_shutdown(void) {
    if (g.win_event_hook) {
        UnhookWinEvent(g.win_event_hook);
        g.win_event_hook = NULL;
    }
    if (g.foreground_hook) {
        UnhookWinEvent(g.foreground_hook);
        g.foreground_hook = NULL;
    }
    if (g.minimize_hook) {
        UnhookWinEvent(g.minimize_hook);
        g.minimize_hook = NULL;
    }
}
