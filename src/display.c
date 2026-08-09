/* ===========================================================================
 * display.c — the physical display itself: resolution, refresh rate, HDR.
 *
 * Everything else in mshell arranges windows INSIDE a monitor and takes the
 * monitor's mode as given. This file is the one place that changes the mode.
 *
 * WHY IT IS HERE AT ALL. mshell replaces explorer.exe, and Settings ->
 * System -> Display is an Explorer-hosted surface. It still opens (ms-settings:
 * is a protocol handler, not a shell window), but reaching it from a shell with
 * no Start menu means spawning it by URI and driving it with the mouse — for
 * something a monitor rule can state once and mshell can re-assert on every
 * start. Refresh rate and HDR in particular are things people flip per task
 * (165Hz for the desktop, HDR only while a game is up), which is exactly the
 * shape of a keybinding.
 *
 * TWO UNRELATED WIN32 APIs LIVE HERE.
 *
 *   Resolution and refresh rate are GDI: EnumDisplaySettingsExW to read and
 *   enumerate, ChangeDisplaySettingsExW to set. Keyed by the GDI device name
 *   ("\\.\DISPLAY1"), which is exactly what Monitor.device already holds.
 *
 *   HDR is CCD (Connecting and Configuring Displays): QueryDisplayConfig gives
 *   you paths, a path's TARGET is the physical output, and the advanced-colour
 *   state hangs off that target. There is no GDI door to it. The bridge between
 *   the two APIs is DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, which hands back
 *   the GDI device name for a path's source — so we can start from the name we
 *   already have and end up at the target we need.
 *
 * WHY THE CCD TYPES ARE RE-DECLARED BELOW. The advanced-colour structures
 * arrived with Windows 10 1803 and reached mingw-w64's wingdi.h considerably
 * later; the build box's headers are not something this file can assume. Every
 * CCD type it needs is therefore declared here under an Ms* name (no clash with
 * whatever the header does or does not define) and every CCD function is
 * resolved with GetProcAddress rather than linked, so an older import library
 * cannot fail the link either. The layouts are ABI-frozen, being how a
 * user-mode process talks to the display stack.
 *
 * MODE CHANGES ARE SESSION-ONLY. ChangeDisplaySettingsExW is called with no
 * flags, which changes the mode dynamically and leaves Windows' own registry
 * display configuration alone. Two reasons. mshell re-applies the rules at
 * every start, so persistence would buy nothing it does not already have; and
 * booting WITHOUT mshell — the recovery path INSTALL.md walks you through —
 * should hand you back the display Windows was configured with, not the one a
 * config file that is no longer running asked for. HDR is the exception and
 * cannot be otherwise: the advanced-colour state IS a persistent system
 * setting, and there is no transient form of it.
 * =========================================================================== */
#include "mshell.h"

/* ---------------------------------------------------------------------------
 * CCD types (see the file header for why these are not taken from wingdi.h)
 * --------------------------------------------------------------------------- */
#define MS_QDC_ONLY_ACTIVE_PATHS      0x00000002u

#define MS_DC_GET_SOURCE_NAME                  1u
#define MS_DC_GET_ADVANCED_COLOR_INFO          9u
#define MS_DC_SET_ADVANCED_COLOR_STATE        10u

/* Bits of MsDcAdvancedColorInfo.value. Spelled as masks rather than as a
 * bitfield so the layout does not depend on how a compiler packs one. */
#define MS_ADV_COLOR_SUPPORTED        0x1u   /* the panel can do HDR          */
#define MS_ADV_COLOR_ENABLED          0x2u   /* and it is on right now        */
#define MS_ADV_COLOR_FORCE_DISABLED   0x8u   /* something has vetoed it       */

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

/* DISPLAYCONFIG_MODE_INFO is only ever passed THROUGH: QueryDisplayConfig
 * insists on somewhere to put the modes and nothing here reads them back. Kept
 * as an opaque block of the documented size rather than transcribing three
 * unions we would never look inside. */
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
    UINT32               value;   /* MS_ADV_COLOR_* */
    UINT32               colorEncoding;
    UINT32               bitsPerColorChannel;
} MsDcAdvancedColorInfo;

typedef struct {
    MsDcDeviceInfoHeader header;
    UINT32               value;   /* bit 0: enable advanced colour */
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

/* All four live in user32, which is already loaded — GetModuleHandleW, so this
 * takes no reference and there is nothing to free. */
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

/* ---------------------------------------------------------------------------
 * GDI device name -> the CCD target behind it.
 *
 * A path's source is what GDI calls "\\.\DISPLAY1"; its target is the physical
 * output that the advanced-colour state belongs to. Only active paths are
 * asked for, so a target that comes back is one with a display actually lit on
 * it.
 * --------------------------------------------------------------------------- */
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

/* ===========================================================================
 * HDR (advanced colour)
 * =========================================================================== */

/* HDR_UNSUPPORTED / HDR_OFF / HDR_ON — see mshell.h. "Unsupported" covers both
 * a panel that cannot do it and a Windows too old to be asked. */
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

    /* Ask first. Setting advanced colour on a panel that does not support it
     * fails anyway, but the failure code says nothing useful, and the common
     * case for a config that names `hdr = true` for every monitor is that one
     * of them is an old 1080p secondary — which deserves a line saying so, not
     * an error. */
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
    /* Already where it was asked to be. Not a failure, and worth short-
     * circuiting: the set call re-negotiates the link even when nothing
     * changes, which blanks the screen for a moment. */
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

/* ===========================================================================
 * Resolution and refresh rate
 * =========================================================================== */

bool display_current_mode(const wchar_t *device, DisplayMode *out) {
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0))
        return false;
    out->width   = (int)dm.dmPelsWidth;
    out->height  = (int)dm.dmPelsHeight;
    out->refresh = (int)dm.dmDisplayFrequency;
    return true;
}

/* Every mode the display will accept at its current colour depth, deduplicated
 * and with interlaced ones dropped.
 *
 * The raw enumeration repeats the same width/height/Hz once per colour depth
 * and per scaling mode, so a 4K panel can hand back several hundred entries for
 * a dozen real modes — a list nobody would read. Returns how many landed in
 * `out`, which may be fewer than the display has if `max` runs out.
 */
int display_modes(const wchar_t *device, DisplayMode *out, int max) {
    DEVMODEW cur = { .dmSize = sizeof cur };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &cur, 0))
        return 0;

    int count = 0;
    DEVMODEW dm = { .dmSize = sizeof dm };
    for (DWORD i = 0; EnumDisplaySettingsExW(device, i, &dm, 0); i++) {
        dm.dmSize = sizeof dm;   /* the call may not preserve it */

        if (dm.dmBitsPerPel != cur.dmBitsPerPel) continue;
        if (dm.dmDisplayFlags & DM_INTERLACED)   continue;

        DisplayMode m = { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight,
                          (int)dm.dmDisplayFrequency };

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

/* Change the mode. Any field left at 0 keeps whatever the display is doing.
 *
 * CDS_TEST first, always. A width/height/Hz triple that the panel cannot show
 * is an easy thing to typo into a config file, and applying one on a machine
 * whose shell IS mshell can leave a black screen with no Settings window to fix
 * it from. The test call asks the driver without touching the output, so a bad
 * mode costs a line in the log instead of a reboot.
 */
bool display_set_mode(const wchar_t *device, const DisplayMode *want) {
    /* Start from the CURRENT mode so the fields not being changed — position
     * above all, which is what places this monitor next to the others — carry
     * over untouched. */
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        log_w(L"display: cannot read the current mode of %ls", device);
        return false;
    }

    DisplayMode now = { (int)dm.dmPelsWidth, (int)dm.dmPelsHeight,
                        (int)dm.dmDisplayFrequency };
    DisplayMode target = now;
    if (want->width > 0 && want->height > 0) {
        target.width  = want->width;
        target.height = want->height;
    }
    if (want->refresh > 0) target.refresh = want->refresh;

    /* Nothing to do. Worth checking rather than letting the driver decide:
     * a redundant mode set still blanks the display for a second or two, and
     * this runs on every config reload. */
    if (target.width   == now.width &&
        target.height  == now.height &&
        target.refresh == now.refresh)
        return true;

    dm.dmPelsWidth        = (DWORD)target.width;
    dm.dmPelsHeight       = (DWORD)target.height;
    dm.dmDisplayFrequency = (DWORD)target.refresh;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY |
                  DM_BITSPERPEL;

    LONG rc = ChangeDisplaySettingsExW(device, &dm, NULL, CDS_TEST, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        log_err(L"display: %ls will not do %dx%d @%dHz (test returned %ld) — "
                L"left at %dx%d @%dHz. `mshell.exe --displays` lists the modes "
                L"it will do.",
                device, target.width, target.height, target.refresh, (long)rc,
                now.width, now.height, now.refresh);
        return false;
    }

    /* No flags: a dynamic change, leaving Windows' stored configuration alone.
     * See the file header. */
    rc = ChangeDisplaySettingsExW(device, &dm, NULL, 0, NULL);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        log_err(L"display: setting %ls to %dx%d @%dHz failed (%ld)",
                device, target.width, target.height, target.refresh, (long)rc);
        return false;
    }

    log_w(L"display: %ls -> %dx%d @%dHz", device, target.width, target.height,
          target.refresh);
    return true;
}

/* ===========================================================================
 * Applying the config's monitor rules
 *
 * Separate from monitors_apply_rules(), which re-resolves the TILING overrides
 * on every single enumeration. Re-asserting a display mode that often would be
 * wrong twice over: changing a mode itself raises WM_DISPLAYCHANGE, and a user
 * who reaches for Windows' own display settings mid-session should not have
 * their choice stamped on a second later by a config file.
 *
 * So a mode is applied when the config SAYS so — at startup and on reload
 * (force) — and otherwise only to a display that has not been seen yet, which
 * is precisely the display that was just plugged in.
 * =========================================================================== */
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

    for (int i = 0; i < g.monitor_count; i++) {
        const Monitor *m = &g.monitors[i];
        if (!m->device[0]) continue;              /* the synthesized fallback */
        if (!force && already_applied(m->device)) continue;

        /* Layered exactly like the tiling overrides: every matching rule
         * applies, in declaration order, each overwriting only what it names. */
        DisplayMode want = {0};
        bool set_hdr = false, hdr = false;
        bool any     = false;

        for (int r = 0; r < g.monitor_rule_count; r++) {
            const MonitorRule *mr = &g.monitor_rules[r];
            bool hit = (mr->device[0]) ? wildcard_match(mr->device, m->device)
                                       : (mr->index == i);
            if (!hit) continue;

            if (mr->set_resolution) {
                want.width  = mr->width;
                want.height = mr->height;
                any = true;
            }
            if (mr->set_refresh) { want.refresh = mr->refresh; any = true; }
            if (mr->set_hdr)     { set_hdr = true; hdr = mr->hdr; any = true; }
        }

        /* Marked whether or not a rule named it: the point of the list is "this
         * display has been through here once", so an unplug/replug of a monitor
         * no rule mentions does not re-run the others. */
        mark_applied(m->device);
        if (!any) continue;

        if (want.width > 0 || want.refresh > 0)
            display_set_mode(m->device, &want);
        if (set_hdr)
            display_hdr_set(m->device, hdr);
    }
}

/* ===========================================================================
 * The bindable actions
 * =========================================================================== */

/* Flip HDR on one monitor and say what happened — a keybinding that changes
 * the whole screen's colour and then says nothing is indistinguishable from a
 * keybinding that did not fire. */
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

/* Step to the next/previous refresh rate the display offers AT ITS CURRENT
 * RESOLUTION, wrapping. Resolution is deliberately held: the reason to bind
 * this is switching between a high rate and a lower one for power or for an
 * app that dislikes the high one, and a binding that silently also changed the
 * resolution would be a trap. */
void display_cycle_refresh(int mon, int dir) {
    if (mon < 0 || mon >= g.monitor_count) return;
    const wchar_t *device = g.monitors[mon].device;
    if (!device[0]) return;

    DisplayMode now;
    if (!display_current_mode(device, &now)) return;

    DisplayMode all[128];
    int n = display_modes(device, all, 128);

    /* The rates for this resolution, in ascending order — the enumeration's own
     * order is the driver's business and is not always sorted. */
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
    if (display_set_mode(device, &want)) {
        wchar_t msg[64];
        _snwprintf(msg, 64, L"%d Hz", rates[next]);
        msg[63] = L'\0';
        notify_show(msg, NOTIFY_INFO, 2000);
    }
}

/* ===========================================================================
 * mshell.exe --displays
 *
 * The discovery half of the feature. A monitor rule is keyed on a device name
 * nothing on screen ever shows you, and asks for a width/height/Hz the panel
 * has to actually support — so without this you would be writing the rule from
 * a guess. Runs standalone: it enumerates the displays itself rather than
 * reading g.monitors, so it works whether or not mshell is the shell, or
 * running at all.
 * =========================================================================== */
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

        /* The monitor's own name ("DELL U2723QE") rather than the adapter's,
         * which is what dd.DeviceString holds at this level. */
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

        wchar_t line[512];
        _snwprintf(line, 512, L"%-14ls %dx%d @%dHz  HDR: %-11ls %ls%ls",
                   dd.DeviceName, cur.width, cur.height, cur.refresh, hdr_s,
                   label,
                   (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
                       ? L"  (primary)" : L"");
        line[511] = L'\0';
        print_w(line);

        /* The modes, wrapped. A 4K panel can offer thirty of them and one line
         * per mode buries the header that names the display. */
        DisplayMode modes[128];
        int n = display_modes(dd.DeviceName, modes, 128);

        wchar_t buf[512];
        int     used = _snwprintf(buf, 512, L"  modes:");
        for (int k = 0; k < n; k++) {
            wchar_t one[32];
            int len = _snwprintf(one, 32, L" %dx%d@%d", modes[k].width,
                                 modes[k].height, modes[k].refresh);
            if (len < 0) continue;
            if (used + len >= 78) {          /* keep it inside a normal console */
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
