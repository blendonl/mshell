#include "mshell.h"
#include "overlay.h"

static const wchar_t *BAR_CLASS = L"mshell_Bar";

#define BAR_TIMER_ID   1
#define BAR_PAD        10
#define BAR_GAP        14
#define BAR_CHIP_PAD   8

#define FLOAT_PAD        22
#define FLOAT_ROW_GAP    12
#define FLOAT_MIN_W      260
#define FLOAT_MAX_W      560
#define FLOAT_BULLET     7
#define FLOAT_BULLET_GAP 10
#define FLOAT_NOTES_MAX  4

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

static OverlayFont s_font[MAX_MONITORS];

static OverlayFont s_float_font;
static OverlayFont s_float_clock_font;

static wchar_t s_desktops[512];
static wchar_t s_layout[32];
static wchar_t s_title[256];
static wchar_t s_clock[32];
static wchar_t s_date[128];
static NotifyItem s_notes[FLOAT_NOTES_MAX];
static int        s_note_count;

static RECT s_float_rect;

#define bar_scale(px, dpi) overlay_scale((px), (dpi))

static int bar_monitor_of(HWND hwnd) {
    for (int i = 0; i < g.monitor_count; i++)
        if (g.bar_windows[i] == hwnd) return i;
    return -1;
}

static int bar_float_monitor(void) {
    return (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
           ? g.focused_monitor : g.primary_monitor;
}

static int bar_text_px(UINT dpi) {
    int px = bar_scale(g.bar_height, dpi) / 2;
    return px < 10 ? 10 : px;
}

static HFONT bar_font(int mon) {
    if (mon < 0 || mon >= MAX_MONITORS) return NULL;

    UINT dpi = monitor_dpi(mon);
    return overlay_font(&s_font[mon], dpi, bar_text_px(dpi));
}

static void build_desktops(wchar_t *out, size_t cap) {
    out[0] = L'\0';
    size_t used = 0;

    for (int i = 0; i < g.desktop_count; i++) {
        const Desktop *d  = &g.desktops[i];
        bool           cur = (d->id == g.current_desktop_id);
        const wchar_t *mark = cur                       ? L"*"
                            : desktop_is_visible(d->id) ? L"+"
                                                        : L"";

        wchar_t chunk[DESKTOP_NAME_MAX + 8];
        _snwprintf(chunk, DESKTOP_NAME_MAX + 8, L"%ls%ls%ls",
                   used ? L"  " : L"", d->name, mark);
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

    if (g.bar_modules & BAR_MOD_TITLE)
        window_title(desktop_get_focused(), title, ARRAYSIZE(title));

    if (g.bar_modules & BAR_MOD_CLOCK) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        _snwprintf(clock, 32, L"%02d:%02d", st.wHour, st.wMinute);
        clock[31] = L'\0';

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

static int float_clock_px(UINT dpi) {
    int px = bar_scale(g.bar_height, dpi) * 3 / 2;
    return px < 20 ? 20 : px;
}

static void float_rule(HDC dc, int W, int y, int pad, UINT dpi, bool draw) {
    if (!draw) return;
    int th = bar_scale(1, dpi);
    if (th < 1) th = 1;
    RECT r = { pad, y, W - pad, y + th };
    overlay_fill(dc, &r, g.bar_dim);
}

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
    return y - gap + pad;
}

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

    if (h <= 0) {
        ShowWindow(hwnd, SW_HIDE);
        SetRectEmpty(&s_float_rect);
        return;
    }

    RECT r;
    r.left = mon.left + (mw - w) / 2;
    r.top  = mon.top  + (mh - h) / 2;
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
        float_relayout(changed);
        return;
    }

    if (!changed) return;

    for (int i = 0; i < g.monitor_count; i++)
        if (g.bar_windows[i]) InvalidateRect(g.bar_windows[i], NULL, FALSE);
}

bool bar_owns_notifications(void) {
    return g.bar_enabled && g.bar_mode == BAR_MODE_FLOATING &&
           (g.bar_modules & BAR_MOD_NOTIFICATIONS) && g.bar_windows[0] != NULL;
}

static void bar_paint_strip(HDC mdc, int W, int H, UINT dpi, int mon) {
    HFONT of = (HFONT)SelectObject(mdc, bar_font(mon < 0 ? 0 : mon));
    SetBkMode(mdc, TRANSPARENT);

    TEXTMETRICW tm;
    GetTextMetricsW(mdc, &tm);
    int pad = bar_scale(BAR_PAD, dpi);
    int gap = bar_scale(BAR_GAP, dpi);
    int y   = (H - tm.tmHeight) / 2;
    int x   = pad;

    if ((g.bar_modules & BAR_MOD_DESKTOPS) && s_desktops[0])
        x += draw_desktop_chips(mdc, x, y, bar_scale(BAR_CHIP_PAD, dpi), true)
             + gap;

    if ((g.bar_modules & BAR_MOD_LAYOUT) && s_layout[0]) {
        SetTextColor(mdc, g.bar_fg);
        int len = (int)wcslen(s_layout);
        TextOutW(mdc, x, y, s_layout, len);
        SIZE sz; GetTextExtentPoint32W(mdc, s_layout, len, &sz);
        x += sz.cx + gap;
    }

    int right = W - pad;
    if ((g.bar_modules & BAR_MOD_CLOCK) && s_clock[0]) {
        int  len = (int)wcslen(s_clock);
        SIZE sz; GetTextExtentPoint32W(mdc, s_clock, len, &sz);
        SetTextColor(mdc, g.bar_fg);
        TextOutW(mdc, right - sz.cx, y, s_clock, len);
        right -= sz.cx + gap;
    }

    if ((g.bar_modules & BAR_MOD_TITLE) && s_title[0] && right > x) {
        RECT tr = { x, 0, right, H };
        SetTextColor(mdc, g.bar_fg);
        DrawTextW(mdc, s_title, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(mdc, of);
}

static void bar_paint(HWND hwnd) {
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
        if (wp == BAR_TIMER_ID) { bar_refresh(); return 0; }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        bar_paint(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool bar_init(void) {
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

static void bar_create_strips(void) {
    for (int i = 0; i < g.monitor_count && i < MAX_MONITORS; i++) {
        RECT f   = g.monitors[i].full;
        int  dpi = (int)monitor_dpi(i);
        int  h   = bar_scale(g.bar_height, (UINT)dpi);
        int  y   = g.bar_bottom ? f.bottom - h : f.top;

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

static void bar_create_float(void) {
    g.bar_windows[0] = overlay_create(
        BAR_CLASS, WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                   WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!g.bar_windows[0]) return;

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

    s_desktops[0] = s_layout[0] = s_title[0] = s_clock[0] = s_date[0] = L'\0';
    s_note_count  = 0;

    notify_resurface();
    bar_refresh();
}

void bar_toggle(void) {
    g.bar_enabled = !g.bar_enabled;

    update_work_area();
    bar_reconfigure();
    tile_current();
}

void bar_set_mode(BarMode mode) {
    if (g.bar_enabled && g.bar_mode == mode) return;

    g.bar_mode    = mode;
    g.bar_enabled = true;

    update_work_area();
    bar_reconfigure();
    tile_current();
}

void bar_reserve_work_area(void) {
    if (!g.bar_enabled || g.bar_mode == BAR_MODE_FLOATING) return;

    for (int i = 0; i < g.monitor_count; i++) {
        int h = bar_scale(g.bar_height, monitor_dpi(i));
        if (g.bar_bottom) g.monitors[i].work_area.bottom -= h;
        else              g.monitors[i].work_area.top    += h;

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
