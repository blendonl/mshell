#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_MON     8
#define GW_          128
#define GH_          72
#define MAX_SAMPLES 20000

typedef struct {
    RECT     rc;
    HDC      mem;
    HBITMAP  bmp, old;
    BYTE    *bits;
    BYTE     prev[GW_ * GH_];
    int      have_prev;
} Mon;

static Mon  g_mon[MAX_MON];
static int  g_mon_count;

static double g_t[MAX_SAMPLES];
static int    g_diff[MAX_MON][MAX_SAMPLES];
static int    g_samples;

static BOOL CALLBACK mon_enum(HMONITOR h, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)lp;
    if (g_mon_count >= MAX_MON) return TRUE;
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(h, &mi);
    g_mon[g_mon_count++].rc = mi.rcMonitor;
    (void)r;
    return TRUE;
}

int wmain(int argc, wchar_t **argv) {
    int seconds = (argc > 1) ? _wtoi(argv[1]) : 8;
    if (seconds <= 0) seconds = 8;

    EnumDisplayMonitors(NULL, NULL, mon_enum, 0);
    HDC screen = GetDC(NULL);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = GW_;
    bi.bmiHeader.biHeight      = -GH_;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    for (int i = 0; i < g_mon_count; i++) {
        Mon *m = &g_mon[i];
        m->mem = CreateCompatibleDC(screen);
        m->bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&m->bits, NULL, 0);
        m->old = SelectObject(m->mem, m->bmp);
        SetStretchBltMode(m->mem, COLORONCOLOR);
        wprintf(L"monitor %d: %ldx%ld at %ld,%ld\n", i,
                m->rc.right - m->rc.left, m->rc.bottom - m->rc.top,
                m->rc.left, m->rc.top);
    }
    fflush(stdout);

    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    while (g_samples < MAX_SAMPLES) {
        QueryPerformanceCounter(&now);
        double t = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (t >= seconds) break;
        g_t[g_samples] = t;

        for (int i = 0; i < g_mon_count; i++) {
            Mon *m = &g_mon[i];
            StretchBlt(m->mem, 0, 0, GW_, GH_, screen,
                       m->rc.left, m->rc.top,
                       m->rc.right - m->rc.left, m->rc.bottom - m->rc.top,
                       SRCCOPY | CAPTUREBLT);

            BYTE lum[GW_ * GH_];
            for (int p = 0; p < GW_ * GH_; p++) {
                BYTE *px = m->bits + p * 4;
                lum[p] = (BYTE)((px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8);
            }

            int d = 0;
            if (m->have_prev) {
                for (int p = 0; p < GW_ * GH_; p++) {
                    int x = (int)lum[p] - (int)m->prev[p];
                    d += (x < 0) ? -x : x;
                }
                d /= (GW_ * GH_);
            }
            memcpy(m->prev, lum, sizeof lum);
            m->have_prev = 1;
            g_diff[i][g_samples] = d;
        }
        g_samples++;
        Sleep(6);
    }

    wprintf(L"\n=== %d frames over %ds (%.0f fps) ===\n",
            g_samples, seconds, g_samples / (double)seconds);

    for (int i = 0; i < g_mon_count; i++) {
        int spikes = 0, maxd = 0;
        double sum = 0;
        for (int s = 1; s < g_samples; s++) {
            int d = g_diff[i][s];
            sum += d;
            if (d > maxd) maxd = d;
            if (d >= 2) spikes++;
        }
        wprintf(L"\nmonitor %d: mean-diff %.2f  max %d  frames-with-change %d/%d\n",
                i, sum / (g_samples ? g_samples : 1), maxd, spikes, g_samples);

        wprintf(L"  spike timeline (diff>=2):\n   ");
        int shown = 0;
        double last = -1;
        for (int s = 1; s < g_samples && shown < 60; s++) {
            if (g_diff[i][s] >= 2) {
                wprintf(L" %.3f(%d)", g_t[s], g_diff[i][s]);
                if (last >= 0) wprintf(L"[+%.0fms]", (g_t[s] - last) * 1000.0);
                last = g_t[s];
                shown++;
                if (shown % 4 == 0) wprintf(L"\n   ");
            }
        }
        wprintf(L"\n");
    }

    for (int i = 0; i < g_mon_count; i++) {
        SelectObject(g_mon[i].mem, g_mon[i].old);
        DeleteObject(g_mon[i].bmp);
        DeleteDC(g_mon[i].mem);
    }
    ReleaseDC(NULL, screen);
    fflush(stdout);
    return 0;
}
