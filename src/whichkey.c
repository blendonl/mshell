/*
 * whichkey.c — submap hint popup ("which-key").
 *
 * When you enter a submap (Win+r resize, Win+x window, …) mshell can pop up a
 * small panel listing that submap's keys and what each one does, the way
 * which-key does in Emacs/Neovim. It is a non-activating layered overlay (same
 * family as border.c / background.c) anchored to the focused monitor —
 * bottom-centre unless mshell.set_whichkey{position=…} says otherwise — and it
 * disappears the moment you leave the submap.
 *
 * Everything about its shape is configurable (placement, maximum size,
 * spacing, font, chrome), so this file measures and paints while the grid
 * arithmetic that decides what fits lives in whichkey_math.c, where `make
 * test` can reach it without a Windows machine.
 *
 * Threading: g.current_map flips inside the low-level keyboard hook, on the
 * hook thread; GDI and window calls must run on the main thread. So the hook
 * only POSTs WM_MSHELL_SUBMAP (keyboard.c: notify_submap) and the main thread
 * handles it by calling whichkey_notify(), which re-reads the authoritative
 * current map under kb_lock and then shows / hides / schedules the panel.
 * Reading a submap's bindings needs no lock: the hook never mutates bindings
 * (only the current_map pointer), and config reload — the only writer — runs on
 * this same main thread, so it can never interleave with a paint.
 */

#include "mshell.h"
#include "overlay.h"
#include "whichkey_math.h"

static const wchar_t *WK_CLASS = L"mshell_WhichKey";

#define WK_TIMER_ID    1

/* Win11 rounded corners; harmless (ignored) on older Windows. */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

typedef struct {
    wchar_t key[24];
    wchar_t label[96];
} WkRow;

/* Prepared state: filled by whichkey_show, consumed by WM_PAINT. Only ever
 * touched on the main thread. */
static OverlayFont s_font;      /* caches the DPI and face it was built for */
static WkRow   s_rows[WHICHKEY_MAX_ROWS];
static int     s_count;
static int     s_cols, s_per_col;
static int     s_key_w, s_label_w, s_row_h, s_header_h;
static wchar_t s_title[80];

/* The configured metrics (design pixels at 96 DPI) scaled for the monitor the
 * panel is about to appear on. mshell is per-monitor DPI aware, so nothing
 * scales them for us — without this the panel renders at a third of its
 * intended size on a 300% display. */
static int     s_pad, s_key_gap, s_col_gap, s_row_vpad, s_hdr_gap, s_border_w;

#define wk_dpi_scale(px, dpi) overlay_scale((px), (dpi))

/* Rebuild the font and the scaled metrics for `dpi`, if they aren't already. */
static void wk_apply_dpi(UINT dpi) {
    s_pad      = wk_dpi_scale(g.whichkey_padding,  dpi);
    s_key_gap  = wk_dpi_scale(g.whichkey_key_gap,  dpi);
    s_col_gap  = wk_dpi_scale(g.whichkey_col_gap,  dpi);
    s_row_vpad = wk_dpi_scale(g.whichkey_row_gap,  dpi);
    s_hdr_gap  = wk_dpi_scale(g.whichkey_hdr_gap,  dpi);
    s_border_w = wk_dpi_scale(g.whichkey_border_w, dpi);

    /* overlay_font_face rebuilds only when dpi, size or family actually
     * changed, so calling this on every show is free. */
    overlay_font_face(&s_font, dpi, wk_dpi_scale(g.whichkey_font_size, dpi),
                      g.whichkey_font);
}

/* max_width / max_height are deliberately one field each rather than two:
 * 0 means "the monitor is the only limit", a value up to 1 is a fraction of
 * the monitor, and anything larger is design pixels. Returns device pixels,
 * or 0 for "unbounded". */
static int wk_resolve_max(float v, int mon_px, UINT dpi) {
    if (v <= 0.0f) return 0;
    if (v <= 1.0f) return (int)((float)mon_px * v);
    return wk_dpi_scale((int)v, dpi);
}

/* The human-readable label for one binding. spawn shows its command, the
 * desktop actions show the desktop they go to, enter_submap shows "+name" (the
 * which-key idiom for a nested prefix), and everything else falls back to the
 * action's canonical name. */
static void wk_label(const KeyBinding *b, wchar_t *out, int cap) {
    /* A label the config wrote wins over anything derived: "browser" reads
     * better in a hint panel than "spawn firefox.exe", and the config is the
     * only thing that knows which it meant. */
    if (b->desc && b->desc[0]) {
        _snwprintf(out, cap, L"%ls", b->desc);
        out[cap - 1] = L'\0';
        return;
    }
    if (b->action == ACTION_ENTER_SUBMAP && b->submap && b->submap->name) {
        _snwprintf(out, cap, L"+%ls", b->submap->name);
    } else if (b->action == ACTION_SPAWN && b->command) {
        /* Show the arguments too — "wt.exe" and "wt.exe -p Ubuntu" on adjacent
         * keys are otherwise indistinguishable in the hint. */
        if (b->args && b->args[0])
            _snwprintf(out, cap, L"%ls %ls", b->command, b->args);
        else
            _snwprintf(out, cap, L"%ls", b->command);
    } else if ((b->action == ACTION_SWITCH_DESKTOP ||
                b->action == ACTION_MOVE_TO_DESKTOP) && b->command) {
        /* "web" reads better than "switch_desktop" in a map that is nothing but
         * desktops — which is what the go/move maps are. */
        _snwprintf(out, cap, L"%ls%ls",
                   b->action == ACTION_MOVE_TO_DESKTOP ? L"→ " : L"", b->command);
    } else {
        const char *n = action_enum_to_name(b->action);
        _snwprintf(out, cap, L"%hs", n ? n : "?");
    }
    out[cap - 1] = L'\0';
}

void whichkey_hide(void) {
    if (!g.whichkey_window) return;
    KillTimer(g.whichkey_window, WK_TIMER_ID);
    ShowWindow(g.whichkey_window, SW_HIDE);
}

/* Measure `map`, size + position the panel over the focused monitor, and show
 * it. Safe to call repeatedly (e.g. entering a nested submap). */
static void whichkey_show(KeyMap *map) {
    if (!g.whichkey_window || !map) return;

    /* --- which monitor, and therefore at what DPI ---
     * Resolved first: the font and every metric below depend on it, and the
     * text measuring further down has to happen with the final font. */
    int  mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
              ? g.focused_monitor : g.primary_monitor;
    UINT dpi = monitor_dpi(mi);
    wk_apply_dpi(dpi);

    /* --- gather the rows we can actually label --- */
    s_count = 0;
    for (int i = 0; i < map->count && s_count < WHICHKEY_MAX_ROWS; i++) {
        const KeyBinding *b = &map->bindings[i];
        const char *kn = vk_to_key_name(b->vk);
        if (!kn) continue;                      /* no display name — skip */
        WkRow *r = &s_rows[s_count++];
        _snwprintf(r->key, 24, L"%hs", kn);
        r->key[23] = L'\0';
        wk_label(b, r->label, 96);
    }
    if (s_count == 0) { whichkey_hide(); return; }

    /* Sort by label, with submap prefixes ("+window") first.
     *
     * Binding order is declaration order, which is an artefact of how the
     * config was written rather than anything the reader knows — so a panel of
     * twenty keys was previously a list to scan rather than read. Prefixes
     * first because they are the ones that lead somewhere else, and an
     * insertion sort because the list is at most WHICHKEY_MAX_ROWS long. */
    for (int i = 1; i < s_count; i++) {
        WkRow key = s_rows[i];
        bool  key_pfx = (key.label[0] == L'+');
        int j = i - 1;
        while (j >= 0) {
            bool j_pfx = (s_rows[j].label[0] == L'+');
            bool after = (j_pfx != key_pfx) ? (key_pfx && !j_pfx)
                                            : (_wcsicmp(s_rows[j].label,
                                                        key.label) > 0);
            if (!after) break;
            s_rows[j + 1] = s_rows[j];
            j--;
        }
        s_rows[j + 1] = key;
    }

    /* Say so rather than silently showing a subset: a map with more keys than
     * fit is exactly when the panel matters most. */
    if (map->count > WHICHKEY_MAX_ROWS)
        log_msg(LOG_WARN, L"which-key: '%ls' has %d bindings; showing the "
                          L"first %d", map->name ? map->name : L"?",
                map->count, WHICHKEY_MAX_ROWS);

    /* Header suffix tells you how to leave: a persisting map names its exit key
     * (Escape unless a custom one replaced it); a one-shot map just says so. */
    wchar_t hint[40];
    if (map->persist) {
        DWORD       ex = map->exit_vk ? map->exit_vk : VK_ESCAPE;
        const char *kn = vk_to_key_name(ex);
        _snwprintf(hint, 40, L"(persist · %hs)", kn ? kn : "Esc");
    } else {
        wcscpy(hint, L"(one-shot)");
    }
    hint[39] = L'\0';

    _snwprintf(s_title, 80, L"%ls  %ls",
               map->name ? map->name : L"submap", hint);
    s_title[79] = L'\0';

    /* --- measure text with our font ---
     * Before the grid shape, which needs the row height to know how many rows
     * a configured max_height leaves room for. */
    HDC   dc  = GetDC(g.whichkey_window);
    HFONT old = (HFONT)SelectObject(dc, s_font.font);

    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    s_row_h    = tm.tmHeight + s_row_vpad;
    s_header_h = tm.tmHeight + s_hdr_gap;

    s_key_w = s_label_w = 0;
    for (int i = 0; i < s_count; i++) {
        SIZE sz;
        GetTextExtentPoint32W(dc, s_rows[i].key,
                              (int)wcslen(s_rows[i].key), &sz);
        if (sz.cx > s_key_w) s_key_w = sz.cx;
        GetTextExtentPoint32W(dc, s_rows[i].label,
                              (int)wcslen(s_rows[i].label), &sz);
        if (sz.cx > s_label_w) s_label_w = sz.cx;
    }
    SIZE hsz;
    GetTextExtentPoint32W(dc, s_title, (int)wcslen(s_title), &hsz);

    SelectObject(dc, old);
    ReleaseDC(g.whichkey_window, dc);

    /* --- the box the panel has to fit inside ---
     * (`mi` was resolved at the top, since the DPI it implies drove everything
     * measured above.) The margin is both the gap to the monitor edge and the
     * inset the panel may never grow past, so a panel pinned to a corner keeps
     * the same breathing room a centred one gets. */
    RECT mon   = g.monitors[mi].full;
    int  mon_w = mon.right - mon.left;
    int  mon_h = mon.bottom - mon.top;

    int margin = (g.whichkey_margin >= 0)
                 ? wk_dpi_scale(g.whichkey_margin, dpi)
                 : mon_h / 20;    /* auto — the historical bottom margin */
    if (margin * 2 >= mon_w || margin * 2 >= mon_h) margin = 0;

    int max_w = wk_resolve_max(g.whichkey_max_w, mon_w, dpi);
    int max_h = wk_resolve_max(g.whichkey_max_h, mon_h, dpi);
    if (max_w <= 0 || max_w > mon_w - margin * 2) max_w = mon_w - margin * 2;
    if (max_h <= 0 || max_h > mon_h - margin * 2) max_h = mon_h - margin * 2;

    /* --- grid shape and panel size (pure arithmetic; see whichkey_math.c) --- */
    WkMetrics wm = {
        .count     = s_count,
        .max_rows  = g.whichkey_max_rows,
        .key_w     = s_key_w,
        .label_w   = s_label_w,
        .header_w  = hsz.cx,
        .row_h     = s_row_h,
        .header_h  = s_header_h,
        .pad       = s_pad,
        .key_gap   = s_key_gap,
        .col_gap   = s_col_gap,
        .min_label = wk_dpi_scale(48, dpi),   /* ~6 characters */
        .max_w     = max_w,
        .max_h     = max_h,
    };
    WkLayout lay;
    wk_layout(&wm, &lay);

    s_cols    = lay.cols;
    s_per_col = lay.per_col;
    s_label_w = lay.label_w;

    /* Say which keys went missing rather than showing a subset that looks
     * complete — a panel is worth less than nothing if it is quietly partial. */
    if (lay.shown < s_count) {
        log_msg(LOG_WARN, L"which-key: '%ls' — %d of %d bindings do not fit "
                          L"the configured max_width/max_height",
                map->name ? map->name : L"?", s_count - lay.shown, s_count);
        s_count = lay.shown;
    }

    /* --- placement: the configured anchor on the focused monitor --- */
    int halign, valign;
    switch (g.whichkey_pos) {
    case WK_POS_LEFT: case WK_POS_TOP_LEFT: case WK_POS_BOTTOM_LEFT:
        halign = 0; break;
    case WK_POS_RIGHT: case WK_POS_TOP_RIGHT: case WK_POS_BOTTOM_RIGHT:
        halign = 2; break;
    default:
        halign = 1; break;
    }
    switch (g.whichkey_pos) {
    case WK_POS_TOP: case WK_POS_TOP_LEFT: case WK_POS_TOP_RIGHT:
        valign = 0; break;
    case WK_POS_CENTER: case WK_POS_LEFT: case WK_POS_RIGHT:
        valign = 1; break;
    default:
        valign = 2; break;
    }

    int w = lay.w, h = lay.h, x, y;
    wk_anchor(halign, valign, mon.left, mon.top, mon_w, mon_h, w, h, margin,
              &x, &y);

    /* Chrome that a reload may have changed. Both are cheap, and applying them
     * here rather than at init is what makes set_whichkey take effect without
     * a restart. */
    SetLayeredWindowAttributes(g.whichkey_window, 0, g.whichkey_opacity,
                               LWA_ALPHA);
    DWORD corner = g.whichkey_rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g.whichkey_window, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    SetWindowPos(g.whichkey_window, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.whichkey_window, NULL, TRUE);
}

/* Re-read the active map (authoritative, under the lock) and act on it. Called
 * on the main thread from the WM_MSHELL_SUBMAP handler and from the delay timer.
 * A submap → show or schedule; root / disabled → hide. */
void whichkey_notify(void) {
    if (!g.whichkey_window) return;
    KillTimer(g.whichkey_window, WK_TIMER_ID);   /* cancel a pending show */
    if (!g.whichkey_enabled) { whichkey_hide(); return; }

    kb_lock();
    KeyMap *m = (g.current_map && g.current_map != g.root_map)
                ? g.current_map : NULL;
    kb_unlock();

    if (!m) { whichkey_hide(); return; }
    if (g.whichkey_delay <= 0) whichkey_show(m);
    else SetTimer(g.whichkey_window, WK_TIMER_ID, (UINT)g.whichkey_delay, NULL);
}

static LRESULT CALLBACK wk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == WK_TIMER_ID) {
            KillTimer(hwnd, WK_TIMER_ID);
            /* Re-read now, in case the map changed during the delay (or a reload
             * rebuilt the keymaps): show what is *currently* active, never a
             * stale pointer. */
            kb_lock();
            KeyMap *m = (g.current_map && g.current_map != g.root_map)
                        ? g.current_map : NULL;
            kb_unlock();
            if (m) whichkey_show(m); else whichkey_hide();
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* fully painted in WM_PAINT — skip erase to avoid flicker */

    case WM_PAINT: {
        /* Double-buffered so the panel never flickers as it repaints. */
        OverlayPaint op;
        HDC mdc = overlay_paint_begin(&op, hwnd);
        if (!mdc) return 0;
        int  W = op.w, H = op.h;
        RECT rc = { 0, 0, W, H };

        overlay_fill(mdc, &rc, g.whichkey_bg);

        /* Outline in the accent colour, as four fills rather than a wide pen:
         * GDI centres a pen on its path, so half of anything thicker than a
         * pixel would fall outside the client area and be clipped away. */
        int bw = s_border_w;
        if (bw > 0) {
            if (bw * 2 > H) bw = H / 2;
            if (bw * 2 > W) bw = W / 2;
            RECT e;
            e = (RECT){ 0, 0, W, bw };          overlay_fill(mdc, &e, g.whichkey_border);
            e = (RECT){ 0, H - bw, W, H };      overlay_fill(mdc, &e, g.whichkey_border);
            e = (RECT){ 0, 0, bw, H };          overlay_fill(mdc, &e, g.whichkey_border);
            e = (RECT){ W - bw, 0, W, H };      overlay_fill(mdc, &e, g.whichkey_border);
        }

        HFONT of = (HFONT)SelectObject(mdc, s_font.font);
        SetBkMode(mdc, TRANSPARENT);

        /* Text is drawn through DrawTextW with DT_END_ELLIPSIS so that a
         * configured max_width truncates at a character boundary with an "…"
         * rather than mid-glyph. DT_NOPREFIX because labels are arbitrary
         * strings — a spawn command containing '&' must not turn into an
         * accelerator underline. */
        const UINT fmt = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS |
                         DT_TOP | DT_LEFT;
        const int  right = W - s_pad;

        /* header */
        SetTextColor(mdc, g.whichkey_key_fg);
        RECT hr = { s_pad, s_pad, right, s_pad + s_header_h };
        DrawTextW(mdc, s_title, -1, &hr, fmt);

        /* rows, laid out column-major */
        int cell_w = s_key_w + s_key_gap + s_label_w;
        int y0     = s_pad + s_header_h;
        for (int i = 0; i < s_count; i++) {
            int col = i / s_per_col;
            int row = i % s_per_col;
            int cx  = s_pad + col * (cell_w + s_col_gap);
            int cy  = y0 + row * s_row_h;

            /* key: right-aligned within the key column, in the accent colour */
            SIZE ksz;
            GetTextExtentPoint32W(mdc, s_rows[i].key,
                                  (int)wcslen(s_rows[i].key), &ksz);
            SetTextColor(mdc, g.whichkey_key_fg);
            TextOutW(mdc, cx + (s_key_w - ksz.cx), cy,
                     s_rows[i].key, (int)wcslen(s_rows[i].key));

            /* label: left-aligned after the gap, in the normal text colour */
            int lx = cx + s_key_w + s_key_gap;
            int lr = cx + cell_w;
            if (lr > right) lr = right;
            if (lx < lr) {
                SetTextColor(mdc, g.whichkey_fg);
                RECT rr = { lx, cy, lr, cy + s_row_h };
                DrawTextW(mdc, s_rows[i].label, -1, &rr, fmt);
            }
        }

        SelectObject(mdc, of);
        overlay_paint_end(&op);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool whichkey_init(void) {
    if (!overlay_register(WK_CLASS, wk_wndproc, false)) return false;

    /* Layered + noactivate + toolwindow + topmost: a transient hint that never
     * takes focus, never appears in Alt+Tab, is skipped by our own window
     * management (toolwindow), and floats above the tiled grid. */
    g.whichkey_window = overlay_create(
        WK_CLASS,
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!g.whichkey_window) return false;

    /* Slight translucency by default, like the focus ring. Both of these are
     * re-applied on every show, so a reload changes them live; setting them
     * here just means the first frame is already right. */
    SetLayeredWindowAttributes(g.whichkey_window, 0, g.whichkey_opacity,
                               LWA_ALPHA);

    DWORD corner = g.whichkey_rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g.whichkey_window, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    /* The font is built on first show, not here: it depends on the DPI of
     * whichever monitor the panel appears on, and is rebuilt when that
     * changes (wk_apply_dpi). */
    return true;
}

void whichkey_shutdown(void) {
    if (g.whichkey_window) {
        KillTimer(g.whichkey_window, WK_TIMER_ID);
    }
    overlay_destroy(&g.whichkey_window, WK_CLASS);
    overlay_font_free(&s_font);
}
