#include "mshell.h"

#include <errno.h>

static void config_apply_defaults(void) {
    g.cfg.inner_gap        = DEFAULT_INNER_GAP;
    g.cfg.outer_gap        = DEFAULT_OUTER_GAP;
    g.cfg.smart_gaps       = false;
    g.cfg.smart_borders    = false;
    g.cfg.border_width     = DEFAULT_BORDER_WIDTH;
    g.cfg.border_color     = DEFAULT_BORDER_COLOR;
    g.cfg.border_color_float  = DEFAULT_BORDER_COLOR;
    g.cfg.border_color_urgent = DEFAULT_BORDER_COLOR;
    g.cfg.corner_pref      = 1;
    g.cfg.background_color = DEFAULT_BACKGROUND_COLOR;
    g.cfg.mouse_enabled    = true;
    g.cfg.mouse_follow     = false;
    g.cfg.mouse_warp       = false;
    g.cfg.mouse_mod_drag   = false;
    g.cfg.mouse_speed      = 0;
    g.cfg.mouse_accel      = -1;
    g.cfg.mouse_swap       = -1;
    g.cfg.bar_enabled      = true;
    g.cfg.bar_mode         = DEFAULT_BAR_MODE;
    g.cfg.bar_bottom       = false;
    g.cfg.bar_height       = DEFAULT_BAR_HEIGHT;
    g.cfg.bar_modules      = BAR_MOD_DEFAULT;
    g.cfg.bar_bg           = DEFAULT_BAR_BG;
    g.cfg.bar_fg           = DEFAULT_BAR_FG;
    g.cfg.bar_accent       = DEFAULT_BAR_ACCENT;
    g.cfg.bar_dim          = DEFAULT_BAR_DIM;
    g.cfg.anim_ms          = 0;
    g.cfg.dim_enabled      = false;
    g.cfg.dim_color        = RGB(0x00, 0x00, 0x00);
    g.cfg.dim_alpha        = 90;
    g.cfg.update_check     = false;
    g.cfg.minimize_never   = false;
    g.cfg.urgency_enabled  = false;
    g.cfg.notify_enabled   = true;
    g.cfg.notify_desktop   = false;
    g.cfg.whichkey_enabled = true;
    g.cfg.whichkey_delay   = DEFAULT_WHICHKEY_DELAY;
    g.cfg.whichkey_bg      = DEFAULT_WHICHKEY_BG;
    g.cfg.whichkey_fg      = DEFAULT_WHICHKEY_FG;
    g.cfg.whichkey_key_fg  = DEFAULT_WHICHKEY_KEY_FG;
    g.cfg.whichkey_border  = DEFAULT_WHICHKEY_BORDER;
    g.cfg.whichkey_pos     = WK_POS_BOTTOM;
    g.cfg.whichkey_margin  = DEFAULT_WHICHKEY_MARGIN;
    g.cfg.whichkey_max_w   = 0.0f;
    g.cfg.whichkey_max_h   = 0.0f;
    g.cfg.whichkey_max_rows = DEFAULT_WHICHKEY_MAX_ROWS;
    g.cfg.whichkey_padding = DEFAULT_WHICHKEY_PADDING;
    g.cfg.whichkey_row_gap = DEFAULT_WHICHKEY_ROW_GAP;
    g.cfg.whichkey_col_gap = DEFAULT_WHICHKEY_COL_GAP;
    g.cfg.whichkey_key_gap = DEFAULT_WHICHKEY_KEY_GAP;
    g.cfg.whichkey_hdr_gap = DEFAULT_WHICHKEY_HDR_GAP;
    wcscpy(g.cfg.whichkey_font, DEFAULT_WHICHKEY_FONT);
    g.cfg.whichkey_font_size = DEFAULT_WHICHKEY_FONT_SIZE;
    g.cfg.whichkey_border_w  = DEFAULT_WHICHKEY_BORDER_W;
    g.cfg.whichkey_opacity   = DEFAULT_WHICHKEY_OPACITY;
    g.cfg.whichkey_rounded   = true;
    g.cfg.block_system_keys = true;
    g.cfg.auto_reload      = true;

    g.cfg.float_policy     = FLOAT_RULES;
    g.cfg.hide_policy      = HIDE_CLOAK;
    g.cfg.fullscreen_policy = FS_CONTENT;
    g.cfg.float_placement  = FLOAT_PLACE_CENTER;
    g.cfg.attach_policy    = ATTACH_END;
    g.cfg.manage_owned     = false;
    g.cfg.float_on_top     = true;
    g.cfg.min_win_w        = DEFAULT_MIN_WIN_W;
    g.cfg.min_win_h        = DEFAULT_MIN_WIN_H;

    g.cfg.default_layout       = LAYOUT_TILING;
    g.cfg.default_master_ratio = DEFAULT_MASTER_RATIO;
    g.cfg.default_nmaster      = DEFAULT_NMASTER;
}

static void config_free_owned(KeyMap *keymaps, int keymap_count,
                              StartupCommand *startup, int startup_count) {
    for (int i = 0; i < keymap_count; i++) {
        for (int j = 0; j < keymaps[i].count; j++) {
            free(keymaps[i].bindings[j].command);
            free(keymaps[i].bindings[j].args);
            free(keymaps[i].bindings[j].cwd);
            free(keymaps[i].bindings[j].desc);
        }
        free(keymaps[i].name);
        free(keymaps[i].bindings);
    }
    for (int i = 0; i < startup_count; i++) {
        free(startup[i].cmd);
        free(startup[i].args);
        free(startup[i].cwd);
    }
}

typedef struct {
    KeyMap    keymaps[MAX_KEYMAPS];
    int       keymap_count;
    KeyMap   *leader_map;
    WindowRule rules[MAX_RULES];
    int       rule_count;
    StartupCommand startup_commands[MAX_STARTUP_COMMANDS];
    int       startup_count;
    DesktopRule desktop_rules[MAX_DESKTOP_RULES];
    int       desktop_rule_count;
    MonitorRule monitor_rules[MAX_MONITOR_RULES];
    int       monitor_rule_count;
    LuaHook   lua_hooks[MAX_LUA_HOOKS];
    int       lua_hook_count;
    wchar_t   start_desktop[DESKTOP_NAME_MAX];
    int       inner_gap, outer_gap, border_width;
    bool      smart_gaps, smart_borders;
    COLORREF  border_color, border_color_float, border_color_urgent;
    int       corner_pref;
    COLORREF  background_color;
    bool      block_system_keys;
    bool      auto_reload;
    bool      mouse_enabled, mouse_follow, mouse_warp, mouse_mod_drag;
    bool      bar_enabled, bar_bottom;
    BarMode   bar_mode;
    int       bar_height;
    unsigned  bar_modules;
    COLORREF  bar_bg, bar_fg, bar_accent, bar_dim;
    int       anim_ms;
    bool      dim_enabled;
    COLORREF  dim_color;
    BYTE      dim_alpha;
    bool      update_check;
    bool      minimize_never;
    bool      urgency_enabled;
    bool      notify_enabled, notify_desktop;
    bool      whichkey_enabled;
    int       whichkey_delay;
    COLORREF  whichkey_bg, whichkey_fg, whichkey_key_fg, whichkey_border;
    WhichKeyPos whichkey_pos;
    int       whichkey_margin, whichkey_max_rows;
    float     whichkey_max_w, whichkey_max_h;
    int       whichkey_padding, whichkey_row_gap, whichkey_col_gap;
    int       whichkey_key_gap, whichkey_hdr_gap;
    wchar_t   whichkey_font[LF_FACESIZE];
    int       whichkey_font_size, whichkey_border_w;
    BYTE      whichkey_opacity;
    bool      whichkey_rounded;
    FloatPolicy  float_policy;
    HidePolicy   hide_policy;
    FullscreenMode fullscreen_policy;
    FloatPlacement float_placement;
    AttachPolicy attach_policy;
    bool      manage_owned, float_on_top;
    int       min_win_w, min_win_h;
    Layout    default_layout;
    float     default_master_ratio;
    int       default_nmaster;
} ConfigSnapshot;

static void config_snapshot_save(ConfigSnapshot *s) {
    memcpy(s->keymaps, g.cfg.keymaps, sizeof(g.cfg.keymaps));
    s->keymap_count = g.cfg.keymap_count;
    s->leader_map   = g.cfg.leader_map;
    memcpy(s->rules, g.cfg.rules, sizeof(g.cfg.rules));
    s->rule_count = g.cfg.rule_count;
    memcpy(s->startup_commands, g.cfg.startup_commands, sizeof(g.cfg.startup_commands));
    s->startup_count = g.cfg.startup_count;
    memcpy(s->desktop_rules, g.cfg.desktop_rules, sizeof(g.cfg.desktop_rules));
    s->desktop_rule_count = g.cfg.desktop_rule_count;
    memcpy(s->monitor_rules, g.cfg.monitor_rules, sizeof(g.cfg.monitor_rules));
    s->monitor_rule_count = g.cfg.monitor_rule_count;
    memcpy(s->lua_hooks, g.cfg.lua_hooks, sizeof(g.cfg.lua_hooks));
    s->lua_hook_count = g.cfg.lua_hook_count;
    wcscpy(s->start_desktop, g.cfg.start_desktop);
    s->inner_gap         = g.cfg.inner_gap;
    s->outer_gap         = g.cfg.outer_gap;
    s->smart_gaps        = g.cfg.smart_gaps;
    s->smart_borders     = g.cfg.smart_borders;
    s->border_width      = g.cfg.border_width;
    s->border_color      = g.cfg.border_color;
    s->border_color_float  = g.cfg.border_color_float;
    s->border_color_urgent = g.cfg.border_color_urgent;
    s->corner_pref       = g.cfg.corner_pref;
    s->background_color  = g.cfg.background_color;
    s->block_system_keys = g.cfg.block_system_keys;
    s->auto_reload       = g.cfg.auto_reload;
    s->mouse_enabled     = g.cfg.mouse_enabled;
    s->mouse_follow      = g.cfg.mouse_follow;
    s->mouse_warp        = g.cfg.mouse_warp;
    s->mouse_mod_drag    = g.cfg.mouse_mod_drag;
    s->bar_enabled       = g.cfg.bar_enabled;
    s->bar_mode          = g.cfg.bar_mode;
    s->bar_bottom        = g.cfg.bar_bottom;
    s->bar_height        = g.cfg.bar_height;
    s->bar_modules       = g.cfg.bar_modules;
    s->bar_bg            = g.cfg.bar_bg;
    s->bar_fg            = g.cfg.bar_fg;
    s->bar_accent        = g.cfg.bar_accent;
    s->bar_dim           = g.cfg.bar_dim;
    s->anim_ms           = g.cfg.anim_ms;
    s->dim_enabled       = g.cfg.dim_enabled;
    s->dim_color         = g.cfg.dim_color;
    s->dim_alpha         = g.cfg.dim_alpha;
    s->update_check      = g.cfg.update_check;
    s->minimize_never    = g.cfg.minimize_never;
    s->urgency_enabled   = g.cfg.urgency_enabled;
    s->notify_enabled    = g.cfg.notify_enabled;
    s->notify_desktop    = g.cfg.notify_desktop;
    s->whichkey_enabled  = g.cfg.whichkey_enabled;
    s->whichkey_delay    = g.cfg.whichkey_delay;
    s->whichkey_bg       = g.cfg.whichkey_bg;
    s->whichkey_fg       = g.cfg.whichkey_fg;
    s->whichkey_key_fg   = g.cfg.whichkey_key_fg;
    s->whichkey_border   = g.cfg.whichkey_border;
    s->whichkey_pos      = g.cfg.whichkey_pos;
    s->whichkey_margin   = g.cfg.whichkey_margin;
    s->whichkey_max_w    = g.cfg.whichkey_max_w;
    s->whichkey_max_h    = g.cfg.whichkey_max_h;
    s->whichkey_max_rows = g.cfg.whichkey_max_rows;
    s->whichkey_padding  = g.cfg.whichkey_padding;
    s->whichkey_row_gap  = g.cfg.whichkey_row_gap;
    s->whichkey_col_gap  = g.cfg.whichkey_col_gap;
    s->whichkey_key_gap  = g.cfg.whichkey_key_gap;
    s->whichkey_hdr_gap  = g.cfg.whichkey_hdr_gap;
    wcscpy(s->whichkey_font, g.cfg.whichkey_font);
    s->whichkey_font_size = g.cfg.whichkey_font_size;
    s->whichkey_border_w  = g.cfg.whichkey_border_w;
    s->whichkey_opacity   = g.cfg.whichkey_opacity;
    s->whichkey_rounded   = g.cfg.whichkey_rounded;
    s->float_policy      = g.cfg.float_policy;
    s->hide_policy       = g.cfg.hide_policy;
    s->fullscreen_policy = g.cfg.fullscreen_policy;
    s->float_placement   = g.cfg.float_placement;
    s->attach_policy     = g.cfg.attach_policy;
    s->manage_owned      = g.cfg.manage_owned;
    s->float_on_top      = g.cfg.float_on_top;
    s->min_win_w         = g.cfg.min_win_w;
    s->min_win_h         = g.cfg.min_win_h;
    s->default_layout       = g.cfg.default_layout;
    s->default_master_ratio = g.cfg.default_master_ratio;
    s->default_nmaster      = g.cfg.default_nmaster;
}

static void config_detach(void) {
    memset(g.cfg.keymaps, 0, sizeof(g.cfg.keymaps));
    memset(g.cfg.startup_commands, 0, sizeof(g.cfg.startup_commands));
    g.cfg.keymap_count  = 0;
    g.cfg.rule_count    = 0;
    g.cfg.startup_count = 0;
    g.cfg.desktop_rule_count = 0;
    g.cfg.monitor_rule_count = 0;
    g.cfg.lua_hook_count     = 0;
    g.cfg.start_desktop[0]   = L'\0';
    g.root_map      = NULL;
    g.current_map   = NULL;
    g.cfg.leader_map    = NULL;
    config_apply_defaults();
}

static void config_snapshot_restore(ConfigSnapshot *s) {
    config_free_owned(g.cfg.keymaps, g.cfg.keymap_count,
                      g.cfg.startup_commands, g.cfg.startup_count);

    memcpy(g.cfg.keymaps, s->keymaps, sizeof(g.cfg.keymaps));
    g.cfg.keymap_count = s->keymap_count;
    g.cfg.leader_map   = s->leader_map;
    memcpy(g.cfg.rules, s->rules, sizeof(g.cfg.rules));
    g.cfg.rule_count = s->rule_count;
    memcpy(g.cfg.startup_commands, s->startup_commands, sizeof(g.cfg.startup_commands));
    g.cfg.startup_count = s->startup_count;
    memcpy(g.cfg.desktop_rules, s->desktop_rules, sizeof(g.cfg.desktop_rules));
    g.cfg.desktop_rule_count = s->desktop_rule_count;
    memcpy(g.cfg.monitor_rules, s->monitor_rules, sizeof(g.cfg.monitor_rules));
    g.cfg.monitor_rule_count = s->monitor_rule_count;
    memcpy(g.cfg.lua_hooks, s->lua_hooks, sizeof(g.cfg.lua_hooks));
    g.cfg.lua_hook_count = s->lua_hook_count;
    wcscpy(g.cfg.start_desktop, s->start_desktop);
    g.cfg.inner_gap         = s->inner_gap;
    g.cfg.outer_gap         = s->outer_gap;
    g.cfg.smart_gaps        = s->smart_gaps;
    g.cfg.smart_borders     = s->smart_borders;
    g.cfg.border_width      = s->border_width;
    g.cfg.border_color      = s->border_color;
    g.cfg.border_color_float  = s->border_color_float;
    g.cfg.border_color_urgent = s->border_color_urgent;
    g.cfg.corner_pref       = s->corner_pref;
    g.cfg.background_color  = s->background_color;
    g.cfg.block_system_keys = s->block_system_keys;
    g.cfg.auto_reload       = s->auto_reload;
    g.cfg.mouse_enabled     = s->mouse_enabled;
    g.cfg.mouse_follow      = s->mouse_follow;
    g.cfg.mouse_warp        = s->mouse_warp;
    g.cfg.mouse_mod_drag    = s->mouse_mod_drag;
    g.cfg.bar_enabled       = s->bar_enabled;
    g.cfg.bar_mode          = s->bar_mode;
    g.cfg.bar_bottom        = s->bar_bottom;
    g.cfg.bar_height        = s->bar_height;
    g.cfg.bar_modules       = s->bar_modules;
    g.cfg.bar_bg            = s->bar_bg;
    g.cfg.bar_fg            = s->bar_fg;
    g.cfg.bar_accent        = s->bar_accent;
    g.cfg.bar_dim           = s->bar_dim;
    g.cfg.anim_ms           = s->anim_ms;
    g.cfg.dim_enabled       = s->dim_enabled;
    g.cfg.dim_color         = s->dim_color;
    g.cfg.dim_alpha         = s->dim_alpha;
    g.cfg.update_check      = s->update_check;
    g.cfg.minimize_never    = s->minimize_never;
    g.cfg.urgency_enabled   = s->urgency_enabled;
    g.cfg.notify_enabled    = s->notify_enabled;
    g.cfg.notify_desktop    = s->notify_desktop;
    g.cfg.whichkey_enabled  = s->whichkey_enabled;
    g.cfg.whichkey_delay    = s->whichkey_delay;
    g.cfg.whichkey_bg       = s->whichkey_bg;
    g.cfg.whichkey_fg       = s->whichkey_fg;
    g.cfg.whichkey_key_fg   = s->whichkey_key_fg;
    g.cfg.whichkey_border   = s->whichkey_border;
    g.cfg.whichkey_pos      = s->whichkey_pos;
    g.cfg.whichkey_margin   = s->whichkey_margin;
    g.cfg.whichkey_max_w    = s->whichkey_max_w;
    g.cfg.whichkey_max_h    = s->whichkey_max_h;
    g.cfg.whichkey_max_rows = s->whichkey_max_rows;
    g.cfg.whichkey_padding  = s->whichkey_padding;
    g.cfg.whichkey_row_gap  = s->whichkey_row_gap;
    g.cfg.whichkey_col_gap  = s->whichkey_col_gap;
    g.cfg.whichkey_key_gap  = s->whichkey_key_gap;
    g.cfg.whichkey_hdr_gap  = s->whichkey_hdr_gap;
    wcscpy(g.cfg.whichkey_font, s->whichkey_font);
    g.cfg.whichkey_font_size = s->whichkey_font_size;
    g.cfg.whichkey_border_w  = s->whichkey_border_w;
    g.cfg.whichkey_opacity   = s->whichkey_opacity;
    g.cfg.whichkey_rounded   = s->whichkey_rounded;
    g.cfg.float_policy      = s->float_policy;
    g.cfg.hide_policy       = s->hide_policy;
    g.cfg.fullscreen_policy = s->fullscreen_policy;
    g.cfg.float_placement   = s->float_placement;
    g.cfg.attach_policy     = s->attach_policy;
    g.cfg.manage_owned      = s->manage_owned;
    g.cfg.float_on_top      = s->float_on_top;
    g.cfg.min_win_w         = s->min_win_w;
    g.cfg.min_win_h         = s->min_win_h;
    g.cfg.default_layout       = s->default_layout;
    g.cfg.default_master_ratio = s->default_master_ratio;
    g.cfg.default_nmaster      = s->default_nmaster;

    g.root_map    = g.cfg.keymap_count > 0 ? &g.cfg.keymaps[0] : NULL;
    g.current_map = g.root_map;
}

static void config_snapshot_free(ConfigSnapshot *s) {
    config_free_owned(s->keymaps, s->keymap_count,
                      s->startup_commands, s->startup_count);
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

    kb_lock();

    ConfigSnapshot snap;
    config_snapshot_save(&snap);
    config_detach();

    lua_State *old_L = g.L;
    g.L = L;

    lua_register_api(L);

    g.root_map    = keymap_new(L"root", false);
    g.current_map = g.root_map;

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
        config_snapshot_restore(&snap);
        lua_close(L);
        g.L = old_L;
        kb_unlock();
        return false;
    }

    g.config_error[0] = '\0';
    g.config_gen++;
    config_snapshot_free(&snap);
    if (old_L) lua_close(old_L);
    kb_unlock();
    return true;
}

void config_load_builtin(void) {
    config_free_owned(g.cfg.keymaps, g.cfg.keymap_count,
                      g.cfg.startup_commands, g.cfg.startup_count);
    config_detach();

    g.root_map    = keymap_new(L"root", false);
    g.current_map = g.root_map;
    if (!g.root_map) return;

    keymap_add_binding(g.root_map, MOD_LWIN | MOD_SHIFT, VK_RETURN,
                       ACTION_SPAWN, 0, NULL, L"cmd.exe", NULL, NULL, NULL, true);
    keymap_add_binding(g.root_map, MOD_LWIN | MOD_SHIFT, 'R',
                       ACTION_RELOAD, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(g.root_map, MOD_LWIN | MOD_SHIFT, 'Q',
                       ACTION_QUIT, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(g.root_map, MOD_LWIN, 'J',
                       ACTION_FOCUS_NEXT, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(g.root_map, MOD_LWIN, 'K',
                       ACTION_FOCUS_PREV, 0, NULL, NULL, NULL, NULL, NULL, true);
    keymap_add_binding(g.root_map, MOD_LWIN | MOD_SHIFT, 'C',
                       ACTION_CLOSE, 0, NULL, NULL, NULL, NULL, NULL, true);

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
    config_free_owned(g.cfg.keymaps, g.cfg.keymap_count,
                      g.cfg.startup_commands, g.cfg.startup_count);
    config_detach();
}
