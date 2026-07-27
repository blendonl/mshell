/*
 * desktop.c — dynamic, name-identified virtual desktops.
 *
 * Desktops are implemented by taking windows off the screen and putting them
 * back — window_hide() / window_show(), which by default cloak through DWM
 * rather than ShowWindow(SW_HIDE) (see HidePolicy in mshell.h; SW_HIDE is what
 * makes GPU-composited apps come back black). No dependency on the Windows 10
 * virtual-desktop API either way.
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

/* ===========================================================================
 * Names
 *
 * Case-insensitive (so `switch_desktop "Web"` and "web" are one desktop), and
 * the first spelling to create the desktop is the one that gets displayed.
 * =========================================================================== */
bool desktop_name_ok(const wchar_t *name) {
    if (!name || !name[0]) return false;
    if (wcslen(name) >= DESKTOP_NAME_MAX) return false;
    /* Whitespace would make a name impossible to tell apart in a log line, and
     * is never what the config meant. */
    for (const wchar_t *p = name; *p; p++)
        if (iswspace(*p)) return false;
    return true;
}

/* A name made only of digits sorts as the number it spells (see below). */
static bool name_is_number(const wchar_t *name, long *out) {
    wchar_t *end = NULL;
    long     v   = wcstol(name, &end, 10);
    if (!end || end == name || *end != L'\0' || v < 0) return false;
    if (out) *out = v;
    return true;
}

/* ===========================================================================
 * Lookup
 * =========================================================================== */
int desktop_slot_by_name(const wchar_t *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g.desktop_count; i++)
        if (_wcsicmp(g.desktops[i].name, name) == 0) return i;
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

    /* A pin that doesn't name a display that exists (one was unplugged, or the
     * config names monitor 3 on a two-head machine) means "wherever it opens"
     * rather than a desktop that tiles into nothing. */
    if (dt->monitor >= g.monitor_count) dt->monitor = -1;
    if (dt->monitor < 0) return;

    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (mw) window_set_monitor(mw, dt->monitor);
    }
}

/* A display was added or removed — re-resolve every desktop's pin. */
void desktop_monitors_changed(void) {
    for (int i = 0; i < g.desktop_count; i++)
        desktop_resolve_monitor(&g.desktops[i]);
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

    /* Anything remembered from last time overrides the rule defaults — a
     * layout you switched to is yours. A reload re-runs this whole function,
     * so editing a rule still wins at that point. */
    session_apply(dt);

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
static int desktop_sort_cmp(const Desktop *a, const Desktop *b) {
    long na, nb;
    bool ia = name_is_number(a->name, &na);
    bool ib = name_is_number(b->name, &nb);

    if (ia && ib) return (na < nb) ? -1 : (na > nb) ? 1 : 0;
    if (ia != ib) return ia ? -1 : 1;          /* numbers before words */
    return _wcsicmp(a->name, b->name);
}

/* Bubble the desktop at `slot` to where the order says it belongs. Only ever
 * called right after an insert at the end, so one pass is enough. */
static int desktop_sort_in(int slot) {
    while (slot > 0 && desktop_sort_cmp(&g.desktops[slot - 1],
                                        &g.desktops[slot]) > 0) {
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
 * the config's set_start_desktop (default "1") decides what we land on. Every
 * other desktop is created by being switched to.
 * =========================================================================== */
void desktop_init(void) {
    /* Where you were last beats where the config says to start: coming back to
     * the desktop you left is what "restart" should feel like. set_start_desktop
     * still decides on a genuinely first run, and deleting session.txt restores
     * that behaviour. */
    const wchar_t *remembered = session_start_desktop();
    const wchar_t *name = remembered            ? remembered
                        : g.start_desktop[0]    ? g.start_desktop
                                                : DEFAULT_START_DESKTOP;

    g.desktop_count      = 0;
    g.current_desktop_id = 0;

    int slot = desktop_create(name);
    if (slot < 0) slot = desktop_create(DEFAULT_START_DESKTOP);   /* bad name */
    if (slot < 0) return;                                          /* can't happen */

    g.current_desktop_id = g.desktops[slot].id;
    wcscpy(g.last_desktop, g.desktops[slot].name);
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
    int cur = desktop_current_slot();

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
            if (d == cur) window_show(mw);
            else          window_hide(mw);
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

    Desktop *cur = desktop_current();
    if (_wcsicmp(cur->name, name) == 0) return;   /* already there */

    /* Remember where we came from BEFORE anything can move it. Only a switch
     * that actually moves records it, which is what makes the pair a toggle:
     * A→B sets last=A, and from B the action goes to A while setting last=B. */
    wchar_t from[DESKTOP_NAME_MAX];
    wcscpy(from, cur->name);
    int from_id = cur->id;

    int slot = desktop_ensure(name);
    if (slot < 0) return;   /* unusable name or the set is full — stay put */

    /* desktop_ensure may have inserted and re-sorted; `cur` is stale now. */
    int target_id = g.desktops[slot].id;

    /* Suppress WinEvent callbacks — the ShowWindow fallback inside
     * window_hide generates EVENT_OBJECT_HIDE, which would otherwise be read
     * as the app hiding itself. Cloaking raises no event we subscribe to, but
     * the guard has to cover the path that does. */
    events_suppress_begin();

    Desktop *old_dt = desktop_by_id(from_id);
    Desktop *new_dt = desktop_by_id(target_id);
    if (!new_dt) { events_suppress_end(); return; }

    /* 0. Sticky windows come with us.
     *
     * Implemented by REASSIGNING them to the target desktop rather than by
     * teaching the tiler, the focus ring and the bar that a window can be on
     * several desktops at once. A sticky window is genuinely only ever on the
     * one you are looking at, so every other part of mshell keeps working
     * unchanged — and "sticky" ends up meaning exactly what it looks like.
     *
     * Done before the hide loop below, so they are never hidden in the first
     * place and there is no flicker. */
    if (old_dt && new_dt->count < MAX_WINDOWS_PER_DESKTOP) {
        for (int i = old_dt->count - 1; i >= 0; i--) {
            HWND h = old_dt->windows[i];
            ManagedWindow *mw = window_find(h);
            if (!mw || !mw->sticky) continue;
            if (new_dt->count >= MAX_WINDOWS_PER_DESKTOP) break;

            memmove(&old_dt->windows[i], &old_dt->windows[i + 1],
                    (size_t)(old_dt->count - i - 1) * sizeof(HWND));
            old_dt->count--;
            if (old_dt->focused >= old_dt->count && old_dt->count > 0)
                old_dt->focused = old_dt->count - 1;

            new_dt->windows[new_dt->count++] = h;
            mw->desktop_id  = target_id;
            mw->has_applied = false;
        }
    }

    /* 1. Take every window on the desktop we're leaving off the screen.
     *
     *    window_hide decides how (cloak by default — see HidePolicy) and is
     *    idempotent, so a window monocle already had hidden stays as it is. */
    if (old_dt) {
        for (int i = 0; i < old_dt->count; i++) {
            window_hide(window_find(old_dt->windows[i]));
        }
    }

    /* 2. Put the target desktop's windows back — except the ones their own app
     *    hid (minimise-to-tray), which window_show refuses on its own: those
     *    are not ours to reveal, and switching to a desktop must not un-tray
     *    everything parked on it.
     *
     *    A window the LAYOUT is holding back (monocle's unfocused windows, the
     *    unshown side of a tabbed container) is skipped too, so it is not
     *    revealed here only for the tile pass below to hide it again a frame
     *    later. collect_clients recomputes the flag every pass, so a stale one
     *    cannot strand a window: the tiler clears it and this then shows it. */
    for (int i = 0; i < new_dt->count; i++) {
        ManagedWindow *mw = window_find(new_dt->windows[i]);
        if (!mw || mw->layout_hidden) continue;
        window_show(mw);
    }

    g.current_desktop_id = target_id;
    wcscpy(g.last_desktop, from);
    events_suppress_end();

    /* 3. A pinned desktop takes the focus to its display, so opening a window
     *    right after switching puts it where the desktop lives. */
    if (new_dt->monitor >= 0 && new_dt->monitor < g.monitor_count)
        g.focused_monitor = new_dt->monitor;

    /* 4. The desktop we just left is destroyed if we left it empty — this is
     *    the other half of "created on demand". Its name is in last_desktop,
     *    so going back re-creates it. */
    desktop_gc(desktop_slot_by_id(from_id));

    /* 5. Tile the new desktop to clean up any positioning drift.
     *    (tile_current has its own suppression guard.) */
    tile_current();

    /* 6. Focus the active window (window_focus handles the foreground-lock
     *    dance and updates the focus ring). An empty desktop focuses NOTHING,
     *    which has to be done deliberately: the windows we just took off the
     *    screen are cloaked, not hidden, so the one you were using still holds
     *    the foreground until somebody takes it away. */
    HWND focus = desktop_get_focused();
    if (focus) window_focus(focus);
    else       window_focus_none();

    /* 7. If we just landed on an empty desktop that has a configured app,
     *    launch it. Must happen after current_desktop_id is updated so the new
     *    window is managed onto THIS desktop. */
    /* Opt-in, because the status bar already lists every live desktop with
     * the current one marked — this is for a setup that turned the bar off. */
    if (g.notify_desktop) {
        wchar_t msg[DESKTOP_NAME_MAX + 16];
        _snwprintf(msg, DESKTOP_NAME_MAX + 15, L"desktop: %ls", name);
        msg[DESKTOP_NAME_MAX + 15] = L'\0';
        notify_show(msg, NOTIFY_INFO, 1200);
    }

    desktop_launch_app_if_empty(desktop_slot_by_id(target_id));

    /* 8. Tell the config, once everything above has settled — the handler gets
     *    the desktop as it now is, plus `from` naming where we came from. */
    bar_refresh();   /* the desktop set and the current one both just changed */
    session_save();  /* which desktop you are on is part of the session */
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

    if (_wcsicmp(g.last_desktop, desktop_current()->name) == 0) {
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
    if (old_dt && _wcsicmp(old_dt->name, name) == 0) return;  /* already there */

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
            memmove(&old_dt->windows[i], &old_dt->windows[i + 1],
                    (size_t)(old_dt->count - i - 1) * sizeof(HWND));
            old_dt->count--;
            if (old_dt->focused >= old_dt->count && old_dt->count > 0)
                old_dt->focused = old_dt->count - 1;
            break;
        }
    }

    /* Suppressed like every other hide mshell performs: an unsuppressed
     * EVENT_OBJECT_HIDE is how we recognise an app minimising itself to the
     * tray, so hiding a window ourselves without the guard would flag it as
     * app-hidden and it would never be shown on the desktop it just moved to. */
    bool was_visible = (old_id == g.current_desktop_id);
    if (was_visible) {
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

    mw->desktop_id  = new_id;
    mw->has_applied = false;

    /* The target desktop may be pinned to a display; a window arriving on it
     * belongs there too. */
    if (new_dt->monitor >= 0 && new_dt->monitor < g.monitor_count)
        window_set_monitor(mw, new_dt->monitor);

    /* Sending away the last window empties the desktop we're standing on — that
     * one survives (see desktop_gc), but a window moved off a BACKGROUND
     * desktop, which happens on reload, can empty one for good. */
    desktop_gc(desktop_slot_by_id(old_id));

    if (was_visible) {
        tile_current();
        HWND next = desktop_get_focused();
        if (next) window_focus(next);
        else      window_focus_none();   /* the window we sent away is cloaked,
                                          * and would otherwise keep the
                                          * keyboard from the other desktop */
    }
}

/* ===========================================================================
 * Add a window to a desktop
 * =========================================================================== */
void desktop_add_window(HWND hwnd, int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;

    Desktop *dt = &g.desktops[slot];
    if (dt->count >= MAX_WINDOWS_PER_DESKTOP) return;

    /* Don't add duplicates */
    for (int i = 0; i < dt->count; i++) {
        if (dt->windows[i] == hwnd) return;
    }

    /* Where the new window lands is governed by the attach policy. */
    int idx;
    switch (g.attach_policy) {
    case ATTACH_MASTER: idx = 0; break;
    case ATTACH_AFTER:
        idx = (dt->focused >= 0 && dt->focused < dt->count)
                ? dt->focused + 1 : dt->count;
        break;
    case ATTACH_END:
    default:            idx = dt->count; break;
    }

    if (idx < dt->count)
        memmove(&dt->windows[idx + 1], &dt->windows[idx],
                (size_t)(dt->count - idx) * sizeof(HWND));

    dt->windows[idx] = hwnd;
    dt->count++;
    dt->focused = idx;

    /* A window landed — a pending auto-launch has materialised (or a manual
     * window arrived). Either way, stop treating this desktop as "launching". */
    dt->app_pending = false;
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
            memmove(&dt->windows[i],
                    &dt->windows[i + 1],
                    (size_t)(dt->count - i - 1) * sizeof(HWND));
            dt->count--;
            if (dt->focused >= dt->count && dt->count > 0) {
                dt->focused = dt->count - 1;
            }
            return;
        }
    }
}

/* ===========================================================================
 * Track focus change
 * =========================================================================== */
/* Push `hwnd` to the front of a desktop's focus history, moving it up rather
 * than duplicating it if it is already there. */
static void desktop_focus_hist_push(Desktop *dt, HWND hwnd) {
    if (dt->focus_hist_n > 0 && dt->focus_hist[0] == hwnd) return;  /* no change */

    int from = dt->focus_hist_n;         /* how much of the list to shift */
    for (int i = 0; i < dt->focus_hist_n; i++) {
        if (dt->focus_hist[i] == hwnd) { from = i; break; }
    }
    if (from >= FOCUS_HIST_MAX) from = FOCUS_HIST_MAX - 1;

    for (int i = from; i > 0; i--)
        dt->focus_hist[i] = dt->focus_hist[i - 1];
    dt->focus_hist[0] = hwnd;

    if (dt->focus_hist_n < FOCUS_HIST_MAX && from == dt->focus_hist_n)
        dt->focus_hist_n++;
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
    for (int d = 0; d < g.desktop_count; d++) {
        Desktop *dt = &g.desktops[d];
        for (int i = 0; i < dt->count; i++) {
            if (dt->windows[i] == hwnd) {
                dt->focused = i;
                desktop_focus_hist_push(dt, hwnd);
                return;
            }
        }
    }
}

/* ===========================================================================
 * Get the currently focused window on the current desktop
 * =========================================================================== */
HWND desktop_get_focused(void) {
    Desktop *dt = desktop_current();
    if (dt->count == 0) return NULL;

    if (dt->focused >= 0 && dt->focused < dt->count) {
        HWND h = dt->windows[dt->focused];
        if (h && IsWindow(h)) return h;
    }

    /* Focused window is gone — find any valid window */
    for (int i = 0; i < dt->count; i++) {
        if (dt->windows[i] && IsWindow(dt->windows[i])) {
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
