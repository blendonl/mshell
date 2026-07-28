/*
 * main.c — shell bootstrap, elevation check, message loop
 */

#include "mshell.h"
#include <wtsapi32.h>   /* WTSRegisterSessionNotification, WTS_SESSION_* */
#include <shlobj.h>     /* SHGetKnownFolderPath, FOLDERID_RoamingAppData    */

/* ---------------------------------------------------------------------------
 * Single global instance
 * --------------------------------------------------------------------------- */
MShell g = {0};

/* ---------------------------------------------------------------------------
 * Quick elevation check
 * --------------------------------------------------------------------------- */
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

/* NB: there is deliberately no relaunch-as-administrator helper here any more.
 * mshell never elevates itself — see the elevation note in WinMain. */

/* The machine's SPI_SETFOREGROUNDLOCKTIMEOUT before we zeroed it, so shutdown
 * can put it back (see WinMain). */
static UINT g_prev_fg_lock_timeout = 0;

/* ---------------------------------------------------------------------------
 * Last-gasp crash handler.
 *
 * mshell hides windows to implement virtual desktops and monocle, so at any
 * moment most managed windows are invisible. If the process dies without
 * un-hiding them they are unreachable — no taskbar button, no Alt+Tab entry —
 * and as the shell there is nothing left to reveal them. The orderly shutdown
 * path handles that; a crash does not reach it.
 *
 * So the very least this can do is give the windows back before the process
 * goes. It does not attempt to continue: the state that produced the fault is
 * not state worth carrying on with, and Winlogon's AutoRestartShell brings a
 * fresh mshell up behind us.
 *
 * Deliberately minimal, because it runs in a process that is already broken:
 * no allocation, no locks, no Lua.
 * --------------------------------------------------------------------------- */
static LONG WINAPI mshell_crash_handler(EXCEPTION_POINTERS *ep) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;

    log_err(L"FATAL: unhandled exception 0x%08lX at %p — restoring hidden "
            L"windows before exiting",
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress
                                      : NULL);

    window_restore_all_visibility();
    session_save();

    /* And give the pointer back. A config with swap_buttons on would otherwise
     * leave a machine whose shell has just died with its mouse buttons the
     * wrong way round and nothing left to change them from. Three
     * SystemParametersInfo calls: no allocation, no locks, no Lua. */
    mouse_restore_pointer();

    log_shutdown();
    return EXCEPTION_CONTINUE_SEARCH;   /* let it crash properly / be reported */
}

/* ---------------------------------------------------------------------------
 * Config file location
 *
 * The user's config lives where Windows keeps per-user application settings:
 *
 *     %APPDATA%\mshell\init.lua
 *     (C:\Users\<you>\AppData\Roaming\mshell\init.lua)
 *
 * That is the roaming AppData known folder — the standard home for editable
 * per-user config, and it survives reinstalling mshell itself.
 *
 * If it isn't there we fall back to config\init.lua next to the .exe, which
 * keeps an unzipped portable copy (and pre-0.4 installs) working unchanged.
 * With neither present we still report the AppData path: that's where the file
 * belongs, so it's what the log — and Win+Shift+R after you create it — names.
 * --------------------------------------------------------------------------- */

/* Copy src into out (out_len wchars incl. NUL). False if it doesn't fit. */
static bool path_copy(wchar_t *out, size_t out_len, const wchar_t *src) {
    size_t len = wcslen(src);
    if (len >= out_len) return false;
    memcpy(out, src, (len + 1) * sizeof(wchar_t));
    return true;
}

/* %APPDATA%\mshell — the directory, not the file. */
static bool appdata_config_dir(wchar_t *out, size_t out_len) {
    PWSTR roaming = NULL;
    bool  ok = false;

    /* KF_FLAG_CREATE materialises Roaming itself if the profile is new; our
     * own subfolder is created by install.bat, not here (a bare run of
     * mshell.exe should leave nothing behind). */
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData,
                                       KF_FLAG_CREATE, NULL, &roaming))) {
        int n = _snwprintf(out, out_len, L"%ls\\mshell", roaming);
        ok = n > 0 && (size_t)n < out_len;
        CoTaskMemFree(roaming);
    }

    /* Winlogon hands the shell a full user environment, so %APPDATA% is a
     * dependable second source if the shell API ever fails us. */
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

/* config\init.lua beside the .exe — the portable / legacy location. */
static bool portable_config_path(wchar_t *out, size_t out_len) {
    wchar_t exe_dir[MAX_PATH];
    DWORD   n = GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    /* strip the filename, keep the directory */
    wchar_t *last_slash = wcsrchr(exe_dir, L'\\');
    if (!last_slash) return false;
    *last_slash = L'\0';

    int w = _snwprintf(out, out_len, L"%ls\\config\\init.lua", exe_dir);
    return w > 0 && (size_t)w < out_len;
}

/* ===========================================================================
 * Crash-loop detection
 *
 * As the shell, a startup crash is not an inconvenience — it is a black screen.
 * Winlogon's AutoRestartShell relaunches us, we crash again, and the loop has
 * no exit that does not involve Task Manager. The config is the likeliest
 * cause, being arbitrary Lua executed during startup.
 *
 * So: record each launch in HKCU. Three launches inside a minute means the
 * previous two did not survive a minute, and this run comes up in safe mode
 * with the config skipped. A run that DOES survive a minute clears the counter
 * from a timer, so ordinary restarts (an upgrade, a sign-out) never accumulate.
 *
 * HKCU rather than a file: it is the one store guaranteed writable and
 * available this early, before the config path has even been resolved.
 * =========================================================================== */
#define CRASHLOOP_KEY     L"Software\\mshell"
#define CRASHLOOP_WINDOW  60ULL   /* seconds */
#define CRASHLOOP_LIMIT   3       /* launches within the window => safe mode */

static ULONGLONG wall_seconds(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10000000ULL;   /* 100ns ticks -> seconds */
}

static bool crashloop_open(HKEY *out, REGSAM extra) {
    return RegCreateKeyExW(HKEY_CURRENT_USER, CRASHLOOP_KEY, 0, NULL,
                           REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | extra,
                           NULL, out, NULL) == ERROR_SUCCESS;
}

/* Returns true if this run should come up in safe mode. */
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

    /* A clock that moved backwards (or a first-ever run) restarts the window
     * rather than being treated as "a very long time ago". */
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

/* Called from a timer once we have been up long enough to count as healthy. */
void crashloop_mark_healthy(void) {
    HKEY k;
    if (!crashloop_open(&k, KEY_SET_VALUE)) return;
    DWORD zero = 0;
    RegSetValueExW(k, L"LaunchCount", 0, REG_DWORD,
                   (const BYTE *)&zero, sizeof(zero));
    RegCloseKey(k);
    log_msg(LOG_INFO, L"survived %llus — crash-loop counter reset", CRASHLOOP_WINDOW);
}

/* AutoRestartShell=0 turns any exit into a logoff, which makes a crash far
 * worse than it needs to be. We cannot set it (HKLM, needs admin), but saying
 * so in the log turns a mystifying sign-out into an explicable one. */
static void warn_if_no_autorestart(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                      0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return;

    /* Winlogon writes this as REG_DWORD on some installs and REG_SZ ("0"/"1")
     * on others, so read raw bytes and interpret by type rather than assuming
     * either. A byte buffer also keeps the DWORD read from type-punning a
     * wchar_t array, which strict aliasing does not allow. */
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

    /* No user config in AppData — try the folder we were launched from. */
    wchar_t portable[MAX_PATH];
    if (portable_config_path(portable, MAX_PATH) && file_exists(portable)) {
        if (path_copy(out, out_len, portable)) return;
    }

    /* Nothing exists yet: name the canonical location so the log points the
     * user at the file they should create. (config_init falls back to the
     * built-in keymap, so mshell still starts.) */
    if (have_appdata && path_copy(out, out_len, appdata)) return;
    path_copy(out, out_len, L"config\\init.lua");
}

/* ---------------------------------------------------------------------------
 * Monitor enumeration
 *
 * Each physical display becomes an entry in g.monitors[]. rcWork excludes any
 * taskbar/appbars — as the shell there are none, so it equals the full monitor;
 * in --test mode it keeps tiled windows clear of explorer's taskbar.
 * --------------------------------------------------------------------------- */
static BOOL CALLBACK mon_enum_proc(HMONITOR hmon, HDC dc, LPRECT rc, LPARAM lp) {
    (void)dc; (void)rc; (void)lp;
    if (g.monitor_count >= MAX_MONITORS) return TRUE;

    /* MONITORINFOEXW rather than MONITORINFO: szDevice is the stable name a
     * per-monitor override and a hotplug are matched on. */
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

/* ---------------------------------------------------------------------------
 * Apply the config's per-monitor overrides to the displays that are actually
 * attached right now.
 *
 * Matched by DEVICE NAME by preference. An index is what the config can most
 * easily write, but it is also what changes when a display is unplugged — the
 * remaining monitors renumber, and "monitor 1 uses columns" silently starts
 * describing a different screen. A name pattern survives that; the index form
 * stays supported because on a fixed desk it is what people will write.
 *
 * Runs on every enumeration, so a hotplug re-resolves rather than leaving a
 * stale override on whichever display inherited the slot.
 * --------------------------------------------------------------------------- */
void monitors_apply_rules(void) {
    for (int i = 0; i < g.monitor_count; i++) {
        Monitor *m = &g.monitors[i];

        /* Inherit-everything is the baseline; a rule then overrides fields. */
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

            /* Layered like the desktop rules: every match applies, in
             * declaration order, each overwriting only what it names. */
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
        /* Fallback: synthesize a single monitor from the primary work area. */
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

    /* --- re-home every window by DEVICE NAME ---
     *
     * Indices are not stable across a hotplug: unplug the middle display and
     * the ones after it renumber, so a window "on monitor 2" silently becomes a
     * window on a different screen. Each window remembers the name of the
     * display it was on, so:
     *
     *   - if that display is still attached (possibly at a new index), the
     *     window follows it there;
     *   - if it is gone, the window falls back to the primary but KEEPS the
     *     remembered name, so plugging the display back in returns it.
     *
     * That last part is the whole point: a laptop docked and undocked all day
     * otherwise accumulates every window on the built-in panel. */
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
            /* Its display is not here. Park it on the primary WITHOUT
             * forgetting where it belongs. */
            if (mw->monitor != g.primary_monitor) {
                mw->monitor     = g.primary_monitor;
                mw->has_applied = false;
            }
            continue;
        }

        /* No remembered display yet (a window managed before this ran):
         * clamp, then adopt whatever it is on now as its home. */
        if (mw->monitor < 0 || mw->monitor >= g.monitor_count)
            mw->monitor = g.primary_monitor;
        wcsncpy(mw->monitor_device, g.monitors[mw->monitor].device,
                CCHDEVICENAME - 1);
        mw->monitor_device[CCHDEVICENAME - 1] = L'\0';
    }
}

/* ---------------------------------------------------------------------------
 * Effective DPI of a monitor, or 96 when we can't tell.
 *
 * mshell is manifested per-monitor DPI aware (src/mshell.exe.manifest), so
 * every coordinate the tiler sees is now a real physical pixel. That is what
 * makes tiling correct on a scaled display — but it also means anything we draw
 * at a fixed pixel size is no longer scaled for us, so the overlays have to do
 * it themselves. This is where they get the factor.
 *
 * GetDpiForMonitor is resolved at RUNTIME rather than imported. shcore.dll
 * exists from Windows 8.1, and a static import of a missing export stops the
 * executable LOADING — which, for the program registered as the Windows shell,
 * is a session that cannot start. Same reasoning as the manifest.
 * --------------------------------------------------------------------------- */
typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);

UINT monitor_dpi(int mon) {
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

    if (!fn || mon < 0 || mon >= g.monitor_count || !g.monitors[mon].handle)
        return 96;

    UINT dpi_x = 96, dpi_y = 96;
    if (FAILED(fn(g.monitors[mon].handle, 0 /* MDT_EFFECTIVE_DPI */,
                  &dpi_x, &dpi_y)))
        return 96;
    return dpi_x ? dpi_x : 96;
}

/* Which monitor index a window currently sits on (nearest, so off-screen
 * windows still resolve). Falls back to the primary monitor. */
int monitor_of_window(HWND hwnd) {
    HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < g.monitor_count; i++)
        if (g.monitors[i].handle == hmon) return i;
    return g.primary_monitor;
}

/* ---------------------------------------------------------------------------
 * Refresh monitors and the primary work area (kept for single-monitor
 * fallbacks and helper-window sizing).
 * --------------------------------------------------------------------------- */
void update_work_area(void) {
    monitors_update();
    /* The bar takes its strip out of each monitor's work area BEFORE anything
     * reads it, so the tiler lays out beneath it without knowing it exists. */
    bar_reserve_work_area();
    g.work_area = g.monitors[g.primary_monitor].work_area;
}

/* ---------------------------------------------------------------------------
 * Hidden message-only window — hosts the message pump that drives both
 * the low-level keyboard hook and the WinEvent hooks.
 * --------------------------------------------------------------------------- */
LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MSHELL_ACTION: {
        /* Deferred keybind action, posted from the low-level keyboard hook.
         * wParam packs the triggering key (low word) + modifier mask (high
         * word) ONLY so we can log the match here, on the main thread — the
         * hook must never do I/O (a slow hook gets dropped and leaves the
         * swallowed Win key stuck). */
        /* lParam is a sequence number into the hook's pending-action ring, not
         * a KeyBinding pointer: the binding it came from may already have been
         * freed by a config reload queued ahead of us. */
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
            /* A vim-style count repeats motion and sizing only; anything
             * else runs once however many digits preceded it, because "3q"
             * must not be three quits. */
            int reps = (count > 1 && action_is_repeatable(action)) ? count : 1;
            for (int i = 0; i < reps; i++)
                execute_action(action, arg, cmd[0] ? cmd : NULL,
                               args[0] ? args : NULL,
                               cwd[0] ? cwd : NULL);
        }
        return 0;
    }

    case WM_MSHELL_SUBMAP:
        /* Active keymap changed (posted by the hook): show or hide the
         * which-key hint. All GDI stays here on the main thread. */
        whichkey_notify();
        return 0;

    case WM_MSHELL_IPC:
        /* An opaque IpcRequest* from the pipe thread, which is blocked waiting
         * on it. ipc_handle_request runs the command AND signals completion —
         * the request's layout is private to ipc.c, and more importantly the
         * signal must happen on every path out, which is easier to guarantee in
         * one place than at each caller. */
        ipc_handle_request((void *)lp);
        return 0;

    case WM_MSHELL_CONFIG_CHANGED:
        /* The config folder was written to and has gone quiet again (posted by
         * the watcher thread in config.c). wParam is its generation. */
        config_on_file_changed((unsigned)wp);
        return 0;

    case WM_DISPLAYCHANGE:
        update_work_area();
        /* A display may have come or gone — a desktop pinned to one that is no
         * longer there has to stop being pinned to it. */
        desktop_monitors_changed();
        background_update();
        bar_reconfigure();     /* monitor set or geometry changed */
        tile_current();
        return 0;

    case WM_SETTINGCHANGE:
        update_work_area();
        desktop_monitors_changed();
        background_update();
        bar_reconfigure();
        tile_current();
        return 0;

    case WM_DPICHANGED:
        /* A display's scale factor changed. Now that we are per-monitor DPI
         * aware, Windows no longer silently rescales anything for us: the
         * monitor rects we cached are stale in physical pixels, so re-read them
         * and lay everything out again. The overlays pick the new DPI up on
         * their next paint via monitor_dpi(). */
        update_work_area();
        background_update();
        whichkey_hide();      /* its font was built for the old DPI */
        bar_reconfigure();    /* ditto, and its height is DPI-scaled */
        tile_current();
        return 0;

    case WM_MSHELL_UPDATE: {
        /* The update thread owns the string until we take it. */
        wchar_t *msg = (wchar_t *)lp;
        if (msg) { notify_show(msg, NOTIFY_INFO, 15000); free(msg); }
        return 0;
    }

    case WM_MSHELL_CAPTURE_KEY:
        launcher_key((DWORD)wp, (wchar_t)lp);
        return 0;

    case WM_MSHELL_MOUSE:
        /* Mod+drag delta, applied here rather than in the hook: SetWindowPos
         * from the hook thread would run inside the input timeout. */
        mouse_mod_drag_apply((int)(LONG)wp, (int)(LONG)lp);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_FOLLOW_MOUSE) { mouse_poll_focus(); return 0; }
        if (wp == TIMER_ANIM)          { anim_tick();        return 0; }
        /* We have been up long enough to count as a healthy run, so the
         * launches recorded before this one were not a loop. One-shot: kill the
         * timer so this is the only time it fires. */
        if (wp == TIMER_CRASHLOOP_HEALTHY) {
            KillTimer(hwnd, TIMER_CRASHLOOP_HEALTHY);
            crashloop_mark_healthy();
            return 0;
        }
        break;

    case WM_WTSSESSION_CHANGE:
        /* Returning from the secure desktop (lock/unlock, fast-user-switch,
         * console reconnect): any key we were holding was released where our
         * hook couldn't see it, so clear stuck modifiers. This is the safety
         * net for locks the registry doesn't stop (idle timeout, UAC, RDP);
         * Win+L itself is disabled by harden.reg's DisableLockWorkstation. */
        if (wp == WTS_SESSION_UNLOCK || wp == WTS_SESSION_LOGON ||
            wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)
            kb_reset_state();
        return 0;

    case WM_QUERYENDSESSION:
        /* Allow shutdown — Windows sends this before logging off */
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {  /* wp == TRUE → session is actually ending */
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

/* ---------------------------------------------------------------------------
 * Create the invisible window that anchors our message pump.
 *
 * It is a real top-level window (never shown), NOT a message-only
 * (HWND_MESSAGE) window: only top-level windows receive the WM_QUERYENDSESSION
 * / WM_ENDSESSION broadcasts we need to shut down cleanly on logoff. It is kept
 * off the taskbar and Alt+Tab with WS_EX_TOOLWINDOW and, being invisible, is
 * never picked up by our own window management.
 * --------------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------------
 * Command-line flag test — matches whole whitespace-delimited tokens only, so
 * "--shell" can't accidentally satisfy a "-s" probe and a stray path fragment
 * can't look like "-t".
 * --------------------------------------------------------------------------- */
/* Everything AFTER `flag` on the command line, or NULL. Used by --tweaks, whose
 * argument is a verb and an optional group rather than a single token. Returns
 * a pointer into `cmd`, so it lives as long as the command line does. */
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

/* ---------------------------------------------------------------------------
 * WinMain — entry point
 * --------------------------------------------------------------------------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)nCmdShow;

    g.hinst   = hInstance;
    g.running = true;

    /* --- --msg / --query ---
     * Before ANYTHING else, including the single-instance mutex: this
     * invocation is a client talking to the mshell that is already running, not
     * a second shell trying to start. */
    {
        int code = 0;
        if (ipc_client_try(&code)) return code;
    }

    /* Init the keymap lock before ANY config load. The hook thread that shares
     * it is created later (kb_init), but config_init() runs first. */
    kb_locks_init();

    /* --- --tweaks: apply, revert or list the registry tweaks ---
     * Before the log and the mutex, like --check: it is a command-line tool
     * that happens to live in the same binary, and it has to work whether or
     * not mshell is running.
     *
     * This is the replacement for importing a .reg file and hoping. Applying
     * reads each existing value first and files it in a backup key, so
     * reverting restores exactly what was there — including "this value did
     * not exist", which a .reg undo cannot express. See tweaks.c. */
    if (has_flag(lpCmdLine, "--tweaks")) {
        const char *arg = flag_value(lpCmdLine, "--tweaks");
        wchar_t     group[64] = {0};
        char        verb[32]  = {0};

        /* --tweaks <verb> [group] */
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
                      "[input|visual|quiet|all]");
        return 1;
    }

    /* --- --check: validate a config and exit ---
     * Deliberately BEFORE the log file is opened. The log is opened with "w",
     * so a --check run while mshell is your shell would truncate the running
     * instance's log out from under it. Nothing here needs the file anyway:
     * the result goes to the console the user just typed into.
     *
     * Before the mutex too, so it works while mshell is already running — and
     * the point is to check a config BEFORE trusting it, since a config error
     * is atomic and the fallback keymap has six bindings. */
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
                     path_u8, g.root_map ? g.root_map->count : 0,
                     g.keymap_count, g.rule_count, g.desktop_rule_count,
                     g.startup_count);
            console_print(msg);
            return 0;
        }
        snprintf(msg, sizeof msg,
                 "FAILED: %s\n  %s\n  Nothing in this file would take effect: "
                 "a config error is atomic.", path_u8, g.config_error);
        console_print(msg);
        return 1;
    }

    /* --- run mode ---
     * --test    : run alongside explorer as an ordinary process (quitting just
     *             exits; respects the taskbar).
     * --shell   : we ARE the Windows shell (set in the Winlogon Shell key).
     *             Quitting logs the session out.
     * --verbose : raise the log level to DEBUG (INFO by default). */
    g.test_mode     = has_flag(lpCmdLine, "--test") || has_flag(lpCmdLine, "-t");
    bool shell_mode = has_flag(lpCmdLine, "--shell");
    bool verbose    = has_flag(lpCmdLine, "--verbose") || has_flag(lpCmdLine, "-v");

    /* --- open the log ALWAYS, not just under --verbose.
     *
     * As the shell there is no console, no taskbar and no tray, so this file is
     * the only place a failure can be reported. It used to be created only with
     * --verbose, which meant a rejected config (and the six-binding fallback
     * keymap that replaces it) logged its reason into a file that did not
     * exist — presenting as "none of my keybinds work" with nothing to go on.
     *
     * The file stays small at the default level: only ERROR/WARN/INFO are
     * written, while the per-keystroke log_w() tracing is DEBUG and stays off.
     * Config can raise it at runtime via mshell.set_log_level("debug"). --- */
    log_init(L"mshell", verbose ? LOG_DEBUG : LOG_INFO);
    log_msg(LOG_INFO, L"=== mshell v%hs starting ===", MSHELL_VERSION);

    /* Armed as early as the log exists: a crash before this point has nothing
     * to restore anyway (no window is hidden until a desktop switch). */
    SetUnhandledExceptionFilter(mshell_crash_handler);

    /* --- single instance ---
     * A second mshell installs a second WH_KEYBOARD_LL hook, a second set of
     * WinEvent hooks, a second desktop backdrop and a second tiler, and the two
     * then fight over the same windows — each undoing the other's placements.
     * The most likely way to get there is double-clicking mshell.exe while it
     * is already your shell, so the check deliberately covers --test as well:
     * running a "test" instance alongside the real one is exactly the
     * conflict, not an exception to it.
     *
     * Local\ rather than Global\ because the shell is per-session — a second
     * user signed in at the same time runs their own mshell legitimately.
     *
     * The handle is intentionally not closed: it is released when the process
     * exits, which is precisely the lifetime we want it to have. */
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

    /* --- elevation ---
     * mshell runs with whatever token it was given and NEVER elevates itself.
     * It used to relaunch-as-admin on a plain double-click, which made elevated
     * the normal way to run it; that is the wrong default, because an elevated
     * mshell turns init.lua into elevated code.
     *
     * The config lives in %APPDATA%\mshell\, which is writable by the
     * unelevated user, and it is executed with the full Lua standard library
     * (os.execute, io, package.loadlib). So when mshell is elevated, anything
     * running as that user at medium integrity can write init.lua and get code
     * execution at high integrity. Auto-reload made that automatic and silent —
     * it fires about 250 ms after the file is written, with no user action at
     * all — so config.c disables the watcher when we are elevated. Win+Shift+R
     * still reloads, which keeps a deliberate keypress in the loop.
     *
     * The real fix is to stop needing elevation: the only thing it actually
     * buys is a keyboard hook that keeps delivering while an elevated window
     * has focus, plus UIPI-privileged SetWindowPos. Both belong in a small
     * helper that has no config and no scripting. Until then this is
     * mitigation, not a cure. */
    g.elevated = is_elevated();   /* warned about below, once the config path
                                   * is known and can be named */

    if (!g.elevated && shell_mode)
        log_w(L"Shell mode at user level (UAC on) — windows owned by elevated "
              L"processes can't be managed (UIPI).");

    /* --- scheduling priority ---
     * A shell must keep answering its own keybinds even while a fullscreen game
     * pegs the machine. Games routinely raise their priority class and saturate
     * the CPU, which under NORMAL priority starves both the low-level keyboard
     * hook (its events time out and are dropped — the "keybinds dead until the
     * game closes" bug) and this thread, which is where each keybind's action
     * actually runs. ABOVE_NORMAL keeps the action-dispatch thread scheduled
     * under that load; the hook thread is lifted further to TIME_CRITICAL in
     * kb_thread_proc. Neither is high enough to hurt the game, and nothing here
     * spins, so there is no starve-the-system risk (unlike REALTIME). */
    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
        log_w(L"SetPriorityClass(ABOVE_NORMAL) failed: %lu", GetLastError());

    /* --- set defaults (overridden by config; config_apply_defaults is the
     *     authoritative reset, this just covers the pre-config window) --- */
    g.inner_gap        = DEFAULT_INNER_GAP;
    g.outer_gap        = DEFAULT_OUTER_GAP;
    g.smart_gaps       = false;
    g.border_width     = DEFAULT_BORDER_WIDTH;
    g.border_color     = DEFAULT_BORDER_COLOR;
    g.background_color = DEFAULT_BACKGROUND_COLOR;
    g.float_policy     = FLOAT_RULES;
    g.fullscreen_policy = FS_CONTENT;
    g.float_placement  = FLOAT_PLACE_CENTER;
    g.attach_policy    = ATTACH_END;
    g.manage_owned     = false;
    g.float_on_top     = true;   /* a float is an overlay, not a peer */
    g.min_win_w        = DEFAULT_MIN_WIN_W;
    g.min_win_h        = DEFAULT_MIN_WIN_H;
    g.block_system_keys = true;   /* block Alt+Tab, Ctrl+Esc, … by default */
    g.whichkey_enabled = true;
    g.whichkey_delay   = DEFAULT_WHICHKEY_DELAY;
    g.whichkey_bg      = DEFAULT_WHICHKEY_BG;
    g.whichkey_fg      = DEFAULT_WHICHKEY_FG;
    g.whichkey_key_fg  = DEFAULT_WHICHKEY_KEY_FG;
    g.whichkey_border  = DEFAULT_WHICHKEY_BORDER;
    g.current_map  = NULL;
    g.root_map     = NULL;

    /* --- make SetForegroundWindow reliable WITHOUT AttachThreadInput ---
     * Disabling the foreground lock timeout lets window_focus() activate windows
     * from our message thread with a plain SetForegroundWindow. That is what
     * lets it avoid AttachThreadInput, which otherwise leaves the held Win key
     * stuck "down" in the focused app after Win+h/l (the "Win acts held" bug). */
    /* Remember what it was: this is a PERSISTED, system-wide user setting, and
     * leaving it at zero after mshell exits means every application on the
     * machine can steal the foreground at will, forever, with nothing to
     * indicate why. Restored in the shutdown path below. */
    if (!SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0,
                               &g_prev_fg_lock_timeout, 0))
        g_prev_fg_lock_timeout = 0;

    if (!SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                               (PVOID)(UINT_PTR)0, SPIF_SENDCHANGE))
        log_w(L"SPI_SETFOREGROUNDLOCKTIMEOUT failed: %lu — window_focus() will "
              L"fall back to SwitchToThisWindow", GetLastError());

    /* --- work area --- */
    update_work_area();

    /* --- message window (needed BEFORE hooks) --- */
    g.message_window = create_message_window(hInstance);
    if (!g.message_window) {
        log_w(L"FATAL: could not create message window");
        return 1;
    }

    /* Ask for lock/unlock notifications (WM_WTSSESSION_CHANGE) so we can clear
     * stuck modifiers when returning from the secure desktop. */
    WTSRegisterSessionNotification(g.message_window, NOTIFY_FOR_THIS_SESSION);

    /* --- crash-loop guard ---
     * Before the config, because deciding to skip it is the whole point. Only
     * as the real shell: under --test a crash costs you a process, not a
     * session, and repeatedly starting and stopping a test instance is a normal
     * thing to do that must not trip safe mode. */
    /* Focus-follows-mouse is polled rather than hooked — see mouse.c. The timer
     * runs unconditionally and the poll returns immediately when the feature is
     * off, so a reload can turn it on without touching the timer. */
    SetTimer(g.message_window, TIMER_FOLLOW_MOUSE, FOLLOW_MOUSE_MS, NULL);

    if (!g.test_mode) {
        warn_if_no_autorestart();
        g.safe_mode = crashloop_record_launch();
        SetTimer(g.message_window, TIMER_CRASHLOOP_HEALTHY,
                 (UINT)(CRASHLOOP_WINDOW * 1000), NULL);
    }

    /* --- configuration (never fatal: falls back to a built-in keymap) --- */
    resolve_config_path(g.config_path, MAX_PATH);
    log_w(L"config: %ls", g.config_path);

    /* Said out loud, and always — not under --verbose — because it changes what
     * the config file IS. Running elevated makes init.lua administrator-level
     * code living in a directory the unelevated user can write. */
    if (g.elevated)
        log_err(L"running ELEVATED: %ls executes with administrator rights — "
                L"treat it as trusted code, and keep the folder it lives in from "
                L"being writable by anything you don't trust. Auto-reload is "
                L"disabled in this mode; reload with Win+Shift+R.",
                g.config_path);

    config_init();

    /* --- the first desktop ---
     * Nothing exists until now: desktops are created on demand, so this brings
     * the one we land on into being. After the config, because the config picks
     * its name (set_start_desktop) and the rules that describe it. */
    /* Read the remembered per-desktop settings BEFORE the first desktop is
     * created, so desktop_apply_rules can use them straight away. */
    session_load();
    desktop_init();

    /* --- desktop backdrop + focus ring + submap hint (need config colors) --- */
    background_init();
    border_init();
    whichkey_init();
    notify_init();
    launcher_init();
    anim_dim_init();
    bar_init();
    /* Monitors were measured before the config was read, so the work areas do
     * not yet account for a bar the config just enabled. Re-measure, then
     * create the bar windows. */
    update_work_area();
    bar_reconfigure();

    /* --- keyboard hook --- */
    if (!kb_init()) {
        log_w(L"FATAL: kb_init failed");
        return 1;
    }
    log_err(L"config loaded: %d root bindings, %d keymaps, %d desktop rules, "
            L"%d startup programs — starting on desktop '%ls'",
            g.root_map ? g.root_map->count : -1, g.keymap_count,
            g.desktop_rule_count, g.startup_count, desktop_current()->name);

    /* --- control channel (mshell.exe --msg / --query) --- */
    helper_init();   /* optional; absent by default */
    ipc_start();

    /* --- WinEvent hooks --- */
    if (!events_init()) {
        log_w(L"FATAL: events_init failed");
        return 1;
    }

    /* --- manage windows that already exist --- */
    events_sync_urgency();   /* honours whatever the config just set */
    mouse_sync_hook();       /* ditto for Mod+drag's WH_MOUSE_LL hook */
    mouse_sync_pointer();    /* and for the pointer settings it borrows */
    update_check_async();    /* opt-in, at most once a day, notify-only */

    window_manage_existing();

    /* --- initial tile --- */
    tile_current();

    /* --- launch startup programs ---
     * ShellExecute resolves PATH, so a bare "alacritty.exe" works only if the
     * installer put it there. A <= 32 return is the documented failure range;
     * report it, because a startup program that never appears otherwise looks
     * exactly like mshell ignoring mshell.spawn(). */
    for (int i = 0; i < g.startup_count; i++) {
        if (g.startup_commands[i].cmd)
            spawn_command(g.startup_commands[i].cmd,
                          g.startup_commands[i].args,
                          g.startup_commands[i].cwd, L"startup");
    }

    /* --- per-desktop auto-launch for the initial desktop ---
     * Only the current desktop: a spawned window is managed onto the desktop
     * you're on, so launching a background desktop's app would put its window
     * on the wrong one. The rest fire lazily on desktop_switch. */
    desktop_launch_app_if_empty(desktop_current_slot());

    log_w(L"mshell started — mode=%ls, on desktop '%ls', %d managed windows",
          g.test_mode ? L"test" : L"shell",
          desktop_current()->name, g.managed_count);

    /* --- message loop --- */
    MSG msg;
    while (g.running && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* --- clean shutdown --- */
    log_w(L"Shutting down…");

    ipc_stop();
    helper_shutdown();
    events_shutdown();
    kb_shutdown();
    config_shutdown();

    /* Un-hide everything we hid FIRST — every window on a desktop you are not
     * looking at, and monocle's stack. They have no taskbar button and no
     * Alt+Tab entry, so leaving them hidden strands them for good.
     *
     * This also covers logoff: WM_ENDSESSION posts a quit, the message loop
     * above drops out, and we arrive here. */
    window_restore_all_visibility();

    /* restore window decorations so apps look normal again */
    window_restore_all_decorations();

    /* tear down our own helper windows */
    bar_shutdown();
    anim_cancel_all();
    anim_dim_shutdown();
    launcher_shutdown();
    notify_shutdown();
    whichkey_shutdown();
    border_shutdown();
    background_shutdown();

    /* Hand the foreground-lock timeout back. It is a persisted, system-wide
     * setting; leaving it at zero would outlive mshell and let anything on the
     * machine steal focus, with nothing left behind to explain why. */
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                          (PVOID)(UINT_PTR)g_prev_fg_lock_timeout,
                          SPIF_SENDCHANGE);

    /* Same argument, for the same reason: pointer speed, acceleration and the
     * button swap are the machine's, and mshell only borrowed them. */
    mouse_restore_pointer();

    /* destroy message window */
    if (g.message_window) {
        WTSUnRegisterSessionNotification(g.message_window);
        DestroyWindow(g.message_window);
    }

    log_msg(LOG_INFO, L"mshell exited");
    log_shutdown();
    return 0;
}
