#include "mshell.h"

#define MS_QDC_ONLY_ACTIVE_PATHS      0x00000002u

#define MS_DC_GET_SOURCE_NAME                  1u
#define MS_DC_GET_ADVANCED_COLOR_INFO          9u
#define MS_DC_SET_ADVANCED_COLOR_STATE        10u

#define MS_ADV_COLOR_SUPPORTED        0x1u
#define MS_ADV_COLOR_ENABLED          0x2u
#define MS_ADV_COLOR_FORCE_DISABLED   0x8u

typedef struct { UINT32 Numerator, Denominator; } MsDcRational;

typedef struct {
    LUID   adapterId;
    UINT32 id;
    UINT32 modeInfoIdx;
    UINT32 statusFlags;
} MsDcPathSourceInfo;

typedef struct {
    LUID         adapterId;
    UINT32       id;
    UINT32       modeInfoIdx;
    UINT32       outputTechnology;
    UINT32       rotation;
    UINT32       scaling;
    MsDcRational refreshRate;
    UINT32       scanLineOrdering;
    BOOL         targetAvailable;
    UINT32       statusFlags;
} MsDcPathTargetInfo;

typedef struct {
    MsDcPathSourceInfo sourceInfo;
    MsDcPathTargetInfo targetInfo;
    UINT32             flags;
} MsDcPathInfo;

typedef struct { unsigned char opaque[64]; } MsDcModeInfo;

typedef struct {
    UINT32 type;
    UINT32 size;
    LUID   adapterId;
    UINT32 id;
} MsDcDeviceInfoHeader;

typedef struct {
    MsDcDeviceInfoHeader header;
    WCHAR                viewGdiDeviceName[CCHDEVICENAME];
} MsDcSourceDeviceName;

typedef struct {
    MsDcDeviceInfoHeader header;
    UINT32               value;
    UINT32               colorEncoding;
    UINT32               bitsPerColorChannel;
} MsDcAdvancedColorInfo;

typedef struct {
    MsDcDeviceInfoHeader header;
    UINT32               value;
} MsDcSetAdvancedColorState;

typedef LONG (WINAPI *GetBufferSizes_fn)(UINT32, UINT32 *, UINT32 *);
typedef LONG (WINAPI *QueryConfig_fn)(UINT32, UINT32 *, MsDcPathInfo *,
                                      UINT32 *, MsDcModeInfo *, void *);
typedef LONG (WINAPI *GetDeviceInfo_fn)(void *);
typedef LONG (WINAPI *SetDeviceInfo_fn)(void *);

static GetBufferSizes_fn s_get_sizes;
static QueryConfig_fn    s_query;
static GetDeviceInfo_fn  s_get_info;
static SetDeviceInfo_fn  s_set_info;
static bool              s_ccd_tried;

static bool ccd_load(void) {
    if (s_ccd_tried) return s_get_sizes && s_query && s_get_info && s_set_info;
    s_ccd_tried = true;

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return false;

    s_get_sizes = (GetBufferSizes_fn)(void *)
                  GetProcAddress(u32, "GetDisplayConfigBufferSizes");
    s_query     = (QueryConfig_fn)(void *)
                  GetProcAddress(u32, "QueryDisplayConfig");
    s_get_info  = (GetDeviceInfo_fn)(void *)
                  GetProcAddress(u32, "DisplayConfigGetDeviceInfo");
    s_set_info  = (SetDeviceInfo_fn)(void *)
                  GetProcAddress(u32, "DisplayConfigSetDeviceInfo");

    if (!s_get_sizes || !s_query || !s_get_info || !s_set_info)
        log_w(L"display: the CCD entry points are missing — HDR control is off");
    return s_get_sizes && s_query && s_get_info && s_set_info;
}

static bool ccd_find_target(const wchar_t *device, LUID *adapter, UINT32 *target) {
    if (!ccd_load() || !device || !device[0]) return false;

    UINT32 n_paths = 0, n_modes = 0;
    if (s_get_sizes(MS_QDC_ONLY_ACTIVE_PATHS, &n_paths, &n_modes) != ERROR_SUCCESS
        || n_paths == 0)
        return false;

    MsDcPathInfo *paths = calloc(n_paths, sizeof *paths);
    MsDcModeInfo *modes = calloc(n_modes ? n_modes : 1, sizeof *modes);
    if (!paths || !modes) { free(paths); free(modes); return false; }

    bool found = false;
    if (s_query(MS_QDC_ONLY_ACTIVE_PATHS, &n_paths, paths, &n_modes, modes,
                NULL) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < n_paths && !found; i++) {
            MsDcSourceDeviceName name = {0};
            name.header.type      = MS_DC_GET_SOURCE_NAME;
            name.header.size      = sizeof name;
            name.header.adapterId = paths[i].sourceInfo.adapterId;
            name.header.id        = paths[i].sourceInfo.id;

            if (s_get_info(&name) != ERROR_SUCCESS) continue;
            if (_wcsicmp(name.viewGdiDeviceName, device) != 0) continue;

            *adapter = paths[i].targetInfo.adapterId;
            *target  = paths[i].targetInfo.id;
            found = true;
        }
    }

    free(paths);
    free(modes);
    return found;
}

int display_hdr_state(const wchar_t *device) {
    LUID   adapter;
    UINT32 target;
    if (!ccd_find_target(device, &adapter, &target)) return HDR_UNSUPPORTED;

    MsDcAdvancedColorInfo info = {0};
    info.header.type      = MS_DC_GET_ADVANCED_COLOR_INFO;
    info.header.size      = sizeof info;
    info.header.adapterId = adapter;
    info.header.id        = target;

    if (s_get_info(&info) != ERROR_SUCCESS) return HDR_UNSUPPORTED;
    if (!(info.value & MS_ADV_COLOR_SUPPORTED)) return HDR_UNSUPPORTED;
    return (info.value & MS_ADV_COLOR_ENABLED) ? HDR_ON : HDR_OFF;
}

bool display_hdr_set(const wchar_t *device, bool on) {
    LUID   adapter;
    UINT32 target;
    if (!ccd_find_target(device, &adapter, &target)) {
        log_w(L"display: %ls has no CCD target — HDR not changed", device);
        return false;
    }

    MsDcAdvancedColorInfo info = {0};
    info.header.type      = MS_DC_GET_ADVANCED_COLOR_INFO;
    info.header.size      = sizeof info;
    info.header.adapterId = adapter;
    info.header.id        = target;
    if (s_get_info(&info) != ERROR_SUCCESS) return false;

    if (!(info.value & MS_ADV_COLOR_SUPPORTED)) {
        log_w(L"display: %ls does not support HDR", device);
        return false;
    }
    if (info.value & MS_ADV_COLOR_FORCE_DISABLED) {
        log_w(L"display: HDR on %ls is force-disabled (a driver, a duplicated "
              L"desktop or a colour profile has vetoed it)", device);
        return false;
    }
    if (!!(info.value & MS_ADV_COLOR_ENABLED) == on) return true;

    MsDcSetAdvancedColorState set = {0};
    set.header.type      = MS_DC_SET_ADVANCED_COLOR_STATE;
    set.header.size      = sizeof set;
    set.header.adapterId = adapter;
    set.header.id        = target;
    set.value            = on ? 1u : 0u;

    LONG rc = s_set_info(&set);
    if (rc != ERROR_SUCCESS) {
        log_w(L"display: turning HDR %ls on %ls failed (0x%08lX)",
              on ? L"on" : L"off", device, (unsigned long)rc);
        return false;
    }
    log_w(L"display: HDR %ls on %ls", on ? L"on" : L"off", device);
    return true;
}

static bool orientation_is_portrait(DWORD orientation) {
    return orientation == DMDO_90 || orientation == DMDO_270;
}

static int degrees_of_orientation(DWORD orientation) {
    switch (orientation) {
        case DMDO_90:  return ROTATE_90;
        case DMDO_180: return ROTATE_180;
        case DMDO_270: return ROTATE_270;
        default:       return ROTATE_0;
    }
}

static bool orientation_of_degrees(int degrees, DWORD *out) {
    switch (degrees) {
        case ROTATE_0:   *out = DMDO_DEFAULT; return true;
        case ROTATE_90:  *out = DMDO_90;      return true;
        case ROTATE_180: *out = DMDO_180;     return true;
        case ROTATE_270: *out = DMDO_270;     return true;
        default:                              return false;
    }
}

static void swap_axes(DisplayMode *m) {
    int t = m->width; m->width = m->height; m->height = t;
}

const wchar_t *rotation_name(int rotation) {
    switch (rotation) {
        case ROTATE_90:  return L"portrait";
        case ROTATE_180: return L"landscape (flipped)";
        case ROTATE_270: return L"portrait (flipped)";
        default:         return L"landscape";
    }
}

int display_rotation(const wchar_t *device) {
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0))
        return ROTATE_0;
    return degrees_of_orientation(dm.dmDisplayOrientation);
}

bool display_current_mode(const wchar_t *device, DisplayMode *out) {
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0))
        return false;
    out->width   = (int)dm.dmPelsWidth;
    out->height  = (int)dm.dmPelsHeight;
    out->refresh = (int)dm.dmDisplayFrequency;
    if (orientation_is_portrait(dm.dmDisplayOrientation)) swap_axes(out);
    return true;
}

int display_modes(const wchar_t *device, DisplayMode *out, int max) {
    DEVMODEW cur = { .dmSize = sizeof cur };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &cur, 0))
        return 0;

    bool rotated = orientation_is_portrait(cur.dmDisplayOrientation);

    int count = 0;
    DEVMODEW dm = { .dmSize = sizeof dm };
    for (DWORD i = 0; EnumDisplaySettingsExW(device, i, &dm, 0); i++) {
        dm.dmSize = sizeof dm;

        if (dm.dmBitsPerPel != cur.dmBitsPerPel) continue;
        if (dm.dmDisplayFlags & DM_INTERLACED)   continue;

        DisplayMode m = { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight,
                          (int)dm.dmDisplayFrequency };
        if (rotated) swap_axes(&m);

        bool seen = false;
        for (int k = 0; k < count && !seen; k++)
            seen = out[k].width   == m.width  &&
                   out[k].height  == m.height &&
                   out[k].refresh == m.refresh;
        if (seen) continue;

        if (count >= max) break;
        out[count++] = m;
    }
    return count;
}

bool display_set_mode(const wchar_t *device, const DisplayMode *want,
                      int rotation) {
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        log_w(L"display: cannot read the current mode of %ls", device);
        return false;
    }

    DWORD now_orientation = dm.dmDisplayOrientation;
    DWORD orientation     = now_orientation;
    if (rotation != ROTATE_KEEP &&
        !orientation_of_degrees(rotation, &orientation)) {
        log_err(L"display: %d is not a rotation — expected 0, 90, 180 or 270",
                rotation);
        return false;
    }
    int now_rotation = degrees_of_orientation(now_orientation);

    DisplayMode now = { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight,
                        (int)dm.dmDisplayFrequency };
    if (orientation_is_portrait(now_orientation)) swap_axes(&now);

    DisplayMode target = now;
    if (want->width > 0 && want->height > 0) {
        target.width  = want->width;
        target.height = want->height;
    }
    if (want->refresh > 0) target.refresh = want->refresh;

    if (target.width   == now.width &&
        target.height  == now.height &&
        target.refresh == now.refresh &&
        orientation    == now_orientation)
        return true;

    DisplayMode pels = target;
    if (orientation_is_portrait(orientation)) swap_axes(&pels);

    dm.dmPelsWidth        = (DWORD)pels.width;
    dm.dmPelsHeight       = (DWORD)pels.height;
    dm.dmDisplayFrequency = (DWORD)target.refresh;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY |
                  DM_BITSPERPEL;
    if (orientation != now_orientation) {
        dm.dmDisplayOrientation = orientation;
        dm.dmFields |= DM_DISPLAYORIENTATION;
    }

    int target_rotation = degrees_of_orientation(orientation);

    LONG rc = ChangeDisplaySettingsExW(device, &dm, NULL, CDS_TEST, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        log_err(L"display: %ls will not do %dx%d @%dHz %ls (test returned %ld) "
                L"— left at %dx%d @%dHz %ls. `mshell.exe --displays` lists the "
                L"modes it will do.",
                device, target.width, target.height, target.refresh,
                rotation_name(target_rotation), (long)rc,
                now.width, now.height, now.refresh,
                rotation_name(now_rotation));
        return false;
    }

    rc = ChangeDisplaySettingsExW(device, &dm, NULL, 0, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        log_err(L"display: setting %ls to %dx%d @%dHz %ls failed (%ld)",
                device, target.width, target.height, target.refresh,
                rotation_name(target_rotation), (long)rc);
        return false;
    }

    log_w(L"display: %ls -> %dx%d @%dHz %ls", device, target.width,
          target.height, target.refresh, rotation_name(target_rotation));
    return true;
}

static int primary_at_origin(const POINTL *pos, int n) {
    for (int i = 0; i < n; i++)
        if (pos[i].x == 0 && pos[i].y == 0) return i;
    return -1;
}

static bool arrangement_submit(const POINTL *pos, int primary, int n) {
    for (int i = 0; i < n; i++) {
        DEVMODEW dm = { .dmSize = sizeof dm };
        if (!EnumDisplaySettingsExW(g.monitors[i].device, ENUM_CURRENT_SETTINGS,
                                    &dm, 0))
            return false;

        dm.dmFields   = DM_POSITION;
        dm.dmPosition = pos[i];

        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
        if (i == primary) flags |= CDS_SET_PRIMARY;

        LONG rc = ChangeDisplaySettingsExW(g.monitors[i].device, &dm, NULL,
                                           flags, NULL);
        if (rc != DISP_CHANGE_SUCCESSFUL) {
            log_err(L"display: %ls will not sit at %+ld%+ld (%ld)",
                    g.monitors[i].device, (long)pos[i].x, (long)pos[i].y,
                    (long)rc);
            return false;
        }
    }
    return true;
}

static void arrangement_apply(void) {
    int n = g.monitor_count;
    if (n <= 0 || n > MAX_MONITORS) return;

    POINTL now[MAX_MONITORS], want[MAX_MONITORS];
    for (int i = 0; i < n; i++) {
        if (!g.monitors[i].device[0]) return;
        DEVMODEW dm = { .dmSize = sizeof dm };
        if (!EnumDisplaySettingsExW(g.monitors[i].device, ENUM_CURRENT_SETTINGS,
                                    &dm, 0))
            return;
        now[i] = want[i] = dm.dmPosition;
    }

    int  primary = primary_at_origin(now, n);
    bool any     = false;

    for (int i = 0; i < n; i++) {
        for (int r = 0; r < g.cfg.monitor_rule_count; r++) {
            const MonitorRule *mr = &g.cfg.monitor_rules[r];
            bool hit = (mr->device[0])
                           ? wildcard_match(mr->device, g.monitors[i].device)
                           : (mr->index == i);
            if (!hit) continue;

            if (mr->set_position) {
                want[i].x = mr->pos_x;
                want[i].y = mr->pos_y;
                any = true;
            }
            if (mr->set_primary && mr->primary) { primary = i; any = true; }
        }
    }
    if (!any || primary < 0) return;

    LONG dx = want[primary].x, dy = want[primary].y;
    for (int i = 0; i < n; i++) { want[i].x -= dx; want[i].y -= dy; }

    bool changed = false;
    for (int i = 0; i < n; i++)
        if (want[i].x != now[i].x || want[i].y != now[i].y) changed = true;
    if (!changed) return;

    if (!arrangement_submit(want, primary, n)) {
        int  back     = primary_at_origin(now, n);
        bool restored = back >= 0 && arrangement_submit(now, back, n);
        if (restored) ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
        log_err(restored
                    ? L"display: the arrangement was not changed"
                    : L"display: the arrangement was not changed and could not "
                      L"be rolled back in full — check Windows' display "
                      L"settings");
        return;
    }

    LONG rc = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        log_err(L"display: applying the arrangement failed (%ld)", (long)rc);
        return;
    }

    for (int i = 0; i < n; i++)
        log_w(L"display: %ls -> %+ld%+ld%ls", g.monitors[i].device,
              (long)want[i].x, (long)want[i].y,
              i == primary ? L" (primary)" : L"");
}

static wchar_t s_applied[MAX_MONITORS][CCHDEVICENAME];
static int     s_applied_count;

static bool already_applied(const wchar_t *device) {
    for (int i = 0; i < s_applied_count; i++)
        if (_wcsicmp(s_applied[i], device) == 0) return true;
    return false;
}

static void mark_applied(const wchar_t *device) {
    if (already_applied(device) || s_applied_count >= MAX_MONITORS) return;
    wcsncpy(s_applied[s_applied_count], device, CCHDEVICENAME - 1);
    s_applied[s_applied_count][CCHDEVICENAME - 1] = L'\0';
    s_applied_count++;
}

void displays_apply_rules(bool force) {
    if (force) s_applied_count = 0;

    bool fresh = false;

    for (int i = 0; i < g.monitor_count; i++) {
        const Monitor *m = &g.monitors[i];
        if (!m->device[0]) continue;
        if (!force && already_applied(m->device)) continue;
        fresh = true;

        DisplayMode want = {0};
        int  rotation = ROTATE_KEEP;
        bool set_hdr = false, hdr = false;
        bool any     = false;

        for (int r = 0; r < g.cfg.monitor_rule_count; r++) {
            const MonitorRule *mr = &g.cfg.monitor_rules[r];
            bool hit = (mr->device[0]) ? wildcard_match(mr->device, m->device)
                                       : (mr->index == i);
            if (!hit) continue;

            if (mr->set_resolution) {
                want.width  = mr->width;
                want.height = mr->height;
                any = true;
            }
            if (mr->set_refresh)  { want.refresh = mr->refresh; any = true; }
            if (mr->set_rotation) { rotation = mr->rotation; any = true; }
            if (mr->set_hdr)      { set_hdr = true; hdr = mr->hdr; any = true; }
        }

        mark_applied(m->device);
        if (!any) continue;

        if (want.width > 0 || want.refresh > 0 || rotation != ROTATE_KEEP)
            display_set_mode(m->device, &want, rotation);
        if (set_hdr)
            display_hdr_set(m->device, hdr);
    }

    if (force || fresh) arrangement_apply();
}

void display_toggle_hdr(int mon) {
    if (mon < 0 || mon >= g.monitor_count) return;
    const wchar_t *device = g.monitors[mon].device;

    int state = display_hdr_state(device);
    if (state == HDR_UNSUPPORTED) {
        notify_show(L"HDR is not available on this display", NOTIFY_WARN, 3000);
        return;
    }

    bool on = (state == HDR_OFF);
    if (display_hdr_set(device, on))
        notify_show(on ? L"HDR on" : L"HDR off", NOTIFY_INFO, 2000);
    else
        notify_show(L"HDR could not be changed — see the log",
                    NOTIFY_WARN, 3000);
}

void display_cycle_refresh(int mon, int dir) {
    if (mon < 0 || mon >= g.monitor_count) return;
    const wchar_t *device = g.monitors[mon].device;
    if (!device[0]) return;

    DisplayMode now;
    if (!display_current_mode(device, &now)) return;

    DisplayMode all[128];
    int n = display_modes(device, all, 128);

    int rates[64], count = 0;
    for (int i = 0; i < n && count < 64; i++) {
        if (all[i].width != now.width || all[i].height != now.height) continue;
        int hz = all[i].refresh, k = count++;
        while (k > 0 && rates[k - 1] > hz) { rates[k] = rates[k - 1]; k--; }
        rates[k] = hz;
    }
    if (count < 2) {
        notify_show(L"this display has only one refresh rate",
                    NOTIFY_WARN, 3000);
        return;
    }

    int at = 0;
    for (int i = 0; i < count; i++) if (rates[i] == now.refresh) { at = i; break; }
    int next = (at + (dir < 0 ? -1 : 1) + count) % count;

    DisplayMode want = { 0, 0, rates[next] };
    if (display_set_mode(device, &want, ROTATE_KEEP)) {
        wchar_t msg[64];
        _snwprintf(msg, 64, L"%d Hz", rates[next]);
        msg[63] = L'\0';
        notify_show(msg, NOTIFY_INFO, 2000);
    }
}

static void rotate_focused(const wchar_t *device, int rotation) {
    DisplayMode keep = {0};
    if (display_set_mode(device, &keep, rotation)) {
        wchar_t msg[64];
        _snwprintf(msg, 64, L"%ls (%d°)", rotation_name(rotation), rotation);
        msg[63] = L'\0';
        notify_show(msg, NOTIFY_INFO, 2000);
    } else {
        notify_show(L"this display will not rotate — see the log",
                    NOTIFY_WARN, 3000);
    }
}

void display_cycle_rotation(int mon, int dir) {
    if (mon < 0 || mon >= g.monitor_count) return;
    const wchar_t *device = g.monitors[mon].device;
    if (!device[0]) return;

    int quarters = display_rotation(device) / 90;
    int next     = (quarters + (dir < 0 ? 3 : 1)) % 4;
    rotate_focused(device, next * 90);
}

void display_toggle_portrait(int mon) {
    if (mon < 0 || mon >= g.monitor_count) return;
    const wchar_t *device = g.monitors[mon].device;
    if (!device[0]) return;

    int now = display_rotation(device);
    rotate_focused(device, (now == ROTATE_90 || now == ROTATE_270)
                               ? ROTATE_0 : ROTATE_90);
}

static void print_w(const wchar_t *line) {
    char u8[1024];
    if (WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, (int)sizeof u8,
                            NULL, NULL) > 0)
        console_print(u8);
}

void display_list(void) {
    DISPLAY_DEVICEW dd = { .cb = sizeof dd };
    bool any = false;

    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        dd.cb = sizeof dd;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        any = true;

        DISPLAY_DEVICEW mon = { .cb = sizeof mon };
        const wchar_t *label = dd.DeviceString;
        if (EnumDisplayDevicesW(dd.DeviceName, 0, &mon, 0) && mon.DeviceString[0])
            label = mon.DeviceString;

        DisplayMode cur = {0};
        display_current_mode(dd.DeviceName, &cur);

        int  hdr = display_hdr_state(dd.DeviceName);
        const wchar_t *hdr_s = hdr == HDR_ON  ? L"on"
                             : hdr == HDR_OFF ? L"off"
                                              : L"unsupported";

        int rot = display_rotation(dd.DeviceName);

        DEVMODEW at = { .dmSize = sizeof at };
        long x = 0, y = 0;
        if (EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &at, 0)) {
            x = (long)at.dmPosition.x;
            y = (long)at.dmPosition.y;
        }

        wchar_t line[512];
        _snwprintf(line, 512,
                   L"%-14ls %dx%d @%dHz %+ld%+ld  %-19ls HDR: %-11ls %ls%ls",
                   dd.DeviceName, cur.width, cur.height, cur.refresh, x, y,
                   rotation_name(rot), hdr_s, label,
                   (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
                       ? L"  (primary)" : L"");
        line[511] = L'\0';
        print_w(line);

        DisplayMode modes[128];
        int n = display_modes(dd.DeviceName, modes, 128);

        wchar_t buf[512];
        int     used = _snwprintf(buf, 512, L"  modes:");
        for (int k = 0; k < n; k++) {
            wchar_t one[32];
            int len = _snwprintf(one, 32, L" %dx%d@%d", modes[k].width,
                                 modes[k].height, modes[k].refresh);
            if (len < 0) continue;
            if (used + len >= 78) {
                buf[used] = L'\0';
                print_w(buf);
                used = _snwprintf(buf, 512, L"        ");
            }
            memcpy(buf + used, one, (size_t)len * sizeof(wchar_t));
            used += len;
        }
        buf[used] = L'\0';
        if (n > 0) print_w(buf);
    }

    if (!any) console_print("no displays are attached to the desktop");
}
