#include "mshell.h"
#include "window_internal.h"
#include "snap.h"
#include "layout_math.h"

#define SNAP_WINDOW_MS 1000
#define SNAP_MAX_TRIES 3

SnapVerdict snap_backoff(ManagedWindow *mw) {
    ULONGLONG now = GetTickCount64();

    if (now - mw->snap_first_at > SNAP_WINDOW_MS) {
        mw->snap_first_at = now;
        mw->snap_tries    = 0;
    }

    if (++mw->snap_tries <= SNAP_MAX_TRIES) return SNAP_KEEP_TRYING;

    return (mw->snap_tries == SNAP_MAX_TRIES + 1) ? SNAP_GIVE_UP_LOUDLY
                                                  : SNAP_GIVE_UP_QUIETLY;
}

void snap_backoff_reset(ManagedWindow *mw) {
    mw->snap_tries = 0;
}

bool window_frame_rect(HWND hwnd, RECT *out) {
    if (!hwnd || !out) return false;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        out, sizeof(*out))))
        return true;
    return GetWindowRect(hwnd, out) != 0;
}

RECT window_adjust_for_frame(HWND hwnd, RECT want) {
    RECT wr, fr;
    if (GetWindowRect(hwnd, &wr) &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &fr, sizeof(fr)))) {
        int l = fr.left   - wr.left;
        int t = fr.top    - wr.top;
        int r = wr.right  - fr.right;
        int b = wr.bottom - fr.bottom;
        if (l < 0 || l > 64) l = 0;
        if (t < 0 || t > 64) t = 0;
        if (r < 0 || r > 64) r = 0;
        if (b < 0 || b > 64) b = 0;
        want.left   -= l;
        want.top    -= t;
        want.right  += r;
        want.bottom += b;
    }
    return want;
}

PlaceResult window_set_pos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
    if (SetWindowPos(hwnd, NULL, x, y, w, h, flags)) return PLACE_OK;

    DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        log_w(L"SetWindowPos(%p) failed: %lu", (void *)hwnd, err);
        return PLACE_REFUSED;
    }

    return helper_set_window_pos(hwnd, x, y, w, h, flags) ? PLACE_VIA_HELPER
                                                          : PLACE_REFUSED;
}

static void window_placement_refused(ManagedWindow *mw) {
    if (mw->place_refused) return;
    mw->place_refused = true;

    if (!mw->is_floating && helper_available()) {
        log_msg(LOG_WARN, L"placement refused for %p with mshelld.exe connected "
                          L"— leaving it in the layout", (void *)mw->hwnd);
        return;
    }

    if (!mw->is_floating && g.cfg.float_policy == FLOAT_NEVER) {
        log_msg(LOG_WARN, L"%p cannot be placed (UIPI) and float_policy is "
                          L"'never' — it stays in the layout without moving",
                (void *)mw->hwnd);
        return;
    }

    if (!mw->is_floating) {
        log_err(L"%p belongs to a higher-integrity process and cannot be placed "
                L"— floating it. Run `install.bat /helper` from an administrator "
                L"prompt to tile it instead (see INSTALL.md).", (void *)mw->hwnd);
        window_set_floating(mw->hwnd, true);
    }
}

#define DPI_SETTLE_TRIES 4
#define DPI_SETTLE_TICKS 4
#define PLACE_SETTLE_EPS 4

static bool rect_settled_at(RECT got, RECT want) {
    return abs((int)(got.left - want.left)) <= PLACE_SETTLE_EPS &&
           abs((int)(got.top  - want.top))  <= PLACE_SETTLE_EPS &&
           abs((int)((got.right  - got.left) -
                     (want.right  - want.left)))  <= PLACE_SETTLE_EPS &&
           abs((int)((got.bottom - got.top) -
                     (want.bottom - want.top)))   <= PLACE_SETTLE_EPS;
}

bool window_placement_crosses_dpi(HWND hwnd, RECT want) {
    if (!hwnd) return false;
    HMONITOR from = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    HMONITOR to   = MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST);
    if (!from || !to || from == to) return false;
    return monitor_dpi_of(from) != monitor_dpi_of(to);
}

static int monitor_of_rect(RECT r) {
    HMONITOR hmon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < g.monitor_count; i++)
        if (g.monitors[i].handle == hmon) return i;
    return g.primary_monitor;
}

static void window_settle_onto_monitor(HWND hwnd, RECT want, UINT flags) {
    RECT got;
    if (!window_frame_rect(hwnd, &got)) return;

    RECT fixed = got;
    if (!rect_clamp_into_monitor(&fixed, monitor_of_rect(want))) return;
    if (fixed.left == got.left && fixed.top == got.top) return;

    RECT adj = window_adjust_for_frame(hwnd, fixed);
    window_set_pos(hwnd, adj.left, adj.top, 0, 0,
                   (flags & ~(UINT)SWP_NOMOVE) | SWP_NOSIZE);

    log_msg(LOG_INFO, L"%p kept its own size and was moved back onto monitor "
                      L"%d at %ld,%ld", (void *)hwnd, monitor_of_rect(want),
            (long)fixed.left, (long)fixed.top);
}

PlaceResult window_place_settled(HWND hwnd, RECT want, UINT flags) {
    if (!hwnd || !IsWindow(hwnd)) return PLACE_REFUSED;

    int tries = 1;
    if (window_placement_crosses_dpi(hwnd, want)) {
        tries  = DPI_SETTLE_TRIES;
        flags |= SWP_NOCOPYBITS;
    }

    PlaceResult res       = PLACE_REFUSED;
    bool        unsettled = true;

    for (int i = 0; i < tries; i++) {
        RECT adj = window_adjust_for_frame(hwnd, want);
        res = window_set_pos(hwnd, adj.left, adj.top, adj.right - adj.left,
                             adj.bottom - adj.top, flags);
        if (res == PLACE_REFUSED) return res;
        if (tries == 1) break;

        RECT got;
        if (!window_frame_rect(hwnd, &got) || rect_settled_at(got, want)) {
            unsettled = false;
            break;
        }
    }

    if (tries > 1) {
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        if (unsettled) {
            log_msg(LOG_WARN, L"%p would not settle at %ldx%ld after crossing "
                              L"to a display of a different scale — it keeps "
                              L"re-sizing itself to the DPI it was given.",
                    (void *)hwnd, (long)(want.right - want.left),
                    (long)(want.bottom - want.top));
            window_settle_onto_monitor(hwnd, want, flags);
        }
    }

    return res;
}

PlaceResult window_apply_rect(ManagedWindow *mw, RECT want, UINT flags) {
    HWND hwnd = mw ? mw->hwnd : NULL;
    if (!hwnd || !IsWindow(hwnd)) return PLACE_REFUSED;

    bool crossed_dpi = window_placement_crosses_dpi(hwnd, want);

    PlaceResult res = window_place_settled(hwnd, want, flags);

    if (res == PLACE_REFUSED) {
        window_placement_refused(mw);
        return res;
    }

    mw->applied_rect     = want;
    mw->has_applied      = true;
    mw->place_refused    = false;
    mw->dpi_settle_left  = crossed_dpi ? DPI_SETTLE_TICKS : 0;
    mw->needs_helper  = (res == PLACE_VIA_HELPER);
    return res;
}

void window_center_float(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->is_floating || !mw->center_float) return;

    if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

    if (mw->fullscreen || window_is_screen_fullscreen(mw)) return;

    RECT cur;
    if (!window_frame_rect(hwnd, &cur)) return;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT area = (g.monitor_count > 0) ? g.monitors[mon].work_area : g.work_area;

    int aw = (int)(area.right - area.left);
    int ah = (int)(area.bottom - area.top);
    int w  = (int)(cur.right - cur.left);
    int h  = (int)(cur.bottom - cur.top);
    if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) return;

    UINT from_dpi = monitor_dpi_of(MonitorFromWindow(hwnd,
                                                     MONITOR_DEFAULTTONEAREST));
    UINT to_dpi   = monitor_dpi(mon);
    if (from_dpi > 0 && to_dpi > 0 && from_dpi != to_dpi) {
        w = MulDiv(w, (int)to_dpi, (int)from_dpi);
        h = MulDiv(h, (int)to_dpi, (int)from_dpi);
    }

    if (w > aw) w = aw;
    if (h > ah) h = ah;

    RECT want = { center_axis(area.left, aw, w), center_axis(area.top, ah, h),
                  0, 0 };
    want.right  = want.left + w;
    want.bottom = want.top  + h;

    events_suppress_begin();
    window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
    events_suppress_end();
}

bool rect_clamp_into_monitor(RECT *r, int mon) {
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;
    RECT area = (mon >= 0 && mon < g.monitor_count) ? g.monitors[mon].work_area
                                                    : g.work_area;

    int aw = (int)(area.right - area.left);
    int ah = (int)(area.bottom - area.top);
    int w  = (int)(r->right - r->left);
    int h  = (int)(r->bottom - r->top);
    if (aw <= 0 || ah <= 0 || w <= 0 || h <= 0) return false;
    if (w > aw) w = aw;
    if (h > ah) h = ah;

    r->left   = clamp_axis(area.left, aw, r->left, w);
    r->top    = clamp_axis(area.top,  ah, r->top,  h);
    r->right  = r->left + w;
    r->bottom = r->top  + h;
    return true;
}

#define FLOAT_REACHABLE_MIN 48

void window_float_keep_reachable(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd) || IsIconic(mw->hwnd)) return;
    if (mw->stashed || mw->fullscreen || window_is_screen_fullscreen(mw)) return;

    RECT r;
    if (!window_frame_rect(mw->hwnd, &r)) return;

    int mon = window_home_monitor(mw);
    if (mon < 0) mon = monitor_of_window(mw->hwnd);
    if (mon < 0 || mon >= g.monitor_count) return;

    RECT area = g.monitors[mon].work_area;
    RECT hit;
    bool grabbable = IntersectRect(&hit, &r, &area) &&
                     (hit.right - hit.left) >= FLOAT_REACHABLE_MIN &&
                     (hit.bottom - hit.top) >= FLOAT_REACHABLE_MIN;

    if (r.top >= area.top && grabbable) return;

    RECT fixed = r;
    if (!rect_clamp_into_monitor(&fixed, mon)) return;
    if (fixed.left == r.left && fixed.top == r.top) return;

    events_suppress_begin();
    window_apply_rect(mw, fixed, SWP_NOZORDER | SWP_NOACTIVATE);
    events_suppress_end();

    log_msg(LOG_INFO, L"%p came back at %ld,%ld, off monitor %d — moved to "
                      L"%ld,%ld so it can be reached", (void *)mw->hwnd,
            (long)r.left, (long)r.top, mon,
            (long)fixed.left, (long)fixed.top);
}

bool window_clamp_into_monitor(ManagedWindow *mw, int mon) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (IsIconic(mw->hwnd)) return false;

    if (mw->stashed) {
        rect_clamp_into_monitor(&mw->stash_rect, mon);
        return false;
    }

    RECT cur;
    if (!window_frame_rect(mw->hwnd, &cur)) return false;
    if (!rect_clamp_into_monitor(&cur, mon)) return false;

    events_suppress_begin();
    window_apply_rect(mw, cur,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();
    return true;
}

bool window_rescue_offscreen(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (IsIconic(mw->hwnd)) return false;
    if (mw->stashed) return false;

    RECT cur;
    if (!window_frame_rect(mw->hwnd, &cur)) return false;
    if (!rect_off_screen(cur)) return false;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;

    if (!window_clamp_into_monitor(mw, mon)) return false;

    log_msg(LOG_INFO, L"rescued %p from off-screen onto monitor %d",
            (void *)mw->hwnd, mon);
    return true;
}

void window_park_over_monitor(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT want = (g.monitor_count > 0) ? g.monitors[mon].full : g.work_area;

    events_suppress_begin();

    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    window_apply_rect(mw, want,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    events_suppress_end();
}

void window_apply_fullscreen(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->fullscreen || !mw->is_floating) return;
    window_park_over_monitor(hwnd);
}

void window_park_float_if_fullscreen(ManagedWindow *mw) {
    if (!mw || !mw->is_floating) return;
    if (!mw->fullscreen && !window_is_screen_fullscreen(mw)) return;

    if (!mw->fullscreen && !mw->fs_has_prev)
        mw->fs_has_prev = window_frame_rect(mw->hwnd, &mw->fs_prev_rect);

    window_park_over_monitor(mw->hwnd);
}

void window_place_float(ManagedWindow *mw) {
    if (!mw || !mw->is_floating) return;

    if (mw->fullscreen || window_is_screen_fullscreen(mw)) {
        window_park_float_if_fullscreen(mw);
        return;
    }

    window_center_float(mw->hwnd);
}

int window_home_monitor(const ManagedWindow *mw) {
    if (!mw) return -1;

    int shown = desktop_monitor_showing(mw->desktop_id);
    if (shown >= 0 && shown < g.monitor_count) return shown;

    const Desktop *dt = desktop_by_id(mw->desktop_id);
    if (dt && dt->monitor >= 0 && dt->monitor < g.monitor_count)
        return dt->monitor;

    return -1;
}

bool window_follow_monitor(ManagedWindow *mw, int mon) {
    if (!mw || !IsWindow(mw->hwnd)) return false;

    window_set_monitor(mw, mon);
    if (!mw->is_floating) return false;

    mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) return false;

    if (mw->stashed) return window_clamp_into_monitor(mw, mon);
    if (IsIconic(mw->hwnd)) return false;

    if (mw->fullscreen || window_is_screen_fullscreen(mw)) {
        if (monitor_of_window(mw->hwnd) == mon &&
            window_covers_monitor(mw->hwnd)) return false;
        window_park_over_monitor(mw->hwnd);
        return true;
    }

    if (monitor_of_window(mw->hwnd) == mon) return false;
    return window_clamp_into_monitor(mw, mon);
}

void window_float_moved(ManagedWindow *mw) {
    if (!mw || !mw->is_floating || !IsWindow(mw->hwnd)) return;
    if (IsIconic(mw->hwnd)) return;

    int at   = monitor_of_window(mw->hwnd);
    int home = mw->fullscreen ? window_home_monitor(mw) : -1;

    if (home < 0 || at == home) {
        window_set_monitor(mw, at);
        return;
    }

    SnapVerdict verdict = snap_backoff(mw);
    if (verdict != SNAP_KEEP_TRYING) {
        if (verdict == SNAP_GIVE_UP_LOUDLY)
            log_msg(LOG_WARN, L"%p keeps putting itself on monitor %d when its "
                              L"desktop is on %d — leaving it there rather "
                              L"than fighting it.", (void *)mw->hwnd, at, home);
        window_set_monitor(mw, at);
        return;
    }

    window_follow_monitor(mw, home);
}

void fs_forget_prev(ManagedWindow *mw) {
    if (mw) mw->fs_has_prev = false;
}

bool window_covers_monitor(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    RECT cur;
    if (!mw || !window_frame_rect(hwnd, &cur)) return false;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT full = (g.monitor_count > 0) ? g.monitors[mon].full : g.work_area;

    const int EPS = 8;
    return cur.left   <= full.left   + EPS && cur.top    <= full.top    + EPS &&
           cur.right  >= full.right  - EPS && cur.bottom >= full.bottom - EPS;
}

static void fs_restore_floating(ManagedWindow *mw) {
    if (!mw || !mw->is_floating || !mw->fs_has_prev || !IsWindow(mw->hwnd)) return;

    RECT p = mw->fs_prev_rect;

    events_suppress_begin();
    window_apply_rect(mw, p,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();

    mw->fs_has_prev = false;
}

void window_set_fullscreen(HWND hwnd, FullscreenMode mode) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    FullscreenMode next = (mw->fs_mode == mode) ? FS_OFF : mode;

    if (mw->is_floating && !mw->fs_has_prev &&
        (next == FS_WINDOW || next == FS_BOTH))
        mw->fs_has_prev = window_frame_rect(hwnd, &mw->fs_prev_rect);

    if (next == FS_WINDOW || next == FS_BOTH) {
        for (int i = 0; i < g.managed_count; i++) {
            ManagedWindow *o = &g.managed[i];
            if (o == mw || o->desktop_id != mw->desktop_id ||
                o->monitor != mw->monitor)
                continue;
            if (window_is_screen_fullscreen(o)) {
                o->fs_mode        = FS_OFF;
                o->app_fullscreen = false;
                o->has_applied    = false;
                fs_restore_floating(o);
            }
        }
    }

    mw->fs_mode        = next;
    mw->app_fullscreen = false;
    mw->has_applied    = false;

    log_w(L"fullscreen: %p mode=%d (float=%d)", (void *)hwnd, (int)next,
          mw->is_floating);

    if (mw->is_floating) {
        if (next == FS_WINDOW || next == FS_BOTH) {
            window_park_over_monitor(hwnd);
        } else if (next == FS_OFF) {
            fs_restore_floating(mw);
        }
    }

    tile_current();
}

void window_verify_placement(void) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (mw->dpi_settle_left <= 0) continue;

        if (!IsWindow(mw->hwnd) || !mw->has_applied || mw->is_floating ||
            mw->layout_hidden || mw->wm_hidden || mw->app_hidden ||
            mw->user_hidden) {
            mw->dpi_settle_left = 0;
            continue;
        }

        if (IsHungAppWindow(mw->hwnd) || anim_is_animating(mw->hwnd)) continue;

        RECT got;
        if (!window_frame_rect(mw->hwnd, &got)) { mw->dpi_settle_left = 0; continue; }
        if (rect_settled_at(got, mw->applied_rect)) {
            mw->dpi_settle_left = 0;
            continue;
        }

        if (--mw->dpi_settle_left <= 0) {
            log_msg(LOG_WARN, L"%p is still %ldx%ld after crossing to a display "
                              L"of a different scale, where the layout gave it "
                              L"%ldx%ld — recording the size it insists on "
                              L"rather than fighting it.", (void *)mw->hwnd,
                    (long)(got.right - got.left), (long)(got.bottom - got.top),
                    (long)(mw->applied_rect.right - mw->applied_rect.left),
                    (long)(mw->applied_rect.bottom - mw->applied_rect.top));
            mw->applied_rect = got;
            continue;
        }

        window_place_settled(mw->hwnd, mw->applied_rect,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                             SWP_NOCOPYBITS);
    }
}
