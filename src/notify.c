#include "mshell.h"
#include "overlay.h"

static const wchar_t *NOTIFY_CLASS = L"mshell_Notify";

#define NOTIFY_MAX      5
#define NOTIFY_TEXT_MAX 512
#define NOTIFY_TIMER_ID 1
#define NOTIFY_TICK_MS  100

#define N_PAD      12
#define N_GAP      8
#define N_MARGIN   16
#define N_ACCENT_W 4
#define N_MAX_W    420

typedef struct {
    wchar_t   text[NOTIFY_TEXT_MAX];
    NotifyKind kind;
    ULONGLONG expires;
    bool      used;
} Toast;

static Toast       s_toasts[NOTIFY_MAX];
static OverlayFont s_font;

COLORREF notify_kind_color(NotifyKind k) {
    switch (k) {
    case NOTIFY_ERROR: return RGB(0xf3, 0x8b, 0xa8);
    case NOTIFY_WARN:  return RGB(0xf9, 0xe2, 0xaf);
    default:           return g.cfg.whichkey_key_fg;
    }
}

static int notify_compact(void) {
    int n = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!s_toasts[i].used) continue;
        if (n != i) s_toasts[n] = s_toasts[i];
        n++;
    }
    for (int i = n; i < NOTIFY_MAX; i++) s_toasts[i].used = false;
    return n;
}

static void notify_relayout(void) {
    if (!g.notify_window) return;

    int n = notify_compact();

    if (bar_owns_notifications()) {
        ShowWindow(g.notify_window, SW_HIDE);
        bar_refresh();
        return;
    }

    if (n == 0) {
        ShowWindow(g.notify_window, SW_HIDE);
        return;
    }

    int mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
             ? g.focused_monitor : g.primary_monitor;
    UINT dpi = monitor_dpi(mi);

    int pad    = overlay_scale(N_PAD,      dpi);
    int gap    = overlay_scale(N_GAP,      dpi);
    int margin = overlay_scale(N_MARGIN,   dpi);
    int accent = overlay_scale(N_ACCENT_W, dpi);
    int maxw   = overlay_scale(N_MAX_W,    dpi);

    overlay_font(&s_font, dpi, overlay_scale(15, dpi));

    HDC   dc  = GetDC(g.notify_window);
    HFONT old = (HFONT)SelectObject(dc, s_font.font);

    int total_h = 0, widest = 0;
    for (int i = 0; i < n; i++) {
        RECT r = { 0, 0, maxw - accent - pad * 2, 0 };
        DrawTextW(dc, s_toasts[i].text, -1, &r,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        int w = r.right + accent + pad * 2;
        int h = r.bottom + pad * 2;
        if (w > widest) widest = w;
        total_h += h + (i ? gap : 0);
    }
    SelectObject(dc, old);
    ReleaseDC(g.notify_window, dc);

    if (widest > maxw) widest = maxw;

    RECT mon = (mi >= 0 && mi < g.monitor_count) ? g.monitors[mi].work_area
                                                 : g.work_area;
    int x = mon.right - widest - margin;
    int y = mon.top + margin;

    SetWindowPos(g.notify_window, HWND_TOPMOST, x, y, widest, total_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g.notify_window, NULL, TRUE);
}

static LRESULT CALLBACK notify_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == NOTIFY_TIMER_ID) {
            ULONGLONG now = GetTickCount64();
            bool changed = false;
            for (int i = 0; i < NOTIFY_MAX; i++) {
                if (s_toasts[i].used && now >= s_toasts[i].expires) {
                    s_toasts[i].used = false;
                    changed = true;
                }
            }
            if (changed) notify_relayout();
            if (notify_compact() == 0) KillTimer(hwnd, NOTIFY_TIMER_ID);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        OverlayPaint op;
        HDC mdc = overlay_paint_begin(&op, hwnd);
        if (!mdc) return 0;

        int mi = (g.focused_monitor >= 0 && g.focused_monitor < g.monitor_count)
                 ? g.focused_monitor : g.primary_monitor;
        UINT dpi = monitor_dpi(mi);
        int  pad    = overlay_scale(N_PAD,      dpi);
        int  gap    = overlay_scale(N_GAP,      dpi);
        int  accent = overlay_scale(N_ACCENT_W, dpi);

        RECT all = { 0, 0, op.w, op.h };
        overlay_fill(mdc, &all, g.cfg.whichkey_bg);

        HFONT of = (HFONT)SelectObject(mdc, s_font.font);
        SetBkMode(mdc, TRANSPARENT);

        int n = notify_compact();
        int y = 0;
        for (int i = 0; i < n; i++) {
            RECT m = { 0, 0, op.w - accent - pad * 2, 0 };
            DrawTextW(mdc, s_toasts[i].text, -1, &m,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            int h = m.bottom + pad * 2;

            RECT stripe = { 0, y, accent, y + h };
            overlay_fill(mdc, &stripe, notify_kind_color(s_toasts[i].kind));

            RECT tr = { accent + pad, y + pad, op.w - pad, y + h - pad };
            SetTextColor(mdc, g.cfg.whichkey_fg);
            DrawTextW(mdc, s_toasts[i].text, -1, &tr,
                      DT_WORDBREAK | DT_NOPREFIX);

            y += h + gap;
        }

        SelectObject(mdc, of);
        overlay_paint_end(&op);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void notify_show(const wchar_t *text, NotifyKind kind, int ms) {
    if (!g.notify_window || !text || !text[0]) return;
    if (!g.cfg.notify_enabled) return;
    if (ms <= 0) ms = 4000;

    int n = notify_compact();
    if (n >= NOTIFY_MAX) {
        n = NOTIFY_MAX - 1;
    }
    for (int i = n; i > 0; i--) s_toasts[i] = s_toasts[i - 1];

    Toast *t = &s_toasts[0];
    memset(t, 0, sizeof(*t));
    wcsncpy(t->text, text, NOTIFY_TEXT_MAX - 1);
    t->kind    = kind;
    t->expires = GetTickCount64() + (ULONGLONG)ms;
    t->used    = true;

    SetTimer(g.notify_window, NOTIFY_TIMER_ID, NOTIFY_TICK_MS, NULL);
    notify_relayout();
}

void notify_resurface(void) {
    notify_relayout();
}

int notify_recent(NotifyItem *out, int max) {
    if (!out || max <= 0) return 0;

    int n = notify_compact();
    if (n > max) n = max;

    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        wcsncpy(out[i].text, s_toasts[i].text,
                sizeof out[i].text / sizeof out[i].text[0] - 1);
        out[i].kind = s_toasts[i].kind;
    }
    return n;
}

bool notify_init(void) {
    if (!overlay_register(NOTIFY_CLASS, notify_wndproc, false)) return false;

    g.notify_window = overlay_create(
        NOTIFY_CLASS,
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!g.notify_window) return false;

    SetLayeredWindowAttributes(g.notify_window, 0, 235, LWA_ALPHA);
    return true;
}

void notify_shutdown(void) {
    if (g.notify_window) KillTimer(g.notify_window, NOTIFY_TIMER_ID);
    overlay_destroy(&g.notify_window, NOTIFY_CLASS);
    overlay_font_free(&s_font);
}
