#include "mshell.h"
#include "overlay.h"

static const wchar_t *DIM_CLASS = L"mshell_Dim";

#define ANIM_TIMER_ID 1
#define ANIM_FPS_MS   16

typedef struct {
    HWND      hwnd;
    RECT      from, to;
    ULONGLONG start;
    bool      active;
} Anim;

static Anim s_anims[MAX_WINDOWS_PER_DESKTOP];
static int  s_anim_n;
static HWND s_dim[MAX_MONITORS];

static float ease(float t) {
    float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

static int lerp(int a, int b, float t) {
    return a + (int)((float)(b - a) * t + (b > a ? 0.5f : -0.5f));
}

static RECT anim_rect_at(const Anim *a, ULONGLONG now) {
    float t = g.anim_ms ? (float)(now - a->start) / (float)g.anim_ms : 1.f;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    float e = ease(t);

    RECT r;
    r.left   = lerp(a->from.left,   a->to.left,   e);
    r.top    = lerp(a->from.top,    a->to.top,    e);
    r.right  = lerp(a->from.right,  a->to.right,  e);
    r.bottom = lerp(a->from.bottom, a->to.bottom, e);
    return r;
}

bool anim_begin(HWND hwnd, RECT from, RECT to) {
    if (!g.anim_ms) return false;

    for (int i = 0; i < s_anim_n; i++) {
        if (s_anims[i].active && s_anims[i].hwnd == hwnd) {
            ULONGLONG now    = GetTickCount64();
            s_anims[i].from  = anim_rect_at(&s_anims[i], now);
            s_anims[i].to    = to;
            s_anims[i].start = now;
            return true;
        }
    }

    int dx = abs(to.left - from.left), dy = abs(to.top - from.top);
    int dw = abs((to.right - to.left) - (from.right - from.left));
    int dh = abs((to.bottom - to.top) - (from.bottom - from.top));
    if (dx + dy + dw + dh < 8) return false;
    if (from.right <= from.left || from.bottom <= from.top) return false;

    if (s_anim_n >= MAX_WINDOWS_PER_DESKTOP) return false;

    Anim *a = &s_anims[s_anim_n++];
    a->hwnd   = hwnd;
    a->from   = from;
    a->to     = to;
    a->start  = GetTickCount64();
    a->active = true;

    if (g.message_window)
        SetTimer(g.message_window, TIMER_ANIM, ANIM_FPS_MS, NULL);
    return true;
}

void anim_tick(void) {
    if (s_anim_n == 0) return;

    ULONGLONG now = GetTickCount64();
    int       live = 0;

    events_suppress_begin();
    for (int i = 0; i < s_anim_n; i++) {
        Anim *a = &s_anims[i];
        if (!a->active) continue;

        if (!IsWindow(a->hwnd)) { a->active = false; continue; }

        float t = g.anim_ms ? (float)(now - a->start) / (float)g.anim_ms : 1.f;

        RECT r   = anim_rect_at(a, now);
        RECT adj = window_adjust_for_frame(a->hwnd, r);
        window_set_pos(a->hwnd, adj.left, adj.top,
                       adj.right - adj.left, adj.bottom - adj.top,
                       SWP_NOZORDER | SWP_NOACTIVATE);

        if (t >= 1.f) a->active = false;
        else          live++;
    }
    events_suppress_end();

    int n = 0;
    for (int i = 0; i < s_anim_n; i++)
        if (s_anims[i].active) s_anims[n++] = s_anims[i];
    s_anim_n = n;

    if (live == 0 && g.message_window) {
        KillTimer(g.message_window, TIMER_ANIM);
        border_refresh();
    }
}

bool anim_is_animating(HWND hwnd) {
    for (int i = 0; i < s_anim_n; i++)
        if (s_anims[i].active && s_anims[i].hwnd == hwnd) return true;
    return false;
}

void anim_cancel(HWND hwnd) {
    for (int i = 0; i < s_anim_n; i++)
        if (s_anims[i].hwnd == hwnd) s_anims[i].active = false;
}

void anim_cancel_all(void) {
    s_anim_n = 0;
    if (g.message_window) KillTimer(g.message_window, TIMER_ANIM);
}

static LRESULT CALLBACK dim_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        overlay_fill(dc, &rc, g.dim_color);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool anim_dim_init(void) {
    if (!overlay_register(DIM_CLASS, dim_wndproc, false)) return false;
    for (int i = 0; i < MAX_MONITORS; i++) {
        s_dim[i] = overlay_create(
            DIM_CLASS,
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW);
    }
    return true;
}

void anim_dim_shutdown(void) {
    for (int i = 0; i < MAX_MONITORS; i++)
        overlay_destroy(&s_dim[i], NULL);
    UnregisterClassW(DIM_CLASS, g.hinst);
}

void anim_dim_refresh(void) {
    if (!g.dim_enabled) {
        for (int i = 0; i < MAX_MONITORS; i++)
            if (s_dim[i]) ShowWindow(s_dim[i], SW_HIDE);
        return;
    }

    HWND focus = desktop_get_focused();
    RECT hole  = {0, 0, 0, 0};
    bool have_hole = focus && window_frame_rect(focus, &hole);

    for (int m = 0; m < g.monitor_count && m < MAX_MONITORS; m++) {
        HWND d = s_dim[m];
        if (!d) continue;

        RECT area = g.monitors[m].full;
        int  w = area.right - area.left, h = area.bottom - area.top;
        if (w <= 0 || h <= 0) { ShowWindow(d, SW_HIDE); continue; }

        HRGN rgn = CreateRectRgn(0, 0, w, h);
        if (have_hole) {
            RECT l = hole;
            l.left   -= area.left; l.right  -= area.left;
            l.top    -= area.top;  l.bottom -= area.top;

            if (l.right > 0 && l.bottom > 0 && l.left < w && l.top < h) {
                HRGN cut = CreateRectRgn(l.left, l.top, l.right, l.bottom);
                CombineRgn(rgn, rgn, cut, RGN_DIFF);
                DeleteObject(cut);
            }
        }
        SetWindowRgn(d, rgn, FALSE);
        SetLayeredWindowAttributes(d, 0, g.dim_alpha, LWA_ALPHA);

        SetWindowPos(d, focus ? focus : HWND_TOP,
                     area.left, area.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(d, NULL, TRUE);
    }

    overlay_raise_all();
}
