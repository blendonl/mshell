#include "mshell.h"

#include <errno.h>

static void config_apply_defaults(MShellConfig *c, Keymaps *keys) {
    memset(c, 0, sizeof *c);
    memset(keys, 0, sizeof *keys);
    c->keymaps = keys;
    keys->block_system_keys = true;

    c->inner_gap        = DEFAULT_INNER_GAP;
    c->outer_gap        = DEFAULT_OUTER_GAP;
    c->smart_gaps       = false;
    c->smart_borders    = false;
    c->border_width     = DEFAULT_BORDER_WIDTH;
    c->border_accent    = DEFAULT_BORDER_ACCENT;
    c->border_accent_width = DEFAULT_BORDER_ACCENT_W;
    c->border_color     = DEFAULT_BORDER_COLOR;
    c->border_color_float  = DEFAULT_BORDER_COLOR;
    c->border_color_urgent = DEFAULT_BORDER_COLOR;
    c->corner_pref      = 1;
    c->background_color = DEFAULT_BACKGROUND_COLOR;
    c->mouse_enabled    = true;
    c->mouse_follow     = false;
    c->mouse_warp       = false;
    c->mouse_mod_drag   = false;
    c->mouse_speed      = 0;
    c->mouse_accel      = -1;
    c->mouse_swap       = -1;
    c->bar_enabled      = true;
    c->bar_mode         = DEFAULT_BAR_MODE;
    c->bar_bottom       = false;
    c->bar_height       = DEFAULT_BAR_HEIGHT;
    c->bar_modules      = BAR_MOD_DEFAULT;
    c->bar_bg           = DEFAULT_BAR_BG;
    c->bar_fg           = DEFAULT_BAR_FG;
    c->bar_accent       = DEFAULT_BAR_ACCENT;
    c->bar_dim          = DEFAULT_BAR_DIM;
    c->anim_ms          = 0;
    c->dim_enabled      = false;
    c->dim_color        = RGB(0x00, 0x00, 0x00);
    c->dim_alpha        = 90;
    c->update_check     = false;
    c->minimize_never   = false;
    c->urgency_enabled  = false;
    c->notify_enabled   = true;
    c->notify_desktop   = false;
    c->whichkey_enabled = true;
    c->whichkey_delay   = DEFAULT_WHICHKEY_DELAY;
    c->whichkey_bg      = DEFAULT_WHICHKEY_BG;
    c->whichkey_fg      = DEFAULT_WHICHKEY_FG;
    c->whichkey_key_fg  = DEFAULT_WHICHKEY_KEY_FG;
    c->whichkey_border  = DEFAULT_WHICHKEY_BORDER;
    c->whichkey_pos     = WK_POS_BOTTOM;
    c->whichkey_margin  = DEFAULT_WHICHKEY_MARGIN;
    c->whichkey_max_w   = 0.0f;
    c->whichkey_max_h   = 0.0f;
    c->whichkey_max_rows = DEFAULT_WHICHKEY_MAX_ROWS;
    c->whichkey_padding = DEFAULT_WHICHKEY_PADDING;
    c->whichkey_row_gap = DEFAULT_WHICHKEY_ROW_GAP;
    c->whichkey_col_gap = DEFAULT_WHICHKEY_COL_GAP;
    c->whichkey_key_gap = DEFAULT_WHICHKEY_KEY_GAP;
    c->whichkey_hdr_gap = DEFAULT_WHICHKEY_HDR_GAP;
    wcscpy(c->whichkey_font, DEFAULT_WHICHKEY_FONT);
    c->whichkey_font_size = DEFAULT_WHICHKEY_FONT_SIZE;
    c->whichkey_border_w  = DEFAULT_WHICHKEY_BORDER_W;
    c->whichkey_opacity   = DEFAULT_WHICHKEY_OPACITY;
    c->whichkey_rounded   = true;
    c->auto_reload      = true;

    c->float_policy     = FLOAT_RULES;
    c->hide_policy      = HIDE_CLOAK;
    c->fullscreen_policy = FS_CONTENT;
    c->float_placement  = FLOAT_PLACE_CENTER;
    c->attach_policy    = ATTACH_END;
    c->manage_owned     = false;
    c->float_on_top     = true;
    c->min_win_w        = DEFAULT_MIN_WIN_W;
    c->min_win_h        = DEFAULT_MIN_WIN_H;

    c->default_layout       = LAYOUT_TILING;
    c->default_master_ratio = DEFAULT_MASTER_RATIO;
    c->default_nmaster      = DEFAULT_NMASTER;
}

static void keymaps_free(const Keymaps *keys) {
    if (!keys) return;
    for (int i = 0; i < keys->count; i++) {
        const KeyMap *km = &keys->maps[i];
        for (int j = 0; j < km->count; j++) {
            free(km->bindings[j].command);
            free(km->bindings[j].args);
            free(km->bindings[j].cwd);
            free(km->bindings[j].desc);
        }
        free(km->name);
        free(km->bindings);
    }
}

static void config_free_owned(const MShellConfig *c) {
    keymaps_free(c->keymaps);
    for (int i = 0; i < c->startup_count; i++) {
        free(c->startup_commands[i].cmd);
        free(c->startup_commands[i].args);
        free(c->startup_commands[i].cwd);
    }
}

static Keymaps s_keymap_pool[2];

void config_reset(void) {
    Keymaps *spare = (g.cfg.keymaps == &s_keymap_pool[0]) ? &s_keymap_pool[1]
                                                          : &s_keymap_pool[0];
    config_apply_defaults(&g.cfg, spare);
    if (!g.active_keymaps) g.active_keymaps = spare;
}

static void config_publish(void) {
    kb_lock();
    g.active_keymaps = g.cfg.keymaps;
    g.current_map    = g.cfg.keymaps->root;
    g.config_gen++;
    kb_unlock();
}

static bool config_dir_of(const wchar_t *path, wchar_t *out, size_t out_len);

static void config_set_package_path(lua_State *L, const wchar_t *config_path) {
    wchar_t dir[MAX_PATH];
    if (!config_dir_of(config_path, dir, MAX_PATH)) return;

    char u8[MAX_PATH * 3];
    if (WideCharToMultiByte(CP_UTF8, 0, dir, -1, u8, (int)sizeof u8,
                            NULL, NULL) <= 0)
        return;

    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }

    lua_getfield(L, -1, "path");
    const char *existing = lua_tostring(L, -1);

    lua_pushfstring(L, "%s\\?.lua;%s\\?\\init.lua;%s",
                    u8, u8, existing ? existing : "");
    lua_setfield(L, -3, "path");

    lua_pop(L, 2);
}

static int load_config_bytes(lua_State *L, const wchar_t *wpath) {
    FILE *f = _wfopen(wpath, L"rb");
    if (!f) {
        lua_pushfstring(L, "cannot open init.lua: %s", strerror(errno));
        return LUA_ERRFILE;
    }

    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz < 0) {
        int e = errno;
        fclose(f);
        lua_pushfstring(L, "cannot read init.lua: %s", strerror(e));
        return LUA_ERRFILE;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        lua_pushliteral(L, "out of memory reading init.lua");
        return LUA_ERRMEM;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    const char *src = buf;
    size_t      len = rd;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
                    (unsigned char)buf[1] == 0xBB &&
                    (unsigned char)buf[2] == 0xBF) {
        src += 3;
        len -= 3;
    }

    int status = luaL_loadbuffer(L, src, len, "@init.lua");
    free(buf);
    return status;
}

bool config_load(const wchar_t *path) {
    lua_State *L = luaL_newstate();
    if (!L) {
        log_w(L"config: failed to create Lua state");
        return false;
    }
    luaL_openlibs(L);

    config_set_package_path(L, (path && path[0]) ? path : L"config\\init.lua");

    MShellConfig snap = g.cfg;
    config_reset();

    lua_State *old_L = g.L;
    g.L = L;

    lua_register_api(L);

    g.cfg.keymaps->root = keymap_new(L"root", false);

    int status = load_config_bytes(L, (path && path[0]) ? path : L"config\\init.lua");
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, 0);
    }

    if (status != LUA_OK) {
        {
            const char *err = lua_tostring(L, -1);
            snprintf(g.config_error, sizeof g.config_error, "%s",
                     err ? err : "unknown error");
        }

        log_err(L"config: LOAD FAILED: %hs", lua_tostring(L, -1));
        log_err(L"config: the ENTIRE file was rejected — an error anywhere in "
                L"init.lua discards every binding and startup program in it, "
                L"not just the failing line.");

        {
            wchar_t msg[NOTIFY_TEXT_CAP];
            _snwprintf(msg, NOTIFY_TEXT_CAP - 1,
                       L"Config error — previous config kept\n%hs",
                       lua_tostring(L, -1));
            msg[NOTIFY_TEXT_CAP - 1] = L'\0';
            notify_show(msg, NOTIFY_ERROR, 12000);
        }
        config_free_owned(&g.cfg);
        g.cfg = snap;
        lua_close(L);
        g.L = old_L;
        return false;
    }

    g.config_error[0] = '\0';
    config_publish();
    config_free_owned(&snap);
    if (old_L) lua_close(old_L);
    return true;
}

void config_load_builtin(void) {
    MShellConfig snap = g.cfg;
    config_reset();

    KeyMap *root = keymap_new(L"root", false);
    g.cfg.keymaps->root = root;

    keymap_add_binding(root, MOD_LWIN | MOD_SHIFT, VK_RETURN,
                       ACTION_SPAWN, 0, NULL, L"cmd.exe", NULL, NULL, NULL, true);
    keymap_add_binding(root, MOD_LWIN | MOD_SHIFT, 'R',
                       ACTION_RELOAD, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(root, MOD_LWIN | MOD_SHIFT, 'Q',
                       ACTION_QUIT, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(root, MOD_LWIN, 'J',
                       ACTION_FOCUS_NEXT, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(root, MOD_LWIN, 'K',
                       ACTION_FOCUS_PREV, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(root, MOD_LWIN | MOD_SHIFT, 'C',
                       ACTION_CLOSE, 0, NULL, NULL, NULL, NULL, NULL, true);

    config_publish();
    config_free_owned(&snap);
}

#define CONFIG_DEBOUNCE_MS   250
#define CONFIG_DEBOUNCE_MAX  2000
#define CONFIG_SETTLE_MS     100
#define CONFIG_SETTLE_MAX    2000

typedef struct {
    wchar_t  dir[MAX_PATH];
    wchar_t  file[MAX_PATH];
    unsigned generation;
} WatchArgs;

static HANDLE   g_watch_thread;
static HANDLE   g_watch_stop_evt;
static wchar_t  g_watch_dir[MAX_PATH];
static unsigned g_watch_generation;

typedef enum {
    BATCH_STOP,
    BATCH_NONE,
    BATCH_RELEVANT
} BatchResult;

#define WATCH_BUF_SIZE 4096

static BatchResult watch_next_batch(HANDLE dir, OVERLAPPED *ov, BYTE *buf,
                                    DWORD ms) {
    ResetEvent(ov->hEvent);
    if (!ReadDirectoryChangesW(dir, buf, WATCH_BUF_SIZE, FALSE,
                               FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_FILE_NAME  |
                               FILE_NOTIFY_CHANGE_SIZE,
                               NULL, ov, NULL))
        return BATCH_STOP;

    HANDLE waits[2] = { g_watch_stop_evt, ov->hEvent };
    DWORD r = WaitForMultipleObjects(2, waits, FALSE, ms);
    if (r == WAIT_TIMEOUT) return BATCH_NONE;
    if (r != WAIT_OBJECT_0 + 1) {
        CancelIoEx(dir, ov);
        DWORD bytes;
        GetOverlappedResult(dir, ov, &bytes, TRUE);
        return BATCH_STOP;
    }

    DWORD bytes = 0;
    if (!GetOverlappedResult(dir, ov, &bytes, FALSE)) {
        return GetLastError() == ERROR_NOTIFY_ENUM_DIR ? BATCH_RELEVANT
                                                       : BATCH_STOP;
    }
    return BATCH_RELEVANT;
}

static bool config_watch_debounce(HANDLE dir, OVERLAPPED *ov, BYTE *buf) {
    for (unsigned waited = 0; waited < CONFIG_DEBOUNCE_MAX;
         waited += CONFIG_DEBOUNCE_MS) {
        BatchResult b = watch_next_batch(dir, ov, buf, CONFIG_DEBOUNCE_MS);
        if (b == BATCH_STOP) return false;
        if (b == BATCH_NONE) return true;
    }
    return true;
}

static bool config_file_stamp(const wchar_t *path, LONGLONG *size,
                              FILETIME *mtime) {
    HANDLE h = CreateFileW(path, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER li;
    bool ok = GetFileSizeEx(h, &li) && GetFileTime(h, NULL, NULL, mtime);
    CloseHandle(h);
    if (!ok) return false;

    *size = li.QuadPart;
    return true;
}

static bool config_watch_settle(const wchar_t *path) {
    LONGLONG prev_size = 0, size = 0;
    FILETIME prev_mtime = {0}, mtime = {0};
    bool     have_prev = config_file_stamp(path, &prev_size, &prev_mtime);

    for (unsigned waited = 0; waited < CONFIG_SETTLE_MAX;
         waited += CONFIG_SETTLE_MS) {
        if (WaitForSingleObject(g_watch_stop_evt, CONFIG_SETTLE_MS)
            == WAIT_OBJECT_0)
            return false;

        if (!config_file_stamp(path, &size, &mtime)) {
            have_prev = false;
            continue;
        }

        if (have_prev && size == prev_size &&
            CompareFileTime(&mtime, &prev_mtime) == 0)
            return true;

        prev_size  = size;
        prev_mtime = mtime;
        have_prev  = true;
    }
    return true;
}

static DWORD WINAPI config_watch_proc(LPVOID param) {
    WatchArgs *wa = (WatchArgs *)param;

    HANDLE dir = CreateFileW(wa->dir, FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                             FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             NULL);
    if (dir == INVALID_HANDLE_VALUE) {
        log_w(L"config: cannot watch %ls (err %lu) — auto-reload inactive until "
              L"the next manual reload", wa->dir, GetLastError());
        free(wa);
        return 1;
    }

    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        CloseHandle(dir);
        free(wa);
        return 1;
    }

    BYTE buf[WATCH_BUF_SIZE];
    for (;;) {
        BatchResult b = watch_next_batch(dir, &ov, buf, INFINITE);
        if (b == BATCH_STOP) break;
        if (!config_watch_debounce(dir, &ov, buf)) break;
        if (!config_watch_settle(wa->file)) break;

        PostMessageW(g.message_window, WM_MSHELL_CONFIG_CHANGED,
                     (WPARAM)wa->generation, 0);
    }

    CloseHandle(ov.hEvent);
    CloseHandle(dir);
    free(wa);
    return 0;
}

static bool config_dir_of(const wchar_t *path, wchar_t *out, size_t out_len) {
    const wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || slash == path) return false;

    size_t len = (size_t)(slash - path);
    if (len >= out_len) return false;
    memcpy(out, path, len * sizeof(wchar_t));
    out[len] = L'\0';
    return true;
}

void config_watch_stop(void) {
    if (!g_watch_thread) return;

    SetEvent(g_watch_stop_evt);
    DWORD r = WaitForSingleObject(g_watch_thread, 2000);
    if (r == WAIT_OBJECT_0) {
        CloseHandle(g_watch_thread);
        CloseHandle(g_watch_stop_evt);
    } else {
        log_w(L"config: watcher did not exit (%lu) — leaking its handles", r);
    }
    g_watch_thread   = NULL;
    g_watch_stop_evt = NULL;
    g_watch_dir[0]   = L'\0';
}

void config_watch_sync(void) {
    wchar_t dir[MAX_PATH];

    bool want = g.cfg.auto_reload && g.message_window && !g.elevated &&
                config_dir_of(g.config_path, dir, MAX_PATH);

    if (g_watch_thread) {
        bool alive = WaitForSingleObject(g_watch_thread, 0) == WAIT_TIMEOUT;
        if (alive && want && _wcsicmp(dir, g_watch_dir) == 0) return;
        config_watch_stop();
    }
    if (!want) return;

    WatchArgs *wa = (WatchArgs *)malloc(sizeof *wa);
    if (!wa) return;
    memcpy(wa->dir, dir, (wcslen(dir) + 1) * sizeof(wchar_t));
    wcsncpy(wa->file, g.config_path, MAX_PATH - 1);
    wa->file[MAX_PATH - 1] = L'\0';
    wa->generation = ++g_watch_generation;

    g_watch_stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_watch_stop_evt) { free(wa); return; }

    g_watch_thread = CreateThread(NULL, 0, config_watch_proc, wa, 0, NULL);
    if (!g_watch_thread) {
        log_w(L"config: watcher CreateThread failed: %lu", GetLastError());
        CloseHandle(g_watch_stop_evt);
        g_watch_stop_evt = NULL;
        free(wa);
        return;
    }

    memcpy(g_watch_dir, dir, (wcslen(dir) + 1) * sizeof(wchar_t));
    log_w(L"config: auto-reload watching %ls", g_watch_dir);
}

void config_on_file_changed(unsigned generation) {
    if (generation != g_watch_generation || !g.cfg.auto_reload) return;

    log_w(L"config: file changed on disk — reloading");
    config_reload();
}

void config_reload(void) {
    if (g.panicked) {
        g.panicked = false;
        log_err(L"panic mode cleared — mshell is handling keys again");
    }

    resolve_config_path(g.config_path, MAX_PATH);
    log_w(L"Reloading config from %ls", g.config_path);
    bool ok = config_load(g.config_path);

    config_watch_sync();

    if (!ok) {
        log_w(L"Config reload FAILED — previous config kept");
        return;
    }
    displays_apply_rules(true);

    background_update();
    update_work_area();
    bar_reconfigure();
    monitors_apply_rules();
    mouse_sync_hook();
    mouse_sync_pointer();
    events_sync_urgency();
    desktop_reapply();
}

bool config_init(void) {
    g.L = NULL;

    if (g.safe_mode) {
        log_err(L"SAFE MODE: %ls was NOT loaded because mshell restarted "
                L"repeatedly in quick succession. The built-in keymap is in "
                L"use: Win+Shift+Return (cmd), Win+Shift+R (reload), "
                L"Win+Shift+Q (quit), Win+J / Win+K (focus), Win+Shift+C "
                L"(close). Fix the config and press Win+Shift+R, or just "
                L"restart once this run has settled.",
                g.config_path);
        config_load_builtin();
        config_watch_sync();
        return true;
    }

    if (!config_load(g.config_path)) {
        log_err(L"config: user config failed — falling back to the built-in "
                L"keymap. ONLY these work: Win+Shift+Return (cmd), Win+Shift+R "
                L"(reload), Win+Shift+Q (quit), Win+J / Win+K (focus), "
                L"Win+Shift+C (close). No startup programs are launched. Fix "
                L"the error above in %ls and press Win+Shift+R.",
                g.config_path);
        config_load_builtin();
    }
    config_watch_sync();
    return true;
}

void config_shutdown(void) {
    config_watch_stop();
    if (g.L) {
        lua_close(g.L);
        g.L = NULL;
    }
    config_free_owned(&g.cfg);
    config_reset();
    g.active_keymaps = g.cfg.keymaps;
    g.current_map    = NULL;
}
