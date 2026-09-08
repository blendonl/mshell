#include "mshell.h"

static void desktop_focus_hist_push(Desktop *dt, HWND hwnd);
static void desktop_unlink_at(Desktop *dt, int i);

static void desktop_fill_monitors(void);

static bool desktop_insert_window(Desktop *dt, HWND hwnd, bool focus_it) {
    if (!dt || dt->count >= MAX_WINDOWS_PER_DESKTOP) return false;

    int idx = desktop_attach_index(g.cfg.attach_policy, dt->focused, dt->count);
    if (idx < 0)         idx = 0;
    if (idx > dt->count) idx = dt->count;

    if (idx < dt->count)
        memmove(&dt->windows[idx + 1], &dt->windows[idx],
                (size_t)(dt->count - idx) * sizeof(HWND));

    dt->windows[idx] = hwnd;
    dt->count++;

    if (focus_it)                dt->focused = idx;
    else if (dt->focused >= idx) dt->focused++;

    dt->app_pending = false;
    return true;
}

bool desktop_name_ok(const wchar_t *name) {
    return desktop_list_name_ok(name, DESKTOP_NAME_MAX);
}

int desktop_slot_by_name(const wchar_t *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g.desktop_count; i++)
        if (desktop_name_eq(g.desktops[i].name, name)) return i;
    return -1;
}

int desktop_slot_by_id(int id) {
    if (id <= 0) return -1;
    for (int i = 0; i < g.desktop_count; i++)
        if (g.desktops[i].id == id) return i;
    return -1;
}

Desktop *desktop_by_id(int id) {
    int slot = desktop_slot_by_id(id);
    return (slot >= 0) ? &g.desktops[slot] : NULL;
}

int desktop_current_slot(void) {
    int slot = desktop_slot_by_id(g.current_desktop_id);
    return (slot >= 0) ? slot : 0;
}

Desktop *desktop_current(void) {
    return &g.desktops[desktop_current_slot()];
}

static int desktop_monitor_span(void) {
    int n = (g.monitor_count > 0) ? g.monitor_count : 1;
    return (n > MAX_MONITORS) ? MAX_MONITORS : n;
}

int desktop_on_monitor(int mon) {
    if (mon < 0 || mon >= MAX_MONITORS) return 0;
    int id = g.monitor_desktop[mon];
    return (id > 0 && desktop_slot_by_id(id) >= 0) ? id : 0;
}

int desktop_monitor_showing(int id) {
    if (id <= 0) return -1;
    for (int m = 0; m < desktop_monitor_span(); m++)
        if (g.monitor_desktop[m] == id) return m;
    return -1;
}

bool desktop_is_visible(int id) {
    return desktop_monitor_showing(id) >= 0;
}

int desktop_monitor_of_window(const ManagedWindow *mw) {
    if (!mw) return -1;
    int mon = desktop_monitor_showing(mw->desktop_id);
    if (mon >= 0) return mon;
    return (mw->monitor >= 0 && mw->monitor < g.monitor_count) ? mw->monitor : -1;
}

int desktop_target_monitor(const Desktop *dt) {
    if (dt && dt->monitor >= 0 && dt->monitor < g.monitor_count)
        return dt->monitor;

    const ManagedWindow *fg = window_find(GetForegroundWindow());
    int mon = fg ? desktop_monitor_showing(fg->desktop_id) : -1;
    if (mon < 0) mon = g.focused_monitor;
    if (mon < 0 || mon >= desktop_monitor_span()) mon = 0;
    return mon;
}

void desktop_place_on_monitor(int id, int mon) {
    if (mon < 0 || mon >= MAX_MONITORS) return;
    g.monitor_desktop[mon] = id;

    Desktop *dt = desktop_by_id(id);
    if (!dt) return;
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (mw) window_follow_monitor(mw, mon);
    }
}

void desktop_sync_current(void) {
    int mon = g.focused_monitor;
    if (mon >= 0 && mon < desktop_monitor_span()) {
        int id = desktop_on_monitor(mon);
        if (id) { g.current_desktop_id = id; return; }
    }
    for (int m = 0; m < desktop_monitor_span(); m++) {
        int id = desktop_on_monitor(m);
        if (id) { g.current_desktop_id = id; return; }
    }
    if (desktop_slot_by_id(g.current_desktop_id) < 0 && g.desktop_count > 0)
        g.current_desktop_id = g.desktops[0].id;
}

int desktop_of_window(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    return mw ? mw->desktop_id : 0;
}

static void desktop_resolve_monitor(Desktop *dt) {
    dt->monitor = -1;
    for (int i = 0; i < g.cfg.desktop_rule_count; i++) {
        const DesktopRule *r = &g.cfg.desktop_rules[i];
        if (!r->set_monitor) continue;
        if (r->name_match[0] && !wildcard_match(r->name_match, dt->name)) continue;
        dt->monitor = r->monitor;
    }

    if (dt->monitor_override >= 0) dt->monitor = dt->monitor_override;

    if (dt->monitor >= g.monitor_count) dt->monitor = -1;
    if (dt->monitor < 0) return;

    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw) continue;
        window_follow_monitor(mw, dt->monitor);
    }
}

void desktop_monitors_changed(void) {
    for (int i = 0; i < g.desktop_count; i++)
        desktop_resolve_monitor(&g.desktops[i]);

    int span = (g.monitor_count > 0) ? g.monitor_count : 1;
    if (span > MAX_MONITORS) span = MAX_MONITORS;

    for (int m = 0; m < MAX_MONITORS; m++) {
        int id = g.monitor_desktop[m];
        if (id && desktop_slot_by_id(id) < 0) g.monitor_desktop[m] = 0;
    }

    for (int m = span; m < MAX_MONITORS; m++) {
        int id = g.monitor_desktop[m];
        g.monitor_desktop[m] = 0;
        if (!id) continue;

        if (g.monitor_desktop[0] == 0) {
            desktop_place_on_monitor(id, 0);
            continue;
        }
        Desktop *dt = desktop_by_id(id);
        if (!dt) continue;
        events_suppress_begin();
        for (int i = 0; i < dt->count; i++)
            window_hide(window_find(dt->windows[i]));
        events_suppress_end();
    }

    for (int m = 0; m < span; m++) {
        int id = desktop_on_monitor(m);
        if (id) desktop_place_on_monitor(id, m);
    }

    desktop_fill_monitors();

    if (g.focused_monitor >= span) g.focused_monitor = 0;
    desktop_sync_current();
}

bool desktop_set_monitor(int slot, int mon) {
    if (slot < 0 || slot >= g.desktop_count) return false;
    if (mon >= g.monitor_count) return false;

    Desktop *dt = &g.desktops[slot];
    dt->monitor_override = (mon < 0) ? -1 : mon;

    desktop_resolve_monitor(dt);

    int here = desktop_monitor_showing(dt->id);
    int want = desktop_target_monitor(dt);
    if (here >= 0 && want != here) {
        desktop_place_on_monitor(desktop_on_monitor(want), here);
        desktop_place_on_monitor(dt->id, want);
        if (g.focused_monitor == here) g.focused_monitor = want;
        desktop_sync_current();
        tile_current();
        mouse_warp_focus();
    } else if (dt->id == g.current_desktop_id) {
        tile_current();
    }

    if (dt->monitor >= 0)
        log_w(L"desktop: '%ls' pinned to monitor %d", dt->name, dt->monitor);
    else
        log_w(L"desktop: '%ls' is no longer pinned to a monitor", dt->name);
    return true;
}

void desktop_apply_rules(int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;
    Desktop *dt = &g.desktops[slot];

    dt->layout       = g.cfg.default_layout;
    dt->master_ratio = g.cfg.default_master_ratio > 0.f ? g.cfg.default_master_ratio
                                                    : DEFAULT_MASTER_RATIO;
    dt->n_master     = g.cfg.default_nmaster > 0 ? g.cfg.default_nmaster : DEFAULT_NMASTER;
    dt->inner_gap    = -1;
    dt->outer_gap    = -1;
    dt->float_all    = false;
    dt->app[0]       = L'\0';
    dt->app_args[0]  = L'\0';
    dt->app_cwd[0]   = L'\0';

    for (int i = 0; i < g.cfg.desktop_rule_count; i++) {
        const DesktopRule *r = &g.cfg.desktop_rules[i];
        if (r->name_match[0] && !wildcard_match(r->name_match, dt->name)) continue;

        if (r->app[0]) {
            wcsncpy(dt->app, r->app, MAX_PATH - 1);
            dt->app[MAX_PATH - 1] = L'\0';
            wcsncpy(dt->app_args, r->app_args, SPAWN_ARGS_MAX - 1);
            dt->app_args[SPAWN_ARGS_MAX - 1] = L'\0';
            wcsncpy(dt->app_cwd, r->app_cwd, MAX_PATH - 1);
            dt->app_cwd[MAX_PATH - 1] = L'\0';
        }
        if (r->set_float)   dt->float_all    = r->float_all;
        if (r->set_layout)  dt->layout       = r->layout;
        if (r->set_ratio)   dt->master_ratio = r->master_ratio;
        if (r->set_nmaster) dt->n_master     = r->n_master;
        if (r->set_gaps) { dt->inner_gap = r->inner_gap;
                           dt->outer_gap = r->outer_gap; }
    }

    desktop_resolve_monitor(dt);
}

static int desktop_sort_in(int slot) {
    while (slot > 0 && desktop_name_cmp(g.desktops[slot - 1].name,
                                        g.desktops[slot].name) > 0) {
        Desktop tmp          = g.desktops[slot - 1];
        g.desktops[slot - 1] = g.desktops[slot];
        g.desktops[slot]     = tmp;
        slot--;
    }
    return slot;
}

static int desktop_create(const wchar_t *name) {
    if (!desktop_name_ok(name)) {
        log_err(L"desktop: '%ls' is not a usable desktop name (it must be "
                L"non-empty, whitespace-free and under %d characters)",
                name ? name : L"(null)", DESKTOP_NAME_MAX);
        return -1;
    }
    if (g.desktop_count >= MAX_DESKTOPS) {
        log_err(L"desktop: cannot create '%ls' — %d desktops already exist, "
                L"which is the maximum. Close the windows on one you aren't "
                L"using and it will disappear by itself.", name, MAX_DESKTOPS);
        return -1;
    }

    int slot = g.desktop_count++;
    Desktop *dt = &g.desktops[slot];
    memset(dt, 0, sizeof(*dt));
    dt->monitor_override = -1;

    int id = ++g.next_desktop_id;
    dt->id = id;
    wcsncpy(dt->name, name, DESKTOP_NAME_MAX - 1);
    dt->name[DESKTOP_NAME_MAX - 1] = L'\0';

    slot = desktop_sort_in(slot);
    desktop_apply_rules(slot);

    log_w(L"desktop: created '%ls' (id %d, %d alive)", name, id,
          g.desktop_count);
    return slot;
}

int desktop_ensure(const wchar_t *name) {
    int slot = desktop_slot_by_name(name);
    return (slot >= 0) ? slot : desktop_create(name);
}

static void desktop_auto_name(wchar_t *out, size_t cap) {
    for (int n = 1; n <= MAX_DESKTOPS + 1; n++) {
        _snwprintf(out, cap, L"%d", n);
        out[cap - 1] = L'\0';
        if (desktop_slot_by_name(out) < 0) return;
    }
}

static void desktop_fill_monitors(void) {
    for (int m = 0; m < desktop_monitor_span(); m++) {
        if (desktop_on_monitor(m)) continue;

        wchar_t name[DESKTOP_NAME_MAX];
        desktop_auto_name(name, DESKTOP_NAME_MAX);

        int slot = desktop_create(name);
        if (slot < 0) return;
        desktop_place_on_monitor(g.desktops[slot].id, m);
    }
}

void desktop_gc(int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;

    Desktop *dt = &g.desktops[slot];
    if (dt->count > 0)                  return;
    if (dt->id == g.current_desktop_id) return;
    if (desktop_is_visible(dt->id))     return;

    log_w(L"desktop: destroyed '%ls' (id %d, empty)", dt->name, dt->id);

    layout_tree_forget(dt->id);

    memmove(&g.desktops[slot], &g.desktops[slot + 1],
            (size_t)(g.desktop_count - slot - 1) * sizeof(Desktop));
    g.desktop_count--;
    memset(&g.desktops[g.desktop_count], 0, sizeof(Desktop));
}

void desktop_init(void) {
    const wchar_t *name = g.cfg.start_desktop[0] ? g.cfg.start_desktop
                                             : DEFAULT_START_DESKTOP;

    g.desktop_count      = 0;
    g.current_desktop_id = 0;

    int slot = desktop_create(name);
    if (slot < 0) slot = desktop_create(DEFAULT_START_DESKTOP);
    if (slot < 0) return;

    memset(g.monitor_desktop, 0, sizeof g.monitor_desktop);

    int mon = desktop_target_monitor(&g.desktops[slot]);
    desktop_place_on_monitor(g.desktops[slot].id, mon);
    g.focused_monitor = mon;

    g.current_desktop_id = g.desktops[slot].id;
    wcscpy(g.last_desktop, g.desktops[slot].name);

    desktop_fill_monitors();
}

void desktop_reapply(void) {
    for (int d = 0; d < g.desktop_count; d++)
        desktop_apply_rules(d);

    events_suppress_begin();
    for (int d = 0; d < g.desktop_count; d++) {
        Desktop *dt = &g.desktops[d];
        for (int i = 0; i < dt->count; i++) {
            HWND h = dt->windows[i];
            if (!h || !IsWindow(h)) continue;
            ManagedWindow *mw = window_find(h);
            if (!mw) continue;
            if (mw->user_hidden) continue;
            if (desktop_is_visible(dt->id)) window_show(mw);
            else                            window_hide(mw);
        }
    }
    events_suppress_end();

    tile_current();

    HWND focus = desktop_get_focused();
    if (focus) window_focus(focus);
    else       window_focus_none();

    desktop_launch_app_if_empty(desktop_current_slot());
}

void desktop_switch(const wchar_t *name) {
    if (!name || !name[0]) return;

    int slot = desktop_ensure(name);
    if (slot < 0) return;

    int target_id = g.desktops[slot].id;

    int shown   = desktop_monitor_showing(target_id);
    int mon     = (shown >= 0) ? shown
                               : desktop_target_monitor(&g.desktops[slot]);
    int prev_id = desktop_on_monitor(mon);

    if (prev_id == target_id) {
        g.focused_monitor = mon;
        desktop_sync_current();
        HWND f = desktop_get_focused();
        if (f) window_focus(f);
        else   window_focus_none();
        mouse_warp_focus();
        bar_refresh();
        return;
    }

    Desktop *prev_dt = desktop_by_id(prev_id);

    wchar_t from[DESKTOP_NAME_MAX];
    wcscpy(from, prev_dt ? prev_dt->name : desktop_current()->name);
    int from_id = prev_dt ? prev_dt->id : 0;

    events_suppress_begin();

    Desktop *new_dt = desktop_by_id(target_id);
    if (!new_dt) { events_suppress_end(); return; }

    int stuck = 0;
    if (prev_dt) {
        for (int i = prev_dt->count - 1; i >= 0; i--) {
            HWND h = prev_dt->windows[i];
            ManagedWindow *mw = window_find(h);
            if (!mw || !mw->sticky) continue;
            if (new_dt->count >= MAX_WINDOWS_PER_DESKTOP) { stuck++; continue; }

            desktop_unlink_at(prev_dt, i);

            desktop_insert_window(new_dt, h, false);
            mw->desktop_id  = target_id;
            mw->has_applied = false;

            desktop_focus_hist_push(new_dt, h);
            window_follow_monitor(mw, mon);
        }
    }
    if (stuck)
        log_err(L"desktop: '%ls' is full (%d windows) — %d sticky window(s) "
                L"could not follow you and stay on '%ls'", new_dt->name,
                MAX_WINDOWS_PER_DESKTOP, stuck, from);

    int other = desktop_monitor_showing(target_id);
    if (other >= 0) {
        desktop_place_on_monitor(prev_id, other);
        desktop_place_on_monitor(target_id, mon);
        if (!prev_id) desktop_fill_monitors();
    } else {
        if (prev_dt) {
            for (int i = 0; i < prev_dt->count; i++)
                window_hide(window_find(prev_dt->windows[i]));
        }
        g.monitor_desktop[mon] = 0;

        for (int i = 0; i < new_dt->count; i++) {
            ManagedWindow *mw = window_find(new_dt->windows[i]);
            if (!mw || mw->user_hidden) continue;
            window_show(mw);
        }
        desktop_place_on_monitor(target_id, mon);
    }

    g.focused_monitor = mon;
    desktop_sync_current();
    wcscpy(g.last_desktop, from);
    events_suppress_end();

    if (from_id) desktop_gc(desktop_slot_by_id(from_id));

    tile_current();

    HWND focus = desktop_get_focused();
    if (focus) window_focus(focus);
    else       window_focus_none();

    mouse_warp_focus();

    if (g.cfg.notify_desktop) {
        const Desktop *landed = desktop_by_id(target_id);
        if (landed) {
            wchar_t msg[DESKTOP_NAME_MAX + 16];
            _snwprintf(msg, DESKTOP_NAME_MAX + 15, L"desktop: %ls",
                       landed->name);
            msg[DESKTOP_NAME_MAX + 15] = L'\0';
            notify_show(msg, NOTIFY_INFO, 1200);
        }
    }

    desktop_launch_app_if_empty(desktop_slot_by_id(target_id));

    bar_refresh();
    lua_fire(LUA_EVENT_DESKTOP_SWITCH, NULL, from);
}

void desktop_switch_last(void) {
    if (!g.last_desktop[0]) return;

    if (desktop_name_eq(g.last_desktop, desktop_current()->name)) {
        log_w(L"last_desktop: already on '%ls' — nothing to go back to",
              g.last_desktop);
        return;
    }
    desktop_switch(g.last_desktop);
}

void desktop_cycle(int delta) {
    if (g.desktop_count < 2 || delta == 0) return;

    int cur  = desktop_current_slot();
    int next = ((cur + delta) % g.desktop_count + g.desktop_count)
               % g.desktop_count;

    wchar_t name[DESKTOP_NAME_MAX];
    wcscpy(name, g.desktops[next].name);
    desktop_switch(name);
}

void desktop_move_window(HWND hwnd, const wchar_t *name) {
    if (!name || !name[0]) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    Desktop *old_dt = desktop_by_id(mw->desktop_id);
    if (old_dt && desktop_name_eq(old_dt->name, name)) return;

    int old_id = mw->desktop_id;

    int slot = desktop_ensure(name);
    if (slot < 0) return;
    int new_id = g.desktops[slot].id;

    if (g.desktops[slot].count >= MAX_WINDOWS_PER_DESKTOP) {
        log_err(L"desktop: '%ls' already holds %d windows, which is the maximum "
                L"— leaving the window where it is", name,
                MAX_WINDOWS_PER_DESKTOP);
        return;
    }

    old_dt = desktop_by_id(old_id);
    if (old_dt) {
        for (int i = 0; i < old_dt->count; i++) {
            if (old_dt->windows[i] != hwnd) continue;
            desktop_unlink_at(old_dt, i);
            break;
        }
    }

    bool was_visible  = desktop_is_visible(old_id);
    bool will_be_seen = desktop_is_visible(new_id);

    if (was_visible && !will_be_seen) {
        events_suppress_begin();
        window_hide(mw);
        events_suppress_end();
    }

    Desktop *new_dt = &g.desktops[slot];
    desktop_insert_window(new_dt, hwnd, true);

    desktop_focus_hist_push(new_dt, hwnd);

    mw->desktop_id  = new_id;
    mw->has_applied = false;

    int new_mon = desktop_monitor_showing(new_id);
    if (new_mon < 0) new_mon = new_dt->monitor;
    if (new_mon >= 0 && new_mon < g.monitor_count)
        window_follow_monitor(mw, new_mon);

    if (!was_visible && will_be_seen) {
        events_suppress_begin();
        window_show(mw);
        events_suppress_end();
    }

    desktop_gc(desktop_slot_by_id(old_id));

    if (was_visible || will_be_seen) {
        tile_current();
        HWND next = desktop_get_focused();
        if (next) window_focus(next);
        else      window_focus_none();
    }
}

bool desktop_add_window(HWND hwnd, int slot) {
    if (slot < 0 || slot >= g.desktop_count) return false;

    Desktop *dt = &g.desktops[slot];

    for (int i = 0; i < dt->count; i++) {
        if (dt->windows[i] == hwnd) return true;
    }

    if (dt->count >= MAX_WINDOWS_PER_DESKTOP) {
        log_err(L"desktop: '%ls' already holds %d windows, which is the "
                L"maximum — %p is not on it", dt->name,
                MAX_WINDOWS_PER_DESKTOP, (void *)hwnd);
        return false;
    }

    return desktop_insert_window(dt, hwnd, true);
}

void desktop_remove_window(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    Desktop *dt = desktop_by_id(mw->desktop_id);
    if (!dt) return;

    for (int i = 0; i < dt->count; i++) {
        if (dt->windows[i] == hwnd) {
            desktop_unlink_at(dt, i);
            return;
        }
    }
}

static void desktop_focus_hist_forget(Desktop *dt, HWND hwnd) {
    for (int i = 0; i < dt->focus_hist_n; i++) {
        if (dt->focus_hist[i] != hwnd) continue;
        memmove(&dt->focus_hist[i], &dt->focus_hist[i + 1],
                (size_t)(dt->focus_hist_n - i - 1) * sizeof(HWND));
        dt->focus_hist_n--;
        return;
    }
}

static void desktop_unlink_at(Desktop *dt, int i) {
    if (!dt || i < 0 || i >= dt->count) return;

    HWND h = dt->windows[i];

    memmove(&dt->windows[i], &dt->windows[i + 1],
            (size_t)(dt->count - i - 1) * sizeof(HWND));
    dt->count--;
    dt->focused = desktop_focus_after_remove(dt->focused, i, dt->count);

    desktop_focus_hist_forget(dt, h);
}

static void desktop_focus_hist_push(Desktop *dt, HWND hwnd) {
    if (dt->focus_hist_n > 0 && dt->focus_hist[0] == hwnd) return;

    int found = -1;
    for (int i = 0; i < dt->focus_hist_n; i++) {
        if (dt->focus_hist[i] == hwnd) { found = i; break; }
    }

    int from = desktop_hist_shift(dt->focus_hist_n, found, FOCUS_HIST_MAX,
                                  &dt->focus_hist_n);

    for (int i = from; i > 0; i--)
        dt->focus_hist[i] = dt->focus_hist[i - 1];
    dt->focus_hist[0] = hwnd;
}

HWND desktop_last_window(void) {
    Desktop *dt = desktop_current();
    if (!dt) return NULL;

    HWND cur = desktop_get_focused();
    for (int i = 0; i < dt->focus_hist_n; i++) {
        HWND h = dt->focus_hist[i];
        if (!h || h == cur || !IsWindow(h)) continue;
        for (int j = 0; j < dt->count; j++)
            if (dt->windows[j] == h) return h;
    }
    return NULL;
}

void desktop_focus_update(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    Desktop *dt = desktop_by_id(mw->desktop_id);
    if (dt) {
        for (int i = 0; i < dt->count; i++) {
            if (dt->windows[i] != hwnd) continue;
            dt->focused = i;
            desktop_focus_hist_push(dt, hwnd);
            return;
        }
    }

    static HWND warned;
    if (warned != hwnd) {
        warned = hwnd;
        log_err(L"desktop: %p says it is on desktop id %d, which %ls — its "
                L"focus was not recorded, its close will not unlink it, and "
                L"that desktop will never empty", (void *)hwnd, mw->desktop_id,
                dt ? L"does not list it" : L"no longer exists");
    }
}

static bool desktop_focusable(HWND h) {
    if (!h || !IsWindow(h)) return false;
    const ManagedWindow *mw = window_find(h);
    if (mw && (mw->app_hidden || mw->user_hidden)) return false;
    return !IsIconic(h);
}

HWND desktop_focused_of(const Desktop *dt) {
    if (!dt || dt->count == 0) return NULL;

    if (dt->focused >= 0 && dt->focused < dt->count) {
        HWND h = dt->windows[dt->focused];
        if (desktop_focusable(h)) return h;
    }
    for (int i = 0; i < dt->count; i++)
        if (desktop_focusable(dt->windows[i])) return dt->windows[i];
    return NULL;
}

HWND desktop_get_focused(void) {
    Desktop *dt = desktop_current();
    if (dt->count == 0) return NULL;

    if (dt->focused >= 0 && dt->focused < dt->count) {
        HWND h = dt->windows[dt->focused];
        if (desktop_focusable(h)) return h;
    }

    for (int i = 0; i < dt->count; i++) {
        if (desktop_focusable(dt->windows[i])) {
            dt->focused = i;
            return dt->windows[i];
        }
    }

    return NULL;
}

void desktop_launch_app_if_empty(int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;

    Desktop *dt = &g.desktops[slot];
    if (dt->id != g.current_desktop_id) return;
    if (dt->count > 0)   return;
    if (dt->app_pending) return;
    if (!dt->app[0])     return;

    wchar_t ctx[DESKTOP_NAME_MAX + 16];
    _snwprintf(ctx, DESKTOP_NAME_MAX + 16, L"desktop '%ls'", dt->name);
    ctx[DESKTOP_NAME_MAX + 15] = L'\0';

    if (!spawn_command(dt->app, dt->app_args, dt->app_cwd, ctx)) return;

    dt->app_pending = true;
}
