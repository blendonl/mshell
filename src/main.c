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

/* Whether this process actually holds the uiAccess token flag. A build made
   with `make UIACCESS=1` only gets it when Windows accepts the manifest, which
   needs the binary signed and under Program Files — so this is the authoritative
   check, not the manifest. */
static bool has_uiaccess(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    DWORD ui = 0, size = sizeof(ui);
    BOOL ok = GetTokenInformation(token, TokenUIAccess, &ui, size, &size);
    CloseHandle(token);
    return ok && ui != 0;
}

static UINT g_prev_fg_lock_timeout = 0;

/* Programs mshell launches inherit its environment. SDL's default is to
   minimize a fullscreen game the moment it loses focus — which fights the
   "nothing minimizes" rule, since there is no taskbar to bring it back from,
   and mshell would then have to restore it after the fact. Turning the hint
   off here stops the minimize happening in the first place, for every SDL1/2/3
   game started from the shell. The variable is a no-op for non-SDL programs. */
static void set_child_env_defaults(void) {
    if (!SetEnvironmentVariableW(L"SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", L"0"))
        log_w(L"could not set SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS for launched "
              L"games: %lu", GetLastError());
}

static LONG WINAPI mshell_crash_handler(EXCEPTION_POINTERS *ep) {
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;

    log_err(L"FATAL: unhandled exception 0x%08lX at %p — restoring hidden "
            L"windows before exiting",
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
            ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress
                                      : NULL);

    window_restore_all_visibility();

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

static bool exe_dir(wchar_t *out, size_t out_len) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)out_len);
    if (n == 0 || n >= out_len) return false;

    wchar_t *last_slash = wcsrchr(out, L'\\');
    if (!last_slash) return false;
    *last_slash = L'\0';
    return true;
}

static bool portable_config_path(wchar_t *out, size_t out_len) {
    wchar_t dir[MAX_PATH];
    if (!exe_dir(dir, MAX_PATH)) return false;

    int w = _snwprintf(out, out_len, L"%ls\\config\\init.lua", dir);
    return w > 0 && (size_t)w < out_len;
}

static void copy_shipped(const wchar_t *from_dir, const wchar_t *to_dir,
                         const wchar_t *name, bool overwrite) {
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    int a = _snwprintf(src, MAX_PATH, L"%ls\\%ls", from_dir, name);
    int b = _snwprintf(dst, MAX_PATH, L"%ls\\%ls", to_dir, name);
    if (a <= 0 || a >= MAX_PATH || b <= 0 || b >= MAX_PATH) return;
    if (!file_exists(src)) return;

    if (CopyFileW(src, dst, !overwrite) && !overwrite)
        log_msg(LOG_INFO, L"config: installed the default %ls", dst);
}

#define DEFAULTS_KEY   L"Software\\mshell"
#define DEFAULTS_VALUE L"DefaultConfigHash"
#define SEED_MAX_BYTES (4u * 1024u * 1024u)

typedef enum {
    SEED_NOTICE_NONE = 0,
    SEED_NOTICE_WELCOME,
    SEED_NOTICE_NEW_DEFAULT,
} SeedNotice;

static SeedNotice s_seed_notice;
static ULONGLONG  s_seed_hash;
static wchar_t    s_seed_new_path[MAX_PATH];

static char *read_small_file(const wchar_t *path, DWORD *len) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;

    char *data = NULL;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart <= SEED_MAX_BYTES) {
        DWORD n = (DWORD)size.QuadPart, got = 0;
        data = malloc(n + 1);
        if (data && ReadFile(f, data, n, &got, NULL) && got == n) {
            *len = n;
        } else {
            free(data);
            data = NULL;
        }
    }
    CloseHandle(f);
    return data;
}

static ULONGLONG fnv1a64(const char *p, DWORD n) {
    ULONGLONG h = 0xcbf29ce484222325ULL;
    for (DWORD i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 0x100000001b3ULL;
    }
    return h ? h : 1;
}

static ULONGLONG defaults_stamp_read(void) {
    ULONGLONG v = 0;
    DWORD sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, DEFAULTS_KEY, DEFAULTS_VALUE,
                     RRF_RT_REG_QWORD, NULL, &v, &sz) != ERROR_SUCCESS)
        return 0;
    return v;
}

static void defaults_stamp_write(ULONGLONG v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, DEFAULTS_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &k,
                        NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, DEFAULTS_VALUE, 0, REG_QWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static void seed_init_lua(const wchar_t *shipped_cfg, const wchar_t *user_dir) {
    wchar_t shipped[MAX_PATH], user[MAX_PATH], fresh[MAX_PATH];
    int a = _snwprintf(shipped, MAX_PATH, L"%ls\\init.lua", shipped_cfg);
    int b = _snwprintf(user,    MAX_PATH, L"%ls\\init.lua", user_dir);
    int c = _snwprintf(fresh,   MAX_PATH, L"%ls\\init.lua.new", user_dir);
    if (a <= 0 || a >= MAX_PATH || b <= 0 || b >= MAX_PATH ||
        c <= 0 || c >= MAX_PATH)
        return;

    DWORD shipped_len = 0;
    char *shipped_data = read_small_file(shipped, &shipped_len);
    if (!shipped_data) return;

    ULONGLONG hash  = fnv1a64(shipped_data, shipped_len);
    ULONGLONG stamp = defaults_stamp_read();

    if (!file_exists(user)) {
        if (CopyFileW(shipped, user, TRUE)) {
            log_msg(LOG_INFO, L"config: installed the default %ls", user);
            s_seed_notice = SEED_NOTICE_WELCOME;
            s_seed_hash   = hash;
        }
    } else if (stamp != hash) {
        DWORD user_len = 0;
        char *user_data = read_small_file(user, &user_len);
        bool  same = user_data && user_len == shipped_len &&
                     memcmp(user_data, shipped_data, user_len) == 0;
        free(user_data);

        if (same) {
            s_seed_notice = stamp ? SEED_NOTICE_NONE : SEED_NOTICE_WELCOME;
            s_seed_hash   = hash;
        } else if (CopyFileW(shipped, fresh, FALSE)) {
            log_msg(LOG_INFO, L"config: the default config changed — the new "
                    L"one is at %ls", fresh);
            s_seed_notice = SEED_NOTICE_NEW_DEFAULT;
            s_seed_hash   = hash;
            wcsncpy(s_seed_new_path, fresh, MAX_PATH - 1);
        }
    }
    free(shipped_data);
}

static void seed_user_config(const wchar_t *user_dir) {
    wchar_t root[MAX_PATH], shipped_cfg[MAX_PATH], shipped_meta[MAX_PATH];
    wchar_t user_meta[MAX_PATH];
    if (!exe_dir(root, MAX_PATH)) return;

    int a = _snwprintf(shipped_cfg,  MAX_PATH, L"%ls\\config", root);
    int b = _snwprintf(shipped_meta, MAX_PATH, L"%ls\\meta", root);
    int c = _snwprintf(user_meta,    MAX_PATH, L"%ls\\meta", user_dir);
    if (a <= 0 || a >= MAX_PATH || b <= 0 || b >= MAX_PATH ||
        c <= 0 || c >= MAX_PATH)
        return;

    CreateDirectoryW(user_dir, NULL);
    seed_init_lua(shipped_cfg, user_dir);
    copy_shipped(shipped_cfg, user_dir, L".luarc.json", false);

    CreateDirectoryW(user_meta, NULL);
    copy_shipped(shipped_meta, user_meta, L"mshell.lua", true);
    copy_shipped(shipped_meta, user_meta, L"types.lua", true);
}

static void show_seed_notice(void) {
    if (!s_seed_hash) return;
    defaults_stamp_write(s_seed_hash);

    switch (s_seed_notice) {
    case SEED_NOTICE_WELCOME:
        notify_show(L"Welcome to mshell. Tap Win for the menu, hold Win for "
                    L"shortcuts: Win+Space launcher, Win+Return terminal, "
                    L"Win+1..9 desktops.", NOTIFY_INFO, 15000);
        break;
    case SEED_NOTICE_NEW_DEFAULT: {
        wchar_t text[MAX_PATH + 128];
        _snwprintf(text, sizeof text / sizeof text[0],
                   L"mshell has a new default config. Yours is unchanged; "
                   L"compare it with %ls", s_seed_new_path);
        text[sizeof text / sizeof text[0] - 1] = L'\0';
        notify_show(text, NOTIFY_INFO, 15000);
        break;
    }
    case SEED_NOTICE_NONE:
        break;
    }
    s_seed_hash = 0;
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

#define TAKEOVER_WAIT_MS  60000

static bool claim_singleton(bool takeover) {
    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\mshell_singleton");
    if (!once) return false;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    if (takeover) {
        log_msg(LOG_INFO, L"takeover: waiting up to %lus for the running mshell "
                L"to exit", (unsigned long)(TAKEOVER_WAIT_MS / 1000));
        DWORD w = WaitForSingleObject(once, TAKEOVER_WAIT_MS);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) return true;
    }

    CloseHandle(once);
    return false;
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
        if (have_appdata) seed_user_config(dir);
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

    SetUnhandledExceptionFilter(mshell_crash_handler);

    if (!claim_singleton(cli_has_flag(lpCmdLine, "--takeover"))) {
        log_err(L"another mshell is already running in this session — "
                L"exiting. Two instances would fight over the keyboard hook "
                L"and the window layout.");
        return 0;
    }

    update_clear_staged_image();

    g.elevated = is_elevated();
    g.uiaccess = has_uiaccess();

    if (g.uiaccess)
        log_err(L"uiAccess GRANTED — mshell can manage windows owned by "
                L"higher-integrity processes directly, and its keyboard hook "
                L"keeps working while one has focus. The mshelld.exe helper is "
                L"not needed in this mode.");
    else if (!g.elevated && shell_mode)
        log_w(L"Shell mode at user level (UAC on) — windows owned by elevated "
              L"processes can't be managed directly (UIPI); the mshelld.exe "
              L"helper covers moving/restoring/closing them. (This is not a "
              L"uiAccess build, or Windows declined the flag — see INSTALL.md.)");

    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS))
        log_w(L"SetPriorityClass(ABOVE_NORMAL) failed: %lu", GetLastError());

    set_child_env_defaults();

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
    settings_sync();
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

    show_seed_notice();

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
