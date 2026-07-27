/*
 * config.c — Lua-based configuration.
 *
 * The config file (%APPDATA%\mshell\init.lua — see resolve_config_path() in
 * main.c) is loaded at startup and on reload. It calls into the C API
 * (lua_api.c) to populate keybindings, rules, and appearance settings.
 * Between loads, Lua is idle.
 *
 * Reload is ATOMIC: the current configuration is snapshotted before the new
 * one is built, and on any failure the snapshot is restored — so a typo in
 * init.lua can never strand you with an empty keymap. If the very first load
 * fails, a minimal built-in keymap keeps the shell usable.
 */

#include "mshell.h"

/* ===========================================================================
 * Apply built-in appearance / policy defaults (before Lua repopulates them)
 * =========================================================================== */
static void config_apply_defaults(void) {
    g.inner_gap        = DEFAULT_INNER_GAP;
    g.outer_gap        = DEFAULT_OUTER_GAP;
    g.smart_gaps       = false;
    g.border_width     = DEFAULT_BORDER_WIDTH;
    g.border_color     = DEFAULT_BORDER_COLOR;
    g.border_color_float  = DEFAULT_BORDER_COLOR;
    g.border_color_urgent = DEFAULT_BORDER_COLOR;
    g.corner_pref      = 1;   /* DWMWCP_DONOTROUND */
    g.background_color = DEFAULT_BACKGROUND_COLOR;
    g.mouse_enabled    = true;
    g.mouse_follow     = false;
    g.mouse_mod_drag   = false;
    g.bar_enabled      = true;
    g.bar_mode         = DEFAULT_BAR_MODE;
    g.bar_bottom       = false;
    g.bar_height       = DEFAULT_BAR_HEIGHT;
    g.bar_modules      = BAR_MOD_DEFAULT;
    g.bar_bg           = DEFAULT_BAR_BG;
    g.bar_fg           = DEFAULT_BAR_FG;
    g.bar_accent       = DEFAULT_BAR_ACCENT;
    g.bar_dim          = DEFAULT_BAR_DIM;
    g.anim_ms          = 0;       /* instant; motion is opt-in */
    g.dim_enabled      = false;
    g.dim_color        = RGB(0x00, 0x00, 0x00);
    g.dim_alpha        = 90;      /* a suggestion, not a blackout */
    g.update_check     = false;   /* opt-in: it is a network request */
    g.minimize_never   = false;   /* 0.8.0 added minimize FOR a reason */
    g.urgency_enabled  = false;   /* costs a system-wide STATECHANGE hook */
    g.notify_enabled   = true;
    g.notify_desktop   = false;   /* the bar already lists the desktops */
    g.whichkey_enabled = true;
    g.whichkey_delay   = DEFAULT_WHICHKEY_DELAY;
    g.whichkey_bg      = DEFAULT_WHICHKEY_BG;
    g.whichkey_fg      = DEFAULT_WHICHKEY_FG;
    g.whichkey_key_fg  = DEFAULT_WHICHKEY_KEY_FG;
    g.whichkey_border  = DEFAULT_WHICHKEY_BORDER;
    g.whichkey_pos     = WK_POS_BOTTOM;
    g.whichkey_margin  = DEFAULT_WHICHKEY_MARGIN;
    g.whichkey_max_w   = 0.0f;   /* the monitor is the only limit */
    g.whichkey_max_h   = 0.0f;
    g.whichkey_max_rows = DEFAULT_WHICHKEY_MAX_ROWS;
    g.whichkey_padding = DEFAULT_WHICHKEY_PADDING;
    g.whichkey_row_gap = DEFAULT_WHICHKEY_ROW_GAP;
    g.whichkey_col_gap = DEFAULT_WHICHKEY_COL_GAP;
    g.whichkey_key_gap = DEFAULT_WHICHKEY_KEY_GAP;
    g.whichkey_hdr_gap = DEFAULT_WHICHKEY_HDR_GAP;
    wcscpy(g.whichkey_font, DEFAULT_WHICHKEY_FONT);
    g.whichkey_font_size = DEFAULT_WHICHKEY_FONT_SIZE;
    g.whichkey_border_w  = DEFAULT_WHICHKEY_BORDER_W;
    g.whichkey_opacity   = DEFAULT_WHICHKEY_OPACITY;
    g.whichkey_rounded   = true;
    g.block_system_keys = true;
    g.auto_reload      = true;

    g.float_policy     = FLOAT_RULES;
    g.hide_policy      = HIDE_CLOAK;    /* cloak, not SW_HIDE — see HidePolicy */
    g.fullscreen_policy = FS_CONTENT;   /* app fullscreen stays in its tile */
    g.float_placement  = FLOAT_PLACE_CENTER;  /* a float is the window you are
                                               * looking at — put it in front  */
    g.attach_policy    = ATTACH_END;
    g.manage_owned     = false;
    g.float_on_top     = true;   /* a float is an overlay, not a peer */
    g.min_win_w        = DEFAULT_MIN_WIN_W;
    g.min_win_h        = DEFAULT_MIN_WIN_H;

    g.default_layout       = LAYOUT_TILING;
    g.default_master_ratio = DEFAULT_MASTER_RATIO;
    g.default_nmaster      = DEFAULT_NMASTER;
}

/* ===========================================================================
 * Free config-owned heap allocations held in the given arrays.
 * Operates on either the live globals or a saved snapshot.
 * =========================================================================== */
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

/* ===========================================================================
 * Snapshot of all config-owned state, used to roll back a failed reload.
 * The keymap/rule/startup arrays are copied shallowly: pointer ownership moves
 * into the snapshot, so the globals can be rebuilt from scratch without freeing
 * anything the old config still needs.
 * =========================================================================== */
typedef struct {
    KeyMap    keymaps[MAX_KEYMAPS];
    int       keymap_count;
    KeyMap   *leader_map;      /* Win-tap target; a pointer into keymaps[]     */
    WindowRule rules[MAX_RULES];
    int       rule_count;
    StartupCommand startup_commands[MAX_STARTUP_COMMANDS];
    int       startup_count;
    DesktopRule desktop_rules[MAX_DESKTOP_RULES];
    int       desktop_rule_count;
    MonitorRule monitor_rules[MAX_MONITOR_RULES];
    int       monitor_rule_count;
    /* mshell.on() handlers. The refs belong to the old lua_State, which a
     * failed load leaves open — so restoring them restores working handlers. */
    LuaHook   lua_hooks[MAX_LUA_HOOKS];
    int       lua_hook_count;
    wchar_t   start_desktop[DESKTOP_NAME_MAX];
    int       inner_gap, outer_gap, border_width;
    bool      smart_gaps;
    COLORREF  border_color, border_color_float, border_color_urgent;
    int       corner_pref;
    COLORREF  background_color;
    bool      block_system_keys;
    bool      auto_reload;
    bool      mouse_enabled, mouse_follow, mouse_mod_drag;
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
    memcpy(s->keymaps, g.keymaps, sizeof(g.keymaps));
    s->keymap_count = g.keymap_count;
    s->leader_map   = g.leader_map;
    memcpy(s->rules, g.rules, sizeof(g.rules));
    s->rule_count = g.rule_count;
    memcpy(s->startup_commands, g.startup_commands, sizeof(g.startup_commands));
    s->startup_count = g.startup_count;
    memcpy(s->desktop_rules, g.desktop_rules, sizeof(g.desktop_rules));
    s->desktop_rule_count = g.desktop_rule_count;
    memcpy(s->monitor_rules, g.monitor_rules, sizeof(g.monitor_rules));
    s->monitor_rule_count = g.monitor_rule_count;
    memcpy(s->lua_hooks, g.lua_hooks, sizeof(g.lua_hooks));
    s->lua_hook_count = g.lua_hook_count;
    wcscpy(s->start_desktop, g.start_desktop);
    s->inner_gap         = g.inner_gap;
    s->outer_gap         = g.outer_gap;
    s->smart_gaps        = g.smart_gaps;
    s->border_width      = g.border_width;
    s->border_color      = g.border_color;
    s->border_color_float  = g.border_color_float;
    s->border_color_urgent = g.border_color_urgent;
    s->corner_pref       = g.corner_pref;
    s->background_color  = g.background_color;
    s->block_system_keys = g.block_system_keys;
    s->auto_reload       = g.auto_reload;
    s->mouse_enabled     = g.mouse_enabled;
    s->mouse_follow      = g.mouse_follow;
    s->mouse_mod_drag    = g.mouse_mod_drag;
    s->bar_enabled       = g.bar_enabled;
    s->bar_mode          = g.bar_mode;
    s->bar_bottom        = g.bar_bottom;
    s->bar_height        = g.bar_height;
    s->bar_modules       = g.bar_modules;
    s->bar_bg            = g.bar_bg;
    s->bar_fg            = g.bar_fg;
    s->bar_accent        = g.bar_accent;
    s->bar_dim           = g.bar_dim;
    s->anim_ms           = g.anim_ms;
    s->dim_enabled       = g.dim_enabled;
    s->dim_color         = g.dim_color;
    s->dim_alpha         = g.dim_alpha;
    s->update_check      = g.update_check;
    s->minimize_never    = g.minimize_never;
    s->urgency_enabled   = g.urgency_enabled;
    s->notify_enabled    = g.notify_enabled;
    s->notify_desktop    = g.notify_desktop;
    s->whichkey_enabled  = g.whichkey_enabled;
    s->whichkey_delay    = g.whichkey_delay;
    s->whichkey_bg       = g.whichkey_bg;
    s->whichkey_fg       = g.whichkey_fg;
    s->whichkey_key_fg   = g.whichkey_key_fg;
    s->whichkey_border   = g.whichkey_border;
    s->whichkey_pos      = g.whichkey_pos;
    s->whichkey_margin   = g.whichkey_margin;
    s->whichkey_max_w    = g.whichkey_max_w;
    s->whichkey_max_h    = g.whichkey_max_h;
    s->whichkey_max_rows = g.whichkey_max_rows;
    s->whichkey_padding  = g.whichkey_padding;
    s->whichkey_row_gap  = g.whichkey_row_gap;
    s->whichkey_col_gap  = g.whichkey_col_gap;
    s->whichkey_key_gap  = g.whichkey_key_gap;
    s->whichkey_hdr_gap  = g.whichkey_hdr_gap;
    wcscpy(s->whichkey_font, g.whichkey_font);
    s->whichkey_font_size = g.whichkey_font_size;
    s->whichkey_border_w  = g.whichkey_border_w;
    s->whichkey_opacity   = g.whichkey_opacity;
    s->whichkey_rounded   = g.whichkey_rounded;
    s->float_policy      = g.float_policy;
    s->hide_policy       = g.hide_policy;
    s->fullscreen_policy = g.fullscreen_policy;
    s->float_placement   = g.float_placement;
    s->attach_policy     = g.attach_policy;
    s->manage_owned      = g.manage_owned;
    s->float_on_top      = g.float_on_top;
    s->min_win_w         = g.min_win_w;
    s->min_win_h         = g.min_win_h;
    s->default_layout       = g.default_layout;
    s->default_master_ratio = g.default_master_ratio;
    s->default_nmaster      = g.default_nmaster;
}

/* Detach the globals from the snapshot's now-owned allocations and reset to a
 * clean slate so the new config can be built fresh. */
static void config_detach(void) {
    /* Zero the pointer-bearing slots so no freed allocation can linger in an
     * unused slot after a config that shrinks the keymap/startup set. The
     * snapshot holds the only surviving references to the old allocations. */
    memset(g.keymaps, 0, sizeof(g.keymaps));
    memset(g.startup_commands, 0, sizeof(g.startup_commands));
    g.keymap_count  = 0;
    g.rule_count    = 0;
    g.startup_count = 0;
    /* Desktop rules hold no heap of their own, so resetting the count is the
     * whole teardown — the live desktops they describe are runtime state and
     * deliberately survive a reload (desktop_reapply re-resolves them). */
    g.desktop_rule_count = 0;
    g.monitor_rule_count = 0;
    g.lua_hook_count     = 0;   /* refs die with the lua_State */
    g.start_desktop[0]   = L'\0';
    g.root_map      = NULL;
    g.current_map   = NULL;
    g.leader_map    = NULL;
    config_apply_defaults();
}

/* Free whatever partial new config was built into the globals, then restore
 * the snapshot verbatim. */
static void config_snapshot_restore(ConfigSnapshot *s) {
    config_free_owned(g.keymaps, g.keymap_count,
                      g.startup_commands, g.startup_count);

    memcpy(g.keymaps, s->keymaps, sizeof(g.keymaps));
    g.keymap_count = s->keymap_count;
    g.leader_map   = s->leader_map;   /* keymaps[] addresses are stable */
    memcpy(g.rules, s->rules, sizeof(g.rules));
    g.rule_count = s->rule_count;
    memcpy(g.startup_commands, s->startup_commands, sizeof(g.startup_commands));
    g.startup_count = s->startup_count;
    memcpy(g.desktop_rules, s->desktop_rules, sizeof(g.desktop_rules));
    g.desktop_rule_count = s->desktop_rule_count;
    memcpy(g.monitor_rules, s->monitor_rules, sizeof(g.monitor_rules));
    g.monitor_rule_count = s->monitor_rule_count;
    memcpy(g.lua_hooks, s->lua_hooks, sizeof(g.lua_hooks));
    g.lua_hook_count = s->lua_hook_count;
    wcscpy(g.start_desktop, s->start_desktop);
    g.inner_gap         = s->inner_gap;
    g.outer_gap         = s->outer_gap;
    g.smart_gaps        = s->smart_gaps;
    g.border_width      = s->border_width;
    g.border_color      = s->border_color;
    g.border_color_float  = s->border_color_float;
    g.border_color_urgent = s->border_color_urgent;
    g.corner_pref       = s->corner_pref;
    g.background_color  = s->background_color;
    g.block_system_keys = s->block_system_keys;
    g.auto_reload       = s->auto_reload;
    g.mouse_enabled     = s->mouse_enabled;
    g.mouse_follow      = s->mouse_follow;
    g.mouse_mod_drag    = s->mouse_mod_drag;
    g.bar_enabled       = s->bar_enabled;
    g.bar_mode          = s->bar_mode;
    g.bar_bottom        = s->bar_bottom;
    g.bar_height        = s->bar_height;
    g.bar_modules       = s->bar_modules;
    g.bar_bg            = s->bar_bg;
    g.bar_fg            = s->bar_fg;
    g.bar_accent        = s->bar_accent;
    g.bar_dim           = s->bar_dim;
    g.anim_ms           = s->anim_ms;
    g.dim_enabled       = s->dim_enabled;
    g.dim_color         = s->dim_color;
    g.dim_alpha         = s->dim_alpha;
    g.update_check      = s->update_check;
    g.minimize_never    = s->minimize_never;
    g.urgency_enabled   = s->urgency_enabled;
    g.notify_enabled    = s->notify_enabled;
    g.notify_desktop    = s->notify_desktop;
    g.whichkey_enabled  = s->whichkey_enabled;
    g.whichkey_delay    = s->whichkey_delay;
    g.whichkey_bg       = s->whichkey_bg;
    g.whichkey_fg       = s->whichkey_fg;
    g.whichkey_key_fg   = s->whichkey_key_fg;
    g.whichkey_border   = s->whichkey_border;
    g.whichkey_pos      = s->whichkey_pos;
    g.whichkey_margin   = s->whichkey_margin;
    g.whichkey_max_w    = s->whichkey_max_w;
    g.whichkey_max_h    = s->whichkey_max_h;
    g.whichkey_max_rows = s->whichkey_max_rows;
    g.whichkey_padding  = s->whichkey_padding;
    g.whichkey_row_gap  = s->whichkey_row_gap;
    g.whichkey_col_gap  = s->whichkey_col_gap;
    g.whichkey_key_gap  = s->whichkey_key_gap;
    g.whichkey_hdr_gap  = s->whichkey_hdr_gap;
    wcscpy(g.whichkey_font, s->whichkey_font);
    g.whichkey_font_size = s->whichkey_font_size;
    g.whichkey_border_w  = s->whichkey_border_w;
    g.whichkey_opacity   = s->whichkey_opacity;
    g.whichkey_rounded   = s->whichkey_rounded;
    g.float_policy      = s->float_policy;
    g.hide_policy       = s->hide_policy;
    g.fullscreen_policy = s->fullscreen_policy;
    g.float_placement   = s->float_placement;
    g.attach_policy     = s->attach_policy;
    g.manage_owned      = s->manage_owned;
    g.float_on_top      = s->float_on_top;
    g.min_win_w         = s->min_win_w;
    g.min_win_h         = s->min_win_h;
    g.default_layout       = s->default_layout;
    g.default_master_ratio = s->default_master_ratio;
    g.default_nmaster      = s->default_nmaster;

    g.root_map    = g.keymap_count > 0 ? &g.keymaps[0] : NULL;
    g.current_map = g.root_map;
}

/* Free a snapshot's owned allocations (the previous config, on success). */
static void config_snapshot_free(ConfigSnapshot *s) {
    config_free_owned(s->keymaps, s->keymap_count,
                      s->startup_commands, s->startup_count);
}

/* ===========================================================================
 * Load a Lua chunk from a wide (Unicode) path.
 *
 * We read the bytes ourselves via _wfopen rather than luaL_loadfile, because
 * luaL_loadfile funnels through the CRT's ANSI fopen and can't open paths with
 * non-ASCII characters. Reading bytes + luaL_loadbuffer also keeps the source
 * UTF-8 clean (string literals reach our API as UTF-8, which lua_api converts
 * with CP_UTF8).
 * =========================================================================== */
/* ===========================================================================
 * Point package.path at the config's own folder.
 *
 * Lua's default search path covers directories relative to the EXECUTABLE and
 * the current working directory — never %APPDATA%\mshell\. And a shell launched
 * by Winlogon has a working directory of C:\Windows\system32, so `require` from
 * init.lua resolved against a folder the user has never heard of and failed.
 * Splitting a config across files is the obvious thing to reach for once it
 * grows, so make it work: the config's folder goes to the FRONT of the path,
 * ahead of the defaults.
 * =========================================================================== */
static bool config_dir_of(const wchar_t *path, wchar_t *out, size_t out_len);

static void config_set_package_path(lua_State *L, const wchar_t *config_path) {
    wchar_t dir[MAX_PATH];
    if (!config_dir_of(config_path, dir, MAX_PATH)) return;

    /* Wide -> UTF-8 for Lua. */
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

    lua_pop(L, 2);   /* old path string + package table */
}

static int load_config_bytes(lua_State *L, const wchar_t *wpath) {
    FILE *f = _wfopen(wpath, L"rb");
    if (!f) return LUA_ERRFILE;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return LUA_ERRFILE; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return LUA_ERRFILE; }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return LUA_ERRMEM; }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    /* Skip a UTF-8 BOM if present — Lua 5.4 does not tolerate one. */
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

/* ===========================================================================
 * Load (or reload) the config file — atomically.
 * =========================================================================== */
bool config_load(const wchar_t *path) {
    lua_State *L = luaL_newstate();
    if (!L) {
        log_w(L"config: failed to create Lua state");
        return false;
    }
    luaL_openlibs(L);

    /* Let init.lua require() modules kept beside it. Must come after
     * luaL_openlibs, which is what creates the package table. */
    config_set_package_path(L, (path && path[0]) ? path : L"config\\init.lua");

    /* The keyboard hook runs on its own thread and reads the keymaps; lock
     * while we tear them down and rebuild so a keystroke mid-reload can never
     * touch half-freed bindings. Held across the (fast) Lua run — a reload is
     * rare and user-initiated, so briefly serialising the hook is fine. */
    kb_lock();

    /* Snapshot the current config, then start the new one from a clean slate.
     * The snapshot owns the old allocations until we commit or roll back. */
    ConfigSnapshot snap;
    config_snapshot_save(&snap);
    config_detach();

    lua_State *old_L = g.L;
    g.L = L;

    lua_register_api(L);

    /* Root keymap is always keymaps[0]. */
    g.root_map    = keymap_new(L"root", false);
    g.current_map = g.root_map;

    int status = load_config_bytes(L, (path && path[0]) ? path : L"config\\init.lua");
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, 0, 0);
    }

    if (status != LUA_OK) {
        /* Keep the message: --check prints it to the console, where the person
         * who just ran it is actually looking. */
        {
            const char *err = lua_tostring(L, -1);
            snprintf(g.config_error, sizeof g.config_error, "%s",
                     err ? err : "unknown error");
        }

        /* log_err, not log_w: this is the one message that explains why none of
         * the user's keybinds exist, and it has to survive a non-verbose run. */
        log_err(L"config: LOAD FAILED: %hs", lua_tostring(L, -1));
        log_err(L"config: the ENTIRE file was rejected — an error anywhere in "
                L"init.lua discards every binding and startup program in it, "
                L"not just the failing line.");

        /* Say it on screen too. The rollback below is exactly what makes this
         * necessary: keeping the previous config running is the right
         * behaviour and also completely silent, so a broken edit is otherwise
         * indistinguishable from one that worked. */
        {
            wchar_t msg[NOTIFY_TEXT_CAP];
            _snwprintf(msg, NOTIFY_TEXT_CAP - 1,
                       L"Config error — previous config kept\n%hs",
                       lua_tostring(L, -1));
            msg[NOTIFY_TEXT_CAP - 1] = L'\0';
            notify_show(msg, NOTIFY_ERROR, 12000);
        }
        /* Roll back: discard the partial new config, restore the snapshot. */
        config_snapshot_restore(&snap);
        lua_close(L);
        g.L = old_L;
        kb_unlock();
        return false;
    }

    /* Success: the new config is live (mshell.set_leader, if the config called
     * it, already pointed g.leader_map into the new keymaps). Free the old
     * config and its Lua VM.
     *
     * The generation bump has to happen while the lock is still held and before
     * the old VM is closed: any Lua binding queued under the old config is
     * identified by its generation, and closing the state invalidates every
     * registry ref that referred to it. */
    g.config_error[0] = '\0';
    g.config_gen++;
    config_snapshot_free(&snap);
    if (old_L) lua_close(old_L);
    kb_unlock();
    return true;
}

/* ===========================================================================
 * Built-in fallback keymap — used when the user's config can't be loaded, so
 * a broken init.lua never leaves you without a working shell.
 * =========================================================================== */
void config_load_builtin(void) {
    config_free_owned(g.keymaps, g.keymap_count,
                      g.startup_commands, g.startup_count);
    config_detach();

    g.root_map    = keymap_new(L"root", false);
    g.current_map = g.root_map;
    if (!g.root_map) return;

    /* Bare essentials: open a terminal, reload, quit, cycle focus, close. */
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

/* ===========================================================================
 * Auto-reload — watch the config's folder and reload when it is saved.
 *
 * We watch the DIRECTORY, not the file. Most editors save by writing a temp
 * file and renaming it over the target, which leaves any file-handle-based
 * watch pointing at a file that no longer exists. Watching the directory also
 * picks up extra Lua modules kept beside init.lua and require()d from it — the
 * whole folder is mshell's config. We never write there, so a reload can't
 * trigger itself.
 *
 * The wait lives on its own thread and only ever PostMessages: the reload has
 * to run on the main thread (config_load takes kb_lock and re-tiles).
 * =========================================================================== */
#define CONFIG_DEBOUNCE_MS   250    /* quiet period before acting on a burst  */
#define CONFIG_DEBOUNCE_MAX  2000   /* ...but never stall longer than this    */

typedef struct {
    wchar_t  dir[MAX_PATH];
    unsigned generation;
} WatchArgs;

static HANDLE   g_watch_thread;
static HANDLE   g_watch_stop_evt;
static wchar_t  g_watch_dir[MAX_PATH];  /* what the live thread is watching   */
static unsigned g_watch_generation;     /* main thread only; bumped per start */

/* Wait out a write burst. True → reload; false → the thread should stop.
 * Either way the change handle is left ARMED, so the caller's next wait is
 * still live (an un-re-armed handle silently never signals again). */
static bool config_watch_debounce(HANDLE *waits, HANDLE chg) {
    for (unsigned waited = 0; waited < CONFIG_DEBOUNCE_MAX;
         waited += CONFIG_DEBOUNCE_MS) {
        if (!FindNextChangeNotification(chg)) return false;   /* re-arm */

        DWORD r = WaitForMultipleObjects(2, waits, FALSE, CONFIG_DEBOUNCE_MS);
        if (r == WAIT_TIMEOUT)      return true;   /* quiet, still armed */
        if (r != WAIT_OBJECT_0 + 1) return false;  /* stop, or the wait broke */
        /* another change — keep waiting */
    }
    /* Something is writing continuously; reload rather than starve, but re-arm
     * first since we just consumed a notification. */
    return FindNextChangeNotification(chg) != 0;
}

static DWORD WINAPI config_watch_proc(LPVOID param) {
    WatchArgs *wa = (WatchArgs *)param;

    HANDLE chg = FindFirstChangeNotificationW(
                     wa->dir, FALSE,
                     FILE_NOTIFY_CHANGE_LAST_WRITE |
                     FILE_NOTIFY_CHANGE_FILE_NAME  |
                     FILE_NOTIFY_CHANGE_SIZE);
    if (chg == INVALID_HANDLE_VALUE) {
        /* Usually the folder doesn't exist yet. Not fatal — Win+Shift+R still
         * reloads, and it re-syncs the watcher once the folder is there. */
        log_w(L"config: cannot watch %ls (err %lu) — auto-reload inactive until "
              L"the next manual reload", wa->dir, GetLastError());
        free(wa);
        return 1;
    }

    HANDLE waits[2] = { g_watch_stop_evt, chg };
    for (;;) {
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            break;                    /* stop requested (or the wait failed) */
        if (!config_watch_debounce(waits, chg)) break;

        PostMessageW(g.message_window, WM_MSHELL_CONFIG_CHANGED,
                     (WPARAM)wa->generation, 0);
    }

    FindCloseChangeNotification(chg);
    free(wa);
    return 0;
}

/* Directory part of `path`, without the trailing separator. */
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
        /* Shouldn't happen — the longest wait inside is one debounce tick.
         * Leak both handles rather than close them under a live thread. */
        log_w(L"config: watcher did not exit (%lu) — leaking its handles", r);
    }
    g_watch_thread   = NULL;
    g_watch_stop_evt = NULL;
    g_watch_dir[0]   = L'\0';
}

/* Start / stop / retarget the watcher to match the current config. Cheap to
 * call after every load: when nothing relevant changed it returns immediately
 * instead of cycling the thread. */
void config_watch_sync(void) {
    wchar_t dir[MAX_PATH];

    /* Never watch while elevated. The config directory is writable by the
     * unelevated user and the config is executed with the full Lua standard
     * library, so auto-reload in an elevated mshell is a silent path from
     * "anything running as this user can write a file" to "code runs with
     * administrator rights", with no user action and a ~250 ms delay.
     * Win+Shift+R still works, which keeps a deliberate keypress in the loop. */
    bool want = g.auto_reload && g.message_window && !g.elevated &&
                config_dir_of(g.config_path, dir, MAX_PATH);

    if (g_watch_thread) {
        /* "Alive" matters: a watcher whose FindFirstChangeNotification failed
         * (folder not created yet) has already exited, and must be restarted
         * rather than counted as watching. */
        bool alive = WaitForSingleObject(g_watch_thread, 0) == WAIT_TIMEOUT;
        if (alive && want && _wcsicmp(dir, g_watch_dir) == 0) return;
        config_watch_stop();
    }
    if (!want) return;

    WatchArgs *wa = (WatchArgs *)malloc(sizeof *wa);
    if (!wa) return;
    memcpy(wa->dir, dir, (wcslen(dir) + 1) * sizeof(wchar_t));
    wa->generation = ++g_watch_generation;

    g_watch_stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual reset */
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

/* WM_MSHELL_CONFIG_CHANGED handler — main thread. */
void config_on_file_changed(unsigned generation) {
    /* A watcher we have since replaced may have posted just before it stopped;
     * its generation no longer matches the live one, so drop it. */
    if (generation != g_watch_generation || !g.auto_reload) return;

    log_w(L"config: file changed on disk — reloading");
    config_reload();
}

/* ===========================================================================
 * Reload config (bound to a key, and by the watcher above). On failure the old
 * config is kept intact.
 * =========================================================================== */
void config_reload(void) {
    /* A reload is the way out of panic mode, and it is cleared HERE rather than
     * in the reload action because panic mode is precisely the state in which
     * no keybinding fires — the hook is passing everything through. What still
     * reaches us is `mshell.exe --msg reload` and saving init.lua, and both
     * arrive through this function. Cleared before the load so that a config
     * with an error still gets you your keys back. */
    if (g.panicked) {
        g.panicked = false;
        log_err(L"panic mode cleared — mshell is handling keys again");
    }

    /* Re-resolve first: if the user has just created %APPDATA%\mshell\init.lua
     * on a session that started from the exe-dir fallback (or from no config at
     * all), Win+Shift+R should pick it up rather than needing a sign-out. */
    resolve_config_path(g.config_path, MAX_PATH);
    log_w(L"Reloading config from %ls", g.config_path);
    bool ok = config_load(g.config_path);

    /* Re-sync the watcher either way: the path may have moved (a config just
     * created in AppData), and after a failed load it is the rolled-back
     * config's auto_reload setting that should be in force. */
    config_watch_sync();

    if (!ok) {
        log_w(L"Config reload FAILED — previous config kept");
        return;
    }
    /* Re-assert visibility/layout for the (preserved) windows and repaint the
     * backdrop in case colors changed. */
    background_update();
    /* The bar's height, position and colours may all have changed, and its
     * strip comes out of the work area the tiler is about to use — so
     * re-measure the work areas first, then rebuild it, then re-tile. */
    update_work_area();
    bar_reconfigure();
    monitors_apply_rules();  /* the config's per-display overrides changed */
    mouse_sync_hook();       /* Mod+drag may have been turned on or off */
    events_sync_urgency();   /* the setting may have flipped either way */
    desktop_reapply();
}

/* ===========================================================================
 * One-time config init at startup. Falls back to a built-in keymap rather than
 * failing to launch, so a bad config can be fixed from inside a running shell.
 * =========================================================================== */
bool config_init(void) {
    g.L = NULL;

    /* Safe mode: we restarted several times in quick succession, which means
     * something here is killing us before anyone can type. The config is by far
     * the likeliest culprit — it is arbitrary Lua that runs at startup — and as
     * the shell there is no other surface to fix it from, so skip it entirely
     * this once and hand over the built-in keymap. */
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
    config_watch_sync();   /* honors g.auto_reload, whichever config won */
    return true;
}

/* ===========================================================================
 * Shutdown
 * =========================================================================== */
void config_shutdown(void) {
    config_watch_stop();
    if (g.L) {
        lua_close(g.L);
        g.L = NULL;
    }
    config_free_owned(g.keymaps, g.keymap_count,
                      g.startup_commands, g.startup_count);
    config_detach();
}
