#include "mshell.h"
#include "window_internal.h"
#include "layout_math.h"
#include "overlay.h"

static HRESULT dwm_set_cloaked(HWND hwnd, bool on) {
    BOOL v = on ? TRUE : FALSE;
    return DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &v, sizeof(v));
}

static bool window_set_cloaked(HWND hwnd, bool on) {
    HRESULT hr = dwm_set_cloaked(hwnd, on);
    if (SUCCEEDED(hr)) return true;
    if (helper_set_cloak(hwnd, on)) return true;

    static bool warned;
    if (!warned) {
        warned = true;
        log_msg(LOG_INFO,
                L"cloak: refused for %p (DwmSetWindowAttribute returned 0x%08lX). "
                L"Expected, and not an integrity boundary the helper can cross: "
                L"DWMWA_CLOAK is owner-only, so no process cloaks another's "
                L"window with it. The shell cloak that Windows' own virtual "
                L"desktops use is IApplicationView::SetCloak, reached through "
                L"CLSID_ImmersiveShell — which explorer.exe registers at "
                L"runtime and mshell replaced, so it is unreachable here "
                L"(tools/probe_shellcloak.c measures this). Hiding sinks the "
                L"window under the backdrop instead, which is what it does by "
                L"default anyway.",
                (void *)hwnd, (unsigned long)hr);
    }
    return false;
}

bool window_on_screen(const ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (mw->wm_hidden || mw->app_hidden) return false;
    return IsWindowVisible(mw->hwnd) != 0;
}

static bool window_sink(ManagedWindow *mw) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg) || !IsWindowVisible(bg)) return false;

    if (mw->made_topmost) {
        mw->made_topmost = false;
        window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
    }
    if (GetWindowLongPtrW(mw->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) return false;

    if (!SetWindowPos(mw->hwnd, bg, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        return false;

    mw->sunk = true;
    return true;
}

static void window_unsink(ManagedWindow *mw) {
    if (!mw->sunk) return;
    mw->sunk = false;
    SetWindowPos(mw->hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

#define STASH_GAP 4000

static bool stash_position(int *x, int *y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    if (vw <= 0) return false;

    *x = vx + vw + STASH_GAP;
    *y = vy;
    return *x < 30000;
}

bool rect_off_screen(RECT r) {
    RECT vs = { GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    vs.right  = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    RECT hit;
    return !IntersectRect(&hit, &r, &vs);
}

static bool window_stash(ManagedWindow *mw) {
    if (IsIconic(mw->hwnd)) return false;

    RECT r;
    int  x, y;
    if (!stash_position(&x, &y) || !GetWindowRect(mw->hwnd, &r)) return false;
    if (rect_off_screen(r)) return false;

    if (window_set_pos(mw->hwnd, x, y, r.right - r.left, r.bottom - r.top,
                       SWP_NOZORDER | SWP_NOACTIVATE) == PLACE_REFUSED)
        return false;

    mw->stash_rect = r;
    mw->stashed    = true;
    return true;
}

static void window_unstash(ManagedWindow *mw) {
    if (!mw->stashed) return;
    mw->stashed = false;

    RECT r = mw->stash_rect;

    if (rect_off_screen(r)) rect_clamp_into_monitor(&r, mw->monitor);

    window_set_pos(mw->hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                   SWP_NOCOPYBITS);
}

static bool window_defer_if_hung(ManagedWindow *mw, const wchar_t *what) {
    if (!IsHungAppWindow(mw->hwnd)) return false;

    mw->vis_deferred = true;

    static bool warned;
    if (!warned) {
        warned = true;
        log_msg(LOG_WARN, L"%ls: %p is not answering — skipped rather than "
                          L"blocking the shell on it. window_verify_visibility "
                          L"retries once its thread starts pumping again.",
                what, (void *)mw->hwnd);
    }
    return true;
}

typedef enum {
    HIDE_BY_SINK = 0,
    HIDE_BY_CLOAK,
    HIDE_BY_STASH,
    HIDE_BY_SW_HIDE,
    HIDE_STRATEGY_COUNT
} HideStrategyId;

typedef struct {
    const wchar_t *name;
    bool (*try_hide)(ManagedWindow *mw);
    void (*try_show)(ManagedWindow *mw, bool force);
    bool (*in_effect)(const ManagedWindow *mw);
} HideStrategy;

static bool sink_try_hide(ManagedWindow *mw) { return window_sink(mw); }

static void sink_try_show(ManagedWindow *mw, bool force) {
    (void)force;
    window_unsink(mw);
}

static bool sink_in_effect(const ManagedWindow *mw) { return mw->sunk; }

static bool cloak_try_hide(ManagedWindow *mw) {
    mw->cloaked = window_set_cloaked(mw->hwnd, true);
    return mw->cloaked;
}

static void cloak_try_show(ManagedWindow *mw, bool force) {
    if (!mw->cloaked && !force) return;
    if (!window_set_cloaked(mw->hwnd, false) && !force)
        log_msg(LOG_WARN, L"show: could not uncloak %p — the window stays "
                          L"invisible", (void *)mw->hwnd);
    mw->cloaked = false;
}

static bool cloak_in_effect(const ManagedWindow *mw) { return mw->cloaked; }

static bool stash_try_hide(ManagedWindow *mw) {
    mw->stashed = window_stash(mw);
    return mw->stashed;
}

static void stash_try_show(ManagedWindow *mw, bool force) {
    (void)force;
    window_unstash(mw);
}

static bool stash_in_effect(const ManagedWindow *mw) { return mw->stashed; }

static bool sw_try_hide(ManagedWindow *mw) {
    ShowWindow(mw->hwnd, SW_HIDE);
    return !IsWindowVisible(mw->hwnd);
}

static void sw_try_show(ManagedWindow *mw, bool force) {
    if (IsWindowVisible(mw->hwnd)) return;
    ShowWindow(mw->hwnd, IsIconic(mw->hwnd) ? SW_SHOWMINNOACTIVE
                                            : force ? SW_SHOWNA
                                                    : SW_SHOWNOACTIVATE);
}

static bool sw_in_effect(const ManagedWindow *mw) {
    return window_hidden_by_showwindow(mw);
}

static const HideStrategy hide_strategies[HIDE_STRATEGY_COUNT] = {
    [HIDE_BY_SINK]    = { L"sunk",    sink_try_hide,  sink_try_show,  sink_in_effect  },
    [HIDE_BY_CLOAK]   = { L"cloaked", cloak_try_hide, cloak_try_show, cloak_in_effect },
    [HIDE_BY_STASH]   = { L"stashed", stash_try_hide, stash_try_show, stash_in_effect },
    [HIDE_BY_SW_HIDE] = { L"SW_HIDE", sw_try_hide,    sw_try_show,    sw_in_effect    },
};

static bool hide_off_screen_without_showwindow(const ManagedWindow *mw) {
    for (int i = 0; i < HIDE_BY_SW_HIDE; i++)
        if (hide_strategies[i].in_effect(mw)) return true;
    return false;
}

static const wchar_t *hide_strategy_name(const ManagedWindow *mw) {
    for (int i = 0; i < HIDE_STRATEGY_COUNT; i++)
        if (hide_strategies[i].in_effect(mw)) return hide_strategies[i].name;
    return L"on screen";
}

static void hide_undo_all(ManagedWindow *mw, bool force) {
    for (int i = 0; i < HIDE_BY_SW_HIDE; i++)
        hide_strategies[i].try_show(mw, force);
}

void window_hide(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return;
    if (mw->wm_hidden) return;
    if (mw->app_hidden) return;
    if (window_defer_if_hung(mw, L"hide")) return;

    mw->wm_hidden = true;
    mw->cloaked   = false;
    mw->sunk      = false;
    mw->stashed   = false;

    if (g.hide_policy == HIDE_CLOAK) {
        for (int i = 0; i < HIDE_BY_SW_HIDE; i++)
            if (hide_strategies[i].try_hide(mw)) break;
    }

    if (!hide_off_screen_without_showwindow(mw) &&
        !hide_strategies[HIDE_BY_SW_HIDE].try_hide(mw)) {
        mw->wm_hidden = false;
        static bool warned;
        if (!warned) {
            warned = true;
            log_err(L"hide: %p could not be taken off the screen by any "
                    L"means — not sunk, not cloaked, not stashed, and "
                    L"SW_HIDE was refused. It will be visible on every "
                    L"desktop. This is what mshelld.exe exists for: run "
                    L"`install.bat /helper` from an administrator prompt "
                    L"(see INSTALL.md).", (void *)mw->hwnd);
        }
        return;
    }

    mw->vis_deferred = false;

    log_msg(LOG_DEBUG, L"hide: %p (%ls)", (void *)mw->hwnd,
            hide_strategy_name(mw));
}

void window_show(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return;
    if (mw->app_hidden) return;
    if (window_defer_if_hung(mw, L"show")) return;

    bool was_off_screen = mw->wm_hidden || mw->cloaked || mw->sunk ||
                          mw->stashed;
    bool was_stashed    = mw->stashed;
    bool was_sunk       = mw->sunk;
    const wchar_t *was  = hide_strategy_name(mw);

    hide_undo_all(mw, false);

    hide_strategies[HIDE_BY_SW_HIDE].try_show(mw, false);

    if (was_off_screen) {
        RedrawWindow(mw->hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        mw->has_applied   = false;
        mw->needs_repaint = true;

        if (mw->is_floating && !was_stashed && !was_sunk) {
            RECT r;
            if (GetWindowRect(mw->hwnd, &r))
                window_set_pos(mw->hwnd, r.left, r.top,
                               r.right - r.left, r.bottom - r.top,
                               SWP_NOZORDER | SWP_NOACTIVATE |
                               SWP_FRAMECHANGED | SWP_NOCOPYBITS);
            mw->needs_repaint = false;
        } else if (mw->is_floating) {
            mw->needs_repaint = false;
        }

        log_msg(LOG_DEBUG, L"show: %p (was %ls)", (void *)mw->hwnd, was);
    }

    mw->wm_hidden    = false;
    mw->vis_deferred = false;

    if (was_off_screen && mw->is_floating) window_float_keep_reachable(mw);
}

void window_restore_all_visibility(void) {
    int shown = 0;

    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!IsWindow(mw->hwnd)) continue;
        if (mw->app_hidden) continue;

        if (hide_off_screen_without_showwindow(mw) ||
            !IsWindowVisible(mw->hwnd)) shown++;

        hide_undo_all(mw, true);

        mw->wm_hidden = false;

        if (IsWindowVisible(mw->hwnd)) continue;

        hide_strategies[HIDE_BY_SW_HIDE].try_show(mw, true);
        shown++;
    }

    if (shown) log_err(L"shutdown: re-showed %d hidden window(s)", shown);
}

void window_verify_visibility(void) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!mw->vis_deferred) continue;
        if (!IsWindow(mw->hwnd)) { mw->vis_deferred = false; continue; }
        if (IsHungAppWindow(mw->hwnd)) continue;
        if (mw->app_hidden || mw->user_hidden) { mw->vis_deferred = false; continue; }

        bool here = desktop_is_visible(mw->desktop_id);

        events_suppress_begin();
        if (!here)                        window_hide(mw);
        else if (!mw->layout_hidden)      window_show(mw);
        else                              mw->vis_deferred = false;
        events_suppress_end();
    }
}

static void window_rehide_surfaced(void) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg)) return;

    int steps = 0;
    for (HWND h = GetWindow(bg, GW_HWNDPREV);
         h && steps < SINK_WALK_MAX;
         h = GetWindow(h, GW_HWNDPREV), steps++) {
        ManagedWindow *mw = window_find(h);
        if (!mw || !mw->sunk) continue;
        if (desktop_is_visible(mw->desktop_id)) continue;

        log_msg(LOG_WARN, L"sink: %p will not stay under the backdrop — "
                          L"taking it off the screen another way", (void *)h);

        mw->sunk      = false;
        mw->wm_hidden = false;

        events_suppress_begin();
        window_hide(mw);
        events_suppress_end();
    }
}

void window_verify_sink(void) {
    if (window_sink_intact()) return;

    log_msg(LOG_DEBUG, L"sink: a hidden window surfaced above the backdrop — "
                       L"re-asserting");
    window_resink();

    if (!window_sink_intact()) window_rehide_surfaced();
}

static BOOL CALLBACK uncloak_stray_proc(HWND hwnd, LPARAM lp) {
    int *n = (int *)lp;

    if (!IsWindowVisible(hwnd)) return TRUE;

    RECT r;
    if (window_is_manageable(hwnd) && !IsIconic(hwnd) &&
        GetWindowRect(hwnd, &r) && rect_off_screen(r)) {
        RECT area = g.work_area;
        int  w    = r.right - r.left, h = r.bottom - r.top;
        int  aw   = (int)(area.right - area.left);
        int  ah   = (int)(area.bottom - area.top);
        if (aw > 0 && ah > 0 && w > 0 && h > 0) {
            if (w > aw) w = aw;
            if (h > ah) h = ah;
            window_set_pos(hwnd, center_axis(area.left, aw, w),
                           center_axis(area.top, ah, h), w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            (*n)++;
            return TRUE;
        }
    }

    int cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                     &cloaked, sizeof(cloaked))))
        return TRUE;
    if (cloaked != DWM_CLOAKED_SHELL) return TRUE;

    if (window_set_cloaked(hwnd, false)) (*n)++;
    return TRUE;
}

void window_uncloak_strays(void) {
    if (g.test_mode) return;

    int n = 0;
    EnumWindows(uncloak_stray_proc, (LPARAM)&n);
    if (n) log_err(L"startup: recovered %d window(s) a previous mshell left "
                   L"hidden — cloaked, or stashed off every display", n);
}
