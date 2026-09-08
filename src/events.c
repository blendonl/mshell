#include "mshell.h"
#include "snap.h"

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

static void on_object_create(HWND hwnd) {
    if (IsWindow(hwnd) && IsWindowVisible(hwnd)) window_manage(hwnd);
}

static void on_object_destroy(HWND hwnd) {
    window_unmanage(hwnd);
}

static void on_object_show(HWND hwnd) {
    if (!IsWindow(hwnd)) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw) {
        window_manage(hwnd);
        return;
    }
    if (!mw->app_hidden) return;

    mw->app_hidden  = false;
    mw->has_applied = false;

    events_suppress_begin();
    if (desktop_is_visible(mw->desktop_id)) window_show(mw);
    else                                    window_hide(mw);
    events_suppress_end();

    if (desktop_is_visible(mw->desktop_id)) tile_current();
}

static void on_object_hide(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (window_hidden_by_showwindow(mw)) return;
    if (!mw || mw->app_hidden) return;

    mw->app_hidden  = true;
    mw->has_applied = false;
    log_w(L"app hid its own window: %p — leaving the layout", (void *)hwnd);
    if (desktop_is_visible(mw->desktop_id)) tile_current();
}

static void on_movesize_start(HWND hwnd) {
    mouse_drag_begin(hwnd);
}

static void on_movesize_end(HWND hwnd) {
    mouse_drag_end(hwnd);
}

static void on_minimize_end(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (mw && !mw->is_floating && desktop_is_visible(mw->desktop_id)) {
        mw->has_applied = false;
        tile_current();
    }
}

static void on_minimize_start(HWND hwnd) {
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

    on_minimize_end(hwnd);
}

static void on_statechange(HWND hwnd) {
    if (!g.cfg.urgency_enabled) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw || mw->urgent) return;
    if (hwnd == GetForegroundWindow()) return;

    mw->urgent = true;
    log_msg(LOG_INFO, L"urgent: a window asked for attention");
    bar_refresh();
}

static void on_foreground(HWND hwnd) {
    window_resink();

    if (IsWindow(hwnd) && window_index_of(hwnd) < 0) window_manage(hwnd);

    if (!IsWindow(hwnd) || window_index_of(hwnd) < 0) return;

    ManagedWindow *mw = window_find(hwnd);
    if (mw && !desktop_is_visible(mw->desktop_id)) {
        if (foreground_bounce_allowed(hwnd)) {
            HWND back = desktop_get_focused();
            if (back && back != hwnd && IsWindow(back)) window_focus(back);
        }
        return;
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

static bool location_settled_where_placed(ManagedWindow *mw, HWND hwnd) {
    if (!mw->has_applied) return false;

    RECT cur;
    RECT a = mw->applied_rect;
    if (window_frame_rect(hwnd, &cur)) {
        const int EPS = 4;
        int dx = abs((int)(cur.left - a.left));
        int dy = abs((int)(cur.top  - a.top));
        int dw = abs((int)((cur.right - cur.left) - (a.right - a.left)));
        int dh = abs((int)((cur.bottom - cur.top) - (a.bottom - a.top)));
        if (dx <= EPS && dy <= EPS && dw <= EPS && dh <= EPS) {
            snap_backoff_reset(mw);
            return true;
        }
    }
    mw->has_applied = false;
    return false;
}

static void on_locationchange(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !desktop_is_visible(mw->desktop_id)) return;

    if (mw->stashed || mw->sunk) return;

    if (mw->is_floating) {
        window_float_moved(mw);

        if (mw->no_decor || mw->fullscreen) window_reassert_rule(hwnd);

        if (hwnd == desktop_get_focused()) border_refresh();
        return;
    }

    if (mw->fs_mode == FS_WINDOW) {
        if (!window_covers_monitor(hwnd)) {
            mw->has_applied = false;
            window_park_over_monitor(hwnd);
        }
        return;
    }
    if (mw->fs_mode == FS_BOTH) return;

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
            return;
        }
        if (covers) return;
    }

    if (IsZoomed(hwnd)) {
        events_suppress_begin();
        ShowWindow(hwnd, SW_RESTORE);
        events_suppress_end();
        mw->has_applied = false;
        tile_current();
        return;
    }

    if (anim_is_animating(hwnd)) return;

    if (location_settled_where_placed(mw, hwnd)) return;

    SnapVerdict verdict = snap_backoff(mw);
    if (verdict != SNAP_KEEP_TRYING) {
        if (verdict == SNAP_GIVE_UP_LOUDLY)
            log_msg(LOG_WARN, L"a window will not stay where the "
                              L"layout puts it: %p — it has a "
                              L"minimum size or re-places itself. "
                              L"Leaving it alone rather than "
                              L"re-tiling in a loop.", (void *)hwnd);
        return;
    }

    tile_current();
}

typedef struct {
    DWORD event;
    void (*handler)(HWND hwnd);
} EventEntry;

static const EventEntry event_handlers[] = {
    { EVENT_OBJECT_LOCATIONCHANGE,  on_locationchange   },
    { EVENT_SYSTEM_FOREGROUND,      on_foreground       },
    { EVENT_OBJECT_CREATE,          on_object_create    },
    { EVENT_OBJECT_DESTROY,         on_object_destroy   },
    { EVENT_OBJECT_SHOW,            on_object_show      },
    { EVENT_OBJECT_HIDE,            on_object_hide      },
    { EVENT_OBJECT_STATECHANGE,     on_statechange      },
    { EVENT_SYSTEM_MOVESIZESTART,   on_movesize_start   },
    { EVENT_SYSTEM_MOVESIZEEND,     on_movesize_end     },
    { EVENT_SYSTEM_MINIMIZESTART,   on_minimize_start   },
    { EVENT_SYSTEM_MINIMIZEEND,     on_minimize_end     },
};

void CALLBACK events_win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD idEventThread, DWORD dwmsEventTime) {
    (void)hook;
    (void)idEventThread;
    (void)dwmsEventTime;

    if (events_suppressed()) return;

    if (idObject != OBJID_WINDOW) return;
    if (idChild  != CHILDID_SELF) return;

    for (size_t i = 0; i < sizeof event_handlers / sizeof event_handlers[0]; i++) {
        if (event_handlers[i].event != event) continue;
        event_handlers[i].handler(hwnd);
        return;
    }
}

typedef struct {
    DWORD          event_min, event_max;
    HWINEVENTHOOK *slot;
    const wchar_t *name;
    const wchar_t *degradation;
} EventHookSpec;

static const EventHookSpec event_hooks[] = {
    { EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, &g.win_event_hook,
      NULL, NULL },

    { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
      &g.location_hook, L"LOCATIONCHANGE",
      L"tiled windows dragged out of place won't snap back" },

    { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, &g.foreground_hook,
      L"FOREGROUND",
      L"focus tracking is degraded (mouse-driven focus won't be seen)" },

    { EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, &g.minimize_hook,
      L"MINIMIZE", L"minimized windows will keep an empty tile" },

    { EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, &g.movesize_hook,
      L"MOVESIZE", L"dragging a tiled window will not swap it" },
};

bool events_init(void) {
    for (size_t i = 0; i < sizeof event_hooks / sizeof event_hooks[0]; i++) {
        const EventHookSpec *h = &event_hooks[i];

        *h->slot = SetWinEventHook(h->event_min, h->event_max, NULL,
                                   events_win_event_proc, 0, 0,
                                   WINEVENT_OUTOFCONTEXT);
        if (*h->slot) continue;

        if (!h->degradation) {
            log_w(L"SetWinEventHook failed: %lu", GetLastError());
            return false;
        }

        log_w(L"SetWinEventHook(%ls) failed: %lu — %ls",
              h->name, GetLastError(), h->degradation);
    }

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

    for (size_t i = 0; i < sizeof event_hooks / sizeof event_hooks[0]; i++) {
        HWINEVENTHOOK *slot = event_hooks[i].slot;
        if (!*slot) continue;
        UnhookWinEvent(*slot);
        *slot = NULL;
    }
}
