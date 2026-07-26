/*
 * whichkey.c — submap hint popup ("which-key").
 *
 * When you enter a submap (Win+r resize, Win+x window, …) mshell can pop up a
 * small panel listing that submap's keys and what each one does, the way
 * which-key does in Emacs/Neovim. It is a non-activating layered overlay (same
 * family as border.c / background.c) docked to the bottom-centre of the focused
 * monitor, and it disappears the moment you leave the submap.
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

static const wchar_t *WK_CLASS = L"mshell_WhichKey";

#define WK_TIMER_ID    1
#define WK_MAX_ROWS    64

/* layout metrics, device pixels */
#define WK_PAD         14   /* panel inner padding                     */
#define WK_ROW_VPAD    6    /* extra vertical space per row            */
#define WK_HDR_GAP     8    /* gap under the header                    */
#define WK_KEY_GAP     10   /* gap between a key and its label         */
#define WK_COL_GAP     30   /* gap between columns                     */
#define WK_MAX_PERCOL  12   /* wrap into a new column past this many   */

/* Win11 rounded corners; harmless (ignored) on older Windows. */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
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
static HFONT   s_font;
static UINT    s_font_dpi;      /* DPI s_font was built for (0 = no font yet) */
static WkRow   s_rows[WK_MAX_ROWS];
static int     s_count;
static int     s_cols, s_per_col;
static int     s_key_w, s_label_w, s_row_h, s_header_h;
static wchar_t s_title[80];

/* Metrics above are design pixels at 96 DPI; these are the same values scaled
 * for the monitor the panel is about to appear on. mshell is per-monitor DPI
 * aware, so nothing scales them for us — without this the panel renders at a
 * third of its intended size on a 300% display. */
static int     s_pad, s_key_gap, s_col_gap, s_row_vpad, s_hdr_gap;

static int wk_dpi_scale(int px, UINT dpi) { return MulDiv(px, (int)dpi, 96); }

/* Rebuild the font and the scaled metrics for `dpi`, if they aren't already. */
static void wk_apply_dpi(UINT dpi) {
    s_pad      = wk_dpi_scale(WK_PAD,      dpi);
    s_key_gap  = wk_dpi_scale(WK_KEY_GAP,  dpi);
    s_col_gap  = wk_dpi_scale(WK_COL_GAP,  dpi);
    s_row_vpad = wk_dpi_scale(WK_ROW_VPAD, dpi);
    s_hdr_gap  = wk_dpi_scale(WK_HDR_GAP,  dpi);

    if (s_font && s_font_dpi == dpi) return;
    if (s_font) DeleteObject(s_font);

    /* Negative height == pixels, so this needs scaling like everything else. */
    s_font = CreateFontW(-wk_dpi_scale(18, dpi), 0, 0, 0, FW_NORMAL,
                         FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
    s_font_dpi = dpi;
}

/* The human-readable label for one binding. spawn shows its command, the
 * desktop actions show the desktop they go to, enter_submap shows "+name" (the
 * which-key idiom for a nested prefix), and everything else falls back to the
 * action's canonical name. */
static void wk_label(const KeyBinding *b, wchar_t *out, int cap) {
    if (b->action == ACTION_ENTER_SUBMAP && b->submap && b->submap->name) {
        _snwprintf(out, cap, L"+%ls", b->submap->name);
    } else if (b->action == ACTION_SPAWN && b->command) {
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
    int mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
             ? g.focused_monitor : g.primary_monitor;
    wk_apply_dpi(monitor_dpi(mi));

    /* --- gather the rows we can actually label --- */
    s_count = 0;
    for (int i = 0; i < map->count && s_count < WK_MAX_ROWS; i++) {
        const KeyBinding *b = &map->bindings[i];
        const char *kn = vk_to_key_name(b->vk);
        if (!kn) continue;                      /* no display name — skip */
        WkRow *r = &s_rows[s_count++];
        _snwprintf(r->key, 24, L"%hs", kn);
        r->key[23] = L'\0';
        wk_label(b, r->label, 96);
    }
    if (s_count == 0) { whichkey_hide(); return; }

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

    /* --- grid shape: wrap into columns so the panel never gets too tall --- */
    s_cols = (s_count + WK_MAX_PERCOL - 1) / WK_MAX_PERCOL;
    if (s_cols < 1) s_cols = 1;
    s_per_col = (s_count + s_cols - 1) / s_cols;

    /* --- measure text with our font --- */
    HDC   dc  = GetDC(g.whichkey_window);
    HFONT old = (HFONT)SelectObject(dc, s_font);

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

    int cell_w  = s_key_w + s_key_gap + s_label_w;
    int inner_w = s_cols * cell_w + (s_cols - 1) * s_col_gap;
    if (hsz.cx > inner_w) inner_w = hsz.cx;       /* don't clip the header */
    int w = inner_w + s_pad * 2;
    int h = s_header_h + s_per_col * s_row_h + s_pad * 2;

    /* --- position: bottom-centre of the focused monitor (`mi` resolved at the
     *     top, since the DPI it implies drove everything measured above) --- */
    RECT mon   = g.monitors[mi].full;
    int  mon_w = mon.right - mon.left;
    int  mon_h = mon.bottom - mon.top;
    if (w > mon_w - 20) w = mon_w - 20;           /* never wider than the screen */
    int x = mon.left + (mon_w - w) / 2;
    int y = mon.bottom - h - mon_h / 20;          /* a small bottom margin */

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
        PAINTSTRUCT ps;
        HDC  hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;

        /* Double-buffer so the panel never flickers as it repaints. */
        HDC     mdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
        HBITMAP obm = (HBITMAP)SelectObject(mdc, bmp);

        HBRUSH bg = CreateSolidBrush(g.whichkey_bg);
        FillRect(mdc, &rc, bg);
        DeleteObject(bg);

        /* 1px outline in the accent colour */
        HPEN   pen = CreatePen(PS_SOLID, 1, g.whichkey_border);
        HPEN   opn = (HPEN)SelectObject(mdc, pen);
        HBRUSH obr = (HBRUSH)SelectObject(mdc, GetStockObject(NULL_BRUSH));
        Rectangle(mdc, 0, 0, W, H);
        SelectObject(mdc, obr);
        SelectObject(mdc, opn);
        DeleteObject(pen);

        HFONT of = (HFONT)SelectObject(mdc, s_font);
        SetBkMode(mdc, TRANSPARENT);

        /* header */
        SetTextColor(mdc, g.whichkey_key_fg);
        TextOutW(mdc, s_pad, s_pad, s_title, (int)wcslen(s_title));

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
            SetTextColor(mdc, g.whichkey_fg);
            TextOutW(mdc, cx + s_key_w + s_key_gap, cy,
                     s_rows[i].label, (int)wcslen(s_rows[i].label));
        }

        SelectObject(mdc, of);
        BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);

        SelectObject(mdc, obm);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool whichkey_init(void) {
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wk_wndproc;
    wc.hInstance     = g.hinst;
    wc.lpszClassName = WK_CLASS;
    RegisterClassExW(&wc);

    /* Layered + noactivate + toolwindow + topmost: a transient hint that never
     * takes focus, never appears in Alt+Tab, is skipped by our own window
     * management (toolwindow), and floats above the tiled grid. */
    g.whichkey_window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        WK_CLASS, L"", WS_POPUP,
        0, 0, 0, 0, NULL, NULL, g.hinst, NULL);
    if (!g.whichkey_window) {
        log_w(L"whichkey: CreateWindowEx failed: %lu", GetLastError());
        return false;
    }

    /* Slight translucency, like the focus ring. */
    SetLayeredWindowAttributes(g.whichkey_window, 0, 235, LWA_ALPHA);

    DWORD corner = DWMWCP_ROUND;
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
        DestroyWindow(g.whichkey_window);
        g.whichkey_window = NULL;
    }
    if (s_font) { DeleteObject(s_font); s_font = NULL; s_font_dpi = 0; }
    UnregisterClassW(WK_CLASS, g.hinst);
}
