#include "mshell.h"
#include "overlay.h"
#include "whichkey_math.h"

static const wchar_t *WK_CLASS = L"mshell_WhichKey";

#define WK_TIMER_ID    1

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

static OverlayFont s_font;
static WkRow   s_rows[WHICHKEY_MAX_ROWS];
static int     s_count;
static int     s_cols, s_per_col;
static int     s_key_w, s_label_w, s_row_h, s_header_h;
static wchar_t s_title[80];

static int     s_pad, s_key_gap, s_col_gap, s_row_vpad, s_hdr_gap, s_border_w;

#define wk_dpi_scale(px, dpi) overlay_scale((px), (dpi))

static void wk_apply_dpi(UINT dpi) {
    s_pad      = wk_dpi_scale(g.whichkey_padding,  dpi);
    s_key_gap  = wk_dpi_scale(g.whichkey_key_gap,  dpi);
    s_col_gap  = wk_dpi_scale(g.whichkey_col_gap,  dpi);
    s_row_vpad = wk_dpi_scale(g.whichkey_row_gap,  dpi);
    s_hdr_gap  = wk_dpi_scale(g.whichkey_hdr_gap,  dpi);
    s_border_w = wk_dpi_scale(g.whichkey_border_w, dpi);

    overlay_font_face(&s_font, dpi, wk_dpi_scale(g.whichkey_font_size, dpi),
                      g.whichkey_font);
}

static int wk_resolve_max(float v, int mon_px, UINT dpi) {
    if (v <= 0.0f) return 0;
    if (v <= 1.0f) return (int)((float)mon_px * v);
    return wk_dpi_scale((int)v, dpi);
}

static void wk_label(const KeyBinding *b, wchar_t *out, int cap) {
    if (b->desc && b->desc[0]) {
        _snwprintf(out, cap, L"%ls", b->desc);
        out[cap - 1] = L'\0';
        return;
    }
    if (b->action == ACTION_ENTER_SUBMAP && b->submap && b->submap->name) {
        _snwprintf(out, cap, L"+%ls", b->submap->name);
    } else if (b->action == ACTION_SPAWN && b->command) {
        if (b->args && b->args[0])
            _snwprintf(out, cap, L"%ls %ls", b->command, b->args);
        else
            _snwprintf(out, cap, L"%ls", b->command);
    } else if ((b->action == ACTION_SWITCH_DESKTOP ||
                b->action == ACTION_MOVE_TO_DESKTOP) && b->command) {
        _snwprintf(out, cap, L"%ls%ls",
                   b->action == ACTION_MOVE_TO_DESKTOP ? L"→ " : L"", b->command);
    } else {
        const char *n = action_enum_to_name(b->action);
        _snwprintf(out, cap, L"%hs",
                   n ? n : (b->action == ACTION_LUA_CALL ? "lua" : "?"));
    }
    out[cap - 1] = L'\0';
}

void whichkey_hide(void) {
    if (!g.whichkey_window) return;
    KillTimer(g.whichkey_window, WK_TIMER_ID);
    ShowWindow(g.whichkey_window, SW_HIDE);
}

static void whichkey_show(KeyMap *map) {
    if (!g.whichkey_window || !map) return;

    int  mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
              ? g.focused_monitor : g.primary_monitor;
    UINT dpi = monitor_dpi(mi);
    wk_apply_dpi(dpi);

    s_count = 0;
    for (int i = 0; i < map->count && s_count < WHICHKEY_MAX_ROWS; i++) {
        const KeyBinding *b = &map->bindings[i];
        const char *kn = vk_to_key_name(b->vk);
        if (!kn) continue;
        WkRow *r = &s_rows[s_count++];
        _snwprintf(r->key, 24, L"%hs", kn);
        r->key[23] = L'\0';
        wk_label(b, r->label, 96);
    }
    if (s_count == 0) { whichkey_hide(); return; }

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

    if (map->count > WHICHKEY_MAX_ROWS)
        log_msg(LOG_WARN, L"which-key: '%ls' has %d bindings; showing the "
                          L"first %d", map->name ? map->name : L"?",
                map->count, WHICHKEY_MAX_ROWS);

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

    RECT mon   = g.monitors[mi].full;
    int  mon_w = mon.right - mon.left;
    int  mon_h = mon.bottom - mon.top;

    int margin = (g.whichkey_margin >= 0)
                 ? wk_dpi_scale(g.whichkey_margin, dpi)
                 : mon_h / 20;
    if (margin * 2 >= mon_w || margin * 2 >= mon_h) margin = 0;

    int max_w = wk_resolve_max(g.whichkey_max_w, mon_w, dpi);
    int max_h = wk_resolve_max(g.whichkey_max_h, mon_h, dpi);
    if (max_w <= 0 || max_w > mon_w - margin * 2) max_w = mon_w - margin * 2;
    if (max_h <= 0 || max_h > mon_h - margin * 2) max_h = mon_h - margin * 2;

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
        .min_label = wk_dpi_scale(48, dpi),
        .max_w     = max_w,
        .max_h     = max_h,
    };
    WkLayout lay;
    wk_layout(&wm, &lay);

    s_cols    = lay.cols;
    s_per_col = lay.per_col;
    s_label_w = lay.label_w;

    if (lay.shown < s_count) {
        log_msg(LOG_WARN, L"which-key: '%ls' — %d of %d bindings do not fit "
                          L"the configured max_width/max_height",
                map->name ? map->name : L"?", s_count - lay.shown, s_count);
        s_count = lay.shown;
    }

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

    SetLayeredWindowAttributes(g.whichkey_window, 0, g.whichkey_opacity,
                               LWA_ALPHA);
    DWORD corner = g.whichkey_rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g.whichkey_window, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    SetWindowPos(g.whichkey_window, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.whichkey_window, NULL, TRUE);
}

void whichkey_notify(void) {
    if (!g.whichkey_window) return;
    KillTimer(g.whichkey_window, WK_TIMER_ID);
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
            kb_lock();
            KeyMap *m = (g.current_map && g.current_map != g.root_map)
                        ? g.current_map : NULL;
            kb_unlock();
            if (m) whichkey_show(m); else whichkey_hide();
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        OverlayPaint op;
        HDC mdc = overlay_paint_begin(&op, hwnd);
        if (!mdc) return 0;
        int  W = op.w, H = op.h;
        RECT rc = { 0, 0, W, H };

        overlay_fill(mdc, &rc, g.whichkey_bg);

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

        const UINT fmt = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS |
                         DT_TOP | DT_LEFT;
        const int  right = W - s_pad;

        SetTextColor(mdc, g.whichkey_key_fg);
        RECT hr = { s_pad, s_pad, right, s_pad + s_header_h };
        DrawTextW(mdc, s_title, -1, &hr, fmt);

        int cell_w = s_key_w + s_key_gap + s_label_w;
        int y0     = s_pad + s_header_h;
        for (int i = 0; i < s_count; i++) {
            int col = i / s_per_col;
            int row = i % s_per_col;
            int cx  = s_pad + col * (cell_w + s_col_gap);
            int cy  = y0 + row * s_row_h;

            SIZE ksz;
            GetTextExtentPoint32W(mdc, s_rows[i].key,
                                  (int)wcslen(s_rows[i].key), &ksz);
            SetTextColor(mdc, g.whichkey_key_fg);
            TextOutW(mdc, cx + (s_key_w - ksz.cx), cy,
                     s_rows[i].key, (int)wcslen(s_rows[i].key));

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

    g.whichkey_window = overlay_create(
        WK_CLASS,
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!g.whichkey_window) return false;

    SetLayeredWindowAttributes(g.whichkey_window, 0, g.whichkey_opacity,
                               LWA_ALPHA);

    DWORD corner = g.whichkey_rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g.whichkey_window, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    return true;
}

void whichkey_shutdown(void) {
    if (g.whichkey_window) {
        KillTimer(g.whichkey_window, WK_TIMER_ID);
    }
    overlay_destroy(&g.whichkey_window, WK_CLASS);
    overlay_font_free(&s_font);
}
