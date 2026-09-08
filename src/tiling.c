#include "mshell.h"
#include "layout_math.h"

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

typedef struct { HWND hwnd; RECT rect; UINT flags; } Placement;
static Placement s_place[MAX_WINDOWS_PER_DESKTOP];
static int       s_place_n;
static int       s_inner;

static void place_reset(void) { s_place_n = 0; }

static RECT inset_rect(RECT r, int d) {
    r.left += d; r.top += d; r.right -= d; r.bottom -= d;
    return r;
}

static void emit(HWND hwnd, RECT cell) {
    if (!hwnd || s_place_n >= MAX_WINDOWS_PER_DESKTOP) return;
    RECT r = inset_rect(cell, s_inner / 2);
    if (r.right  - r.left < 20) r.right  = r.left + 20;
    if (r.bottom - r.top  < 20) r.bottom = r.top  + 20;
    s_place[s_place_n].hwnd  = hwnd;
    s_place[s_place_n].rect  = r;
    s_place[s_place_n].flags = 0;
    s_place_n++;
}

static void tree_emit_cb(HWND hwnd, RECT area, void *ctx) {
    (void)ctx;
    emit(hwnd, area);
}

static void emit_raw(HWND hwnd, RECT rect) {
    if (!hwnd || s_place_n >= MAX_WINDOWS_PER_DESKTOP) return;
    s_place[s_place_n].hwnd  = hwnd;
    s_place[s_place_n].rect  = rect;
    s_place[s_place_n].flags = 0;
    s_place_n++;
}

typedef struct { HWND hwnd; ManagedWindow *mw; } Client;

static int collect_clients(Desktop *dt, int mon, Client *out) {
    int n = 0;
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw || mw->is_floating || mw->tracked_only) continue;
        if (mw->app_hidden) continue;
        if (IsIconic(dt->windows[i])) continue;
        int wmon = mw->monitor;
        if (wmon < 0 || wmon >= g.monitor_count) wmon = 0;
        if (wmon != mon) continue;
        mw->layout_hidden = false;
        out[n].hwnd = dt->windows[i];
        out[n].mw   = mw;
        n++;
    }
    return n;
}

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

static void layout_master_stack(const LayoutParams *lp, Client *cs, int n) {
    int nm = lp->n_master; if (nm < 1) nm = 1; if (nm > n) nm = n;
    int nstack = n - nm;

    if (nstack == 0) {
        stack_vertical(cs, 0, n, lp->area);
        return;
    }

    int total_w  = lp->area.right - lp->area.left;
    int master_w = (int)((float)total_w * lp->master_ratio);
    int min_col  = total_w / 10; if (min_col < 1) min_col = 1;
    master_w = clamp_i(master_w, min_col, total_w - min_col);

    RECT ma = lp->area; ma.right = lp->area.left + master_w;
    RECT sa = lp->area; sa.left  = lp->area.left + master_w;
    stack_vertical(cs, 0,  nm, ma);
    stack_vertical(cs, nm, n,  sa);
}

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

static void layout_columns(const LayoutParams *lp, Client *cs, int n) {
    stack_horizontal(cs, 0, n, lp->area);
}

static void layout_centered(const LayoutParams *lp, Client *cs, int n) {
    int nm = lp->n_master; if (nm < 1) nm = 1; if (nm > n) nm = n;
    int nstack = n - nm;

    if (nstack == 0) { stack_vertical(cs, 0, n, lp->area); return; }

    int total_w  = lp->area.right - lp->area.left;
    int master_w = (int)((float)total_w * lp->master_ratio);
    master_w = clamp_i(master_w, total_w / 5, (total_w * 4) / 5);
    int side = (total_w - master_w) / 2;

    int ln = nstack / 2;
    int rn = nstack - ln;

    RECT mid   = lp->area;
    mid.left   = lp->area.left + side;
    mid.right  = lp->area.left + side + master_w;
    RECT left  = lp->area; left.right  = lp->area.left + side;
    RECT right = lp->area; right.left  = lp->area.left + side + master_w;

    if (ln == 0) mid.left = lp->area.left;

    stack_vertical(cs, 0,        nm,        mid);
    stack_vertical(cs, nm,       nm + rn,   right);
    stack_vertical(cs, nm + rn,  n,         left);
}

static void layout_spiral(const LayoutParams *lp, Client *cs, int n) {
    RECT r = lp->area;
    for (int i = 0; i < n; i++) {
        if (i == n - 1) { emit(cs[i].hwnd, r); break; }
        RECT cell = r;
        if (i % 2 == 0) {
            int w = r.right - r.left;
            cell.right = r.left + w / 2;
            r.left     = r.left + w / 2;
        } else {
            int h = r.bottom - r.top;
            cell.bottom = r.top + h / 2;
            r.top       = r.top + h / 2;
        }
        emit(cs[i].hwnd, cell);
    }
}

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

static void layout_monocle(const LayoutParams *lp, Client *cs, int n) {
    HWND focus  = lp->focus;
    HWND target = cs[0].hwnd;
    for (int i = 0; i < n; i++)
        if (cs[i].hwnd == focus) { target = focus; break; }

    for (int i = 0; i < n; i++) {
        if (cs[i].hwnd == target) {
            cs[i].mw->layout_hidden = false;
            emit(cs[i].hwnd, lp->area);
        } else {
            cs[i].mw->layout_hidden = true;
        }
    }
}

static void tile_monitor(Desktop *dt, int mon, RECT work) {
    Client cs[MAX_WINDOWS_PER_DESKTOP];
    int n = collect_clients(dt, mon, cs);
    if (n == 0) return;

    RECT full = (g.monitor_count > 0 && mon >= 0 && mon < g.monitor_count)
                  ? g.monitors[mon].full : work;
    int keep = 0;
    for (int i = 0; i < n; i++) {
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

    LayoutParams lp;
    const Monitor *M = (mon >= 0 && mon < g.monitor_count) ? &g.monitors[mon]
                                                           : NULL;
    int inner = (dt->inner_gap >= 0) ? dt->inner_gap
              : (M && M->inner_gap >= 0) ? M->inner_gap : g.cfg.inner_gap;
    int outer = (dt->outer_gap >= 0) ? dt->outer_gap
              : (M && M->outer_gap >= 0) ? M->outer_gap : g.cfg.outer_gap;
    if (g.cfg.smart_gaps && n == 1) { inner = 0; outer = 0; }

    int pre = outer - inner / 2;
    if (pre < 0) pre = 0;

    lp.area         = inset_rect(work, pre);
    lp.inner        = inner;
    lp.master_ratio = (M && M->master_ratio > 0.f) ? M->master_ratio
                                                    : dt->master_ratio;
    lp.n_master     = (M && M->n_master > 0) ? M->n_master : dt->n_master;
    lp.focus        = desktop_focused_of(dt);

    s_inner = lp.inner;

    Layout lay = (M && M->layout != LAYOUT_COUNT) ? M->layout : dt->layout;

    switch (lay) {
    case LAYOUT_TILING:   layout_master_stack(&lp, cs, n); break;
    case LAYOUT_MONOCLE:  layout_monocle(&lp, cs, n);      break;
    case LAYOUT_GRID:     layout_grid(&lp, cs, n);         break;
    case LAYOUT_SPIRAL:   layout_spiral(&lp, cs, n);       break;
    case LAYOUT_CENTERED: layout_centered(&lp, cs, n);     break;
    case LAYOUT_BSTACK:   layout_bstack(&lp, cs, n);       break;
    case LAYOUT_COLUMNS:  layout_columns(&lp, cs, n);      break;

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

static void place_one(HWND hwnd, RECT want, UINT flags) {
    ManagedWindow *mw = window_find(hwnd);
    if (mw) { window_apply_rect(mw, want, flags); return; }

    window_place_settled(hwnd, want, flags);
}

static void flush_placements(void) {
    if (s_place_n <= 0) return;

    for (int d = 0; d < g.desktop_count; d++) {
        Desktop *dt = &g.desktops[d];
        if (!desktop_is_visible(dt->id)) continue;
        for (int i = 0; i < dt->count; i++) {
            ManagedWindow *mw = window_find(dt->windows[i]);
            if (!mw || mw->is_floating) continue;
            if (mw->layout_hidden) window_hide(mw);
        }
    }

    for (int i = 0; i < s_place_n; i++)
        window_show(window_find(s_place[i].hwnd));

    int batched[MAX_WINDOWS_PER_DESKTOP];
    int batched_n = 0;

    HDWP hdwp = BeginDeferWindowPos(s_place_n);

    for (int i = 0; i < s_place_n; i++) {
        HWND hwnd = s_place[i].hwnd;
        RECT want = s_place[i].rect;
        ManagedWindow *mw = window_find(hwnd);

        if (mw && mw->has_applied && !mw->needs_repaint &&
            rect_eq(mw->applied_rect, want)) continue;

        bool crosses_dpi = window_placement_crosses_dpi(hwnd, want);
        if (crosses_dpi) anim_cancel(hwnd);

        if (!crosses_dpi &&
            mw && (mw->has_applied || anim_is_animating(hwnd)) &&
            anim_begin(hwnd, mw->applied_rect, want)) {
            mw->applied_rect = want;
            continue;
        }

        bool mw_needs_helper = mw && mw->needs_helper;

        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;

        if (mw && mw->needs_repaint) {
            flags |= SWP_NOCOPYBITS;
            mw->needs_repaint = false;
        }

        if (hdwp && !mw_needs_helper && !crosses_dpi) {
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
        }

        place_one(hwnd, want, flags);
    }

    if (!hdwp) return;

    if (EndDeferWindowPos(hdwp)) {
        for (int b = 0; b < batched_n; b++) {
            ManagedWindow *mw = window_find(s_place[batched[b]].hwnd);
            if (!mw) continue;
            mw->applied_rect  = s_place[batched[b]].rect;
            mw->has_applied   = true;
            mw->place_refused = false;
        }
        return;
    }

    log_msg(LOG_WARN, L"tiling: EndDeferWindowPos refused the batch (%lu) — "
                      L"placing %d window(s) individually",
            GetLastError(), batched_n);

    for (int b = 0; b < batched_n; b++) {
        Placement *p = &s_place[batched[b]];
        place_one(p->hwnd, p->rect, p->flags);
    }
}

void tile_current(void) {
    events_suppress_begin();
    place_reset();

    if (g.monitor_count <= 0) {
        tile_monitor(desktop_current(), 0, g.work_area);
    } else {
        for (int m = 0; m < g.monitor_count; m++) {
            Desktop *dt = desktop_by_id(desktop_on_monitor(m));
            if (!dt) continue;
            tile_monitor(dt, m, g.monitors[m].work_area);
        }
    }

    flush_placements();
    window_enforce_zorder();
    events_suppress_end();

    border_refresh();
    bar_refresh();
}
