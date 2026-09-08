#include "mshell.h"

#define FG_BOUNCE_WINDOW_MS   2000
#define FG_BOUNCE_LIMIT       4
#define FG_BOUNCE_COOLDOWN_MS 10000

static bool foreground_bounce_allowed(HWND hwnd) {
    static HWND  offender;
    static DWORD since;
    static int   bounces;
    static bool  backed_off;
    static bool  warned;

    DWORD now = GetTickCount();

    if (hwnd != offender) {
        offender   = hwnd;
        since      = now;
        bounces    = 0;
        backed_off = false;
    }

    DWORD elapsed = now - since;

    if (backed_off) {
        if (elapsed < FG_BOUNCE_COOLDOWN_MS) return false;
        since      = now;
        bounces    = 0;
        backed_off = false;
    } else if (elapsed > FG_BOUNCE_WINDOW_MS) {
        since   = now;
        bounces = 0;
    }

    if (++bounces <= FG_BOUNCE_LIMIT) return true;

    backed_off = true;
    since      = now;
    if (!warned) {
        warned = true;
        log_msg(LOG_WARN, L"foreground: %p keeps taking the foreground from a "
                          L"desktop that is not on screen. Backing off rather "
                          L"than trading activations with it — it keeps the "
                          L"foreground until you focus something else.",
                (void *)hwnd);
    }
    return false;
}

void CALLBACK events_win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD idEventThread, DWORD dwmsEventTime) {
    (void)hook;
    (void)idEventThread;
    (void)dwmsEventTime;

    if (events_suppressed()) return;

    if (idObject != OBJID_WINDOW) return;
    if (idChild  != CHILDID_SELF) return;

    switch (event) {

    case EVENT_OBJECT_CREATE:
        if (IsWindow(hwnd) && IsWindowVisible(hwnd)) {
            window_manage(hwnd);
        }
        break;

    case EVENT_OBJECT_DESTROY:
        window_unmanage(hwnd);
        break;

    case EVENT_OBJECT_SHOW:
        if (IsWindow(hwnd)) {
            ManagedWindow *mw = window_find(hwnd);
            if (!mw) {
                window_manage(hwnd);
            } else if (mw->app_hidden) {
                mw->app_hidden  = false;
                mw->has_applied = false;
                events_suppress_begin();
                if (desktop_is_visible(mw->desktop_id)) {
                    window_show(mw);
                } else {
                    window_hide(mw);
                }
                events_suppress_end();
                if (desktop_is_visible(mw->desktop_id)) tile_current();
            }
        }
        break;

    case EVENT_OBJECT_HIDE:
        {
            ManagedWindow *mw = window_find(hwnd);
            if (window_hidden_by_showwindow(mw)) break;
            if (mw && !mw->app_hidden) {
                mw->app_hidden  = true;
                mw->has_applied = false;
                log_w(L"app hid its own window: %p — leaving the layout",
                      (void *)hwnd);
                if (desktop_is_visible(mw->desktop_id)) tile_current();
            }
        }
        break;

    case EVENT_SYSTEM_MOVESIZESTART:
        mouse_drag_begin(hwnd);
        break;

    case EVENT_SYSTEM_MOVESIZEEND:
        mouse_drag_end(hwnd);
        break;

    case EVENT_SYSTEM_MINIMIZESTART:
        if (g.cfg.minimize_never) {
            ManagedWindow *mw = window_find(hwnd);
            if (mw && !mw->app_hidden) {
                static HWND      last;
                static ULONGLONG first_at;
                static int       tries;

                ULONGLONG now = GetTickCount64();
                if (hwnd != last || now - first_at > 1000) {
                    last = hwnd; first_at = now; tries = 0;
                }
                if (++tries <= 3) {
                    events_suppress_begin();
                    ShowWindow(hwnd, SW_RESTORE);
                    events_suppress_end();
                    mw->has_applied = false;
                } else if (tries == 4) {
                    log_msg(LOG_WARN, L"minimize policy: a window keeps "
                                      L"minimizing itself — letting it");
                }
            }
        }
        __attribute__((fallthrough));
    case EVENT_SYSTEM_MINIMIZEEND:
        {
            ManagedWindow *mw = window_find(hwnd);
            if (mw && !mw->is_floating &&
                desktop_is_visible(mw->desktop_id)) {
                mw->has_applied = false;
                tile_current();
            }
        }
        break;

    case EVENT_OBJECT_STATECHANGE: {
        if (!g.cfg.urgency_enabled) break;
        ManagedWindow *mw = window_find(hwnd);
        if (!mw || mw->urgent) break;
        if (hwnd == GetForegroundWindow()) break;
        mw->urgent = true;
        log_msg(LOG_INFO, L"urgent: a window asked for attention");
        bar_refresh();
        break;
    }

    case EVENT_SYSTEM_FOREGROUND:
        window_resink();

        if (IsWindow(hwnd) && window_index_of(hwnd) < 0) window_manage(hwnd);

        if (IsWindow(hwnd) && window_index_of(hwnd) >= 0) {
            ManagedWindow *mw = window_find(hwnd);
            if (mw && !desktop_is_visible(mw->desktop_id)) {
                if (foreground_bounce_allowed(hwnd)) {
                    HWND back = desktop_get_focused();
                    if (back && back != hwnd && IsWindow(back))
                        window_focus(back);
                }
                break;
            }
            desktop_focus_update(hwnd);
            int mon = desktop_monitor_of_window(mw);
            if (mon >= 0 && mon < g.monitor_count) {
                g.focused_monitor = mon;
                desktop_sync_current();
            }
            window_raise_floats();
            border_refresh();
        }
        break;

    case EVENT_OBJECT_LOCATIONCHANGE:
        {
            ManagedWindow *mw = window_find(hwnd);
            if (!mw || !desktop_is_visible(mw->desktop_id)) break;

            if (mw->stashed || mw->sunk) break;

            if (mw->is_floating) {
                window_float_moved(mw);

                if (mw->no_decor || mw->fullscreen) window_reassert_rule(hwnd);

                if (hwnd == desktop_get_focused()) border_refresh();
                break;
            }

            if (mw->fs_mode == FS_WINDOW) {
                if (!window_covers_monitor(hwnd)) {
                    mw->has_applied = false;
                    window_park_over_monitor(hwnd);
                }
                break;
            }
            if (mw->fs_mode == FS_BOTH) break;

            if (g.cfg.fullscreen_policy == FS_BOTH || mw->app_fullscreen) {
                bool covers = (g.cfg.fullscreen_policy == FS_BOTH) &&
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
                if (covers) break;
            }

            if (IsZoomed(hwnd)) {
                events_suppress_begin();
                ShowWindow(hwnd, SW_RESTORE);
                events_suppress_end();
                mw->has_applied = false;
                tile_current();
                break;
            }

            if (anim_is_animating(hwnd)) break;

            if (mw->has_applied) {
                RECT cur;
                RECT a = mw->applied_rect;
                if (window_frame_rect(hwnd, &cur)) {
                    const int EPS = 4;
                    int dx = abs((int)(cur.left - a.left));
                    int dy = abs((int)(cur.top  - a.top));
                    int dw = abs((int)((cur.right - cur.left) - (a.right - a.left)));
                    int dh = abs((int)((cur.bottom - cur.top) - (a.bottom - a.top)));
                    if (dx <= EPS && dy <= EPS && dw <= EPS && dh <= EPS) {
                        mw->snap_tries = 0;
                        break;
                    }
                }
                mw->has_applied = false;
            }

            {
                ULONGLONG now = GetTickCount64();
                if (now - mw->snap_first_at > 1000) {
                    mw->snap_first_at = now;
                    mw->snap_tries    = 0;
                }
                if (++mw->snap_tries > 3) {
                    if (mw->snap_tries == 4)
                        log_msg(LOG_WARN, L"a window will not stay where the "
                                          L"layout puts it: %p — it has a "
                                          L"minimum size or re-places itself. "
                                          L"Leaving it alone rather than "
                                          L"re-tiling in a loop.",
                                (void *)hwnd);
                    break;
                }
            }

            tile_current();
        }
        break;

    default:
        break;
    }
}

void mouse_drag_begin(HWND hwnd) {
    if (!g.cfg.mouse_enabled) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw || mw->is_floating) return;

    g.drag_hwnd = hwnd;
    GetCursorPos(&g.drag_start);
}

void mouse_drag_end(HWND hwnd) {
    if (!g.cfg.mouse_enabled || g.drag_hwnd != hwnd) { g.drag_hwnd = NULL; return; }
    g.drag_hwnd = NULL;

    POINT drop;
    if (!GetCursorPos(&drop)) { tile_current(); return; }

    Desktop *dt = desktop_current();

    int from = -1, to = -1;
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw || mw->is_floating || !mw->has_applied) continue;

        if (dt->windows[i] == hwnd) { from = i; continue; }

        RECT r = mw->applied_rect;
        if (drop.x >= r.left && drop.x < r.right &&
            drop.y >= r.top  && drop.y < r.bottom)
            to = i;
    }

    if (from >= 0 && to >= 0 && from != to) {
        hwnd_swap(&dt->windows[from], &dt->windows[to]);
        dt->focused = to;
        log_w(L"mouse: swapped tiles %d <-> %d", from, to);
    }

    tile_current();
}

bool events_init(void) {
    g.win_event_hook = SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_HIDE,
        NULL,
        events_win_event_proc,
        0, 0,
        WINEVENT_OUTOFCONTEXT
    );

    if (!g.win_event_hook) {
        log_w(L"SetWinEventHook failed: %lu", GetLastError());
        return false;
    }

    g.location_hook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_LOCATIONCHANGE,
        NULL,
        events_win_event_proc,
        0, 0,
        WINEVENT_OUTOFCONTEXT
    );

    if (!g.location_hook) {
        log_w(L"SetWinEventHook(LOCATIONCHANGE) failed: %lu — tiled windows "
              L"dragged out of place won't snap back", GetLastError());
    }

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

    g.movesize_hook = SetWinEventHook(
        EVENT_SYSTEM_MOVESIZESTART,
        EVENT_SYSTEM_MOVESIZEEND,
        NULL,
        events_win_event_proc,
        0, 0,
        WINEVENT_OUTOFCONTEXT
    );

    if (!g.movesize_hook)
        log_w(L"SetWinEventHook(MOVESIZE) failed: %lu — dragging a tiled window "
              L"will not swap it", GetLastError());

    return true;
}

void events_sync_urgency(void) {
    if (g.cfg.urgency_enabled && !g.statechange_hook) {
        g.statechange_hook = SetWinEventHook(
            EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE,
            NULL, events_win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (g.statechange_hook)
            log_msg(LOG_INFO, L"urgency tracking on (STATECHANGE hook)");
        else
            log_msg(LOG_WARN, L"SetWinEventHook(STATECHANGE) failed: %lu — "
                              L"urgency will not be noticed", GetLastError());
    } else if (!g.cfg.urgency_enabled && g.statechange_hook) {
        UnhookWinEvent(g.statechange_hook);
        g.statechange_hook = NULL;
        log_msg(LOG_INFO, L"urgency tracking off");

        for (int i = 0; i < g.managed_count; i++) g.managed[i].urgent = false;
    }
}

void events_shutdown(void) {
    if (g.statechange_hook) {
        UnhookWinEvent(g.statechange_hook);
        g.statechange_hook = NULL;
    }
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
    if (g.location_hook) {
        UnhookWinEvent(g.location_hook);
        g.location_hook = NULL;
    }
    if (g.movesize_hook) {
        UnhookWinEvent(g.movesize_hook);
        g.movesize_hook = NULL;
    }
}
