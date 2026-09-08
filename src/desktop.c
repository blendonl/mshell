/*
 * desktop.c — dynamic, name-identified virtual desktops.
 *
 * Desktops are implemented by taking windows off the screen and putting them
 * back — window_hide() / window_show(). How is window.c's business and not
 * this file's: the default policy tries sinking under the backdrop first, then
 * cloaking, then stashing, and only falls back to ShowWindow(SW_HIDE), which is
 * what makes GPU-composited apps come back black (see HidePolicy in mshell.h).
 * No dependency on the Windows 10 virtual-desktop API by any of those routes.
 *
 * The model (see the Desktop comment in mshell.h): a desktop IS its name, and
 * the set of desktops is whatever exists at this instant. Switching to a name
 * nobody is using creates that desktop; leaving one with no windows on it
 * destroys it. There is no count to configure, no 1..9, and no index: "1" is a
 * desktop whose name is the character '1', addressed exactly the way "web" is.
 *
 * What each desktop DOES — which app opens on it, whether its windows float,
 * its layout, which monitor it lives on — comes from the desktop rules the
 * config declared, applied at creation (desktop_apply_rules).
 */

#include "mshell.h"

/* Defined with the rest of the focus bookkeeping at the bottom; needed by the
 * sticky loop and the move path, which land a window on a desktop, and by the
 * three paths that take one off again. */
static void desktop_focus_hist_push(Desktop *dt, HWND hwnd);
static void desktop_unlink_at(Desktop *dt, int i);

static void desktop_fill_monitors(void);

/* ===========================================================================
 * Names
 *
 * What a usable name is, when two names mean the same desktop, and how the set
 * is ordered all live in desktop_list.c — pure, and covered by `make test`.
 * This wrapper exists only to bind the length limit, so no caller has to
 * remember it.
 * =========================================================================== */
bool desktop_name_ok(const wchar_t *name) {
    return desktop_list_name_ok(name, DESKTOP_NAME_MAX);
}

/* ===========================================================================
 * Lookup
 * =========================================================================== */
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

/* The desktop you are standing on. Slot 0 is the fallback for the window
 * between startup and desktop_init() — never NULL, so callers can dereference
 * it without a guard the way they always could. */
int desktop_current_slot(void) {
    int slot = desktop_slot_by_id(g.current_desktop_id);
    return (slot >= 0) ? slot : 0;
}

Desktop *desktop_current(void) {
    return &g.desktops[desktop_current_slot()];
}

/* ===========================================================================
 * The per-monitor desktop set.
 *
 * A desktop is shown on at most ONE display, so g.monitor_desktop is both
 * "what is display m showing" and, read backwards, "where is desktop X". That
 * single-place rule is what makes a switch to a desktop that is already up
 * elsewhere a SWAP of two displays rather than a second copy of it — there is
 * one window list, and a window is in one place.
 *
 * Visibility is now "on some display", not "on the current desktop": with two
 * displays showing two desktops, the desktop you are not focused on is still
 * on screen, and every hide/show decision has to ask the first question rather
 * than the second.
 * =========================================================================== */
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

/* Where a desktop wants to be: the display its rule pinned it to, and the one
 * you are looking at when nothing pinned it. This is what makes an existing
 * `desktop_rule("web", { monitor = 1 })` config mean what it always looked
 * like it meant — switching to "web" lights up display 1 and leaves the other
 * display alone. */
int desktop_target_monitor(const Desktop *dt) {
    if (dt && dt->monitor >= 0 && dt->monitor < g.monitor_count)
        return dt->monitor;

    const ManagedWindow *fg = window_find(GetForegroundWindow());
    int mon = fg ? desktop_monitor_showing(fg->desktop_id) : -1;
    if (mon < 0) mon = g.focused_monitor;
    if (mon < 0 || mon >= desktop_monitor_span()) mon = 0;
    return mon;
}

/* Hand a display to a desktop, and give that desktop's windows the display.
 * A desktop's windows live where the desktop is shown — the tiler slices a
 * monitor's work area for the windows whose `monitor` names it, so this is
 * what actually puts them there. */
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

/* current_desktop_id is derived, not stored: it is whatever the focused
 * display is showing. Everything that means "the desktop the user is driving"
 * — where a new window lands, which layout a keybind adjusts — keeps reading
 * it and keeps being right.
 *
 * A display showing nothing is a real state (a second monitor before you have
 * switched to anything on it), so fall back to a display that IS showing
 * something rather than leaving the id dangling at a dead desktop. */
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

/* ===========================================================================
 * Rules — resolve a desktop's settings from the config.
 *
 * The global defaults first, then EVERY rule whose pattern matches, in
 * declaration order, each overwriting only the fields it set. So a broad rule
 * sets the house style and a specific one below it overrides a field or two:
 *
 *   desktop_rule("*",    { layout = "tiling" })
 *   desktop_rule("chat", { layout = "monocle", app = "discord.exe" })
 *
 * Re-run on config reload (desktop_reapply), which is why it always starts from
 * the defaults: a field a rule no longer sets has to fall back, not linger.
 * =========================================================================== */
/* Resolve just the monitor pin, and move the desktop's windows onto it.
 *
 * Split out of desktop_apply_rules because a display change has to redo this
 * and nothing else: a pin that no longer names a display has to lapse, but a
 * layout you switched at runtime is yours to keep.
 *
 * Pinning is not advisory — every window already on the desktop moves too, so
 * the desktop is on one display however its windows got there. */
static void desktop_resolve_monitor(Desktop *dt) {
    dt->monitor = -1;
    for (int i = 0; i < g.desktop_rule_count; i++) {
        const DesktopRule *r = &g.desktop_rules[i];
        if (!r->set_monitor) continue;
        if (r->name_match[0] && !wildcard_match(r->name_match, dt->name)) continue;
        dt->monitor = r->monitor;
    }

    if (dt->monitor_override >= 0) dt->monitor = dt->monitor_override;

    /* A pin that doesn't name a display that exists (one was unplugged, or the
     * config names monitor 3 on a two-head machine) means "wherever it opens"
     * rather than a desktop that tiles into nothing. */
    if (dt->monitor >= g.monitor_count) dt->monitor = -1;
    if (dt->monitor < 0) return;

    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw) continue;
        /* Not gated on the record having changed. A float whose record
         * already said this display while the window sat on the other one is
         * precisely the case the pin exists to correct, and asking
         * window_set_monitor whether anything changed skipped it — see
         * window_follow_monitor, which asks the window instead. */
        window_follow_monitor(mw, dt->monitor);
    }
}

/* A display was added or removed — re-resolve every desktop's pin. */
void desktop_monitors_changed(void) {
    for (int i = 0; i < g.desktop_count; i++)
        desktop_resolve_monitor(&g.desktops[i]);

    /* A display that was unplugged takes its slot in the per-monitor set with
     * it. The desktop it was showing is not lost — its windows are still on
     * it — but it has to stop claiming a display that no longer exists, or
     * desktop_is_visible keeps answering yes for a desktop nobody can see and
     * its windows are never hidden or tiled again.
     *
     * It moves to display 0 when display 0 is free, because a desktop that was
     * in front of you a moment ago should still be; otherwise it goes off the
     * screen and switching to it brings it back. */
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

    /* A desktop that is ON SCREEN moves for real, and the display it lands on
     * hands back whatever it was showing — the same trade a switch makes, so
     * "send this desktop to the other monitor" and "swap my two monitors" are
     * one operation seen from two sides. */
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

    dt->layout       = g.default_layout;
    dt->master_ratio = g.default_master_ratio > 0.f ? g.default_master_ratio
                                                    : DEFAULT_MASTER_RATIO;
    dt->n_master     = g.default_nmaster > 0 ? g.default_nmaster : DEFAULT_NMASTER;
    dt->inner_gap    = -1;   /* -1 = inherit the global setting */
    dt->outer_gap    = -1;
    dt->float_all    = false;
    dt->app[0]       = L'\0';
    dt->app_args[0]  = L'\0';
    dt->app_cwd[0]   = L'\0';

    /* A config that says nothing about a desktop's layout lets your runtime
     * choice persist (g.default_layout is the baseline above, not a rule), and
     * a config that DOES name one is the source of truth for it from the next
     * reload on. */
    for (int i = 0; i < g.desktop_rule_count; i++) {
        const DesktopRule *r = &g.desktop_rules[i];
        if (r->name_match[0] && !wildcard_match(r->name_match, dt->name)) continue;

        if (r->app[0]) {
            wcsncpy(dt->app, r->app, MAX_PATH - 1);
            dt->app[MAX_PATH - 1] = L'\0';
            /* The arguments travel with the app they belong to — a later rule
             * that replaces the app must not inherit the old one's. */
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

    /* The monitor pin is deliberately not part of the precedence above: it has
     * its own, inside desktop_resolve_monitor, where a runtime
     * desktop_to_monitor outranks the rule until you clear it. */
    desktop_resolve_monitor(dt);
}

/* ===========================================================================
 * Order — how the live desktops are kept sorted.
 *
 * Numeric names come first in numeric order, then the rest alphabetically, so
 * next_desktop/prev_desktop step through them the way you'd read them off a
 * list: 1, 2, 10, chat, web. Sorting by name rather than by creation time is
 * what keeps that order STABLE — with desktops appearing and vanishing under
 * you, "the next one" has to mean the same thing every time, and creation
 * order does not survive a desktop being destroyed and re-created.
 * =========================================================================== */
/* Bubble the desktop at `slot` to where the order says it belongs. Only ever
 * called right after an insert at the end, so one pass is enough. The ordering
 * itself is desktop_name_cmp, in desktop_list.c, where `make test` covers it. */
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

/* ===========================================================================
 * Create / destroy
 * =========================================================================== */

/* Create the desktop `name`. Returns its slot, or -1 if the name is unusable or
 * we are already at MAX_DESKTOPS. Callers use desktop_ensure(), not this. */
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

    /* Sorting moves the new entry, so `dt` is stale from here on — the slot it
     * returns is the only valid handle. */
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

/* Destroy an empty desktop you are no longer on. This is what makes the set
 * dynamic: nothing has to be cleaned up by hand, and a desktop you used once
 * costs nothing once its last window is gone.
 *
 * The desktop you are STANDING on is never destroyed however empty it is —
 * you have to be somewhere, and an empty desktop is a perfectly good place to
 * open the next window. Its name lives on in g.last_desktop either way, so
 * last_desktop re-creates it rather than failing.
 *
 * A pending auto-launch does NOT hold a desktop open. Leaving before the app's
 * window appears is rare, and the window is managed onto whatever desktop is
 * current when it arrives regardless — so waiting for it would strand an empty
 * desktop permanently in exchange for nothing.
 *
 * Safe to call speculatively: it checks the conditions itself. */
void desktop_gc(int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;

    Desktop *dt = &g.desktops[slot];
    if (dt->count > 0)                  return; /* still has windows      */
    if (dt->id == g.current_desktop_id) return; /* you are standing on it */
    if (desktop_is_visible(dt->id))     return; /* another display shows it */

    log_w(L"desktop: destroyed '%ls' (id %d, empty)", dt->name, dt->id);

    /* Release its BSP tree now rather than leaving the slot to be reclaimed
     * lazily — desktop ids are monotonic, so a long session would otherwise
     * keep every dead desktop's tree until the table wrapped. */
    layout_tree_forget(dt->id);

    memmove(&g.desktops[slot], &g.desktops[slot + 1],
            (size_t)(g.desktop_count - slot - 1) * sizeof(Desktop));
    g.desktop_count--;
    memset(&g.desktops[g.desktop_count], 0, sizeof(Desktop));
}

/* ===========================================================================
 * Startup — bring the first desktop into existence.
 *
 * Called once, after the config has been read: nothing exists before this, and
 * the desktop rule that claimed `default` (or "1", when none did) decides what
 * we land on. Every other desktop is created by being switched to.
 * =========================================================================== */
void desktop_init(void) {
    /* The desktop rule that claimed `default` decides every start; with none,
     * DEFAULT_START_DESKTOP does. */
    const wchar_t *name = g.start_desktop[0] ? g.start_desktop
                                             : DEFAULT_START_DESKTOP;

    g.desktop_count      = 0;
    g.current_desktop_id = 0;

    int slot = desktop_create(name);
    if (slot < 0) slot = desktop_create(DEFAULT_START_DESKTOP);   /* bad name */
    if (slot < 0) return;                                          /* can't happen */

    /* Every display starts showing nothing; the desktop we land on claims the
     * one its rule names (or display 0). */
    memset(g.monitor_desktop, 0, sizeof g.monitor_desktop);

    int mon = desktop_target_monitor(&g.desktops[slot]);
    desktop_place_on_monitor(g.desktops[slot].id, mon);
    g.focused_monitor = mon;

    g.current_desktop_id = g.desktops[slot].id;
    wcscpy(g.last_desktop, g.desktops[slot].name);

    desktop_fill_monitors();
}

/* ===========================================================================
 * Re-assert visibility + settings for every desktop.
 *
 * Called after a config reload: the desktops and their windows survive (they
 * are runtime state, not config), but the rules that describe them may have
 * changed, so re-resolve each one and make sure exactly the current desktop's
 * windows are showing.
 * =========================================================================== */
void desktop_reapply(void) {
    for (int d = 0; d < g.desktop_count; d++)
        desktop_apply_rules(d);

    events_suppress_begin();
    for (int d = 0; d < g.desktop_count; d++) {
        Desktop *dt = &g.desktops[d];
        for (int i = 0; i < dt->count; i++) {
            HWND h = dt->windows[i];
            if (!h || !IsWindow(h)) continue;
            /* A window the app hid itself stays hidden wherever it lives —
             * a reload must not drag every tray-minimised app back on screen.
             * window_hide/window_show both refuse such a window themselves;
             * the find is needed for the handle either way. */
            ManagedWindow *mw = window_find(h);
            if (!mw) continue;
            /* ...and a window the USER stowed stays stowed, for the same
             * reason: a reload is not a request to un-stow the scratchpad. */
            if (mw->user_hidden) continue;
            /* On screen means "on SOME display" — a reload must not take the
             * other monitor's desktop down. */
            if (desktop_is_visible(dt->id)) window_show(mw);
            else                            window_hide(mw);
        }
    }
    events_suppress_end();

    tile_current();

    HWND focus = desktop_get_focused();
    if (focus) window_focus(focus);
    else       window_focus_none();

    /* A reload may have given the current desktop an `app` it didn't have. */
    desktop_launch_app_if_empty(desktop_current_slot());
}

/* ===========================================================================
 * Switch to a desktop by name — creating it if it doesn't exist yet.
 * =========================================================================== */
void desktop_switch(const wchar_t *name) {
    if (!name || !name[0]) return;

    int slot = desktop_ensure(name);
    if (slot < 0) return;   /* unusable name or the set is full — stay put */

    /* Ids, not pointers, from here on: desktop_ensure may have inserted and
     * re-sorted the table, and desktop_gc below moves it again. */
    int target_id = g.desktops[slot].id;

    /* WHICH DISPLAY this switch is about. A desktop pinned by a rule lights up
     * the display it is pinned to whatever you were looking at; an unpinned one
     * takes over the display you are on. Only that one display changes — the
     * others keep showing what they were showing, which is the whole point of
     * the per-monitor set. */
    int shown   = desktop_monitor_showing(target_id);
    int mon     = (shown >= 0) ? shown
                               : desktop_target_monitor(&g.desktops[slot]);
    int prev_id = desktop_on_monitor(mon);

    /* Already up on that display: nothing to switch, so this is a request to
     * LOOK at it. Moving the focus (and the pointer) there is exactly what
     * makes `g t` / `g b` the natural way to move between two displays that
     * are each showing their own desktop. */
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

    /* Remember where we came from BEFORE anything can move it. Only a switch
     * that actually moves records it, which is what makes the pair a toggle:
     * A→B sets last=A, and from B the action goes to A while setting last=B. */
    wchar_t from[DESKTOP_NAME_MAX];
    wcscpy(from, prev_dt ? prev_dt->name : desktop_current()->name);
    int from_id = prev_dt ? prev_dt->id : 0;

    /* Suppress WinEvent callbacks — the ShowWindow fallback inside
     * window_hide generates EVENT_OBJECT_HIDE, which would otherwise be read
     * as the app hiding itself. Sinking raises no event we subscribe to, but
     * the guard has to cover the path that does. */
    events_suppress_begin();

    Desktop *new_dt = desktop_by_id(target_id);
    if (!new_dt) { events_suppress_end(); return; }

    /* 0. Sticky windows come with us.
     *
     * Implemented by REASSIGNING them to the target desktop rather than by
     * teaching the tiler, the focus ring and the bar that a window can be on
     * several desktops at once. A sticky window is genuinely only ever on the
     * one you are looking at, so every other part of mshell keeps working
     * unchanged — and "sticky" ends up meaning exactly what it looks like. */
    int stuck = 0;   /* sticky windows the target had no room for */
    if (prev_dt) {
        for (int i = prev_dt->count - 1; i >= 0; i--) {
            HWND h = prev_dt->windows[i];
            ManagedWindow *mw = window_find(h);
            if (!mw || !mw->sticky) continue;
            if (new_dt->count >= MAX_WINDOWS_PER_DESKTOP) { stuck++; continue; }

            desktop_unlink_at(prev_dt, i);

            new_dt->windows[new_dt->count++] = h;
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
        /* 1a. The desktop you asked for is already on the OTHER display, so the
         *     two displays trade. Nothing is hidden and nothing is shown —
         *     both desktops stay on screen, they simply change places, and the
         *     windows follow because a desktop's windows live where it is
         *     shown. This is also what "swap the desktops between my monitors"
         *     is: switch to the one on the other screen. */
        desktop_place_on_monitor(prev_id, other);
        desktop_place_on_monitor(target_id, mon);
    } else {
        /* 1b. Take the display's outgoing desktop off the screen...
         *
         *     Only THIS display's. A window on a desktop still shown on another
         *     monitor must not be hidden — that is the bug the old global
         *     switch had by construction, because there was only ever one
         *     desktop on screen to hide. */
        if (prev_dt) {
            for (int i = 0; i < prev_dt->count; i++)
                window_hide(window_find(prev_dt->windows[i]));
        }
        g.monitor_desktop[mon] = 0;

        /* 2.  ...and put the incoming one back — except the ones their own app
         *     hid (minimise-to-tray), which window_show refuses on its own, and
         *     the ones the USER stowed (a scratchpad put away with
         *     toggle_scratchpad), which is not the same thing at all. */
        for (int i = 0; i < new_dt->count; i++) {
            ManagedWindow *mw = window_find(new_dt->windows[i]);
            if (!mw || mw->user_hidden) continue;
            window_show(mw);
        }
        desktop_place_on_monitor(target_id, mon);
    }

    /* 3. The display this happened on is the one you are now on, and
     *    current_desktop_id follows the focus. */
    g.focused_monitor = mon;
    desktop_sync_current();
    wcscpy(g.last_desktop, from);
    events_suppress_end();

    /* 4. The desktop we just left is destroyed if we left it empty — this is
     *    the other half of "created on demand". desktop_gc declines while it is
     *    still on another display, so a swap never destroys anything. */
    if (from_id) desktop_gc(desktop_slot_by_id(from_id));

    /* 5. Re-tile every display: two of them just changed what they show. */
    tile_current();

    /* 6. Focus the active window (window_focus handles the foreground-lock
     *    dance and updates the focus ring). An empty desktop focuses NOTHING,
     *    which has to be done deliberately: the windows we just took off the
     *    screen are sunk, not destroyed, so the one you were using still holds
     *    the foreground until somebody takes it away. */
    HWND focus = desktop_get_focused();
    if (focus) window_focus(focus);
    else       window_focus_none();

    mouse_warp_focus();

    /* Opt-in, because the status bar already lists every live desktop with
     * the current one marked — this is for a setup that turned the bar off. */
    if (g.notify_desktop) {
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

    /* 7. Tell the config, once everything above has settled — the handler gets
     *    the desktop as it now is, plus `from` naming where we came from. */
    bar_refresh();   /* the desktop set and the current one both just changed */
    lua_fire(LUA_EVENT_DESKTOP_SWITCH, NULL, from);
}

/* ===========================================================================
 * Jump back to the desktop we came from.
 *
 * Bound to the "last_desktop" action; pressing it twice returns you to where
 * you started (desktop_switch swaps the pair on every move). Because it works
 * on the NAME, it still goes back to a desktop that was destroyed behind you —
 * it is simply re-created, empty, with its rules applied. At startup, and until
 * the first switch happens, last == current and this does nothing.
 * =========================================================================== */
void desktop_switch_last(void) {
    if (!g.last_desktop[0]) return;

    if (desktop_name_eq(g.last_desktop, desktop_current()->name)) {
        log_w(L"last_desktop: already on '%ls' — nothing to go back to",
              g.last_desktop);
        return;
    }
    desktop_switch(g.last_desktop);
}

/* ===========================================================================
 * Step to the next/previous desktop that currently exists.
 *
 * The counterpart to naming every desktop: with the set created on demand, this
 * is how you reach one you never bound a key to. Wraps around, and never
 * creates anything — cycling through an empty set is a no-op.
 * =========================================================================== */
void desktop_cycle(int delta) {
    if (g.desktop_count < 2 || delta == 0) return;

    int cur  = desktop_current_slot();
    int next = ((cur + delta) % g.desktop_count + g.desktop_count)
               % g.desktop_count;

    /* Copy the name out: desktop_switch may destroy the desktop we're on, and
     * with it the array entry `next` points at. */
    wchar_t name[DESKTOP_NAME_MAX];
    wcscpy(name, g.desktops[next].name);
    desktop_switch(name);
}

/* ===========================================================================
 * Move a window to another desktop — creating that desktop if needed.
 * =========================================================================== */
void desktop_move_window(HWND hwnd, const wchar_t *name) {
    if (!name || !name[0]) return;

    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    Desktop *old_dt = desktop_by_id(mw->desktop_id);
    if (old_dt && desktop_name_eq(old_dt->name, name)) return;  /* already there */

    int old_id = mw->desktop_id;

    int slot = desktop_ensure(name);
    if (slot < 0) return;
    int new_id = g.desktops[slot].id;

    /* Check there is ROOM before dismantling anything.
     *
     * This used to be checked further down, after the window had already been
     * unlinked from its old desktop and hidden — so a full target left the
     * window hidden, owned by a desktop whose window list did not contain it,
     * and therefore never shown by anything again. Refusing up front leaves it
     * exactly where it was. */
    if (g.desktops[slot].count >= MAX_WINDOWS_PER_DESKTOP) {
        log_err(L"desktop: '%ls' already holds %d windows, which is the maximum "
                L"— leaving the window where it is", name,
                MAX_WINDOWS_PER_DESKTOP);
        return;
    }

    /* Remove from the old desktop (desktop_ensure may have re-sorted). */
    old_dt = desktop_by_id(old_id);
    if (old_dt) {
        for (int i = 0; i < old_dt->count; i++) {
            if (old_dt->windows[i] != hwnd) continue;
            desktop_unlink_at(old_dt, i);
            break;
        }
    }

    /* Suppressed like every other hide mshell performs: an unsuppressed
     * EVENT_OBJECT_HIDE is how we recognise an app minimising itself to the
     * tray, so hiding a window ourselves without the guard would flag it as
     * app-hidden and it would never be shown on the desktop it just moved to. */
    bool was_visible  = desktop_is_visible(old_id);
    bool will_be_seen = desktop_is_visible(new_id);

    /* Only hide it if it is going somewhere you cannot see. With two displays
     * showing two desktops, throwing a window at the OTHER one is a move
     * between two on-screen desktops, and hiding it there would lose it. */
    if (was_visible && !will_be_seen) {
        events_suppress_begin();
        window_hide(mw);
        events_suppress_end();
    }

    /* Add to the target desktop. Note this bypasses the attach policy on
     * purpose: a window you deliberately threw at a desktop goes to the end,
     * where you'll find it, rather than displacing that desktop's master. */
    Desktop *new_dt = &g.desktops[slot];   /* capacity checked above */
    new_dt->windows[new_dt->count] = hwnd;
    new_dt->focused = new_dt->count;
    new_dt->count++;
    new_dt->app_pending = false;   /* no longer empty — cancel auto-launch */

    /* It is the window you were last in on that desktop — you just put it
     * there. Without this, last_window on the target could not reach it. */
    desktop_focus_hist_push(new_dt, hwnd);

    mw->desktop_id  = new_id;
    mw->has_applied = false;

    /* A window arriving on a desktop belongs on the display that desktop is
     * SHOWN on, falling back to the one its rule pins it to while it is not up
     * anywhere. */
    int new_mon = desktop_monitor_showing(new_id);
    if (new_mon < 0) new_mon = new_dt->monitor;
    if (new_mon >= 0 && new_mon < g.monitor_count)
        window_follow_monitor(mw, new_mon);

    /* Arriving on a desktop that IS up: it has to be put back on the screen,
     * because nothing else will — the target is not being switched to. */
    if (!was_visible && will_be_seen) {
        events_suppress_begin();
        window_show(mw);
        events_suppress_end();
    }

    /* Sending away the last window empties the desktop we're standing on — that
     * one survives (see desktop_gc), but a window moved off a BACKGROUND
     * desktop, which happens on reload, can empty one for good. */
    desktop_gc(desktop_slot_by_id(old_id));

    if (was_visible || will_be_seen) {
        tile_current();
        HWND next = desktop_get_focused();
        if (next) window_focus(next);
        else      window_focus_none();   /* the window we sent away is still
                                          * composited, not un-shown, and would
                                          * otherwise keep the keyboard from
                                          * the other desktop */
    }
}

/* ===========================================================================
 * Add a window to a desktop
 * =========================================================================== */
bool desktop_add_window(HWND hwnd, int slot) {
    if (slot < 0 || slot >= g.desktop_count) return false;

    Desktop *dt = &g.desktops[slot];

    /* Already here? Then it is on the desktop, which is what the caller asked
     * for — and answering that BEFORE the capacity check matters, because a
     * full desktop would otherwise report a window it already holds as one it
     * could not take. */
    for (int i = 0; i < dt->count; i++) {
        if (dt->windows[i] == hwnd) return true;
    }

    if (dt->count >= MAX_WINDOWS_PER_DESKTOP) {
        log_err(L"desktop: '%ls' already holds %d windows, which is the "
                L"maximum — %p is not on it", dt->name,
                MAX_WINDOWS_PER_DESKTOP, (void *)hwnd);
        return false;
    }

    /* Where the new window lands is governed by the attach policy. */
    int idx = desktop_attach_index(g.attach_policy, dt->focused, dt->count);

    if (idx < dt->count)
        memmove(&dt->windows[idx + 1], &dt->windows[idx],
                (size_t)(dt->count - idx) * sizeof(HWND));

    dt->windows[idx] = hwnd;
    dt->count++;
    dt->focused = idx;

    /* A window landed — a pending auto-launch has materialised (or a manual
     * window arrived). Either way, stop treating this desktop as "launching". */
    dt->app_pending = false;
    return true;
}

/* ===========================================================================
 * Remove a window from whichever desktop it lives on.
 *
 * Does NOT garbage-collect the desktop it empties: window_unmanage still has
 * work to do with the desktop id afterwards. It calls desktop_gc itself.
 * =========================================================================== */
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

/* ===========================================================================
 * Track focus change
 * =========================================================================== */
/* Drop `hwnd` from a desktop's focus history.
 *
 * Called when the window LEAVES the desktop, not when it dies. desktop_last_window
 * validates entries at read time, which handles a window that has simply closed —
 * but Windows reuses HWND values, so an entry left behind can come back to life
 * as an unrelated window that happens to have landed on this desktop, and
 * last_window would then jump to a window you were never in. */
static void desktop_focus_hist_forget(Desktop *dt, HWND hwnd) {
    for (int i = 0; i < dt->focus_hist_n; i++) {
        if (dt->focus_hist[i] != hwnd) continue;
        memmove(&dt->focus_hist[i], &dt->focus_hist[i + 1],
                (size_t)(dt->focus_hist_n - i - 1) * sizeof(HWND));
        dt->focus_hist_n--;
        return;
    }
}

/* Take the window at `i` out of a desktop's list, and out of its focus history
 * with it.
 *
 * The three places a window leaves a desktop — closed, moved away, or carried
 * off by a sticky follow — all have to do exactly this, and both halves fail
 * silently when they are wrong. The list shifts under `focused`, so a window
 * removed before it leaves keybindings walking from the wrong place; the
 * history outlives the HWND, which the OS hands out again.
 */
static void desktop_unlink_at(Desktop *dt, int i) {
    if (!dt || i < 0 || i >= dt->count) return;

    HWND h = dt->windows[i];

    memmove(&dt->windows[i], &dt->windows[i + 1],
            (size_t)(dt->count - i - 1) * sizeof(HWND));
    dt->count--;
    dt->focused = desktop_focus_after_remove(dt->focused, i, dt->count);

    desktop_focus_hist_forget(dt, h);
}

/* Push `hwnd` to the front of a desktop's focus history, moving it up rather
 * than duplicating it if it is already there. */
static void desktop_focus_hist_push(Desktop *dt, HWND hwnd) {
    if (dt->focus_hist_n > 0 && dt->focus_hist[0] == hwnd) return;  /* no change */

    int found = -1;
    for (int i = 0; i < dt->focus_hist_n; i++) {
        if (dt->focus_hist[i] == hwnd) { found = i; break; }
    }

    /* How far to shift, and how long the list ends up — desktop_list.c, where
     * `make test` covers the saturating case. */
    int from = desktop_hist_shift(dt->focus_hist_n, found, FOCUS_HIST_MAX,
                                  &dt->focus_hist_n);

    for (int i = from; i > 0; i--)
        dt->focus_hist[i] = dt->focus_hist[i - 1];
    dt->focus_hist[0] = hwnd;
}

/* The most recent still-live window that is not the current one. Entries are
 * validated here rather than pruned on close: a window dying is precisely when
 * this gets called, so the check has to happen at read time anyway. */
HWND desktop_last_window(void) {
    Desktop *dt = desktop_current();
    if (!dt) return NULL;

    HWND cur = desktop_get_focused();
    for (int i = 0; i < dt->focus_hist_n; i++) {
        HWND h = dt->focus_hist[i];
        if (!h || h == cur || !IsWindow(h)) continue;
        /* Must still be on this desktop — it may have been moved away. */
        for (int j = 0; j < dt->count; j++)
            if (dt->windows[j] == h) return h;
    }
    return NULL;
}

void desktop_focus_update(HWND hwnd) {
    /* The window knows which desktop it is on, so ask it rather than scanning
     * every desktop's list for it. Not only cheaper: a scan would quietly find
     * the window on SOME desktop even when it is not on the one it claims, and
     * that disagreement is exactly the invariant mshell.h says must never
     * break — worth a line in the log, not a silent workaround. */
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

    /* Both ways of getting here are the same broken fact: a window naming a
     * desktop that does not list it, or one naming a desktop that no longer
     * exists. Reported once per offending window, the way the minimize
     * repeat-guard does it — focus events arrive constantly, and a broken
     * invariant must not turn the log into a flood. */
    static HWND warned;
    if (warned != hwnd) {
        warned = hwnd;
        log_err(L"desktop: %p says it is on desktop id %d, which %ls — its "
                L"focus was not recorded, its close will not unlink it, and "
                L"that desktop will never empty", (void *)hwnd, mw->desktop_id,
                dt ? L"does not list it" : L"no longer exists");
    }
}

/* ===========================================================================
 * Get the currently focused window on the current desktop
 * =========================================================================== */
/* Can this window be handed the keyboard right now?
 *
 * "Alive" is not enough, because the answer is fed straight to window_focus(),
 * which RESTORES an iconic window (SW_RESTORE) and, when SetForegroundWindow is
 * refused, falls back to SwitchToThisWindow — which un-hides. So naming a
 * minimised or trayed window here does not merely focus something invisible: it
 * drags it back onto the screen. That is what made a desktop switch un-minimise
 * the window you left minimised and un-tray the one the app had trayed, undoing
 * the whole point of app_hidden.
 *
 * IsIconic rather than a flag, matching collect_clients: it is authoritative
 * even if a minimize event was missed. */
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

    /* The remembered window is gone, minimised or away in the tray — hand the
     * keyboard to a sibling that is actually on screen.
     *
     * dt->focused moves with it, and has to: execute_action reads this function
     * and then dt->focused, and a keybinding that computes its target from a
     * different window than the one it was told is focused walks from the wrong
     * place. Getting back to a minimised window is ACTION_RESTORE's job — it
     * scans windows[] itself and is deliberately not filtered by any of this. */
    for (int i = 0; i < dt->count; i++) {
        if (desktop_focusable(dt->windows[i])) {
            dt->focused = i;
            return dt->windows[i];
        }
    }

    return NULL;
}

/* ===========================================================================
 * Per-desktop auto-launch.
 *
 * If the desktop is empty and a rule gave it an `app`, spawn that app. Called
 * when a desktop becomes current: on desktop_switch, at startup, and after a
 * reload that may have assigned it one.
 *
 * The spawned window appears asynchronously and is picked up by the normal
 * WinEvent → window_manage path, so window rules, decoration stripping and
 * tiling all apply to it — nothing special is needed here.
 *
 * `app_pending` guards the async gap: after we fire the launch the window does
 * not exist yet, so switching away and back before it appears must NOT spawn a
 * second copy. It is set here and cleared the instant a window lands on the
 * desktop (desktop_add_window), so closing the app and returning relaunches it.
 * It does NOT hold the desktop open against desktop_gc — see the note there.
 * Leaving before the window appears is rare, and the window is managed onto
 * whatever desktop is current when it arrives regardless, so waiting for it
 * would strand an empty desktop permanently in exchange for nothing.
 *
 * Only the CURRENT desktop may auto-launch: window_manage assigns new windows
 * to the current desktop, so launching for a background one would misplace the
 * window.
 * =========================================================================== */
void desktop_launch_app_if_empty(int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;

    Desktop *dt = &g.desktops[slot];
    if (dt->id != g.current_desktop_id) return;   /* not where windows land  */
    if (dt->count > 0)   return;   /* not empty — nothing to do            */
    if (dt->app_pending) return;   /* a launch is already in flight        */
    if (!dt->app[0])     return;   /* no app configured for this desktop   */

    wchar_t ctx[DESKTOP_NAME_MAX + 16];
    _snwprintf(ctx, DESKTOP_NAME_MAX + 16, L"desktop '%ls'", dt->name);
    ctx[DESKTOP_NAME_MAX + 15] = L'\0';

    /* On failure do NOT latch app_pending, so a corrected config takes effect
     * on the next visit instead of the desktop staying stuck as "launching". */
    if (!spawn_command(dt->app, dt->app_args, dt->app_cwd, ctx)) return;

    dt->app_pending = true;
}
