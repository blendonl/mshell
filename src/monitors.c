#include "mshell.h"

static BOOL CALLBACK mon_enum_proc(HMONITOR hmon, HDC dc, LPRECT rc, LPARAM lp) {
    (void)dc; (void)rc; (void)lp;
    if (g.monitor_count >= MAX_MONITORS) return TRUE;

    MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
    if (!GetMonitorInfoW(hmon, (LPMONITORINFO)&mi)) return TRUE;

    Monitor *m = &g.monitors[g.monitor_count];
    m->handle    = hmon;
    m->full      = mi.rcMonitor;
    m->work_area = mi.rcWork;
    wcsncpy(m->device, mi.szDevice, CCHDEVICENAME - 1);
    m->device[CCHDEVICENAME - 1] = L'\0';
    if (mi.dwFlags & MONITORINFOF_PRIMARY) g.primary_monitor = g.monitor_count;
    g.monitor_count++;
    return TRUE;
}

void monitors_apply_rules(void) {
    for (int i = 0; i < g.monitor_count; i++) {
        Monitor *m = &g.monitors[i];

        m->inner_gap    = -1;
        m->outer_gap    = -1;
        m->n_master     = -1;
        m->master_ratio = -1.f;
        m->layout       = LAYOUT_COUNT;

        for (int r = 0; r < g.monitor_rule_count; r++) {
            const MonitorRule *mr = &g.monitor_rules[r];

            bool hit = (mr->device[0])
                     ? wildcard_match(mr->device, m->device)
                     : (mr->index == i);
            if (!hit) continue;

            if (mr->set_gaps)    { m->inner_gap = mr->inner_gap;
                                   m->outer_gap = mr->outer_gap; }
            if (mr->set_nmaster)   m->n_master     = mr->n_master;
            if (mr->set_ratio)     m->master_ratio = mr->master_ratio;
            if (mr->set_layout)    m->layout       = mr->layout;
        }
    }
}

void monitors_update(void) {
    g.monitor_count   = 0;
    g.primary_monitor = 0;
    EnumDisplayMonitors(NULL, NULL, mon_enum_proc, 0);

    if (g.monitor_count == 0) {
        RECT wa;
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            wa.left = 0; wa.top = 0;
            wa.right  = GetSystemMetrics(SM_CXSCREEN);
            wa.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        g.monitors[0].handle    = NULL;
        g.monitors[0].full      = wa;
        g.monitors[0].work_area = wa;
        g.monitor_count = 1;
    }

    monitors_apply_rules();

    if (g.focused_monitor < 0 || g.focused_monitor >= g.monitor_count)
        g.focused_monitor = g.primary_monitor;

    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];

        if (mw->monitor_device[0]) {
            int found = -1;
            for (int m = 0; m < g.monitor_count; m++)
                if (_wcsicmp(g.monitors[m].device, mw->monitor_device) == 0) {
                    found = m; break;
                }
            if (found >= 0) {
                if (mw->monitor != found) {
                    mw->monitor     = found;
                    mw->has_applied = false;
                }
                continue;
            }
            if (mw->monitor != g.primary_monitor) {
                mw->monitor     = g.primary_monitor;
                mw->has_applied = false;
            }
            continue;
        }

        if (mw->monitor < 0 || mw->monitor >= g.monitor_count)
            mw->monitor = g.primary_monitor;
        wcsncpy(mw->monitor_device, g.monitors[mw->monitor].device,
                CCHDEVICENAME - 1);
        mw->monitor_device[CCHDEVICENAME - 1] = L'\0';
    }

    for (int i = 0; i < g.managed_count; i++)
        if (g.managed[i].is_floating) window_rescue_offscreen(&g.managed[i]);
}

typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);

UINT monitor_dpi_of(HMONITOR handle) {
    static GetDpiForMonitorFn fn     = NULL;
    static bool               probed = false;

    if (!probed) {
        probed = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore)
            fn = (GetDpiForMonitorFn)(void *)
                     GetProcAddress(shcore, "GetDpiForMonitor");
        if (!fn)
            log_w(L"GetDpiForMonitor unavailable — assuming 96 DPI everywhere");
    }

    if (!fn || !handle) return 96;

    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(fn(handle, 0 , &dpi_x, &dpi_y)))
        return 96;
    return dpi_x ? dpi_x : 96;
}

UINT monitor_dpi(int mon) {
    if (mon < 0 || mon >= g.monitor_count) return 96;
    return monitor_dpi_of(g.monitors[mon].handle);
}

int monitor_of_window(HWND hwnd) {
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < g.monitor_count; i++)
        if (g.monitors[i].handle == hmon) return i;
    return g.primary_monitor;
}

void update_work_area(void) {
    monitors_update();
    bar_reserve_work_area();
    g.work_area = g.monitors[g.primary_monitor].work_area;
}
