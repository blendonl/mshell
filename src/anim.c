/* ===========================================================================
 * anim.c — window movement animation, and dimming of unfocused windows.
 *
 * Both are off by default. They are the two features here that cost frames
 * rather than bytes, and a tiling WM's whole appeal is that windows are where
 * you put them instantly.
 *
 * ANIMATION and the two mechanisms it has to not fight.
 *
 * The tiler's anti-flicker skip (`rect_eq(mw->applied_rect, want)`) and the
 * drift detector (`events.c`, EPS=4) both exist to answer "did WE put the
 * window there?". An animation makes the honest answer "not yet" for a few
 * hundred milliseconds, during which the window is deliberately not where the
 * layout says it should be — which is exactly the condition the drift detector
 * reads as a window escaping and snaps back.
 *
 * So `applied_rect` records the TARGET, not the current frame. The layout's
 * bookkeeping stays truthful about where the window is going, the drift
 * detector compares against that, and each frame is applied with suppression
 * held so our own moves never re-enter the event handler. An animation is a
 * detour on the way to a rect the rest of mshell already believes in.
 *
 * DIMMING, and why it does not touch the app's window.
 *
 * The obvious implementation — WS_EX_LAYERED plus SetLayeredWindowAttributes on
 * each unfocused window — is wrong here. Making another process's top-level
 * window layered changes how it is composited: GPU-accelerated and fullscreen
 * apps can lose their swap chain, render black, or drop to a software path, and
 * a window manager that quietly breaks the rendering of the applications it
 * manages is worse than one that does not dim.
 *
 * Instead there is ONE overlay per monitor: a click-through scrim covering the
 * work area with the focused window's rect PUNCHED OUT of its region. Nothing
 * is done to anybody's window at all — the same SetWindowRgn technique border.c
 * already uses for the focus ring, applied inside-out.
 * =========================================================================== */
#include "mshell.h"
#include "overlay.h"

static const wchar_t *DIM_CLASS = L"mshell_Dim";

#define ANIM_TIMER_ID 1
#define ANIM_FPS_MS   16      /* ~60 Hz */

typedef struct {
    HWND      hwnd;
    RECT      from, to;
    ULONGLONG start;
    bool      active;
} Anim;

static Anim s_anims[MAX_WINDOWS_PER_DESKTOP];
static int  s_anim_n;
static HWND s_dim[MAX_MONITORS];

/* ---------------------------------------------------------------------------
 * Animation
 * --------------------------------------------------------------------------- */

/* Ease-out cubic: fast to start, settling at the end. Movement that decelerates
 * reads as the window arriving; linear movement reads as it being dragged. */
static float ease(float t) {
    float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

static int lerp(int a, int b, float t) {
    return a + (int)((float)(b - a) * t + (b > a ? 0.5f : -0.5f));
}

/* Where an animation has the window at `now`. One function so a re-target and
 * a frame agree on what "currently" means: the re-target continues from what
 * is on screen instead of restarting from where the last move began, which is
 * a visible jump backwards before the window sets off again. */
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

/* Called by the tiler instead of moving a window directly. Returns false when
 * animation is off or the move is not worth animating, in which case the caller
 * places the window itself. */
bool anim_begin(HWND hwnd, RECT from, RECT to) {
    if (!g.anim_ms) return false;

    /* Re-target an animation already running for this window rather than
     * queueing a second one: two animations of one window fight.
     *
     * Tested BEFORE the heuristics below, which decide whether to START
     * moving — a window already in flight has answered that question. Asking
     * them first would measure the distance from a `from` that is stale by
     * definition, and a "too small to animate" verdict hands the window back
     * to the caller to place outright: a teleport, mid-motion, with the
     * animation still running behind it. */
    for (int i = 0; i < s_anim_n; i++) {
        if (s_anims[i].active && s_anims[i].hwnd == hwnd) {
            /* `from` is where the CALLER thinks the window is — the last rect
             * the layout assigned, which for a window mid-flight is where it
             * is headed, not where it is. Continue from the frame actually on
             * screen. */
            ULONGLONG now    = GetTickCount64();
            s_anims[i].from  = anim_rect_at(&s_anims[i], now);
            s_anims[i].to    = to;
            s_anims[i].start = now;
            return true;
        }
    }

    /* A window that is appearing (no previous rect) or moving a trivial
     * distance is placed outright — animating either looks like a glitch
     * rather than motion. */
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

/* One frame. Suppression is held across the whole pass: these are our moves and
 * must not re-enter the drift detector. */
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

    /* Compact so the array does not grow without bound over a session. */
    int n = 0;
    for (int i = 0; i < s_anim_n; i++)
        if (s_anims[i].active) s_anims[n++] = s_anims[i];
    s_anim_n = n;

    if (live == 0 && g.message_window) {
        KillTimer(g.message_window, TIMER_ANIM);
        /* The ring follows the window; it was left behind during the motion
         * because refreshing it per frame is a repaint per frame. */
        border_refresh();
    }
}

/* A window that is being animated must not be re-placed by a tiling pass
 * mid-flight, or it teleports. */
bool anim_is_animating(HWND hwnd) {
    for (int i = 0; i < s_anim_n; i++)
        if (s_anims[i].active && s_anims[i].hwnd == hwnd) return true;
    return false;
}

void anim_cancel_all(void) {
    s_anim_n = 0;
    if (g.message_window) KillTimer(g.message_window, TIMER_ANIM);
}

/* ---------------------------------------------------------------------------
 * Dimming
 * --------------------------------------------------------------------------- */
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
        /* Alpha is set per refresh, not here: the config can change it. */
    }
    return true;
}

void anim_dim_shutdown(void) {
    for (int i = 0; i < MAX_MONITORS; i++)
        overlay_destroy(&s_dim[i], NULL);
    UnregisterClassW(DIM_CLASS, g.hinst);
}

/* Re-place the scrims. Called from the same points as border_refresh. */
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

        /* The scrim covers the monitor with the focused window punched out. In
         * window-local coordinates, so the region moves with it. */
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
        SetWindowRgn(d, rgn, FALSE);   /* the window owns `rgn` now */
        SetLayeredWindowAttributes(d, 0, g.dim_alpha, LWA_ALPHA);

        /* Just below the focused window: over everything else, under the thing
         * you are looking at, and under the ring which is raised after it. */
        SetWindowPos(d, focus ? focus : HWND_TOP,
                     area.left, area.top, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(d, NULL, TRUE);
    }

    /* A focused float lives in the topmost band, and inserting the scrim under
     * it drags the scrim up there too — over the status bar, which would then
     * be dimmed along with the wallpaper. Our own surfaces stay above it. The
     * scrim demotes itself again the moment the focus is an ordinary window,
     * because SetWindowPos below a non-topmost window clears the style. */
    overlay_raise_all();
}
