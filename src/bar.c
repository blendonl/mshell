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
 * a non-activating, tool-window overlay painted with double-buffered GDI. Two
 * things make it different:
 *
 *   - There is ONE PER MONITOR. A bar you can only see on the primary display
 *     is not much use on a multi-head desk. They all show the same thing: the
 *     desktop set is global in mshell (a desktop spans every monitor), so there
 *     is no per-monitor desktop list to show.
 *
 *   - It RESERVES SPACE. bar_reserve_work_area() shrinks each monitor's work
 *     area, and because the tiler lays out into work_area while the fullscreen
 *     paths use the monitor's full bounds, tiled windows sit below the bar and
 *     a fullscreen window still covers it. That fell out of an existing
 *     distinction rather than needing a new one.
 */

#include "mshell.h"
#include "overlay.h"

static const wchar_t *BAR_CLASS = L"mshell_Bar";

#define BAR_TIMER_ID   1
#define BAR_PAD        10   /* design px at 96 DPI, scaled per monitor */
#define BAR_GAP        14   /* between sections                        */
#define BAR_CHIP_PAD   8    /* inside a desktop chip                   */

/* One font per monitor: displays can have different DPI, so a single shared
 * font would be wrong on at least one of them. */
static OverlayFont s_font[MAX_MONITORS];

/* What is currently on screen, so a refresh can skip repainting when nothing
 * changed. The bar is refreshed from tiling and focus paths that run often. */
static wchar_t s_desktops[512];
static wchar_t s_layout[32];
static wchar_t s_title[256];
static wchar_t s_clock[32];

#define bar_scale(px, dpi) overlay_scale((px), (dpi))

/* Which monitor a given bar window belongs to, or -1. */
static int bar_monitor_of(HWND hwnd) {
    for (int i = 0; i < g.monitor_count; i++)
        if (g.bar_windows[i] == hwnd) return i;
    return -1;
}

static HFONT bar_font(int mon) {
    if (mon < 0 || mon >= MAX_MONITORS) return NULL;

    /* Sized off the bar height rather than fixed, so a taller bar gets larger
     * text instead of a lot of empty space. */
    UINT dpi = monitor_dpi(mon);
    int  px  = bar_scale(g.bar_height, dpi) / 2;
    if (px < 10) px = 10;

    /* Rebuilds only when the DPI or the computed size actually changed, so a
     * bar_height change at reload is picked up without a special case. */
    return overlay_font(&s_font[mon], dpi, px);
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
    case LAYOUT_COLUMNS:  return L"|||";
    case LAYOUT_COUNT:    break;
    }
    return L"[]=";
}

/* Rebuild every section. True if anything differs from what is on screen. */
static bool rebuild_content(void) {
    wchar_t desktops[512] = {0}, layout[32] = {0};
    wchar_t title[256]    = {0}, clock[32]  = {0};

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
    }

    bool changed = wcscmp(desktops, s_desktops) != 0 ||
                   wcscmp(layout,   s_layout)   != 0 ||
                   wcscmp(title,    s_title)    != 0 ||
                   wcscmp(clock,    s_clock)    != 0;

    if (changed) {
        wcscpy(s_desktops, desktops);
        wcscpy(s_layout,   layout);
        wcscpy(s_title,    title);
        wcscpy(s_clock,    clock);
    }
    return changed;
}

void bar_refresh(void) {
    if (!g.bar_enabled) return;
    if (!rebuild_content()) return;   /* nothing on screen would change */

    for (int i = 0; i < g.monitor_count; i++)
        if (g.bar_windows[i]) InvalidateRect(g.bar_windows[i], NULL, FALSE);
}

/* ===========================================================================
 * Paint
 * =========================================================================== */
static void bar_paint(HWND hwnd) {
    /* Double-buffered, like whichkey: the bar repaints on focus and tiling
     * changes, which are exactly the moments a flicker would be noticed. */
    OverlayPaint op;
    HDC mdc = overlay_paint_begin(&op, hwnd);
    if (!mdc) return;

    int  W = op.w, H = op.h;
    RECT rc = { 0, 0, W, H };

    int  mon = bar_monitor_of(hwnd);
    UINT dpi = monitor_dpi(mon < 0 ? g.primary_monitor : mon);

    overlay_fill(mdc, &rc, g.bar_bg);

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
    if ((g.bar_modules & BAR_MOD_DESKTOPS) && s_desktops[0]) {
        const wchar_t *p = s_desktops;
        while (*p) {
            while (*p == L' ') p++;
            const wchar_t *start = p;
            while (*p && *p != L' ') p++;
            int len = (int)(p - start);
            if (!len) break;

            bool current = (start[len - 1] == L'*');
            SetTextColor(mdc, current ? g.bar_accent : g.bar_dim);
            TextOutW(mdc, x, y, start, len);

            SIZE sz;
            GetTextExtentPoint32W(mdc, start, len, &sz);
            x += sz.cx + bar_scale(BAR_CHIP_PAD, dpi);
        }
        x += gap - bar_scale(BAR_CHIP_PAD, dpi);
    }

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
    /* Registered once here; bar_reconfigure creates one window per monitor
     * from it, which is why overlay_register has to tolerate re-registration. */
    return overlay_register(BAR_CLASS, bar_wndproc, false);
}

static void bar_destroy_windows(void) {
    for (int i = 0; i < MAX_MONITORS; i++) {
        if (!g.bar_windows[i]) continue;
        KillTimer(g.bar_windows[i], BAR_TIMER_ID);
        DestroyWindow(g.bar_windows[i]);
        g.bar_windows[i] = NULL;
    }
}

void bar_reconfigure(void) {
    bar_destroy_windows();
    if (!g.bar_enabled) return;

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

    /* Force the next refresh to paint: the cached strings describe whatever
     * was on the previous set of windows, which no longer exist. */
    s_desktops[0] = s_layout[0] = s_title[0] = s_clock[0] = L'\0';
    bar_refresh();
}

void bar_reserve_work_area(void) {
    if (!g.bar_enabled) return;

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
    UnregisterClassW(BAR_CLASS, g.hinst);
}
