/*
 * tiling.c — layout algorithms and window placement.
 *
 * Layouts: master-stack, monocle, grid, spiral (fibonacci), centered-master,
 * bottom-stack, and columns. Each desktop is tiled per-monitor: its windows are
 * grouped by the monitor they live on and each monitor's subset is laid out
 * independently into that monitor's work area.
 *
 * Placement is two-phase. The layout functions only *record* a target rect per
 * window into a buffer (via emit()); flush_placements() then applies them all
 * in one BeginDeferWindowPos batch — atomic, flicker-free, and skipping windows
 * that are already where we want them. Floating windows are never placed.
 */

#include "mshell.h"
#include "layout_math.h"

/* DWM's "real" visible frame (excludes the invisible resize border DWM keeps
 * around top-level windows). Positioning against these bounds makes gaps
 * pixel-accurate instead of ~7px too wide on non-stripped windows. */
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

/* ===========================================================================
 * Placement buffer — one target rect per tiled window, filled by the layout
 * functions and drained by flush_placements().
 * =========================================================================== */
/* `flags` is filled in by flush_placements rather than by the layouts: it
 * depends on what the WINDOW needs (a fresh reveal wants SWP_NOCOPYBITS), not
 * on where the layout decided to put it. It is stored per placement so a batch
 * that has to be replayed individually replays with the same flags it was
 * queued with. */
typedef struct { HWND hwnd; RECT rect; UINT flags; } Placement;
static Placement s_place[MAX_WINDOWS_PER_DESKTOP];
static int       s_place_n;
static int       s_inner;   /* inner gap in effect for the monitor being tiled */

static void place_reset(void) { s_place_n = 0; }

static RECT inset_rect(RECT r, int d) {
    r.left += d; r.top += d; r.right -= d; r.bottom -= d;
    return r;
}

/* Record a window's target. `cell` is its slot in the (already outer-gap
 * inset) area; we shrink it by half the inner gap on every side so two adjacent
 * cells end up a full inner_gap apart. */
static void emit(HWND hwnd, RECT cell) {
    if (!hwnd || s_place_n >= MAX_WINDOWS_PER_DESKTOP) return;
    RECT r = inset_rect(cell, s_inner / 2);
    if (r.right  - r.left < 20) r.right  = r.left + 20;
    if (r.bottom - r.top  < 20) r.bottom = r.top  + 20;
    s_place[s_place_n].hwnd  = hwnd;
    s_place[s_place_n].rect  = r;
    s_place[s_place_n].flags = 0;   /* flush_placements decides these */
    s_place_n++;
}

/* Record a target verbatim — no gap inset, no minimum. Fullscreen windows are
 * placed with this: they are outside the layout and must reach every edge. */
/* The tree calls back through this rather than knowing what a Placement is.
 * Defined after emit() so it can simply forward. */
static void tree_emit_cb(HWND hwnd, RECT area, void *ctx) {
    (void)ctx;
    emit(hwnd, area);
}

static void emit_raw(HWND hwnd, RECT rect) {
    if (!hwnd || s_place_n >= MAX_WINDOWS_PER_DESKTOP) return;
    s_place[s_place_n].hwnd  = hwnd;
    s_place[s_place_n].rect  = rect;
    s_place[s_place_n].flags = 0;   /* flush_placements decides these */
    s_place_n++;
}

/* ===========================================================================
 * Collect the tiled (non-floating) windows of a desktop that live on `mon`,
 * preserving the desktop's window order.
 * =========================================================================== */
typedef struct { HWND hwnd; ManagedWindow *mw; } Client;

static int collect_clients(Desktop *dt, int mon, Client *out) {
    int n = 0;
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        /* tracked_only implies is_floating; it is checked anyway, because
         * "the layout never sees a tracked window" is the invariant the
         * whole tier rests on. */
        if (!mw || mw->is_floating || mw->tracked_only) continue;
        /* The app hid this one itself (minimise-to-tray). It keeps its place in
         * the desktop's window order — so it lands back where it was when the
         * app shows it again — but it gets no tile, and in particular is kept
         * out of the placement list, which force-shows anything invisible. */
        if (mw->app_hidden) continue;
        /* Minimized windows are still WS_VISIBLE, so nothing else filters them
         * out — but SetWindowPos on an iconic window only edits the rect it
         * will restore to, so giving one a cell just leaves that cell empty.
         * IsIconic rather than a tracked flag: it is authoritative even if a
         * minimize event was missed. */
        if (IsIconic(dt->windows[i])) continue;
        int wmon = mw->monitor;
        if (wmon < 0 || wmon >= g.monitor_count) wmon = 0;   /* defensive */
        if (wmon != mon) continue;
        /* layout_hidden is an OUTPUT of the pass about to run, not state that
         * survives it: monocle and the BSP tree set it for the windows they
         * hold back, and every other layout holds nothing back. Clearing it
         * here is what makes that true — otherwise leaving monocle would strand
         * yesterday's unfocused windows hidden, and the desktop-switch show
         * loop (which skips layout_hidden windows) would never reveal them. */
        mw->layout_hidden = false;
        out[n].hwnd = dt->windows[i];
        out[n].mw   = mw;
        n++;
    }
    return n;
}

/* ===========================================================================
 * Divide `rect` among clients [from, to) — vertically or horizontally —
 * proportionally to each client's cfact. The last client absorbs any rounding
 * remainder so cells always tile the axis exactly (no seams, no overlap).
 * =========================================================================== */
/* Gather the cfacts of clients [from, to) into `facts`. */
static int collect_facts(Client *cs, int from, int to, float *facts) {
    int n = 0;
    for (int i = from; i < to; i++) facts[n++] = cs[i].mw->cfact;
    return n;
}

static void stack_vertical(Client *cs, int from, int to, RECT rect) {
    float facts[MAX_WINDOWS_PER_DESKTOP];
    int   sizes[MAX_WINDOWS_PER_DESKTOP];

    int n = collect_facts(cs, from, to, facts);
    if (n <= 0) return;

    split_span(rect.bottom - rect.top, facts, n, sizes);

    int y = rect.top;
    for (int i = 0; i < n; i++) {
        RECT cell = { rect.left, y, rect.right, y + sizes[i] };
        emit(cs[from + i].hwnd, cell);
        y += sizes[i];
    }
}

static void stack_horizontal(Client *cs, int from, int to, RECT rect) {
    float facts[MAX_WINDOWS_PER_DESKTOP];
    int   sizes[MAX_WINDOWS_PER_DESKTOP];

    int n = collect_facts(cs, from, to, facts);
    if (n <= 0) return;

    split_span(rect.right - rect.left, facts, n, sizes);

    int x = rect.left;
    for (int i = 0; i < n; i++) {
        RECT cell = { x, rect.top, x + sizes[i], rect.bottom };
        emit(cs[from + i].hwnd, cell);
        x += sizes[i];
    }
}

/* ===========================================================================
 * master-stack — n_master windows in a left column, the rest stacked right.
 *
 *   ┌──────────┬──────┐
 *   │          │  1   │
 *   │  MASTER  ├──────┤
 *   │          │  2   │
 *   └──────────┴──────┘
 * =========================================================================== */
static void layout_master_stack(const LayoutParams *lp, Client *cs, int n) {
    int nm = lp->n_master; if (nm < 1) nm = 1; if (nm > n) nm = n;
    int nstack = n - nm;

    if (nstack == 0) {                 /* everything in the master column */
        stack_vertical(cs, 0, n, lp->area);
        return;
    }

    int total_w  = lp->area.right - lp->area.left;
    int master_w = (int)((float)total_w * lp->master_ratio);
    int min_col  = total_w / 10; if (min_col < 1) min_col = 1;
    master_w = clamp_i(master_w, min_col, total_w - min_col);

    RECT ma = lp->area; ma.right = lp->area.left + master_w;   /* master column */
    RECT sa = lp->area; sa.left  = lp->area.left + master_w;   /* stack column  */
    stack_vertical(cs, 0,  nm, ma);
    stack_vertical(cs, nm, n,  sa);
}

/* ===========================================================================
 * bottom-stack — master row on top, stack row beneath (horizontal master).
 * =========================================================================== */
static void layout_bstack(const LayoutParams *lp, Client *cs, int n) {
    int nm = lp->n_master; if (nm < 1) nm = 1; if (nm > n) nm = n;
    int nstack = n - nm;

    if (nstack == 0) { stack_horizontal(cs, 0, n, lp->area); return; }

    int total_h  = lp->area.bottom - lp->area.top;
    int master_h = (int)((float)total_h * lp->master_ratio);
    int min_row  = total_h / 10; if (min_row < 1) min_row = 1;
    master_h = clamp_i(master_h, min_row, total_h - min_row);

    RECT ma = lp->area; ma.bottom = lp->area.top + master_h;
    RECT sa = lp->area; sa.top    = lp->area.top + master_h;
    stack_horizontal(cs, 0,  nm, ma);
    stack_horizontal(cs, nm, n,  sa);
}

/* ===========================================================================
 * columns — every window an equal (cfact-weighted) vertical column.
 * =========================================================================== */
static void layout_columns(const LayoutParams *lp, Client *cs, int n) {
    stack_horizontal(cs, 0, n, lp->area);
}

/* ===========================================================================
 * centered-master — master column in the middle, stack split left/right.
 * =========================================================================== */
static void layout_centered(const LayoutParams *lp, Client *cs, int n) {
    int nm = lp->n_master; if (nm < 1) nm = 1; if (nm > n) nm = n;
    int nstack = n - nm;

    if (nstack == 0) { stack_vertical(cs, 0, n, lp->area); return; }

    int total_w  = lp->area.right - lp->area.left;
    int master_w = (int)((float)total_w * lp->master_ratio);
    master_w = clamp_i(master_w, total_w / 5, (total_w * 4) / 5);
    int side = (total_w - master_w) / 2;

    int ln = nstack / 2;             /* left column count  */
    int rn = nstack - ln;            /* right column count */

    RECT mid   = lp->area;
    mid.left   = lp->area.left + side;
    mid.right  = lp->area.left + side + master_w;
    RECT left  = lp->area; left.right  = lp->area.left + side;
    RECT right = lp->area; right.left  = lp->area.left + side + master_w;

    /* With a single stack window the left column would be blank — give that
     * space back to the master column so nothing is wasted. */
    if (ln == 0) mid.left = lp->area.left;

    stack_vertical(cs, 0,        nm,        mid);
    stack_vertical(cs, nm,       nm + rn,   right);
    stack_vertical(cs, nm + rn,  n,         left);
}

/* ===========================================================================
 * spiral — fibonacci / dwindle: each window halves the remaining lp->area,
 * alternating the split axis; the last window takes what's left.
 * =========================================================================== */
static void layout_spiral(const LayoutParams *lp, Client *cs, int n) {
    RECT r = lp->area;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) { emit(cs[i].hwnd, r); break; }
        RECT cell = r;
        if (i % 2 == 0) {                 /* split vertically */
            int w = r.right - r.left;
            cell.right = r.left + w / 2;
            r.left     = r.left + w / 2;
        } else {                          /* split horizontally */
            int h = r.bottom - r.top;
            cell.bottom = r.top + h / 2;
            r.top       = r.top + h / 2;
        }
        emit(cs[i].hwnd, cell);
    }
}

/* ===========================================================================
 * grid — roughly square grid of equal cells.
 * =========================================================================== */
static void layout_grid(const LayoutParams *lp, Client *cs, int n) {
    int cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;

    int total_w = lp->area.right  - lp->area.left;
    int total_h = lp->area.bottom - lp->area.top;
    int cell_w  = total_w / cols;
    int cell_h  = total_h / rows;

    for (int i = 0; i < n; i++) {
        int col = i % cols, row = i / cols;
        RECT cell;
        cell.left   = lp->area.left + col * cell_w;
        cell.top    = lp->area.top  + row * cell_h;
        cell.right  = (col == cols - 1) ? lp->area.right  : cell.left + cell_w;
        cell.bottom = (row == rows - 1) ? lp->area.bottom : cell.top  + cell_h;
        emit(cs[i].hwnd, cell);
    }
}

/* ===========================================================================
 * monocle — one window fills the monitor; the rest of that monitor's tiled
 * windows are hidden so focus-cycling swaps which one shows.
 * =========================================================================== */
static void layout_monocle(const LayoutParams *lp, Client *cs, int n) {
    HWND focus  = desktop_get_focused();
    HWND target = cs[0].hwnd;
    for (int i = 0; i < n; i++)
        if (cs[i].hwnd == focus) { target = focus; break; }

    /* Marking rather than hiding inline: flush_placements does the actual
     * ShowWindow, which keeps every "in the layout but not on screen" decision
     * in one place — the same flag tabbed containers set. */
    for (int i = 0; i < n; i++) {
        if (cs[i].hwnd == target) {
            cs[i].mw->layout_hidden = false;
            emit(cs[i].hwnd, lp->area);
        } else {
            cs[i].mw->layout_hidden = true;
        }
    }
}

/* ===========================================================================
 * Tile one monitor's slice of a desktop.
 * =========================================================================== */
static void tile_monitor(Desktop *dt, int mon, RECT work) {
    Client cs[MAX_WINDOWS_PER_DESKTOP];
    int n = collect_clients(dt, mon, cs);
    if (n == 0) return;

    /* ---- fullscreen windows come out of the layout first ----
     * A window covering the monitor takes its *full* bounds (not the work area,
     * no gaps) and the remaining windows tile underneath it as if it weren't
     * there — so leaving fullscreen reveals the layout already in place.
     * FS_BOTH and an app-driven fullscreen own their own geometry: place them
     * once to get them onto the display, then leave them alone, or every tile
     * pass would fight the rect the app chose for itself. */
    RECT full = (g.monitor_count > 0 && mon >= 0 && mon < g.monitor_count)
                  ? g.monitors[mon].full : work;
    int keep = 0;
    for (int i = 0; i < n; i++) {
        /* Sunk under the backdrop, or stashed off every display: mshell took
         * this one off the screen without hiding it (window.c) — monocle's
         * unfocused windows, a stowed scratchpad. window_covers_monitor says no
         * for a stashed one and the fullscreen branch below would park it back
         * over the monitor, on screen and in front of what monocle is showing,
         * while every flag still says it is away. A sunk one would be raised out
         * from under the backdrop by the same placement. Both keep their place
         * in the list — monocle cycles through these — and are simply not
         * placed. */
        if (cs[i].mw->stashed || cs[i].mw->sunk) { cs[keep++] = cs[i]; continue; }

        if (window_is_screen_fullscreen(cs[i].mw)) {
            if (cs[i].mw->fs_mode == FS_WINDOW || !window_covers_monitor(cs[i].hwnd))
                emit_raw(cs[i].hwnd, full);
            continue;
        }
        cs[keep++] = cs[i];
    }
    n = keep;
    if (n == 0) return;

    /* ---- resolve this monitor's layout parameters, once ----
     * Everything the layouts are allowed to depend on is decided here. Gaps
     * shrink the work area by the outer gap (minus the half-inner that emit()
     * adds back at the edges); smart gaps are evaluated per monitor, against
     * that monitor's own client count. A per-desktop or per-monitor override
     * belongs in this block and nowhere else. */
    LayoutParams lp;
    /* Precedence, most specific first: the desktop's own override, then the
     * monitor's, then the global. A desktop that asked for zero gaps means it
     * on every display it spans; a monitor override describes a screen's
     * habits and should not overrule that. */
    const Monitor *M = (mon >= 0 && mon < g.monitor_count) ? &g.monitors[mon]
                                                           : NULL;
    int inner = (dt->inner_gap >= 0) ? dt->inner_gap
              : (M && M->inner_gap >= 0) ? M->inner_gap : g.inner_gap;
    int outer = (dt->outer_gap >= 0) ? dt->outer_gap
              : (M && M->outer_gap >= 0) ? M->outer_gap : g.outer_gap;
    if (g.smart_gaps && n == 1) { inner = 0; outer = 0; }

    int pre = outer - inner / 2;
    if (pre < 0) pre = 0;

    lp.area         = inset_rect(work, pre);
    lp.inner        = inner;
    lp.master_ratio = (M && M->master_ratio > 0.f) ? M->master_ratio
                                                    : dt->master_ratio;
    lp.n_master     = (M && M->n_master > 0) ? M->n_master : dt->n_master;

    s_inner = lp.inner;   /* emit() reads this for the per-cell half-gap */

    /* A vertical secondary that always wants columns is the case this exists
     * for: the desktop spans both displays, so the layout has to be able to
     * differ per screen even though the desktop is one thing. */
    Layout lay = (M && M->layout != LAYOUT_COUNT) ? M->layout : dt->layout;

    switch (lay) {
    case LAYOUT_TILING:   layout_master_stack(&lp, cs, n); break;
    case LAYOUT_MONOCLE:  layout_monocle(&lp, cs, n);      break;
    case LAYOUT_GRID:     layout_grid(&lp, cs, n);         break;
    case LAYOUT_SPIRAL:   layout_spiral(&lp, cs, n);       break;
    case LAYOUT_CENTERED: layout_centered(&lp, cs, n);     break;
    case LAYOUT_BSTACK:   layout_bstack(&lp, cs, n);       break;
    case LAYOUT_COLUMNS:  layout_columns(&lp, cs, n);      break;

    /* Manual splits. The tree keeps its own structure but not its own window
     * list — it reconciles against the desktop's on every pass — so switching
     * to and from this layout needs nothing but the case.
     *
     * One tree PER MONITOR, hence `mon`: this pass must place the windows on
     * this display and no others. The fallback is for the case where there is
     * no tree to be had (the pool is exhausted, which takes 32 desktop/monitor
     * pairs in the manual layout at once) — these windows are tiled by the
     * default layout instead of being left wherever they happened to be. */
    case LAYOUT_BSP:
        if (!layout_tree_run(dt, mon, lp.area, tree_emit_cb, NULL))
            layout_master_stack(&lp, cs, n);
        break;
    case LAYOUT_COUNT:    break;
    }
}

static bool rect_eq(RECT a, RECT b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

/* Place one window outside the batch, recording the outcome when there is a
 * ManagedWindow to record it on. Everything in the placement buffer normally
 * has one — the layouts only ever emit windows collect_clients found — but the
 * buffer is keyed by HWND, so this stays honest if that ever stops being true.
 */
static void place_one(HWND hwnd, RECT want, UINT flags) {
    ManagedWindow *mw = window_find(hwnd);
    if (mw) { window_apply_rect(mw, want, flags); return; }

    RECT adj = window_adjust_for_frame(hwnd, want);
    window_set_pos(hwnd, adj.left, adj.top, adj.right - adj.left,
                   adj.bottom - adj.top, flags);
}

/* Apply the whole placement buffer in one deferred batch. */
static void flush_placements(void) {
    if (s_place_n <= 0) return;

    /* Showing and hiding can't be deferred into the batch, so both happen
     * here, before anything moves.
     *
     * layout_hidden is the layout's own decision (monocle's unfocused windows,
     * the unshown side of a tabbed container) and is deliberately distinct from
     * app_hidden, which is the APP hiding itself to the tray — mshell must not
     * un-hide the latter. window_hide/window_show enforce that themselves. */
    Desktop *cur = desktop_current();
    for (int i = 0; i < cur->count; i++) {
        ManagedWindow *mw = window_find(cur->windows[i]);
        if (!mw || mw->is_floating) continue;
        if (mw->layout_hidden) window_hide(mw);
    }

    /* Anything with a tile belongs on the screen. Note this runs AFTER the
     * loop above, so a window that was layout_hidden a moment ago and has a
     * tile again this pass ends up shown, not hidden. */
    for (int i = 0; i < s_place_n; i++)
        window_show(window_find(s_place[i].hwnd));

    /* Which placements went into the deferred batch, so they can be recorded
     * once it commits — or replayed one at a time if it does not. Recording
     * while QUEUEING was the bug: DeferWindowPos only queues, EndDeferWindowPos
     * is what moves anything, and a batch containing one window we are not
     * allowed to move fails as a whole. Every window on the desktop then stayed
     * exactly where it was while its ManagedWindow claimed the new rect — after
     * which the no-op skip below matched that claim forever and the layout
     * never recovered. */
    int batched[MAX_WINDOWS_PER_DESKTOP];
    int batched_n = 0;

    HDWP hdwp = BeginDeferWindowPos(s_place_n);

    for (int i = 0; i < s_place_n; i++) {
        HWND hwnd = s_place[i].hwnd;
        RECT want = s_place[i].rect;               /* desired visible frame */
        ManagedWindow *mw = window_find(hwnd);

        /* Skip windows already parked exactly here — avoids flicker and the
         * move→LOCATIONCHANGE→re-tile feedback loop.
         *
         * needs_repaint is the exception, and the reason it exists: a window
         * that just came back on screen IS already parked exactly here — that
         * is precisely why it would otherwise be skipped and never told to
         * draw, which is what left a whole desktop black on the way back. */
        if (mw && mw->has_applied && !mw->needs_repaint &&
            rect_eq(mw->applied_rect, want)) continue;

        /* --- animation ---
         * anim_begin takes over the move and drives it over the next few
         * frames. applied_rect is still set to the TARGET below, not to the
         * frame currently on screen: the layout's bookkeeping stays truthful
         * about where the window is going. That is NOT what keeps the drift
         * detector off an in-flight window — every intermediate frame differs
         * from the target, which is precisely what drift looks like — so the
         * detector asks anim_is_animating instead. See the LOCATIONCHANGE
         * handler.
         *
         * anim_is_animating here as well as has_applied: something else may have
         * cleared has_applied while the window was mid-flight (a show, a
         * minimize, a rule re-assert). Placing it outright then would fight the
         * animation still running — one teleport per pass — where handing it
         * back to anim_begin simply re-targets the motion in progress. */
        if (mw && (mw->has_applied || anim_is_animating(hwnd)) &&
            anim_begin(hwnd, mw->applied_rect, want)) {
            mw->applied_rect = want;
            continue;
        }

        /* A window we have already had to route through the helper stays on
         * that path: batching it would fail the batch for everything else. */
        bool mw_needs_helper = mw && mw->needs_helper;

        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;

        /* Freshly revealed: forbid Windows from blitting the client area's
         * saved bits into the "new" position. They are the stale ones we are
         * trying to get rid of, and copying them forward is exactly how a
         * window keeps showing the wrong thing after a move that did not
         * actually move it. */
        if (mw && mw->needs_repaint) {
            flags |= SWP_NOCOPYBITS;
            mw->needs_repaint = false;
        }

        /* Windows owned by a higher-integrity process cannot go through our
         * DeferWindowPos batch — the whole batch would fail — so they take the
         * individual path, which falls back to the privileged helper and
         * records the outcome honestly. A window with no ManagedWindow has
         * nothing to record either way, so it may as well ride the batch. */
        if (hdwp && !mw_needs_helper) {
            RECT adj = window_adjust_for_frame(hwnd, want);
            HDWP next = DeferWindowPos(hdwp, hwnd, NULL, adj.left, adj.top,
                                       adj.right - adj.left,
                                       adj.bottom - adj.top, flags);
            if (next) {
                hdwp = next;
                s_place[i].flags = flags;
                batched[batched_n++] = i;
                continue;
            }
            /* Queueing itself failed — place it directly instead. */
        }

        place_one(hwnd, want, flags);
    }

    if (!hdwp) return;

    if (EndDeferWindowPos(hdwp)) {
        /* Committed: every queued window really is where it was put. */
        for (int b = 0; b < batched_n; b++) {
            ManagedWindow *mw = window_find(s_place[batched[b]].hwnd);
            if (!mw) continue;
            mw->applied_rect  = s_place[batched[b]].rect;
            mw->has_applied   = true;
            mw->place_refused = false;
        }
        return;
    }

    /* The batch was refused, so NOTHING in it moved. Replay it one window at a
     * time: window_apply_rect places what it can, falls back to the helper for
     * what it cannot, records only what actually happened, and — the part that
     * makes this self-correcting — sets needs_helper on the window that caused
     * this, so the next pass keeps it out of the batch and the other windows
     * are never held hostage to it again.
     *
     * This is why no up-front "is this window elevated?" probe is needed. That
     * would mean opening a process per window on the busiest path in the
     * program to answer a question the failing call answers for free. */
    log_msg(LOG_WARN, L"tiling: EndDeferWindowPos refused the batch (%lu) — "
                      L"placing %d window(s) individually",
            GetLastError(), batched_n);

    for (int b = 0; b < batched_n; b++) {
        Placement *p = &s_place[batched[b]];
        place_one(p->hwnd, p->rect, p->flags);
    }
}

/* ===========================================================================
 * Tile a specific desktop, by slot — valid only until the next desktop is
 * created or destroyed, so callers pass one they just looked up.
 * =========================================================================== */
void tile_desktop(int slot) {
    if (slot < 0 || slot >= g.desktop_count) return;

    Desktop *dt = &g.desktops[slot];

    /* Suppress WinEvent callbacks while we move windows around. */
    events_suppress_begin();

    place_reset();

    if (g.monitor_count <= 0) {
        tile_monitor(dt, 0, g.work_area);           /* single-monitor fallback */
    } else {
        for (int m = 0; m < g.monitor_count; m++)
            tile_monitor(dt, m, g.monitors[m].work_area);
    }

    flush_placements();
    window_enforce_zorder();

    events_suppress_end();
}

/* ===========================================================================
 * Tile the current desktop
 * =========================================================================== */
void tile_current(void) {
    tile_desktop(desktop_current_slot());
    /* Window geometry just changed — move the focus ring to match, and let the
     * bar re-read the layout and window counts. bar_refresh() compares against
     * what is already drawn, so calling it from here (which is often) is
     * cheap. */
    border_refresh();
    bar_refresh();
}
