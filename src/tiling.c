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
typedef struct { HWND hwnd; RECT rect; } Placement;
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
    s_place[s_place_n].hwnd = hwnd;
    s_place[s_place_n].rect = r;
    s_place_n++;
}

/* Record a target verbatim — no gap inset, no minimum. Fullscreen windows are
 * placed with this: they are outside the layout and must reach every edge. */
static void emit_raw(HWND hwnd, RECT rect) {
    if (!hwnd || s_place_n >= MAX_WINDOWS_PER_DESKTOP) return;
    s_place[s_place_n].hwnd = hwnd;
    s_place[s_place_n].rect = rect;
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
        if (!mw || mw->is_floating) continue;
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

    for (int i = 0; i < n; i++) {
        if (cs[i].hwnd == target)          emit(cs[i].hwnd, lp->area);
        else if (IsWindowVisible(cs[i].hwnd)) ShowWindow(cs[i].hwnd, SW_HIDE);
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
    int inner = (dt->inner_gap >= 0) ? dt->inner_gap : g.inner_gap;
    int outer = (dt->outer_gap >= 0) ? dt->outer_gap : g.outer_gap;
    if (g.smart_gaps && n == 1) { inner = 0; outer = 0; }

    int pre = outer - inner / 2;
    if (pre < 0) pre = 0;

    lp.area         = inset_rect(work, pre);
    lp.inner        = inner;
    lp.master_ratio = dt->master_ratio;
    lp.n_master     = dt->n_master;

    s_inner = lp.inner;   /* emit() reads this for the per-cell half-gap */

    switch (dt->layout) {
    case LAYOUT_TILING:   layout_master_stack(&lp, cs, n); break;
    case LAYOUT_MONOCLE:  layout_monocle(&lp, cs, n);      break;
    case LAYOUT_GRID:     layout_grid(&lp, cs, n);         break;
    case LAYOUT_SPIRAL:   layout_spiral(&lp, cs, n);       break;
    case LAYOUT_CENTERED: layout_centered(&lp, cs, n);     break;
    case LAYOUT_BSTACK:   layout_bstack(&lp, cs, n);       break;
    case LAYOUT_COLUMNS:  layout_columns(&lp, cs, n);      break;
    case LAYOUT_COUNT:    break;
    }
}

static bool rect_eq(RECT a, RECT b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

/* Apply the whole placement buffer in one deferred batch. */
static void flush_placements(void) {
    if (s_place_n <= 0) return;

    /* ShowWindow can't be deferred — reveal any window a prior monocle pass
     * hid before we start moving things. */
    for (int i = 0; i < s_place_n; i++)
        if (!IsWindowVisible(s_place[i].hwnd))
            ShowWindow(s_place[i].hwnd, SW_SHOWNOACTIVATE);

    HDWP hdwp = BeginDeferWindowPos(s_place_n);

    for (int i = 0; i < s_place_n; i++) {
        HWND hwnd = s_place[i].hwnd;
        RECT want = s_place[i].rect;               /* desired visible frame */
        ManagedWindow *mw = window_find(hwnd);

        /* Skip windows already parked exactly here — avoids flicker and the
         * move→LOCATIONCHANGE→re-tile feedback loop. */
        if (mw && mw->has_applied && rect_eq(mw->applied_rect, want)) continue;

        /* A window we have already had to route through the helper stays on
         * that path: batching it would fail the batch for everything else. */
        bool mw_needs_helper = mw && mw->needs_helper;

        RECT adj = window_adjust_for_frame(hwnd, want);
        int x = adj.left, y = adj.top;
        int w = adj.right - adj.left, h = adj.bottom - adj.top;
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;

        /* Windows owned by a higher-integrity process cannot go through our
         * DeferWindowPos batch — the whole batch would fail — so they take the
         * individual path, which falls back to the privileged helper. */
        if (hdwp && !mw_needs_helper) {
            HDWP next = DeferWindowPos(hdwp, hwnd, NULL, x, y, w, h, flags);
            if (next) hdwp = next;
            else      window_set_pos(hwnd, x, y, w, h, flags);
        } else {
            window_set_pos(hwnd, x, y, w, h, flags);
        }

        if (mw) { mw->applied_rect = want; mw->has_applied = true; }
    }

    if (hdwp) EndDeferWindowPos(hdwp);
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
