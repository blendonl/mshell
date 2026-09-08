#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define MAX_TRACK   512
#define MAX_SNAP    512
#define MAX_EVENTS  4096

typedef struct {
    HWND    hwnd;
    wchar_t cls[64];
    wchar_t title[96];
    int     monitor;
    int     last_z;
    int     z_changes;
    int     vis_flips;
    int     rect_changes;
    int     min_z, max_z;
    int     crossed_backdrop;
    int     last_above_bg;
    BOOL    last_vis;
    RECT    last_rect;
    int     seen;
} Track;

typedef struct {
    double  t;
    wchar_t what[160];
} Event;

static Track  g_track[MAX_TRACK];
static int    g_track_count;
static Event  g_events[MAX_EVENTS];
static int    g_event_count;

static HMONITOR g_mons[16];
static int      g_mon_count;

static BOOL CALLBACK mon_enum(HMONITOR h, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r; (void)lp;
    if (g_mon_count < 16) g_mons[g_mon_count++] = h;
    return TRUE;
}

static int monitor_index(HWND h) {
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
    for (int i = 0; i < g_mon_count; i++) if (g_mons[i] == m) return i;
    return -1;
}

static Track *track_find(HWND h) {
    for (int i = 0; i < g_track_count; i++)
        if (g_track[i].hwnd == h) return &g_track[i];
    if (g_track_count >= MAX_TRACK) return NULL;

    Track *t = &g_track[g_track_count++];
    memset(t, 0, sizeof *t);
    t->hwnd = h;
    GetClassNameW(h, t->cls, 64);
    GetWindowTextW(h, t->title, 96);
    t->monitor = monitor_index(h);
    t->last_z  = -1;
    t->min_z   = 9999;
    t->max_z   = -1;
    t->last_above_bg = -1;
    t->last_vis = IsWindowVisible(h);
    GetWindowRect(h, &t->last_rect);
    return t;
}

static void ev(double t, const wchar_t *fmt, ...) {
    if (g_event_count >= MAX_EVENTS) return;
    Event *e = &g_events[g_event_count++];
    e->t = t;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(e->what, 159, fmt, ap);
    va_end(ap);
    e->what[159] = 0;
}

static BOOL rect_eq(const RECT *a, const RECT *b) {
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

int wmain(int argc, wchar_t **argv) {
    int seconds = (argc > 1) ? _wtoi(argv[1]) : 6;
    if (seconds <= 0) seconds = 6;

    EnumDisplayMonitors(NULL, NULL, mon_enum, 0);

    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    HWND    snap[MAX_SNAP];
    int     ticks = 0;
    int     bg_z_changes = 0, last_bg_z = -1;

    wprintf(L"probe_flicker: sampling %d monitors for %ds\n", g_mon_count, seconds);
    fflush(stdout);

    for (;;) {
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (t >= seconds) break;

        int n = 0;
        for (HWND h = GetTopWindow(NULL); h && n < MAX_SNAP; h = GetWindow(h, GW_HWNDNEXT))
            snap[n++] = h;

        int bg_z = -1;
        for (int i = 0; i < n; i++) {
            wchar_t c[64];
            GetClassNameW(snap[i], c, 64);
            if (wcscmp(c, L"mshell_Background") == 0) { bg_z = i; break; }
        }
        if (bg_z >= 0 && last_bg_z >= 0 && bg_z != last_bg_z) {
            bg_z_changes++;
            ev(t, L"backdrop z %d -> %d (of %d)", last_bg_z, bg_z, n);
        }
        if (bg_z >= 0) last_bg_z = bg_z;

        for (int i = 0; i < n; i++) {
            HWND h = snap[i];
            Track *tr = track_find(h);
            if (!tr) continue;
            tr->seen++;

            if (tr->last_z >= 0 && tr->last_z != i) {
                tr->z_changes++;
                if (tr->z_changes < 4000 && tr->z_changes <= 200)
                    ev(t, L"z %-24.24ls %d -> %d", tr->cls, tr->last_z, i);
            }
            tr->last_z = i;
            if (i < tr->min_z) tr->min_z = i;
            if (i > tr->max_z) tr->max_z = i;

            if (bg_z >= 0) {
                int above = (i < bg_z);
                if (tr->last_above_bg >= 0 && above != tr->last_above_bg) {
                    tr->crossed_backdrop++;
                    ev(t, L"CROSS %-22.22ls now %ls backdrop", tr->cls,
                       above ? L"ABOVE" : L"below");
                }
                tr->last_above_bg = above;
            }

            BOOL vis = IsWindowVisible(h);
            if (vis != tr->last_vis) {
                tr->vis_flips++;
                ev(t, L"VIS   %-22.22ls -> %ls", tr->cls, vis ? L"shown" : L"hidden");
            }
            tr->last_vis = vis;

            RECT r;
            if (GetWindowRect(h, &r)) {
                if (!rect_eq(&r, &tr->last_rect)) {
                    tr->rect_changes++;
                    if (tr->rect_changes <= 60)
                        ev(t, L"RECT  %-22.22ls %ld,%ld %ldx%ld", tr->cls,
                           r.left, r.top, r.right - r.left, r.bottom - r.top);
                }
                tr->last_rect = r;
            }
        }
        ticks++;
        Sleep(8);
    }

    wprintf(L"\n=== %d ticks over %ds; backdrop z-index changed %d times ===\n",
            ticks, seconds, bg_z_changes);

    wprintf(L"\n--- churn per window (z-changes / backdrop-crossings / vis-flips / rect-changes) ---\n");
    for (int pass = 0; pass < 25; pass++) {
        int best = -1, bestscore = 0;
        for (int i = 0; i < g_track_count; i++) {
            if (g_track[i].seen < 0) continue;
            int score = g_track[i].z_changes + g_track[i].crossed_backdrop * 50 +
                        g_track[i].vis_flips * 50 + g_track[i].rect_changes * 10;
            if (score > bestscore) { bestscore = score; best = i; }
        }
        if (best < 0) break;
        Track *t = &g_track[best];
        wprintf(L"  z=%-5d cross=%-4d vis=%-4d rect=%-4d mon=%-2d z[%d..%d] %-22.22ls | %.40ls\n",
                t->z_changes, t->crossed_backdrop, t->vis_flips, t->rect_changes,
                t->monitor, t->min_z, t->max_z, t->cls, t->title);
        t->seen = -1;
    }

    wprintf(L"\n--- first 120 events ---\n");
    for (int i = 0; i < g_event_count && i < 120; i++)
        wprintf(L"  %7.3f  %ls\n", g_events[i].t, g_events[i].what);
    wprintf(L"\n(%d events total)\n", g_event_count);
    fflush(stdout);
    return 0;
}
