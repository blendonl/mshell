#include "mshell.h"
#include <wtsapi32.h>
#include <shlobj.h>

MShell g = {0};

static bool is_elevated(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elev = {0};
    DWORD size = sizeof(elev);
    BOOL ok = GetTokenInformation(token, TokenElevation, &elev, size, &size);
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

static UINT g_prev_fg_lock_timeout = 0;

static LONG WINAPI mshell_crash_handler(EXCEPTION_POINTERS *ep) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;

    log_err(L"FATAL: unhandled exception 0x%08lX at %p — restoring hidden "
            L"windows before exiting",
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress
                                      : NULL);

    window_restore_all_visibility();

    mouse_restore_pointer();

    log_shutdown();
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool path_copy(wchar_t *out, size_t out_len, const wchar_t *src) {
    size_t len = wcslen(src);
    if (len >= out_len) return false;
    memcpy(out, src, (len + 1) * sizeof(wchar_t));
    return true;
}

static bool appdata_config_dir(wchar_t *out, size_t out_len) {
    PWSTR roaming = NULL;
    bool  ok = false;

    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData,
                                       KF_FLAG_CREATE, NULL, &roaming))) {
        int n = _snwprintf(out, out_len, L"%ls\\mshell", roaming);
        ok = n > 0 && (size_t)n < out_len;
        CoTaskMemFree(roaming);
    }

    if (!ok) {
        const wchar_t *env = _wgetenv(L"APPDATA");
        if (env && env[0]) {
            int n = _snwprintf(out, out_len, L"%ls\\mshell", env);
            ok = n > 0 && (size_t)n < out_len;
        }
    }
    if (!ok) out[0] = L'\0';
    return ok;
}

static bool file_exists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool portable_config_path(wchar_t *out, size_t out_len) {
    wchar_t exe_dir[MAX_PATH];
    DWORD   n = GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    wchar_t *last_slash = wcsrchr(exe_dir, L'\\');
    if (!last_slash) return false;
    *last_slash = L'\0';

    int w = _snwprintf(out, out_len, L"%ls\\config\\init.lua", exe_dir);
    return w > 0 && (size_t)w < out_len;
}

#define CRASHLOOP_KEY     L"Software\\mshell"
#define CRASHLOOP_WINDOW  60ULL
#define CRASHLOOP_LIMIT   3

static ULONGLONG wall_seconds(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000000ULL;
}

static bool crashloop_open(HKEY *out, REGSAM extra) {
    return RegCreateKeyExW(HKEY_CURRENT_USER, CRASHLOOP_KEY, 0, NULL,
                           REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | extra,
                           NULL, out, NULL) == ERROR_SUCCESS;
}

static bool crashloop_record_launch(void) {
    HKEY k;
    if (!crashloop_open(&k, KEY_SET_VALUE)) return false;

    DWORD     count = 0, sz = sizeof(count);
    ULONGLONG first = 0;
    DWORD     fsz   = sizeof(first);
    RegQueryValueExW(k, L"LaunchCount", NULL, NULL, (LPBYTE)&count, &sz);
    sz = fsz;
    RegQueryValueExW(k, L"LaunchFirst", NULL, NULL, (LPBYTE)&first, &sz);

    ULONGLONG now = wall_seconds();

    if (count == 0 || first == 0 || now < first || now - first > CRASHLOOP_WINDOW) {
        count = 1;
        first = now;
    } else {
        count++;
    }

    RegSetValueExW(k, L"LaunchCount", 0, REG_DWORD,
                   (const BYTE *)&count, sizeof(count));
    RegSetValueExW(k, L"LaunchFirst", 0, REG_QWORD,
                   (const BYTE *)&first, sizeof(first));
    RegCloseKey(k);

    if (count >= CRASHLOOP_LIMIT) {
        log_err(L"crash loop: %lu launches within %llu seconds — starting in "
                L"SAFE MODE (config skipped).", (unsigned long)count,
                CRASHLOOP_WINDOW);
        return true;
    }
    log_msg(LOG_INFO, L"launch %lu of the current %llus window",
            (unsigned long)count, CRASHLOOP_WINDOW);
    return false;
}

void crashloop_mark_healthy(void) {
    HKEY k;
    if (!crashloop_open(&k, KEY_SET_VALUE)) return;
    DWORD zero = 0;
    RegSetValueExW(k, L"LaunchCount", 0, REG_DWORD,
                   (const BYTE *)&zero, sizeof(zero));
    RegCloseKey(k);
    log_msg(LOG_INFO, L"survived %llus — crash-loop counter reset", CRASHLOOP_WINDOW);
}

static void warn_if_no_autorestart(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return;

    BYTE  buf[16] = {0};
    DWORD sz = sizeof(buf), type = 0;
    if (RegQueryValueExW(k, L"AutoRestartShell", NULL, &type,
                         buf, &sz) == ERROR_SUCCESS) {
        bool off = false;
        if (type == REG_DWORD && sz >= sizeof(DWORD)) {
            DWORD v;
            memcpy(&v, buf, sizeof(v));
            off = (v == 0);
        } else if (sz >= sizeof(wchar_t)) {
            wchar_t c;
            memcpy(&c, buf, sizeof(c));
            off = (c == L'0');
        }
        if (off)
            log_err(L"AutoRestartShell is 0: if mshell exits or crashes, "
                    L"Windows will LOG YOU OUT rather than restart the shell.");
    }
    RegCloseKey(k);
}

void resolve_config_path(wchar_t *out, size_t out_len) {
    wchar_t dir[MAX_PATH];
    wchar_t appdata[MAX_PATH];
    bool    have_appdata = false;

    if (appdata_config_dir(dir, MAX_PATH)) {
        int n = _snwprintf(appdata, MAX_PATH, L"%ls\\init.lua", dir);
        have_appdata = n > 0 && (size_t)n < MAX_PATH;
    }
    if (have_appdata && file_exists(appdata)) {
        if (path_copy(out, out_len, appdata)) return;
    }

    wchar_t portable[MAX_PATH];
    if (portable_config_path(portable, MAX_PATH) && file_exists(portable)) {
        if (path_copy(out, out_len, portable)) return;
    }

    if (have_appdata && path_copy(out, out_len, appdata)) return;
    path_copy(out, out_len, L"config\\init.lua");
}

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

        for (int r = 0; r < g.cfg.monitor_rule_count; r++) {
            const MonitorRule *mr = &g.cfg.monitor_rules[r];

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

static int s_resink_left;

static int s_spi_broadcast_depth;

BOOL spi_set_broadcast(UINT action, UINT ui_param, PVOID pv_param) {
    s_spi_broadcast_depth++;
    BOOL ok = SystemParametersInfoW(action, ui_param, pv_param, SPIF_SENDCHANGE);
    if (s_spi_broadcast_depth > 0) s_spi_broadcast_depth--;
    return ok;
}

static bool setting_change_affects_layout(WPARAM wp, LPARAM lp) {
    if (s_spi_broadcast_depth > 0) return false;

    switch (wp) {
    case SPI_SETWORKAREA:
    case SPI_SETNONCLIENTMETRICS:
        return true;
    default:
        break;
    }

    if (wp != 0) return false;

    const wchar_t *area = (const wchar_t *)lp;
    return area && _wcsicmp(area, L"WindowMetrics") == 0;
}

LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MSHELL_ACTION: {
        Action  action;
        int     arg;
        wchar_t cmd[MAX_PATH];
        wchar_t args[SPAWN_ARGS_MAX];
        wchar_t cwd[MAX_PATH];
        int     count = 0;

        if (kb_take_pending((unsigned)lp, &action, &arg,
                            cmd, MAX_PATH, args, SPAWN_ARGS_MAX,
                            cwd, MAX_PATH, &count)) {
            log_w(L"hook match: vk=0x%02X mods=0x%X -> action=%d",
                  (unsigned)(wp & 0xFFFF), (unsigned)((wp >> 16) & 0xFFFF),
                  (int)action);
            int reps = (count > 1 && action_is_repeatable(action)) ? count : 1;
            for (int i = 0; i < reps; i++)
                execute_action(action, arg, cmd[0] ? cmd : NULL,
                               args[0] ? args : NULL,
                               cwd[0] ? cwd : NULL);
        }
        return 0;
    }

    case WM_MSHELL_SUBMAP:
        whichkey_notify();
        return 0;

    case WM_MSHELL_IPC:
        ipc_handle_request((void *)lp);
        return 0;

    case WM_MSHELL_CONFIG_CHANGED:
        config_on_file_changed((unsigned)wp);
        return 0;

    case WM_DISPLAYCHANGE:
        update_work_area();
        displays_apply_rules(false);
        desktop_monitors_changed();
        background_update();
        bar_reconfigure();
        tile_current();
        s_resink_left = RESINK_RETRIES;
        SetTimer(hwnd, TIMER_RESINK, RESINK_RETRY_MS, NULL);
        return 0;

    case WM_SETTINGCHANGE:
        if (!setting_change_affects_layout(wp, lp)) {
            const wchar_t *area = (wp == 0 && lp) ? (const wchar_t *)lp : L"";
            log_w(L"settings: ignoring WM_SETTINGCHANGE wParam=%u area='%ls'%ls",
                  (unsigned)wp, area,
                  s_spi_broadcast_depth > 0 ? L" (our own broadcast)" : L"");
            return 0;
        }
        update_work_area();
        desktop_monitors_changed();
        background_update();
        bar_reconfigure();
        tile_current();
        return 0;

    case WM_DPICHANGED:
        update_work_area();
        background_update();
        whichkey_hide();
        bar_reconfigure();
        tile_current();
        return 0;

    case WM_MSHELL_UPDATE: {
        wchar_t *msg = (wchar_t *)lp;
        if (msg) {
            NotifyKind kind = (NotifyKind)LOWORD(wp);
            int        ms   = (int)HIWORD(wp);
            notify_show(msg, kind, ms > 0 ? ms : 15000);
            free(msg);
        }
        return 0;
    }

    case WM_MSHELL_RESTART:
        log_w(L"update: handing over to the build just installed");
        g.running = false;
        PostQuitMessage(0);
        return 0;

    case WM_MSHELL_CAPTURE_KEY:
        launcher_key((DWORD)wp, (wchar_t)lp);
        return 0;

    case WM_MSHELL_MOUSE:
        mouse_mod_drag_apply((int)(LONG)wp, (int)(LONG)lp);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_FOLLOW_MOUSE) { mouse_poll_focus();  return 0; }
        if (wp == TIMER_SINK_VERIFY) {
            window_verify_visibility();
            window_verify_placement();
            window_verify_sink();
            return 0;
        }
        if (wp == TIMER_ANIM)          { anim_tick();        return 0; }
        if (wp == TIMER_RESINK) {
            update_work_area();
            background_update();
            bar_reconfigure();
            tile_current();
            window_resink();
            if (--s_resink_left <= 0) KillTimer(hwnd, TIMER_RESINK);
            return 0;
        }
        if (wp == TIMER_CRASHLOOP_HEALTHY) {
            KillTimer(hwnd, TIMER_CRASHLOOP_HEALTHY);
            crashloop_mark_healthy();
            return 0;
        }
        break;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_UNLOCK || wp == WTS_SESSION_LOGON ||
            wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)
            kb_reset_state();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {
            g.running = false;
            PostQuitMessage(0);
        }
        return 0;

    case WM_CLOSE:
        g.running = false;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

static HWND create_message_window(HINSTANCE hinst) {
    const wchar_t *class_name = L"mshell_MessageWindow";

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MessageWndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    return CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"mshell", WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hinst, NULL);
}

static const char *flag_value(const char *cmd, const char *flag) {
    if (!cmd) return NULL;
    size_t flen = strlen(flag);
    for (const char *p = cmd; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - start) == flen && strncmp(start, flag, flen) == 0) {
            while (*p == ' ' || *p == '\t') p++;
            return *p ? p : NULL;
        }
    }
    return NULL;
}

static bool has_flag(const char *cmd, const char *flag) {
    if (!cmd) return false;
    size_t flen = strlen(flag);
    for (const char *p = cmd; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if ((size_t)(p - start) == flen && strncmp(start, flag, flen) == 0)
            return true;
    }
    return false;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)nCmdShow;

    g.hinst   = hInstance;
    g.running = true;

    {
        int code = 0;
        if (ipc_client_try(&code)) return code;
    }

    kb_locks_init();

    if (has_flag(lpCmdLine, "--displays")) {
        display_list();
        return 0;
    }

    if (has_flag(lpCmdLine, "--tweaks")) {
        const char *arg = flag_value(lpCmdLine, "--tweaks");
        wchar_t     group[64] = {0};
        char        verb[32]  = {0};

        if (arg) sscanf(arg, "%31s %63ls", verb, group);

        if (!verb[0] || !strcmp(verb, "list")) {
            tweaks_list();
            return 0;
        }
        if (!strcmp(verb, "apply")) {
            char msg[128];
            snprintf(msg, sizeof msg, "applied %d tweaks",
                     tweaks_apply(group[0] ? group : NULL));
            console_print(msg);
            return 0;
        }
        if (!strcmp(verb, "revert")) {
            char msg[128];
            snprintf(msg, sizeof msg, "reverted %d tweaks",
                     tweaks_revert(group[0] ? group : NULL));
            console_print(msg);
            return 0;
        }
        if (!strcmp(verb, "reg") || !strcmp(verb, "reg-undo")) {
            tweaks_emit_reg(group[0] ? group : NULL,
                            strcmp(verb, "reg-undo") == 0);
            return 0;
        }
        console_print("usage: mshell --tweaks <list|apply|revert|reg|reg-undo> "
                      "[input|visual|quiet|apps|all]");
        return 1;
    }

    if (has_flag(lpCmdLine, "--check")) {
        kb_locks_init();
        resolve_config_path(g.config_path, MAX_PATH);

        char msg[1024];
        char path_u8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, g.config_path, -1, path_u8,
                            (int)sizeof path_u8, NULL, NULL);

        if (config_load(g.config_path)) {
            snprintf(msg, sizeof msg,
                     "ok: %s\n  %d root bindings, %d keymaps, %d window rules, "
                     "%d desktop rules, %d startup programs",
                     path_u8, g.cfg.keymaps->root ? g.cfg.keymaps->root->count : 0,
                     g.cfg.keymaps->count, g.cfg.rule_count, g.cfg.desktop_rule_count,
                     g.cfg.startup_count);
            console_print(msg);
            return 0;
        }
        snprintf(msg, sizeof msg,
                 "FAILED: %s\n  %s\n  Nothing in this file would take effect: "
                 "a config error is atomic.", path_u8, g.config_error);
        console_print(msg);
        return 1;
    }

    g.test_mode     = has_flag(lpCmdLine, "--test") || has_flag(lpCmdLine, "-t");
    bool shell_mode = has_flag(lpCmdLine, "--shell");
    bool verbose    = has_flag(lpCmdLine, "--verbose") || has_flag(lpCmdLine, "-v");

    log_init(L"mshell", verbose ? LOG_DEBUG : LOG_INFO);
    log_msg(LOG_INFO, L"=== mshell v%hs starting ===", MSHELL_VERSION);

    update_clear_staged_image();

    SetUnhandledExceptionFilter(mshell_crash_handler);

    {
        HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\mshell_singleton");
        if (!once || GetLastError() == ERROR_ALREADY_EXISTS) {
            log_err(L"another mshell is already running in this session — "
                    L"exiting. Two instances would fight over the keyboard hook "
                    L"and the window layout.");
            if (once) CloseHandle(once);
            return 0;
        }
    }

    g.elevated = is_elevated();

    if (!g.elevated && shell_mode)
        log_w(L"Shell mode at user level (UAC on) — windows owned by elevated "
              L"processes can't be managed (UIPI).");

    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
        log_w(L"SetPriorityClass(ABOVE_NORMAL) failed: %lu", GetLastError());

    config_reset();

    if (!SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0,
                               &g_prev_fg_lock_timeout, 0))
        g_prev_fg_lock_timeout = 0;

    if (!spi_set_broadcast(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)(UINT_PTR)0))
        log_w(L"SPI_SETFOREGROUNDLOCKTIMEOUT failed: %lu — window_focus() will "
              L"fall back to SwitchToThisWindow", GetLastError());

    update_work_area();

    g.message_window = create_message_window(hInstance);
    if (!g.message_window) {
        log_w(L"FATAL: could not create message window");
        return 1;
    }

    WTSRegisterSessionNotification(g.message_window, NOTIFY_FOR_THIS_SESSION);

    SetTimer(g.message_window, TIMER_FOLLOW_MOUSE, FOLLOW_MOUSE_MS, NULL);
    SetTimer(g.message_window, TIMER_SINK_VERIFY, SINK_VERIFY_MS, NULL);

    if (!g.test_mode) {
        warn_if_no_autorestart();
        g.safe_mode = crashloop_record_launch();
        SetTimer(g.message_window, TIMER_CRASHLOOP_HEALTHY,
                 (UINT)(CRASHLOOP_WINDOW * 1000), NULL);
    }

    resolve_config_path(g.config_path, MAX_PATH);
    log_w(L"config: %ls", g.config_path);

    if (g.elevated)
        log_err(L"running ELEVATED: %ls executes with administrator rights — "
                L"treat it as trusted code, and keep the folder it lives in from "
                L"being writable by anything you don't trust. Auto-reload is "
                L"disabled in this mode; reload with Win+Shift+R.",
                g.config_path);

    config_init();

    displays_apply_rules(true);

    desktop_init();

    background_init();
    border_init();
    whichkey_init();
    notify_init();
    launcher_init();
    anim_dim_init();
    bar_init();
    update_work_area();
    bar_reconfigure();

    if (!kb_init()) {
        log_w(L"FATAL: kb_init failed");
        return 1;
    }
    log_err(L"config loaded: %d root bindings, %d keymaps, %d desktop rules, "
            L"%d startup programs — starting on desktop '%ls'",
            g.active_keymaps->root ? g.active_keymaps->root->count : -1,
            g.active_keymaps->count,
            g.cfg.desktop_rule_count, g.cfg.startup_count, desktop_current()->name);

    helper_init();
    ipc_start();

    if (!events_init()) {
        log_w(L"FATAL: events_init failed");
        return 1;
    }

    events_sync_urgency();
    mouse_sync_hook();
    mouse_sync_pointer();
    update_check_async();

    window_manage_existing();

    tile_current();

    for (int i = 0; i < g.cfg.startup_count; i++) {
        if (g.cfg.startup_commands[i].cmd)
            spawn_command(g.cfg.startup_commands[i].cmd,
                          g.cfg.startup_commands[i].args,
                          g.cfg.startup_commands[i].cwd, L"startup");
    }

    desktop_launch_app_if_empty(desktop_current_slot());

    log_w(L"mshell started — mode=%ls, on desktop '%ls', %d managed windows",
          g.test_mode ? L"test" : L"shell",
          desktop_current()->name, g.managed_count);

    MSG msg;
    while (g.running && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    log_w(L"Shutting down…");

    ipc_stop();
    helper_shutdown();
    events_shutdown();
    kb_shutdown();
    config_shutdown();

    window_restore_all_visibility();

    window_restore_all_decorations();

    bar_shutdown();
    anim_cancel_all();
    anim_dim_shutdown();
    launcher_shutdown();
    notify_shutdown();
    whichkey_shutdown();
    border_shutdown();
    background_shutdown();

    spi_set_broadcast(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                      (PVOID)(UINT_PTR)g_prev_fg_lock_timeout);

    mouse_restore_pointer();

    if (g.message_window) {
        WTSUnRegisterSessionNotification(g.message_window);
        DestroyWindow(g.message_window);
    }

    log_msg(LOG_INFO, L"mshell exited");
    log_shutdown();
    return 0;
}
