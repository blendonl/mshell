/*
 * bar.c — status bar.
 *
 * mshell removes the taskbar and, until now, put nothing in its place. That is
 * a bigger hole here than in most tiling WMs: desktops are DYNAMIC and
 * identified by name, so the set of them changes as you work and there is no
 * fixed 1..9 to reason about. Without a bar you cannot see which desktop you
 * are on, which others exist, or what layout is active — and the which-key
 * panel only appears while a submap is open.
 *
 * Structurally this is the same kind of object as whichkey.c and background.c:
 * a non-activating, tool-window overlay painted with double-buffered GDI.
 *
 * There are two SHAPES for the same content, chosen by g.bar_mode:
 *
 *   TOP_BAR   An edge-to-edge strip, ONE PER MONITOR — a bar you can only see
 *             on the primary display is not much use on a multi-head desk.
 *             They all show the same thing: the desktop set is global in
 *             mshell (a desktop spans every monitor), so there is no
 *             per-monitor desktop list to show. It RESERVES SPACE:
 *             bar_reserve_work_area() shrinks each monitor's work area, and
 *             because the tiler lays out into work_area while the fullscreen
 *             paths use the monitor's full bounds, tiled windows sit below the
 *             bar and a fullscreen window still covers it. That fell out of an
 *             existing distinction rather than needing a new one.
 *
 *   FLOATING  ONE panel in the middle of the focused monitor, sections stacked
 *             instead of laid out in a row, reserving nothing. It follows the
 *             focus rather than appearing on every display, because three
 *             copies of the same clock in the middle of three screens is not
 *             three times as useful. Having height to spend is what lets it
 *             carry what a one-line strip cannot: a large clock, the date, and
 *             mshell's live notifications — in this mode the panel IS the
 *             notification surface and notify.c stands its toasts down.
 *
 * The two shapes share their content (built once, in rebuild_content) and
 * their change detection, and diverge only in measuring and painting.
 */

#include "mshell.h"
#include "overlay.h"

static const wchar_t *BAR_CLASS = L"mshell_Bar";

#define BAR_TIMER_ID   1
#define BAR_PAD        10   /* design px at 96 DPI, scaled per monitor */
#define BAR_GAP        14   /* between sections                        */
#define BAR_CHIP_PAD   8    /* inside a desktop chip                   */

/* Floating panel, design px at 96 DPI. Its type scale comes from bar_height
 * (see float_clock_px), so one knob still sizes the whole bar. */
#define FLOAT_PAD        22   /* panel inner padding                       */
#define FLOAT_ROW_GAP    12   /* between stacked sections                  */
#define FLOAT_MIN_W      260
#define FLOAT_MAX_W      560
#define FLOAT_BULLET     7    /* the dot beside a notification             */
#define FLOAT_BULLET_GAP 10
#define FLOAT_NOTES_MAX  4    /* notifications listed; the newest ones     */

/* Win11 rounded corners; harmless (ignored) on older Windows. */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

/* One font per monitor: displays can have different DPI, so a single shared
 * font would be wrong on at least one of them. */
static OverlayFont s_font[MAX_MONITORS];

/* The floating panel is a single window that moves between monitors, so it
 * keeps its own pair — body text and the big clock — rebuilt for whichever
 * DPI it currently sits on. */
static OverlayFont s_float_font;
static OverlayFont s_float_clock_font;

/* What is currently on screen, so a refresh can skip repainting when nothing
 * changed. The bar is refreshed from tiling and focus paths that run often. */
static wchar_t s_desktops[512];
static wchar_t s_layout[32];
static wchar_t s_title[256];
static wchar_t s_clock[32];
static wchar_t s_date[128];                    /* floating only */
static NotifyItem s_notes[FLOAT_NOTES_MAX];    /* floating only */
static int        s_note_count;

/* Where the floating panel currently is, so a refresh that changes neither its
 * size nor its position does not move a window. */
static RECT s_float_rect;

#define bar_scale(px, dpi) overlay_scale((px), (dpi))

/* Which monitor a given bar window belongs to, or -1. */
static int bar_monitor_of(HWND hwnd) {
    for (int i = 0; i < g.monitor_count; i++)
        if (g.bar_windows[i] == hwnd) return i;
    return -1;
}

/* The monitor the floating panel belongs on: wherever the focus is. */
static int bar_float_monitor(void) {
    return (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
           ? g.focused_monitor : g.primary_monitor;
}

/* Body text is sized off the bar height rather than fixed, so a taller bar
 * gets larger text instead of a lot of empty space. */
static int bar_text_px(UINT dpi) {
    int px = bar_scale(g.bar_height, dpi) / 2;
    return px < 10 ? 10 : px;
}

static HFONT bar_font(int mon) {
    if (mon < 0 || mon >= MAX_MONITORS) return NULL;

    /* Rebuilds only when the DPI or the computed size actually changed, so a
     * bar_height change at reload is picked up without a special case. */
    UINT dpi = monitor_dpi(mon);
    return overlay_font(&s_font[mon], dpi, bar_text_px(dpi));
}

/* ===========================================================================
 * Content
 * =========================================================================== */

/* "1 2 web*" — every live desktop, the current one marked. The marker is a
 * trailing '*' rather than only a colour so the bar still reads correctly in a
 * screenshot, at a glance, or for anyone who can't separate the two colours. */
static void build_desktops(wchar_t *out, size_t cap) {
    out[0] = L'\0';
    size_t used = 0;

    for (int i = 0; i < g.desktop_count; i++) {
        const Desktop *d  = &g.desktops[i];
        bool           cur = (d->id == g.current_desktop_id);

        wchar_t chunk[DESKTOP_NAME_MAX + 8];
        _snwprintf(chunk, DESKTOP_NAME_MAX + 8, L"%ls%ls%ls",
                   used ? L"  " : L"", d->name, cur ? L"*" : L"");
        chunk[DESKTOP_NAME_MAX + 7] = L'\0';

        size_t n = wcslen(chunk);
        if (used + n + 1 >= cap) break;
        memcpy(out + used, chunk, n * sizeof(wchar_t));
        used += n;
        out[used] = L'\0';
    }
}

static const wchar_t *layout_label(Layout l) {
    switch (l) {
    case LAYOUT_TILING:   return L"[]=";
    case LAYOUT_MONOCLE:  return L"[M]";
    case LAYOUT_GRID:     return L"[#]";
    case LAYOUT_SPIRAL:   return L"[@]";
    case LAYOUT_CENTERED: return L"[|]";
    case LAYOUT_BSTACK:   return L"[T]";
    case LAYOUT_BSP:      return L"[+]";
    case LAYOUT_COLUMNS:  return L"|||";
    case LAYOUT_COUNT:    break;
    }
    return L"[]=";
}

/* Rebuild every section. True if anything differs from what is on screen. */
static bool rebuild_content(void) {
    wchar_t desktops[512] = {0}, layout[32] = {0};
    wchar_t title[256]    = {0}, clock[32]  = {0};
    wchar_t date[128]     = {0};
    NotifyItem notes[FLOAT_NOTES_MAX];
    int        note_count = 0;
    bool       floating   = (g.bar_mode == BAR_MODE_FLOATING);

    memset(notes, 0, sizeof notes);

    if (g.bar_modules & BAR_MOD_DESKTOPS)
        build_desktops(desktops, 512);

    const Desktop *cur = desktop_current();
    if (g.bar_modules & BAR_MOD_LAYOUT)
        _snwprintf(layout, 32, L"%ls", layout_label(cur->layout));

    if (g.bar_modules & BAR_MOD_TITLE) {
        HWND f = desktop_get_focused();
        if (f && IsWindow(f)) GetWindowTextW(f, title, 256);
        title[255] = L'\0';
    }

    if (g.bar_modules & BAR_MOD_CLOCK) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        _snwprintf(clock, 32, L"%02d:%02d", st.wHour, st.wMinute);
        clock[31] = L'\0';

        /* The date exists only in the floating panel: a strip has no second
         * line to put it on. It is the user's long-date format rather than one
         * of ours because it is read, not parsed. */
        if (floating &&
            !GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, NULL,
                            date, 128))
            date[0] = L'\0';
    }

    if (floating && (g.bar_modules & BAR_MOD_NOTIFICATIONS))
        note_count = notify_recent(notes, FLOAT_NOTES_MAX);

    bool changed = wcscmp(desktops, s_desktops) != 0 ||
                   wcscmp(layout,   s_layout)   != 0 ||
                   wcscmp(title,    s_title)    != 0 ||
                   wcscmp(clock,    s_clock)    != 0 ||
                   wcscmp(date,     s_date)     != 0 ||
                   note_count != s_note_count;

    for (int i = 0; !changed && i < note_count; i++)
        changed = notes[i].kind != s_notes[i].kind ||
                  wcscmp(notes[i].text, s_notes[i].text) != 0;

    if (changed) {
        wcscpy(s_desktops, desktops);
        wcscpy(s_layout,   layout);
        wcscpy(s_title,    title);
        wcscpy(s_clock,    clock);
        wcscpy(s_date,     date);
        memcpy(s_notes, notes, sizeof notes);
        s_note_count = note_count;
    }
    return changed;
}

/* Draw the desktop chips from `x`, or only measure them, and return the width
 * they occupy (no trailing pad). Shared so the strip — which starts them at
 * the left edge — and the panel — which has to know their width before it can
 * centre them — highlight the current desktop identically. */
static int draw_desktop_chips(HDC dc, int x, int y, int chip_pad, bool draw) {
    const wchar_t *p = s_desktops;
    int x0 = x;

    while (*p) {
        while (*p == L' ') p++;
        const wchar_t *start = p;
        while (*p && *p != L' ') p++;
        int len = (int)(p - start);
        if (!len) break;

        if (draw) {
            bool current = (start[len - 1] == L'*');
            SetTextColor(dc, current ? g.bar_accent : g.bar_dim);
            TextOutW(dc, x, y, start, len);
        }

        SIZE sz;
        GetTextExtentPoint32W(dc, start, len, &sz);
        x += sz.cx + chip_pad;
    }
    return x > x0 ? x - x0 - chip_pad : 0;
}

/* ===========================================================================
 * Floating panel — measure and paint
 *
 * One function does both: the layout pass runs it with draw = false to learn
 * the height a given width implies, and WM_PAINT runs it again with draw =
 * true. Keeping them in one body is what stops the two from drifting apart,
 * which is the failure mode of a measure/paint pair that share only a comment.
 * =========================================================================== */

static int float_clock_px(UINT dpi) {
    int px = bar_scale(g.bar_height, dpi) * 3 / 2;
    return px < 20 ? 20 : px;
}

/* A 1px (scaled) rule between sections. */
static void float_rule(HDC dc, int W, int y, int pad, UINT dpi, bool draw) {
    if (!draw) return;
    int th = bar_scale(1, dpi);
    if (th < 1) th = 1;
    RECT r = { pad, y, W - pad, y + th };
    overlay_fill(dc, &r, g.bar_dim);
}

/* Lay the panel out into `W` pixels of width, painting if asked, and return
 * the total height it needs. 0 when there is nothing to show at all. */
static int float_render(HDC dc, int W, UINT dpi, bool draw) {
    int pad    = bar_scale(FLOAT_PAD,     dpi);
    int gap    = bar_scale(FLOAT_ROW_GAP, dpi);
    int rule_h = bar_scale(1, dpi) + gap;

    HFONT body = overlay_font(&s_float_font,       dpi, bar_text_px(dpi));
    HFONT big  = overlay_font(&s_float_clock_font, dpi, float_clock_px(dpi));
    HFONT old  = (HFONT)SelectObject(dc, body);
    SetBkMode(dc, TRANSPARENT);

    TEXTMETRICW tm, tmb;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, big);
    GetTextMetricsW(dc, &tmb);
    SelectObject(dc, body);

    int inner = W - pad * 2;
    int y     = pad;
    int rows  = 0;

    /* --- the time, large and centred, with the date under it --- */
    if ((g.bar_modules & BAR_MOD_CLOCK) && s_clock[0]) {
        SelectObject(dc, big);
        int  len = (int)wcslen(s_clock);
        SIZE sz;
        GetTextExtentPoint32W(dc, s_clock, len, &sz);
        if (draw) {
            SetTextColor(dc, g.bar_fg);
            TextOutW(dc, pad + (inner - sz.cx) / 2, y, s_clock, len);
        }
        y += tmb.tmHeight;
        SelectObject(dc, body);

        if (s_date[0]) {
            len = (int)wcslen(s_date);
            GetTextExtentPoint32W(dc, s_date, len, &sz);
            if (draw) {
                SetTextColor(dc, g.bar_dim);
                TextOutW(dc, pad + (inner - sz.cx) / 2, y, s_date, len);
            }
            y += tm.tmHeight;
        }
        y += gap;
        rows++;
    }

    /* --- desktops and layout on one centred line --- */
    bool has_desktops = (g.bar_modules & BAR_MOD_DESKTOPS) && s_desktops[0];
    bool has_layout   = (g.bar_modules & BAR_MOD_LAYOUT)   && s_layout[0];
    if (has_desktops || has_layout) {
        int  chip = bar_scale(BAR_CHIP_PAD, dpi);
        int  lgap = bar_scale(BAR_GAP,      dpi);
        int  dw   = has_desktops ? draw_desktop_chips(dc, 0, 0, chip, false) : 0;
        int  lw   = 0;
        SIZE lsz  = { 0, 0 };

        if (has_layout) {
            GetTextExtentPoint32W(dc, s_layout, (int)wcslen(s_layout), &lsz);
            lw = lsz.cx + (has_desktops ? lgap : 0);
        }

        if (rows) { float_rule(dc, W, y, pad, dpi, draw); y += rule_h; }

        int x = pad + (inner - (dw + lw)) / 2;
        if (x < pad) x = pad;
        if (has_desktops) x += draw_desktop_chips(dc, x, y, chip, draw);
        if (has_layout) {
            if (has_desktops) x += lgap;
            if (draw) {
                SetTextColor(dc, g.bar_fg);
                TextOutW(dc, x, y, s_layout, (int)wcslen(s_layout));
            }
        }
        y += tm.tmHeight + gap;
        rows++;
    }

    /* --- the focused window's title, centred and clipped --- */
    if ((g.bar_modules & BAR_MOD_TITLE) && s_title[0]) {
        if (draw) {
            RECT tr = { pad, y, W - pad, y + tm.tmHeight };
            SetTextColor(dc, g.bar_fg);
            DrawTextW(dc, s_title, -1, &tr,
                      DT_SINGLELINE | DT_CENTER | DT_VCENTER |
                      DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        y += tm.tmHeight + gap;
        rows++;
    }

    /* --- live notifications, newest first ---
     * Left-aligned and wrapped, unlike everything above it: these are
     * sentences, and a centred wrapped sentence is harder to read than a short
     * centred label. */
    if (s_note_count > 0) {
        int bullet = bar_scale(FLOAT_BULLET,     dpi);
        int bgap   = bar_scale(FLOAT_BULLET_GAP, dpi);

        if (rows) { float_rule(dc, W, y, pad, dpi, draw); y += rule_h; }

        for (int i = 0; i < s_note_count; i++) {
            RECT m = { 0, 0, inner - bullet - bgap, 0 };
            DrawTextW(dc, s_notes[i].text, -1, &m,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            int h = m.bottom;
            if (h < tm.tmHeight) h = tm.tmHeight;

            if (draw) {
                /* The dot sits on the FIRST line's centre rather than the
                 * block's, so a three-line message does not leave it stranded
                 * halfway down. */
                int  by = y + (tm.tmHeight - bullet) / 2;
                RECT b  = { pad, by, pad + bullet, by + bullet };
                overlay_fill(dc, &b, notify_kind_color(s_notes[i].kind));

                RECT tr = { pad + bullet + bgap, y, W - pad, y + h };
                SetTextColor(dc, g.bar_fg);
                DrawTextW(dc, s_notes[i].text, -1, &tr,
                          DT_WORDBREAK | DT_NOPREFIX);
            }
            y += h + (i + 1 < s_note_count ? gap / 2 : 0);
        }
        y += gap;
        rows++;
    }

    SelectObject(dc, old);
    if (!rows) return 0;
    return y - gap + pad;   /* the last section's trailing gap becomes padding */
}

/* The width the panel would like: the widest line it has, clamped so that a
 * long window title cannot stretch it across the display. */
static int float_width(HDC dc, UINT dpi, int max_w) {
    int pad = bar_scale(FLOAT_PAD, dpi);

    HFONT body = overlay_font(&s_float_font,       dpi, bar_text_px(dpi));
    HFONT big  = overlay_font(&s_float_clock_font, dpi, float_clock_px(dpi));
    HFONT old  = (HFONT)SelectObject(dc, body);

    int  want = 0;
    SIZE sz;

    if ((g.bar_modules & BAR_MOD_DESKTOPS) && s_desktops[0]) {
        int w = draw_desktop_chips(dc, 0, 0, bar_scale(BAR_CHIP_PAD, dpi),
                                   false);
        if ((g.bar_modules & BAR_MOD_LAYOUT) && s_layout[0]) {
            GetTextExtentPoint32W(dc, s_layout, (int)wcslen(s_layout), &sz);
            w += bar_scale(BAR_GAP, dpi) + sz.cx;
        }
        if (w > want) want = w;
    }

    if ((g.bar_modules & BAR_MOD_TITLE) && s_title[0]) {
        GetTextExtentPoint32W(dc, s_title, (int)wcslen(s_title), &sz);
        if (sz.cx > want) want = sz.cx;
    }

    if (s_date[0]) {
        GetTextExtentPoint32W(dc, s_date, (int)wcslen(s_date), &sz);
        if (sz.cx > want) want = sz.cx;
    }

    if ((g.bar_modules & BAR_MOD_CLOCK) && s_clock[0]) {
        SelectObject(dc, big);
        GetTextExtentPoint32W(dc, s_clock, (int)wcslen(s_clock), &sz);
        if (sz.cx > want) want = sz.cx;
        SelectObject(dc, body);
    }

    SelectObject(dc, old);

    want += pad * 2;

    int lo = bar_scale(FLOAT_MIN_W, dpi);
    int hi = bar_scale(FLOAT_MAX_W, dpi);
    if (hi > max_w) hi = max_w;
    if (want < lo)  want = lo;
    if (want > hi)  want = hi;
    return want;
}

/* Size and place the panel in the middle of the focused monitor. `changed`
 * says whether the content differs from what is painted; the geometry is
 * compared here, because focus can move to another monitor without a single
 * character of the content changing. */
static void float_relayout(bool changed) {
    HWND hwnd = g.bar_windows[0];
    if (!hwnd) return;

    int  mi  = bar_float_monitor();
    UINT dpi = monitor_dpi(mi);
    RECT mon = g.monitors[mi].full;
    int  mw  = mon.right - mon.left;
    int  mh  = mon.bottom - mon.top;

    HDC dc = GetDC(hwnd);
    int w  = float_width(dc, dpi, mw - bar_scale(FLOAT_PAD, dpi) * 2);
    int h  = float_render(dc, w, dpi, false);
    ReleaseDC(hwnd, dc);

    if (h <= 0) {                      /* every module is off or empty */
        ShowWindow(hwnd, SW_HIDE);
        SetRectEmpty(&s_float_rect);
        return;
    }

    RECT r;
    r.left = mon.left + (mw - w) / 2;
    r.top  = mon.top  + (mh - h) / 2;
    /* A panel taller than the display — a silly bar_height, say — should look
     * wrong rather than disappear, the same call bar_reserve_work_area makes
     * about a strip that would eat the whole screen. */
    if (r.top < mon.top) r.top = mon.top;
    r.right  = r.left + w;
    r.bottom = r.top  + h;

    bool moved = !EqualRect(&r, &s_float_rect) || !IsWindowVisible(hwnd);
    if (moved) {
        s_float_rect = r;
        SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (moved || changed) InvalidateRect(hwnd, NULL, FALSE);
}

void bar_refresh(void) {
    if (!g.bar_enabled) return;

    bool changed = rebuild_content();

    if (g.bar_mode == BAR_MODE_FLOATING) {
        float_relayout(changed);   /* its geometry can change on its own */
        return;
    }

    if (!changed) return;          /* nothing on screen would change */

    for (int i = 0; i < g.monitor_count; i++)
        if (g.bar_windows[i]) InvalidateRect(g.bar_windows[i], NULL, FALSE);
}

bool bar_owns_notifications(void) {
    /* The window check matters: if creating the panel failed, notify.c has to
     * keep raising its own toasts rather than send messages nowhere. */
    return g.bar_enabled && g.bar_mode == BAR_MODE_FLOATING &&
           (g.bar_modules & BAR_MOD_NOTIFICATIONS) && g.bar_windows[0] != NULL;
}

/* ===========================================================================
 * Paint
 * =========================================================================== */
static void bar_paint_strip(HDC mdc, int W, int H, UINT dpi, int mon) {
    HFONT of = (HFONT)SelectObject(mdc, bar_font(mon < 0 ? 0 : mon));
    SetBkMode(mdc, TRANSPARENT);

    TEXTMETRICW tm;
    GetTextMetricsW(mdc, &tm);
    int pad = bar_scale(BAR_PAD, dpi);
    int gap = bar_scale(BAR_GAP, dpi);
    int y   = (H - tm.tmHeight) / 2;      /* vertically centred */
    int x   = pad;

    /* --- desktops, current one in the accent colour ---
     * Drawn token by token so only the current desktop is highlighted; the
     * string built above already carries the '*' marker. */
    if ((g.bar_modules & BAR_MOD_DESKTOPS) && s_desktops[0])
        x += draw_desktop_chips(mdc, x, y, bar_scale(BAR_CHIP_PAD, dpi), true)
             + gap;

    /* --- layout --- */
    if ((g.bar_modules & BAR_MOD_LAYOUT) && s_layout[0]) {
        SetTextColor(mdc, g.bar_fg);
        int len = (int)wcslen(s_layout);
        TextOutW(mdc, x, y, s_layout, len);
        SIZE sz; GetTextExtentPoint32W(mdc, s_layout, len, &sz);
        x += sz.cx + gap;
    }

    /* --- clock, right-aligned; measured first so the title knows its room --- */
    int right = W - pad;
    if ((g.bar_modules & BAR_MOD_CLOCK) && s_clock[0]) {
        int  len = (int)wcslen(s_clock);
        SIZE sz; GetTextExtentPoint32W(mdc, s_clock, len, &sz);
        SetTextColor(mdc, g.bar_fg);
        TextOutW(mdc, right - sz.cx, y, s_clock, len);
        right -= sz.cx + gap;
    }

    /* --- focused window title, filling whatever is left, clipped --- */
    if ((g.bar_modules & BAR_MOD_TITLE) && s_title[0] && right > x) {
        RECT tr = { x, 0, right, H };
        SetTextColor(mdc, g.bar_fg);
        DrawTextW(mdc, s_title, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(mdc, of);
}

static void bar_paint(HWND hwnd) {
    /* Double-buffered, like whichkey: the bar repaints on focus and tiling
     * changes, which are exactly the moments a flicker would be noticed. */
    OverlayPaint op;
    HDC mdc = overlay_paint_begin(&op, hwnd);
    if (!mdc) return;

    int  W = op.w, H = op.h;
    RECT rc = { 0, 0, W, H };

    bool floating = (g.bar_mode == BAR_MODE_FLOATING);
    int  mon = floating ? bar_float_monitor() : bar_monitor_of(hwnd);
    UINT dpi = monitor_dpi(mon < 0 ? g.primary_monitor : mon);

    overlay_fill(mdc, &rc, g.bar_bg);

    if (floating) {
        /* 1px outline in the accent colour, as on the which-key panel: a
         * translucent panel over a busy window needs an edge to read as one
         * object. */
        HPEN   pen = CreatePen(PS_SOLID, 1, g.bar_accent);
        HPEN   opn = (HPEN)SelectObject(mdc, pen);
        HBRUSH obr = (HBRUSH)SelectObject(mdc, GetStockObject(NULL_BRUSH));
        Rectangle(mdc, 0, 0, W, H);
        SelectObject(mdc, obr);
        SelectObject(mdc, opn);
        DeleteObject(pen);

        float_render(mdc, W, dpi, true);
    } else {
        bar_paint_strip(mdc, W, H, dpi, mon);
    }

    overlay_paint_end(&op);
}

static LRESULT CALLBACK bar_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        /* The clock. Ticking every second but only repainting when the text
         * actually changes, so this costs a string compare 59 times a minute
         * rather than a repaint. */
        if (wp == BAR_TIMER_ID) { bar_refresh(); return 0; }
        break;

    case WM_ERASEBKGND:
        return 1;   /* fully painted below */

    case WM_PAINT:
        bar_paint(hwnd);   /* owns Begin/EndPaint via OverlayPaint */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ===========================================================================
 * Lifecycle
 * =========================================================================== */
bool bar_init(void) {
    /* Registered once here; bar_reconfigure creates the windows from it, which
     * is why overlay_register has to tolerate re-registration. */
    return overlay_register(BAR_CLASS, bar_wndproc, false);
}

static void bar_destroy_windows(void) {
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (!g.bar_windows[i]) continue;
        KillTimer(g.bar_windows[i], BAR_TIMER_ID);
        DestroyWindow(g.bar_windows[i]);
        g.bar_windows[i] = NULL;
    }
    SetRectEmpty(&s_float_rect);
}

/* top_bar: one edge-to-edge strip per monitor. */
static void bar_create_strips(void) {
    for (int i = 0; i < g.monitor_count && i < MAX_MONITORS; i++) {
        RECT f   = g.monitors[i].full;
        int  dpi = (int)monitor_dpi(i);
        int  h   = bar_scale(g.bar_height, (UINT)dpi);
        int  y   = g.bar_bottom ? f.bottom - h : f.top;

        /* NOACTIVATE + TOOLWINDOW: never takes focus, never appears in
         * Alt+Tab, and skipped by our own window management (which also
         * ignores the class by name). TOPMOST so a floating window cannot
         * cover it — a fullscreen window still can, because those are placed
         * against the monitor's full bounds. */
        g.bar_windows[i] = overlay_create(
            BAR_CLASS, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        if (!g.bar_windows[i]) continue;

        if (g.bar_modules & BAR_MOD_CLOCK)
            SetTimer(g.bar_windows[i], BAR_TIMER_ID, 1000, NULL);

        SetWindowPos(g.bar_windows[i], HWND_TOPMOST,
                     f.left, y, f.right - f.left, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

/* floating: one panel, which the first refresh sizes and centres. */
static void bar_create_float(void) {
    /* LAYERED + TRANSPARENT on top of the strip's styles: it sits over the
     * middle of the screen, where every click it swallowed would be a click
     * meant for the window underneath. */
    g.bar_windows[0] = overlay_create(
        BAR_CLASS, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                   WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!g.bar_windows[0]) return;

    /* Slightly translucent and round-cornered, like the which-key panel: it is
     * over your windows rather than beside them. */
    SetLayeredWindowAttributes(g.bar_windows[0], 0, 235, LWA_ALPHA);

    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g.bar_windows[0], DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    if (g.bar_modules & BAR_MOD_CLOCK)
        SetTimer(g.bar_windows[0], BAR_TIMER_ID, 1000, NULL);
}

void bar_reconfigure(void) {
    bar_destroy_windows();

    if (g.bar_enabled) {
        if (g.bar_mode == BAR_MODE_FLOATING) bar_create_float();
        else                                 bar_create_strips();
    }

    /* Force the next refresh to paint: the cached strings describe whatever
     * was on the previous set of windows, which no longer exist. */
    s_desktops[0] = s_layout[0] = s_title[0] = s_clock[0] = s_date[0] = L'\0';
    s_note_count  = 0;

    /* A reload can have moved notifications between the panel and the toasts,
     * and whichever of the two just lost them is still showing them. */
    notify_resurface();
    bar_refresh();
}

void bar_toggle(void) {
    g.bar_enabled = !g.bar_enabled;

    /* A hidden top_bar has to give its strip back — and a re-shown one to take
     * it again — before anything is laid out into the work area. */
    update_work_area();
    bar_reconfigure();
    tile_current();
}

void bar_reserve_work_area(void) {
    /* Floating reserves nothing: it is over the windows, not beside them. */
    if (!g.bar_enabled || g.bar_mode == BAR_MODE_FLOATING) return;

    for (int i = 0; i < g.monitor_count; i++) {
        int h = bar_scale(g.bar_height, monitor_dpi(i));
        if (g.bar_bottom) g.monitors[i].work_area.bottom -= h;
        else              g.monitors[i].work_area.top    += h;

        /* Never let the bar eat the whole display — a silly height in the
         * config should look wrong, not leave nowhere to put windows. */
        if (g.monitors[i].work_area.bottom <= g.monitors[i].work_area.top)
            g.monitors[i].work_area = g.monitors[i].full;
    }
}

void bar_shutdown(void) {
    bar_destroy_windows();
    for (int i = 0; i < MAX_MONITORS; i++)
        overlay_font_free(&s_font[i]);
    overlay_font_free(&s_float_font);
    overlay_font_free(&s_float_clock_font);
    UnregisterClassW(BAR_CLASS, g.hinst);
}
