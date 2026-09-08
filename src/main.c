#include "mshell.h"
#include "cli.h"
#include "msgwin.h"
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



static bool init_config(void) {
    config_init();
    return true;
}

static bool init_displays(void) {
    displays_apply_rules(true);
    return true;
}

static bool init_desktops(void) {
    desktop_init();
    return true;
}

static bool init_background(void) {
    background_init();
    return true;
}

static bool init_border(void) {
    border_init();
    return true;
}

static bool init_whichkey(void) {
    whichkey_init();
    return true;
}

static bool init_notify(void) {
    notify_init();
    return true;
}

static bool init_launcher(void) {
    launcher_init();
    return true;
}

static bool init_anim(void) {
    anim_dim_init();
    return true;
}

static void shutdown_anim(void) {
    anim_cancel_all();
    anim_dim_shutdown();
}

static bool init_bar(void) {
    bar_init();
    update_work_area();
    bar_reconfigure();
    return true;
}

static bool init_helper(void) {
    helper_init();
    return true;
}

static bool init_ipc(void) {
    ipc_start();
    return true;
}

typedef struct {
    const char *name;
    bool (*init)(void);
    void (*shutdown)(void);
} Subsystem;

static const Subsystem subsystems[] = {
    { "config",     init_config,     config_shutdown     },
    { "displays",   init_displays,   NULL                },
    { "desktops",   init_desktops,   NULL                },
    { "background", init_background, background_shutdown },
    { "borders",    init_border,     border_shutdown     },
    { "which-key",  init_whichkey,   whichkey_shutdown   },
    { "notify",     init_notify,     notify_shutdown     },
    { "launcher",   init_launcher,   launcher_shutdown   },
    { "animation",  init_anim,       shutdown_anim       },
    { "bar",        init_bar,        bar_shutdown        },
    { "keyboard",   kb_init,         kb_shutdown         },
    { "helper",     init_helper,     helper_shutdown     },
    { "ipc",        init_ipc,        ipc_stop            },
    { "events",     events_init,     events_shutdown     },
};

#define SUBSYSTEM_COUNT ((int)(sizeof subsystems / sizeof subsystems[0]))

static int subsystems_init(void) {
    for (int i = 0; i < SUBSYSTEM_COUNT; i++)
        if (!subsystems[i].init()) return i;
    return SUBSYSTEM_COUNT;
}

static void mshell_teardown(int started) {
    for (int i = started - 1; i >= 0; i--)
        if (subsystems[i].shutdown) subsystems[i].shutdown();

    window_restore_all_visibility();
    window_restore_all_decorations();

    spi_set_broadcast(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                      (PVOID)(UINT_PTR)g_prev_fg_lock_timeout);

    mouse_restore_pointer();

    if (g.message_window) {
        WTSUnRegisterSessionNotification(g.message_window);
        DestroyWindow(g.message_window);
        g.message_window = NULL;
    }
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

    {
        int code = 0;
        if (cli_run_subcommand(lpCmdLine, &code)) return code;
    }

    g.test_mode     = cli_has_flag(lpCmdLine, "--test") ||
                      cli_has_flag(lpCmdLine, "-t");
    bool shell_mode = cli_has_flag(lpCmdLine, "--shell");
    bool verbose    = cli_has_flag(lpCmdLine, "--verbose") ||
                      cli_has_flag(lpCmdLine, "-v");

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
        mshell_teardown(0);
        log_shutdown();
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

    int started = subsystems_init();
    if (started < SUBSYSTEM_COUNT) {
        log_w(L"FATAL: %hs failed to start", subsystems[started].name);
        mshell_teardown(started);
        log_shutdown();
        return 1;
    }

    log_err(L"config loaded: %d root bindings, %d keymaps, %d desktop rules, "
            L"%d startup programs — starting on desktop '%ls'",
            g.active_keymaps->root ? g.active_keymaps->root->count : -1,
            g.active_keymaps->count,
            g.cfg.desktop_rule_count, g.cfg.startup_count,
            desktop_current()->name);

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

    mshell_teardown(SUBSYSTEM_COUNT);

    log_msg(LOG_INFO, L"mshell exited");
    log_shutdown();
    return 0;
}
