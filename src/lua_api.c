#include "mshell.h"

static void u8_to_w(const char *s, wchar_t *out, int out_count) {
    if (!s || out_count <= 0) { if (out_count > 0) out[0] = L'\0'; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, out_count);
    if (n <= 0) out[out_count - 1] = L'\0', out[0] = L'\0';
}

static wchar_t *u8_to_w_dup(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return _wcsdup(L"");
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void reject_at_runtime(lua_State *L, const char *fn);

static void push_wstr(lua_State *L, const wchar_t *w) {
    if (!w || !w[0]) { lua_pushstring(L, ""); return; }

    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) { lua_pushstring(L, ""); return; }

    char stack_buf[512];
    char *buf = (n <= (int)sizeof stack_buf) ? stack_buf : (char *)malloc((size_t)n);
    if (!buf) { lua_pushstring(L, ""); return; }

    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, NULL, NULL);
    lua_pushstring(L, buf);
    if (buf != stack_buf) free(buf);
}

static void set_str (lua_State *L, const char *k, const wchar_t *v) {
    push_wstr(L, v);            lua_setfield(L, -2, k);
}
static void set_int (lua_State *L, const char *k, lua_Integer v) {
    lua_pushinteger(L, v);      lua_setfield(L, -2, k);
}
static void set_bool(lua_State *L, const char *k, bool v) {
    lua_pushboolean(L, v);      lua_setfield(L, -2, k);
}

static KeyMap *find_keymap(const char *name) {
    wchar_t wname[256];
    u8_to_w(name, wname, 256);
    for (int i = 0; i < g.keymap_count; i++) {
        if (_wcsicmp(g.keymaps[i].name, wname) == 0) {
            return &g.keymaps[i];
        }
    }
    return NULL;
}

const char *layout_to_name(Layout l) {
    switch (l) {
    case LAYOUT_TILING:   return "tiling";
    case LAYOUT_MONOCLE:  return "monocle";
    case LAYOUT_GRID:     return "grid";
    case LAYOUT_SPIRAL:   return "spiral";
    case LAYOUT_CENTERED: return "centered";
    case LAYOUT_BSTACK:   return "bstack";
    case LAYOUT_COLUMNS:  return "columns";
    case LAYOUT_BSP:      return "bsp";
    case LAYOUT_COUNT:    break;
    }
    return "tiling";
}

static bool layout_from_name(const char *s, Layout *out) {
    if      (strcmp(s, "tiling")   == 0) *out = LAYOUT_TILING;
    else if (strcmp(s, "monocle")  == 0) *out = LAYOUT_MONOCLE;
    else if (strcmp(s, "grid")     == 0) *out = LAYOUT_GRID;
    else if (strcmp(s, "spiral")   == 0) *out = LAYOUT_SPIRAL;
    else if (strcmp(s, "centered") == 0) *out = LAYOUT_CENTERED;
    else if (strcmp(s, "bstack")   == 0) *out = LAYOUT_BSTACK;
    else if (strcmp(s, "columns")  == 0) *out = LAYOUT_COLUMNS;
    else if (strcmp(s, "bsp")      == 0) *out = LAYOUT_BSP;
    else return false;
    return true;
}

#define MSHELL_SPEC_KEY "__mshell_spec"

static int  l_action_call(lua_State *L);
static void push_window(lua_State *L, HWND hwnd);

static int l_call_self(lua_State *L) {
    lua_remove(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}

static int api_value_spec(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);

    if (lua_iscfunction(L, idx) && lua_tocfunction(L, idx) == l_action_call) {
        if (!lua_getupvalue(L, idx, 1)) return -1;
        int i = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return i;
    }
    if (lua_istable(L, idx) && lua_getmetatable(L, idx)) {
        lua_getfield(L, -1, MSHELL_SPEC_KEY);
        int i = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : -1;
        lua_pop(L, 2);
        return i;
    }
    return -1;
}

static DWORD parse_mods(lua_State *L, int idx) {
    DWORD mods = 0;
    if (!lua_istable(L, idx)) return 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_isstring(L, -1)) mods |= mod_name_to_flag(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return mods;
}

static void bind_value(lua_State *L, KeyMap *map, DWORD mods, DWORD vk,
                       int vidx, const char *desc, bool default_terminal,
                       const char *where) {
    vidx = lua_absindex(L, vidx);

    wchar_t *desc_w = (desc && desc[0]) ? u8_to_w_dup(desc) : NULL;
    bool     terminal = default_terminal;

    int si = api_value_spec(L, vidx);
    if (si >= 0) {
        const ApiEntry *e = api_spec_at(si);
        if (!e || e->kind != API_ACTION) {
            free(desc_w);
            luaL_error(L, "%s: mshell.%s is not something a key can do",
                       where, e ? e->path : "?");
        }
        if (e->flags & API_PAYLOAD_STR) {
            free(desc_w);
            luaL_error(L, "%s: mshell.%s needs an argument, so bind a function "
                          "instead: function() mshell.%s(...) end",
                       where, e->path, e->path);
        }
        keymap_add_binding(map, mods, vk, e->action, 0, NULL, NULL, NULL, NULL,
                           desc_w, terminal);
        free(desc_w);
        return;
    }

    if (lua_isfunction(L, vidx)) {
        lua_pushvalue(L, vidx);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        keymap_add_binding(map, mods, vk, ACTION_LUA_CALL, ref, NULL, NULL,
                           NULL, NULL, desc_w, terminal);
        free(desc_w);
        return;
    }

    if (lua_type(L, vidx) == LUA_TSTRING) {
        const char *nm = lua_tostring(L, vidx);
        KeyMap     *sm = find_keymap(nm);
        if (sm) {
            keymap_add_binding(map, mods, vk, ACTION_ENTER_SUBMAP,
                               (int)(sm - g.keymaps), sm, NULL, NULL, NULL,
                               desc_w, false);
            free(desc_w);
            return;
        }
        free(desc_w);
        const ApiEntry *old = api_spec_by_legacy(nm);
        if (old)
            luaL_error(L, "%s: actions are functions now — write mshell.%s "
                          "instead of \"%s\"", where, old->path, nm);
        luaL_error(L, "%s: no submap named '%s' (define it with "
                      "mshell.keys.submap before entering it)", where, nm);
    }

    free(desc_w);
    luaL_error(L, "%s: expected an action, a function, or a submap name; got %s",
               where, luaL_typename(L, vidx));
}

static void bind_table_value(lua_State *L, KeyMap *map, DWORD mods, DWORD vk,
                             int tidx, bool default_terminal,
                             const char *where) {
    tidx = lua_absindex(L, tidx);

    lua_getfield(L, tidx, "desc");
    const char *desc = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    bool terminal = default_terminal;
    lua_getfield(L, tidx, "terminal");
    if (lua_isboolean(L, -1)) terminal = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_rawgeti(L, tidx, 1);
    if (lua_isnil(L, -1))
        luaL_error(L, "%s: a {…} binding needs the action or function first",
                   where);

    bind_value(L, map, mods, vk, -1, desc, terminal, where);
    lua_pop(L, 2);
}

static void bind_any(lua_State *L, KeyMap *map, DWORD mods, DWORD vk,
                     int vidx, bool default_terminal, const char *where) {
    vidx = lua_absindex(L, vidx);
    if (lua_istable(L, vidx) && api_value_spec(L, vidx) < 0)
        bind_table_value(L, map, mods, vk, vidx, default_terminal, where);
    else
        bind_value(L, map, mods, vk, vidx, NULL, default_terminal, where);
}

static int lua_mshell_bind(lua_State *L) {
    reject_at_runtime(L, "keys.bind");

    DWORD mods = parse_mods(L, 1);

    const char *key_str = luaL_checkstring(L, 2);
    DWORD vk = key_name_to_vk(key_str);
    if (vk == 0) return luaL_error(L, "unknown key: %s", key_str);

    if (!g.root_map) return luaL_error(L, "root keymap not initialized");

    const char *desc     = NULL;
    bool        terminal = true;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "desc");
        if (lua_isstring(L, -1)) desc = lua_tostring(L, -1);
        lua_getfield(L, 4, "terminal");
        if (lua_isboolean(L, -1)) terminal = (bool)lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    bind_value(L, g.root_map, mods, vk, 3, desc, terminal, "keys.bind");
    return 0;
}

static int lua_mshell_submap(lua_State *L) {
    reject_at_runtime(L, "keys.submap");
    const char *name    = luaL_checkstring(L, 1);
    bool        persist = false;
    DWORD       exit_vk = 0;

    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        lua_getfield(L, 3, "persist");
        if (lua_isboolean(L, -1)) persist = (bool)lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "sticky");
        if (lua_isboolean(L, -1)) persist = (bool)lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "exit");
        if (lua_isstring(L, -1)) {
            const char *ek = lua_tostring(L, -1);
            exit_vk = key_name_to_vk(ek);
            if (exit_vk == 0)
                return luaL_error(L, "submap '%s': unknown exit key '%s'",
                                  name, ek);
        }
        lua_pop(L, 1);
    }

    if (exit_vk && !persist)
        return luaL_error(L, "submap '%s': 'exit' needs a persisting submap "
                             "(set persist = true)", name);

    wchar_t wname[256];
    u8_to_w(name, wname, 256);

    KeyMap *km = keymap_new(wname, persist);
    if (!km) return luaL_error(L, "too many keymaps");
    km->exit_vk = exit_vk;

    bool term = !persist;

    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            return luaL_error(L, "submap '%s': keys must be key-name strings "
                                 "(e.g. h = mshell.window.focus.left); got a "
                                 "%s key", name, luaL_typename(L, -2));

        const char *key_str = lua_tostring(L, -2);
        DWORD vk = key_str ? key_name_to_vk(key_str) : 0;
        if (vk == 0)
            return luaL_error(L, "submap '%s': unknown key '%s'", name,
                              key_str ? key_str : "?");

        char where[160];
        snprintf(where, sizeof where, "submap '%s', key '%s'", name, key_str);
        bind_any(L, km, 0, vk, -1, term, where);

        lua_pop(L, 1);
    }

    return 0;
}

static int lua_mshell_set_leader(lua_State *L) {
    reject_at_runtime(L, "keys.leader");
    const char *name = luaL_checkstring(L, 1);
    KeyMap *km = find_keymap(name);
    if (!km)
        return luaL_error(L, "set_leader: submap '%s' not found "
                             "(define it with mshell.submap before set_leader)",
                          name);
    if (km == g.root_map)
        return luaL_error(L, "set_leader: the leader must be a submap, not root");
    g.leader_map = km;
    return 0;
}

static int lua_mshell_set_gaps(lua_State *L) {
    int inner = (int)luaL_checkinteger(L, 1);
    int outer = (int)luaL_optinteger(L, 2, inner);
    g.inner_gap = clamp_i(inner, 0, 100);
    g.outer_gap = clamp_i(outer, 0, 100);
    return 0;
}

static int lua_mshell_set_smart_gaps(lua_State *L) {
    g.smart_gaps = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_set_smart_borders(lua_State *L) {
    g.smart_borders = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_block_system_keys(lua_State *L) {
    g.block_system_keys = lua_toboolean(L, 1);
    return 0;
}

static COLORREF rgb_from_lua(unsigned c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static int lua_mshell_set_border(lua_State *L) {
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "width");
        if (lua_isnumber(L, -1))
            g.border_width = clamp_i((int)lua_tointeger(L, -1), 0, 10);
        lua_pop(L, 1);

        lua_getfield(L, 1, "focused");
        if (lua_isnumber(L, -1)) {
            g.border_color = rgb_from_lua((unsigned)lua_tointeger(L, -1));
            g.border_color_float  = g.border_color;
            g.border_color_urgent = g.border_color;
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "floating");
        if (lua_isnumber(L, -1))
            g.border_color_float = rgb_from_lua((unsigned)lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "urgent");
        if (lua_isnumber(L, -1))
            g.border_color_urgent = rgb_from_lua((unsigned)lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, 1, "corners");
        if (lua_isstring(L, -1)) {
            const char *c = lua_tostring(L, -1);
            if      (!strcmp(c, "square"))  g.corner_pref = 1;
            else if (!strcmp(c, "round"))   g.corner_pref = 2;
            else if (!strcmp(c, "small"))   g.corner_pref = 3;
            else if (!strcmp(c, "default")) g.corner_pref = 0;
            else {
                lua_pop(L, 1);
                return luaL_error(L, "set_border: unknown corners '%s' "
                                     "(square|round|small|default)", c);
            }
        }
        lua_pop(L, 1);
        return 0;
    }

    int width = (int)luaL_checkinteger(L, 1);
    g.border_width = clamp_i(width, 0, 10);

    if (lua_gettop(L) >= 2) {
        g.border_color = rgb_from_lua((unsigned)luaL_checkinteger(L, 2));
        g.border_color_float  = g.border_color;
        g.border_color_urgent = g.border_color;
    }
    return 0;
}

static int lua_mshell_set_background(lua_State *L) {
    unsigned c = (unsigned)luaL_checkinteger(L, 1);
    g.background_color = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    return 0;
}

static int lua_mshell_set_auto_reload(lua_State *L) {
    g.auto_reload = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_set_verbose(lua_State *L) {
    log_set_level(lua_toboolean(L, 1) ? LOG_DEBUG : LOG_INFO);
    return 0;
}

static int lua_mshell_set_log_level(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    LogLevel lvl;
    if (!log_level_from_name(name, &lvl))
        return luaL_error(L, "unknown log level: %s "
                             "(error|warn|info|debug|trace)", name);
    log_set_level(lvl);
    return 0;
}

static int lua_mshell_desktop_rule(lua_State *L) {
    reject_at_runtime(L, "desktop.rule");
    const char *pattern = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    if (g.desktop_rule_count >= MAX_DESKTOP_RULES)
        return luaL_error(L, "desktop_rule: too many rules (max %d)",
                          MAX_DESKTOP_RULES);

    DesktopRule *r = &g.desktop_rules[g.desktop_rule_count];
    memset(r, 0, sizeof(*r));
    u8_to_w(pattern, r->name_match, DESKTOP_NAME_MAX);
    if (!r->name_match[0])
        return luaL_error(L, "desktop_rule: the pattern is empty — use \"*\" to "
                             "match every desktop");

    lua_getfield(L, 2, "default");
    {
        int t = lua_type(L, -1);
        if (t == LUA_TSTRING)
            return luaL_error(L, "desktop_rule '%s': default is true or false "
                                 "now — \"always\" and \"remember\" are gone "
                                 "along with the session file, and `default` "
                                 "decides every start", pattern);
        if (t != LUA_TNIL && t != LUA_TBOOLEAN)
            return luaL_error(L, "desktop_rule '%s': default must be true or "
                                 "false", pattern);

        if (t == LUA_TBOOLEAN && lua_toboolean(L, -1)) {
            if (wcspbrk(r->name_match, L"*?"))
                return luaL_error(L, "desktop_rule '%s': default needs a "
                                     "literal desktop name, not a pattern — "
                                     "mshell has to know which single desktop "
                                     "to create at startup", pattern);
            if (!desktop_name_ok(r->name_match))
                return luaL_error(L, "desktop_rule '%s': not a usable desktop "
                                     "name — it must be non-empty, contain no "
                                     "spaces, and be under %d characters",
                                  pattern, DESKTOP_NAME_MAX);

            wcscpy(g.start_desktop, r->name_match);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "app");
    if (lua_isstring(L, -1)) {
        u8_to_w(lua_tostring(L, -1), r->app, MAX_PATH);
    } else if (lua_istable(L, -1)) {
        int t = lua_absindex(L, -1);
        lua_rawgeti(L, t, 1);
        if (lua_isstring(L, -1)) u8_to_w(lua_tostring(L, -1), r->app, MAX_PATH);
        lua_rawgeti(L, t, 2);
        if (lua_isstring(L, -1))
            u8_to_w(lua_tostring(L, -1), r->app_args, SPAWN_ARGS_MAX);
        lua_rawgeti(L, t, 3);
        if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, t, "cwd"); }
        if (lua_isstring(L, -1))
            u8_to_w(lua_tostring(L, -1), r->app_cwd, MAX_PATH);
        lua_pop(L, 3);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "gaps");
    if (lua_isnumber(L, -1)) {
        int v = clamp_i((int)lua_tointeger(L, -1), 0, 100);
        r->set_gaps = true; r->inner_gap = v; r->outer_gap = v;
    } else if (lua_istable(L, -1)) {
        int t = lua_absindex(L, -1);
        lua_rawgeti(L, t, 1);
        lua_rawgeti(L, t, 2);
        int inner = lua_isnumber(L, -2) ? (int)lua_tointeger(L, -2) : 0;
        int outer = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : inner;
        lua_pop(L, 2);
        r->set_gaps  = true;
        r->inner_gap = clamp_i(inner, 0, 100);
        r->outer_gap = clamp_i(outer, 0, 100);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "float");
    if (!lua_isnil(L, -1)) { r->set_float = true;
                             r->float_all = (bool)lua_toboolean(L, -1); }
    lua_pop(L, 1);

    lua_getfield(L, 2, "layout");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (!layout_from_name(s, &r->layout))
            return luaL_error(L, "desktop_rule '%s': unknown layout '%s'",
                              pattern, s);
        r->set_layout = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "master_ratio");
    if (lua_isnumber(L, -1)) {
        r->set_ratio    = true;
        r->master_ratio = clamp_f((float)lua_tonumber(L, -1), 0.2f, 0.9f);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "nmaster");
    if (lua_isnumber(L, -1)) {
        r->set_nmaster = true;
        r->n_master    = clamp_i((int)lua_tointeger(L, -1), 1, 20);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "monitor");
    if (lua_isnumber(L, -1)) {
        int m = (int)lua_tointeger(L, -1);
        if (m < 0 || m >= MAX_MONITORS)
            return luaL_error(L, "desktop_rule '%s': monitor %d is out of range "
                                 "(0..%d)", pattern, m, MAX_MONITORS - 1);
        r->set_monitor = true;
        r->monitor     = m;
    }
    lua_pop(L, 1);

    g.desktop_rule_count++;
    return 0;
}

static int lua_mshell_set_master_ratio(lua_State *L) {
    g.default_master_ratio = clamp_f((float)luaL_checknumber(L, 1), 0.2f, 0.9f);
    return 0;
}

static int lua_mshell_set_nmaster(lua_State *L) {
    g.default_nmaster = clamp_i((int)luaL_checkinteger(L, 1), 1, 20);
    return 0;
}

static int lua_mshell_set_layout(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if (!layout_from_name(s, &g.default_layout))
        return luaL_error(L, "unknown layout: %s", s);
    return 0;
}

static int lua_mshell_set_float_policy(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "never") == 0) g.float_policy = FLOAT_NEVER;
    else if (strcmp(s, "rules") == 0) g.float_policy = FLOAT_RULES;
    else return luaL_error(L, "float_policy must be 'rules' or 'never'");
    return 0;
}

static int lua_mshell_set_hide_policy(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "cloak") == 0) g.hide_policy = HIDE_CLOAK;
    else if (strcmp(s, "hide")  == 0) g.hide_policy = HIDE_SHOWWINDOW;
    else return luaL_error(L, "hide_policy must be 'cloak' or 'hide'");
    return 0;
}

static int lua_mshell_set_fullscreen_policy(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "contain") == 0) g.fullscreen_policy = FS_CONTENT;
    else if (strcmp(s, "monitor") == 0) g.fullscreen_policy = FS_BOTH;
    else return luaL_error(L, "fullscreen_policy must be 'contain' or 'monitor'");
    return 0;
}

static int lua_mshell_set_float_placement(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "center") == 0) g.float_placement = FLOAT_PLACE_CENTER;
    else if (strcmp(s, "none")   == 0) g.float_placement = FLOAT_PLACE_NONE;
    else return luaL_error(L, "float_placement must be 'center' or 'none'");
    return 0;
}

static int lua_mshell_set_attach(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "end")    == 0) g.attach_policy = ATTACH_END;
    else if (strcmp(s, "master") == 0) g.attach_policy = ATTACH_MASTER;
    else if (strcmp(s, "after")  == 0) g.attach_policy = ATTACH_AFTER;
    else return luaL_error(L, "attach must be 'end', 'master', or 'after'");
    return 0;
}

static int lua_mshell_set_manage_owned(lua_State *L) {
    g.manage_owned = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_set_float_on_top(lua_State *L) {
    g.float_on_top = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_set_min_window_size(lua_State *L) {
    g.min_win_w = clamp_i((int)luaL_checkinteger(L, 1), 0, 100000);
    g.min_win_h = clamp_i((int)luaL_checkinteger(L, 2), 0, 100000);
    return 0;
}

static int lua_mshell_rule(lua_State *L) {
    reject_at_runtime(L, "window.rule");
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *action_str = luaL_checkstring(L, 2);

    RuleAction action;
    if      (strcmp(action_str, "float")  == 0) action = RULE_FLOAT;
    else if (strcmp(action_str, "ignore") == 0) action = RULE_IGNORE;
    else if (strcmp(action_str, "manage") == 0) action = RULE_MANAGE;
    else return luaL_error(L, "rule: unknown action '%s' "
                              "(expected 'manage', 'float' or 'ignore')",
                           action_str);

    if (g.rule_count >= MAX_RULES) {
        luaL_error(L, "too many rules");
        return 0;
    }

    WindowRule *r = &g.rules[g.rule_count++];
    memset(r, 0, sizeof(*r));
    r->action = action;

    lua_getfield(L, 1, "class");
    if (lua_isstring(L, -1)) {
        u8_to_w(lua_tostring(L, -1), r->class_match, 256);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "process");
    if (lua_isstring(L, -1)) {
        u8_to_w(lua_tostring(L, -1), r->process_match, 256);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "path");
    if (lua_isstring(L, -1)) {
        u8_to_w(lua_tostring(L, -1), r->path_match, MAX_PATH);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "title");
    if (lua_isstring(L, -1)) {
        u8_to_w(lua_tostring(L, -1), r->title_match, 256);
    }
    lua_pop(L, 1);

    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "desktop");
        if (lua_isstring(L, -1)) {
            u8_to_w(lua_tostring(L, -1), r->desktop, DESKTOP_NAME_MAX);
            if (!desktop_name_ok(r->desktop)) {
                const char *n = lua_tostring(L, -1);
                lua_pop(L, 1);
                return luaL_error(L, "rule: '%s' is not a usable desktop name "
                                     "(no whitespace, 1..%d characters)",
                                  n, DESKTOP_NAME_MAX - 1);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "monitor");
        if (lua_isnumber(L, -1)) {
            r->set_monitor = true;
            r->monitor     = (int)lua_tointeger(L, -1);
            if (r->monitor < 0) {
                lua_pop(L, 1);
                return luaL_error(L, "rule: monitor must be >= 0");
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "geometry");
        if (lua_istable(L, -1)) {
            int t = lua_absindex(L, -1);
            int v[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4; i++) {
                lua_rawgeti(L, t, i + 1);
                v[i] = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
                lua_pop(L, 1);
            }
            if (v[2] <= 0 || v[3] <= 0) {
                lua_pop(L, 1);
                return luaL_error(L, "rule: geometry needs {x, y, w, h} with a "
                                     "positive width and height");
            }
            r->set_geometry = true;
            r->x = v[0]; r->y = v[1]; r->w = v[2]; r->h = v[3];
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "start_fullscreen");
        if (lua_isboolean(L, -1)) r->start_fullscreen = lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "center");
        if (lua_isboolean(L, -1)) {
            r->set_center = true;
            r->center     = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, 1, "dialog");
    if (lua_isboolean(L, -1)) {
        r->set_dialog = true;
        r->dialog     = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        lua_getfield(L, 3, "ring");
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1))
            r->no_ring = true;
        lua_pop(L, 1);

        lua_getfield(L, 3, "decorate");
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1))
            r->no_decor = true;
        lua_pop(L, 1);

        lua_getfield(L, 3, "fullscreen");
        if (lua_toboolean(L, -1))
            r->fullscreen = true;
        lua_pop(L, 1);
    }

    return 0;
}

static int lua_mshell_spawn(lua_State *L) {
    reject_at_runtime(L, "exec.startup");
    const char *cmd  = luaL_checkstring(L, 1);
    const char *args = luaL_optstring(L, 2, NULL);
    const char *cwd  = luaL_optstring(L, 3, NULL);

    if (g.startup_count >= MAX_STARTUP_COMMANDS) {
        return luaL_error(L, "too many startup commands (max %d)",
                          MAX_STARTUP_COMMANDS);
    }

    wchar_t *wcmd = u8_to_w_dup(cmd);
    if (!wcmd) return luaL_error(L, "out of memory");

    wchar_t *wargs = NULL;
    if (args && args[0]) {
        wargs = u8_to_w_dup(args);
        if (!wargs) { free(wcmd); return luaL_error(L, "out of memory"); }
    }

    wchar_t *wcwd = NULL;
    if (cwd && cwd[0]) {
        wcwd = u8_to_w_dup(cwd);
        if (!wcwd) { free(wcmd); free(wargs);
                     return luaL_error(L, "out of memory"); }
    }

    g.startup_commands[g.startup_count].cmd  = wcmd;
    g.startup_commands[g.startup_count].args = wargs;
    g.startup_commands[g.startup_count].cwd  = wcwd;
    g.startup_count++;
    return 0;
}

static int lua_mshell_set_update_check(lua_State *L) {
    g.update_check = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_set_animation(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    g.anim_ms = clamp_i(ms, 0, 200);
    return 0;
}

static int lua_mshell_set_dim(lua_State *L) {
    if (!lua_istable(L, 1)) {
        g.dim_enabled = lua_toboolean(L, 1);
        return 0;
    }
    lua_getfield(L, 1, "enabled");
    if (!lua_isnil(L, -1)) g.dim_enabled = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "color");
    if (lua_isnumber(L, -1))
        g.dim_color = rgb_from_lua((unsigned)lua_tointeger(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "opacity");
    if (lua_isnumber(L, -1))
        g.dim_alpha = (BYTE)clamp_i((int)lua_tointeger(L, -1), 0, 255);
    lua_pop(L, 1);
    return 0;
}

static int lua_mshell_set_mouse_tbl(lua_State *L) {
    reject_at_runtime(L, "mouse.setup");
    if (!lua_istable(L, 1)) {
        g.mouse_enabled = lua_toboolean(L, 1);
        return 0;
    }
    lua_getfield(L, 1, "drag_swap");
    if (!lua_isnil(L, -1)) g.mouse_enabled = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "follow");
    if (!lua_isnil(L, -1)) g.mouse_follow = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "warp");
    if (!lua_isnil(L, -1)) g.mouse_warp = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "mod_drag");
    if (!lua_isnil(L, -1)) g.mouse_mod_drag = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "speed");
    if (lua_isnumber(L, -1))
        g.mouse_speed = clamp_i((int)lua_tointeger(L, -1), 1, 20);
    lua_pop(L, 1);

    lua_getfield(L, 1, "accel");
    if (!lua_isnil(L, -1)) g.mouse_accel = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "swap_buttons");
    if (!lua_isnil(L, -1)) g.mouse_swap = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    return 0;
}

static int lua_mshell_monitor_rule(lua_State *L) {
    if (g.monitor_rule_count >= MAX_MONITOR_RULES)
        return luaL_error(L, "too many monitor rules (max %d)",
                          MAX_MONITOR_RULES);
    luaL_checktype(L, 2, LUA_TTABLE);

    MonitorRule *r = &g.monitor_rules[g.monitor_rule_count];
    memset(r, 0, sizeof(*r));
    r->index = -1;

    if (lua_isnumber(L, 1)) {
        r->index = (int)lua_tointeger(L, 1);
        if (r->index < 0)
            return luaL_error(L, "monitor_rule: index must be >= 0");
    } else if (lua_isstring(L, 1)) {
        u8_to_w(lua_tostring(L, 1), r->device, CCHDEVICENAME);
        if (!r->device[0])
            return luaL_error(L, "monitor_rule: the device pattern is empty");
    } else {
        return luaL_error(L, "monitor_rule: expected a device name or an index");
    }

    lua_getfield(L, 2, "gaps");
    if (lua_isnumber(L, -1)) {
        int v = clamp_i((int)lua_tointeger(L, -1), 0, 100);
        r->set_gaps = true; r->inner_gap = v; r->outer_gap = v;
    } else if (lua_istable(L, -1)) {
        int t = lua_absindex(L, -1);
        lua_rawgeti(L, t, 1);
        lua_rawgeti(L, t, 2);
        int inner = lua_isnumber(L, -2) ? (int)lua_tointeger(L, -2) : 0;
        int outer = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : inner;
        lua_pop(L, 2);
        r->set_gaps  = true;
        r->inner_gap = clamp_i(inner, 0, 100);
        r->outer_gap = clamp_i(outer, 0, 100);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "nmaster");
    if (lua_isnumber(L, -1)) {
        r->set_nmaster = true;
        r->n_master    = clamp_i((int)lua_tointeger(L, -1), 1, 20);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "master_ratio");
    if (lua_isnumber(L, -1)) {
        r->set_ratio     = true;
        r->master_ratio  = clamp_f((float)lua_tonumber(L, -1), 0.2f, 0.9f);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "layout");
    if (lua_isstring(L, -1)) {
        Layout lay;
        if (!layout_from_name(lua_tostring(L, -1), &lay)) {
            const char *n = lua_tostring(L, -1);
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: unknown layout '%s'", n);
        }
        r->set_layout = true;
        r->layout     = lay;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "resolution");
    if (lua_istable(L, -1)) {
        int t = lua_absindex(L, -1);
        lua_rawgeti(L, t, 1);
        lua_rawgeti(L, t, 2);
        int w = lua_isnumber(L, -2) ? (int)lua_tointeger(L, -2) : 0;
        int h = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : 0;
        lua_pop(L, 2);
        if (w <= 0 || h <= 0) {
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: resolution needs a positive "
                                 "width and height, e.g. { 2560, 1440 }");
        }
        r->set_resolution = true;
        r->width = w; r->height = h;
    } else if (lua_isstring(L, -1)) {
        int w = 0, h = 0;
        if (sscanf(lua_tostring(L, -1), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
            const char *s = lua_tostring(L, -1);
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: cannot read the resolution "
                                 "'%s' — expected \"2560x1440\" or "
                                 "{ 2560, 1440 }", s);
        }
        r->set_resolution = true;
        r->width = w; r->height = h;
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "monitor_rule: resolution must be a table or a "
                             "string");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "refresh");
    if (lua_isnumber(L, -1)) {
        int hz = (int)lua_tointeger(L, -1);
        if (hz <= 0) {
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: refresh must be positive Hz");
        }
        r->set_refresh = true;
        r->refresh     = hz;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "rotation");
    if (lua_isnumber(L, -1)) {
        int deg = (int)lua_tointeger(L, -1);
        if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: rotation must be 0, 90, 180 or "
                                 "270 degrees, or a name like \"portrait\"");
        }
        r->set_rotation = true;
        r->rotation     = deg;
    } else if (lua_isstring(L, -1)) {
        const char *n = lua_tostring(L, -1);
        int deg = -1;
        if      (!strcmp(n, "landscape"))         deg = ROTATE_0;
        else if (!strcmp(n, "portrait"))          deg = ROTATE_90;
        else if (!strcmp(n, "landscape_flipped")) deg = ROTATE_180;
        else if (!strcmp(n, "portrait_flipped"))  deg = ROTATE_270;
        if (deg < 0) {
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: unknown rotation '%s' — "
                                 "expected landscape, portrait, "
                                 "landscape_flipped, portrait_flipped, or a "
                                 "number of degrees", n);
        }
        r->set_rotation = true;
        r->rotation     = deg;
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "monitor_rule: rotation must be a number or a "
                             "string");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "hdr");
    if (lua_isboolean(L, -1)) {
        r->set_hdr = true;
        r->hdr     = (bool)lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "primary");
    if (lua_isboolean(L, -1)) {
        r->set_primary = true;
        r->primary     = (bool)lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "position");
    if (lua_istable(L, -1)) {
        int t = lua_absindex(L, -1);
        lua_rawgeti(L, t, 1);
        lua_rawgeti(L, t, 2);
        bool ok = lua_isnumber(L, -2) && lua_isnumber(L, -1);
        int  x  = (int)lua_tointeger(L, -2);
        int  y  = (int)lua_tointeger(L, -1);
        lua_pop(L, 2);
        if (!ok) {
            lua_pop(L, 1);
            return luaL_error(L, "monitor_rule: position needs an x and a y, "
                                 "e.g. { 3840, 0 }");
        }
        r->set_position = true;
        r->pos_x = x; r->pos_y = y;
    } else if (!lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "monitor_rule: position must be a table like "
                             "{ 3840, 0 }");
    }
    lua_pop(L, 1);

    g.monitor_rule_count++;
    return 0;
}

static int lua_mshell_set_minimize_policy(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    if      (!strcmp(p, "allow")) g.minimize_never = false;
    else if (!strcmp(p, "never")) g.minimize_never = true;
    else return luaL_error(L, "set_minimize_policy: expected \"allow\" or "
                              "\"never\", got '%s'", p);
    return 0;
}

static int lua_mshell_set_urgency(lua_State *L) {
    g.urgency_enabled = lua_toboolean(L, 1);
    return 0;
}

static int lua_mshell_notify(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    const char *kind = luaL_optstring(L, 2, "info");
    int         ms   = (int)luaL_optinteger(L, 3, 4000);

    NotifyKind k;
    if      (!_stricmp(kind, "info"))  k = NOTIFY_INFO;
    else if (!_stricmp(kind, "warn"))  k = NOTIFY_WARN;
    else if (!_stricmp(kind, "error")) k = NOTIFY_ERROR;
    else return luaL_error(L, "notify: unknown kind '%s' (info|warn|error)", kind);

    wchar_t w[NOTIFY_TEXT_CAP];
    u8_to_w(text, w, NOTIFY_TEXT_CAP);
    notify_show(w, k, ms);
    return 0;
}

static int lua_mshell_set_notify(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "enabled");
    if (!lua_isnil(L, -1)) g.notify_enabled = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "desktop_switch");
    if (!lua_isnil(L, -1)) g.notify_desktop = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);
    return 0;
}

static int lua_mshell_setenv(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!name[0]) return luaL_error(L, "setenv: the name is empty");
    if (strchr(name, '='))
        return luaL_error(L, "setenv: '%s' contains '=', which cannot be part "
                             "of a variable name", name);

    wchar_t wname[256];
    u8_to_w(name, wname, 256);

    if (lua_isnoneornil(L, 2)) {
        SetEnvironmentVariableW(wname, NULL);
        return 0;
    }

    const char *value = luaL_checkstring(L, 2);
    wchar_t *wvalue = u8_to_w_dup(value);
    if (!wvalue) return luaL_error(L, "out of memory");
    bool ok = SetEnvironmentVariableW(wname, wvalue) != 0;
    free(wvalue);

    if (!ok) return luaL_error(L, "setenv: could not set %s (error %lu)",
                               name, GetLastError());
    return 0;
}

static void push_monitor_fields(lua_State *L, int i) {
    const Monitor *m   = &g.monitors[i];
    UINT           dpi = monitor_dpi(i);

    lua_createtable(L, 0, 16);
    set_int (L, "x",           m->full.left);
    set_int (L, "y",           m->full.top);
    set_int (L, "width",       m->full.right  - m->full.left);
    set_int (L, "height",      m->full.bottom - m->full.top);
    set_int (L, "work_x",      m->work_area.left);
    set_int (L, "work_y",      m->work_area.top);
    set_int (L, "work_width",  m->work_area.right  - m->work_area.left);
    set_int (L, "work_height", m->work_area.bottom - m->work_area.top);
    set_int (L, "dpi",         (lua_Integer)dpi);
    lua_pushnumber(L, (lua_Number)dpi / 96.0); lua_setfield(L, -2, "scale");
    set_bool(L, "primary",     i == g.primary_monitor);
    set_bool(L, "focused",     i == g.focused_monitor);

    set_str(L, "device", m->device);
    DisplayMode mode = {0};
    if (display_current_mode(m->device, &mode))
        set_int(L, "refresh", mode.refresh);
    if (m->device[0]) set_int(L, "rotation", display_rotation(m->device));
    int hdr = display_hdr_state(m->device);
    if (hdr != HDR_UNSUPPORTED) set_bool(L, "hdr", hdr == HDR_ON);
}

static int lua_mshell_get_monitors(lua_State *L) {
    lua_createtable(L, g.monitor_count, 0);
    for (int i = 0; i < g.monitor_count; i++) {
        push_monitor_fields(L, i);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static void push_desktop_fields(lua_State *L, const Desktop *d) {
    set_str (L, "name",         d->name);
    set_bool(L, "current",      d->id == g.current_desktop_id);
    set_int (L, "windows",      d->count);
    lua_pushstring(L, layout_to_name(d->layout)); lua_setfield(L, -2, "layout");
    lua_pushnumber(L, d->master_ratio); lua_setfield(L, -2, "master_ratio");
    set_int (L, "nmaster",      d->n_master);
    set_bool(L, "float",        d->float_all);
    if (d->monitor >= 0) set_int(L, "monitor", d->monitor);
    if (d->app[0])       set_str(L, "app",     d->app);
}

static int lua_mshell_get_desktops(lua_State *L) {
    lua_createtable(L, g.desktop_count, 0);
    for (int i = 0; i < g.desktop_count; i++) {
        lua_createtable(L, 0, 9);
        push_desktop_fields(L, &g.desktops[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int lua_mshell_get_current_desktop(lua_State *L) {
    int slot = desktop_slot_by_id(g.current_desktop_id);
    if (slot < 0) { lua_pushnil(L); return 1; }

    lua_createtable(L, 0, 9);
    push_desktop_fields(L, &g.desktops[slot]);
    return 1;
}

static int lua_mshell_desktop_to_monitor(lua_State *L) {
    if (!g.lua_running)
        return luaL_error(L, "mshell.desktop_to_monitor can only be called at "
                             "runtime, from a binding or a callback — no "
                             "desktops exist while the config is loading. Use "
                             "mshell.desktop_rule(name, { monitor = n }) there");

    int slot, mon, mon_idx = 1;

    if (lua_type(L, 1) == LUA_TSTRING) {
        const char *name_u8 = lua_tostring(L, 1);
        wchar_t     name[DESKTOP_NAME_MAX];
        u8_to_w(name_u8, name, DESKTOP_NAME_MAX);

        slot = desktop_slot_by_name(name);
        if (slot < 0)
            return luaL_error(L, "desktop_to_monitor: no desktop named '%s' "
                                 "exists right now", name_u8);
        mon_idx = 2;
    } else {
        slot = desktop_current_slot();
    }

    mon = lua_isnoneornil(L, mon_idx) ? -1
                                      : (int)luaL_checkinteger(L, mon_idx);
    if (mon >= g.monitor_count)
        return luaL_error(L, "desktop_to_monitor: monitor %d does not exist "
                             "(%d attached)", mon, g.monitor_count);

    lua_pushboolean(L, desktop_set_monitor(slot, mon));
    return 1;
}

static int lua_mshell_get_focused_window(lua_State *L) {
    HWND hwnd = desktop_get_focused();
    if (!hwnd || !IsWindow(hwnd)) { lua_pushnil(L); return 1; }
    push_window(L, hwnd);
    return 1;
}

static int lua_mshell_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    log_err(L"[lua] %hs", msg);
    return 0;
}

static int lua_mshell_set_bar(lua_State *L) {
    reject_at_runtime(L, "bar.setup");
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "enabled");
    if (!lua_isnil(L, -1)) g.bar_enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "mode");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if      (strcmp(s, "top_bar")  == 0) g.bar_mode = BAR_MODE_TOP_BAR;
        else if (strcmp(s, "floating") == 0) g.bar_mode = BAR_MODE_FLOATING;
        else return luaL_error(L, "set_bar: mode must be 'top_bar' or "
                                  "'floating'");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "position");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if      (strcmp(s, "top")    == 0) g.bar_bottom = false;
        else if (strcmp(s, "bottom") == 0) g.bar_bottom = true;
        else return luaL_error(L, "set_bar: position must be 'top' or 'bottom'");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "height");
    if (lua_isnumber(L, -1))
        g.bar_height = clamp_i((int)lua_tointeger(L, -1), 12, 200);
    lua_pop(L, 1);

    const struct { const char *field; COLORREF *dst; } colors[] = {
        {"bg",     &g.bar_bg},
        {"fg",     &g.bar_fg},
        {"accent", &g.bar_accent},
        {"dim",    &g.bar_dim},
    };
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        lua_getfield(L, 1, colors[i].field);
        if (lua_isnumber(L, -1)) {
            unsigned c = (unsigned)lua_tointeger(L, -1);
            *colors[i].dst = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, 1, "modules");
    if (lua_istable(L, -1)) {
        unsigned mods = 0;
        int      t    = lua_absindex(L, -1);
        int      n    = (int)lua_rawlen(L, t);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, t, i);
            const char *m = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
            if      (m && strcmp(m, "desktops") == 0) mods |= BAR_MOD_DESKTOPS;
            else if (m && strcmp(m, "layout")   == 0) mods |= BAR_MOD_LAYOUT;
            else if (m && strcmp(m, "title")    == 0) mods |= BAR_MOD_TITLE;
            else if (m && strcmp(m, "clock")    == 0) mods |= BAR_MOD_CLOCK;
            else if (m && strcmp(m, "notifications") == 0)
                mods |= BAR_MOD_NOTIFICATIONS;
            else {
                lua_pop(L, 2);
                return luaL_error(L, "set_bar: unknown module '%s' (expected "
                                     "desktops, layout, title, clock or "
                                     "notifications)", m ? m : "?");
            }
            lua_pop(L, 1);
        }
        g.bar_modules = mods;
    }
    lua_pop(L, 1);

    return 0;
}

static int lua_mshell_set_whichkey(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "enabled");
    if (!lua_isnil(L, -1)) g.whichkey_enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "rounded");
    if (!lua_isnil(L, -1)) g.whichkey_rounded = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "position");
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        const struct { const char *name; WhichKeyPos pos; } places[] = {
            {"bottom",       WK_POS_BOTTOM},
            {"top",          WK_POS_TOP},
            {"center",       WK_POS_CENTER},
            {"centre",       WK_POS_CENTER},
            {"left",         WK_POS_LEFT},
            {"right",        WK_POS_RIGHT},
            {"top_left",     WK_POS_TOP_LEFT},
            {"top_right",    WK_POS_TOP_RIGHT},
            {"bottom_left",  WK_POS_BOTTOM_LEFT},
            {"bottom_right", WK_POS_BOTTOM_RIGHT},
        };
        size_t i = 0;
        for (; i < sizeof places / sizeof places[0]; i++)
            if (strcmp(s, places[i].name) == 0) {
                g.whichkey_pos = places[i].pos;
                break;
            }
        if (i == sizeof places / sizeof places[0]) {
            lua_pop(L, 1);
            return luaL_error(L, "set_whichkey: unknown position '%s' (expected "
                                 "bottom, top, center, left, right, top_left, "
                                 "top_right, bottom_left or bottom_right)", s);
        }
    }
    lua_pop(L, 1);

    const struct { const char *field; int *dst; int lo, hi; } ints[] = {
        {"delay",          &g.whichkey_delay,      0, 5000},
        {"margin",         &g.whichkey_margin,    -1, 2000},
        {"max_rows",       &g.whichkey_max_rows,   1, WHICHKEY_MAX_ROWS},
        {"padding",        &g.whichkey_padding,    0, 200},
        {"row_spacing",    &g.whichkey_row_gap,    0, 100},
        {"column_spacing", &g.whichkey_col_gap,    0, 400},
        {"key_spacing",    &g.whichkey_key_gap,    0, 200},
        {"header_spacing", &g.whichkey_hdr_gap,    0, 200},
        {"font_size",      &g.whichkey_font_size,  6, 96},
        {"border_width",   &g.whichkey_border_w,   0, 20},
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        lua_getfield(L, 1, ints[i].field);
        if (lua_isnumber(L, -1))
            *ints[i].dst = clamp_i((int)lua_tointeger(L, -1),
                                   ints[i].lo, ints[i].hi);
        lua_pop(L, 1);
    }

    const struct { const char *field; float *dst; } maxes[] = {
        {"max_width",  &g.whichkey_max_w},
        {"max_height", &g.whichkey_max_h},
    };
    for (size_t i = 0; i < sizeof maxes / sizeof maxes[0]; i++) {
        lua_getfield(L, 1, maxes[i].field);
        if (lua_isnumber(L, -1)) {
            double v = lua_tonumber(L, -1);
            if (v < 0)      v = 0;
            if (v > 20000)  v = 20000;
            *maxes[i].dst = (float)v;
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, 1, "opacity");
    if (lua_isnumber(L, -1))
        g.whichkey_opacity = (BYTE)clamp_i((int)lua_tointeger(L, -1), 0, 255);
    lua_pop(L, 1);

    lua_getfield(L, 1, "font");
    if (lua_isstring(L, -1))
        u8_to_w(lua_tostring(L, -1), g.whichkey_font, LF_FACESIZE);
    lua_pop(L, 1);

    const struct { const char *field; COLORREF *dst; } colors[] = {
        {"bg",     &g.whichkey_bg},
        {"fg",     &g.whichkey_fg},
        {"key_fg", &g.whichkey_key_fg},
        {"border", &g.whichkey_border},
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        lua_getfield(L, 1, colors[i].field);
        if (lua_isnumber(L, -1)) {
            unsigned c = (unsigned)lua_tointeger(L, -1);
            *colors[i].dst = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        }
        lua_pop(L, 1);
    }
    return 0;
}

void lua_run_ref(int ref) {
    if (!g.L || ref == LUA_NOREF || ref == LUA_REFNIL) return;

    if (g.lua_running) {
        log_w(L"config: Lua is already running; nested call ignored");
        return;
    }

    g.lua_running = true;

    lua_rawgeti(g.L, LUA_REGISTRYINDEX, ref);
    if (lua_isfunction(g.L, -1)) {
        if (lua_pcall(g.L, 0, 0, 0) != LUA_OK) {
            log_err(L"config: error in Lua binding: %hs",
                    lua_tostring(g.L, -1) ? lua_tostring(g.L, -1) : "?");
            lua_pop(g.L, 1);
        }
    } else {
        lua_pop(g.L, 1);
        log_err(L"config: Lua binding ref %d is not a function", ref);
    }

    g.lua_running = false;
}

static const struct { const char *name; LuaEvent ev; } lua_event_names[] = {
    {"window_open",    LUA_EVENT_WINDOW_OPEN},
    {"window_close",   LUA_EVENT_WINDOW_CLOSE},
    {"desktop_switch", LUA_EVENT_DESKTOP_SWITCH},
    {"focus",          LUA_EVENT_FOCUS},
    {NULL,             LUA_EVENT_COUNT}
};

static int lua_mshell_on(lua_State *L) {
    reject_at_runtime(L, "on");

    const char *ev_name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    LuaEvent ev = LUA_EVENT_COUNT;
    for (int i = 0; lua_event_names[i].name; i++)
        if (strcmp(ev_name, lua_event_names[i].name) == 0)
            ev = lua_event_names[i].ev;

    if (ev == LUA_EVENT_COUNT) {
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        for (int i = 0; lua_event_names[i].name; i++) {
            if (i) luaL_addstring(&b, ", ");
            luaL_addstring(&b, lua_event_names[i].name);
        }
        luaL_pushresult(&b);
        return luaL_error(L, "mshell.on: unknown event '%s' (expected one of: "
                             "%s)", ev_name, lua_tostring(L, -1));
    }

    if (g.lua_hook_count >= MAX_LUA_HOOKS)
        return luaL_error(L, "mshell.on: too many handlers (max %d)",
                          MAX_LUA_HOOKS);

    lua_pushvalue(L, 2);
    g.lua_hooks[g.lua_hook_count].event = ev;
    g.lua_hooks[g.lua_hook_count].ref   = luaL_ref(L, LUA_REGISTRYINDEX);
    g.lua_hook_count++;
    return 0;
}

static void push_event_arg(lua_State *L, LuaEvent ev, HWND hwnd,
                           const wchar_t *name) {
    if (ev == LUA_EVENT_DESKTOP_SWITCH) {
        int slot = desktop_slot_by_id(g.current_desktop_id);
        lua_createtable(L, 0, 10);
        if (slot >= 0) push_desktop_fields(L, &g.desktops[slot]);
        if (name && name[0]) { push_wstr(L, name); lua_setfield(L, -2, "from"); }
        return;
    }

    push_window(L, (hwnd && IsWindow(hwnd)) ? hwnd : NULL);
}

void lua_fire(LuaEvent ev, HWND hwnd, const wchar_t *name) {
    if (!g.L || g.lua_hook_count == 0) return;
    if (g.lua_running) {
        log_w(L"config: event %d fired while Lua was running — skipped", (int)ev);
        return;
    }

    g.lua_running = true;

    for (int i = 0; i < g.lua_hook_count; i++) {
        if (g.lua_hooks[i].event != ev) continue;

        lua_rawgeti(g.L, LUA_REGISTRYINDEX, g.lua_hooks[i].ref);
        if (!lua_isfunction(g.L, -1)) { lua_pop(g.L, 1); continue; }

        push_event_arg(g.L, ev, hwnd, name);
        if (lua_pcall(g.L, 1, 0, 0) != LUA_OK) {
            log_err(L"config: error in '%hs' handler: %hs",
                    lua_event_names[ev].name,
                    lua_tostring(g.L, -1) ? lua_tostring(g.L, -1) : "?");
            lua_pop(g.L, 1);
        }
    }

    g.lua_running = false;
}

static void reject_at_runtime(lua_State *L, const char *fn) {
    if (g.lua_running)
        luaL_error(L, "mshell.%s can only be called while the config is "
                      "loading, not from a binding or callback", fn);
}

#define MSHELL_WINDOW_MT "mshell.Window"

typedef struct { HWND hwnd; } LuaWindow;

static void push_window(lua_State *L, HWND hwnd) {
    if (!hwnd) { lua_pushnil(L); return; }
    LuaWindow *w = (LuaWindow *)lua_newuserdatauv(L, sizeof *w, 0);
    w->hwnd = hwnd;
    luaL_setmetatable(L, MSHELL_WINDOW_MT);
}

static HWND to_window(lua_State *L, int idx) {
    LuaWindow *w = (LuaWindow *)luaL_testudata(L, idx, MSHELL_WINDOW_MT);
    return w ? w->hwnd : NULL;
}

static HWND opt_window(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return NULL;
    HWND h = to_window(L, idx);
    if (!h) luaL_argerror(L, idx, "expected a window");
    return IsWindow(h) ? h : NULL;
}

static HWND check_window(lua_State *L, int idx) {
    HWND h = to_window(L, idx);
    if (!h) luaL_argerror(L, idx, "expected a window");
    if (!IsWindow(h)) luaL_error(L, "that window is gone");
    return h;
}

static int push_window_field(lua_State *L, HWND hwnd, const char *k) {
    if (strcmp(k, "hwnd") == 0) {
        lua_pushinteger(L, (lua_Integer)(intptr_t)hwnd);
        return 1;
    }
    if (strcmp(k, "valid") == 0) {
        lua_pushboolean(L, IsWindow(hwnd) != 0);
        return 1;
    }
    if (!IsWindow(hwnd)) {
        if (strcmp(k, "title") == 0 || strcmp(k, "class") == 0 ||
            strcmp(k, "process") == 0 || strcmp(k, "path") == 0 ||
            strcmp(k, "floating") == 0 || strcmp(k, "fullscreen") == 0 ||
            strcmp(k, "monitor") == 0 || strcmp(k, "desktop") == 0) {
            lua_pushnil(L);
            return 1;
        }
        return 0;
    }

    if (strcmp(k, "title") == 0) {
        wchar_t title[256] = {0};
        GetWindowTextW(hwnd, title, 256);
        push_wstr(L, title);
        return 1;
    }
    if (strcmp(k, "class") == 0) {
        wchar_t cls[256] = {0};
        GetClassNameW(hwnd, cls, 256);
        push_wstr(L, cls);
        return 1;
    }
    if (strcmp(k, "process") == 0 || strcmp(k, "path") == 0) {
        wchar_t path[MAX_PATH] = {0};
        window_process_path(hwnd, path, MAX_PATH);
        if (strcmp(k, "path") == 0) { push_wstr(L, path); return 1; }
        const wchar_t *proc = path;
        for (const wchar_t *p = path; *p; p++)
            if (*p == L'\\' || *p == L'/') proc = p + 1;
        push_wstr(L, proc);
        return 1;
    }

    const ManagedWindow *mw = window_find(hwnd);
    if (strcmp(k, "floating") == 0) {
        lua_pushboolean(L, mw && mw->is_floating);
        return 1;
    }
    if (strcmp(k, "fullscreen") == 0) {
        lua_pushboolean(L, mw && window_is_screen_fullscreen(mw));
        return 1;
    }
    if (strcmp(k, "monitor") == 0) {
        if (mw) lua_pushinteger(L, mw->monitor); else lua_pushnil(L);
        return 1;
    }
    if (strcmp(k, "desktop") == 0) {
        const Desktop *d = mw ? desktop_by_id(mw->desktop_id) : NULL;
        if (d) push_wstr(L, d->name); else lua_pushnil(L);
        return 1;
    }
    if (strcmp(k, "managed") == 0) {
        lua_pushboolean(L, mw != NULL);
        return 1;
    }
    return 0;
}

static int lua_window_index(lua_State *L) {
    HWND        hwnd = to_window(L, 1);
    const char *k    = luaL_checkstring(L, 2);

    if (push_window_field(L, hwnd, k)) return 1;

    luaL_getmetatable(L, MSHELL_WINDOW_MT);
    lua_getfield(L, -1, "methods");
    lua_getfield(L, -1, k);
    return 1;
}

static int lua_window_eq(lua_State *L) {
    lua_pushboolean(L, to_window(L, 1) == to_window(L, 2));
    return 1;
}

static int lua_window_tostring(lua_State *L) {
    HWND    hwnd     = to_window(L, 1);
    wchar_t title[80] = {0};
    if (IsWindow(hwnd)) GetWindowTextW(hwnd, title, 80);
    char t[240];
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t, sizeof t, NULL, NULL);
    lua_pushfstring(L, "window(%p, \"%s\")", (void *)hwnd, t);
    return 1;
}

static Action direction_action(lua_State *L, const char *dir, Action left,
                               Action down, Action up, Action right) {
    if (strcmp(dir, "left")  == 0) return left;
    if (strcmp(dir, "down")  == 0) return down;
    if (strcmp(dir, "up")    == 0) return up;
    if (strcmp(dir, "right") == 0) return right;
    luaL_error(L, "unknown direction '%s' (left, down, up or right)", dir);
    return ACTION_NONE;
}

static void split_target_and_spec(lua_State *L, HWND *target, int *spec) {
    if (lua_isnoneornil(L, 1) && lua_gettop(L) >= 2) {
        *target = NULL;
        *spec   = 2;
    } else if (to_window(L, 1)) {
        *target = opt_window(L, 1);
        *spec   = 2;
    } else {
        *target = NULL;
        *spec   = 1;
    }
}

static int window_move_to_monitor(lua_State *L, HWND target, int stack_idx);

static int lua_mshell_window_focus(lua_State *L) {
    HWND h = check_window(L, 1);
    window_focus(h);
    return 0;
}

static int lua_mshell_window_move(lua_State *L) {
    HWND target; int spec;
    split_target_and_spec(L, &target, &spec);

    if (lua_isstring(L, spec)) {
        Action a = direction_action(L, lua_tostring(L, spec), ACTION_MOVE_LEFT,
                                    ACTION_MOVE_DOWN, ACTION_MOVE_UP,
                                    ACTION_MOVE_RIGHT);
        execute_action_on(a, target, 0, NULL, NULL, NULL);
        return 0;
    }

    luaL_checktype(L, spec, LUA_TTABLE);

    lua_getfield(L, spec, "dir");
    if (lua_isstring(L, -1)) {
        Action a = direction_action(L, lua_tostring(L, -1), ACTION_MOVE_LEFT,
                                    ACTION_MOVE_DOWN, ACTION_MOVE_UP,
                                    ACTION_MOVE_RIGHT);
        lua_pop(L, 1);
        execute_action_on(a, target, 0, NULL, NULL, NULL);
        return 0;
    }
    lua_pop(L, 1);

    lua_getfield(L, spec, "desktop");
    if (lua_isstring(L, -1)) {
        wchar_t *name = u8_to_w_dup(lua_tostring(L, -1));
        lua_pop(L, 1);
        if (!name) return luaL_error(L, "out of memory");
        execute_action_on(ACTION_MOVE_TO_DESKTOP, target, 0, name, NULL, NULL);
        free(name);
        return 0;
    }
    lua_pop(L, 1);

    lua_getfield(L, spec, "monitor");
    if (!lua_isnil(L, -1)) {
        int rc = window_move_to_monitor(L, target, -1);
        lua_pop(L, 1);
        return rc;
    }
    lua_pop(L, 1);

    return luaL_error(L, "mshell.window.move needs dir, desktop or monitor");
}

static int window_move_to_monitor(lua_State *L, HWND target, int stack_idx) {
    int idx = stack_idx < 0 ? lua_gettop(L) : stack_idx;

    if (lua_isstring(L, idx) && !lua_isnumber(L, idx)) {
        const char *step = lua_tostring(L, idx);
        Action a;
        if (strcmp(step, "next") == 0)      a = ACTION_MOVE_TO_MONITOR_NEXT;
        else if (strcmp(step, "prev") == 0) a = ACTION_MOVE_TO_MONITOR_PREV;
        else return luaL_error(L, "monitor must be an index, \"next\" or \"prev\"");
        execute_action_on(a, target, 0, NULL, NULL, NULL);
        return 0;
    }

    int mon = (int)luaL_checkinteger(L, idx);
    if (mon < 0 || mon >= g.monitor_count)
        return luaL_error(L, "no monitor %d (there are %d)", mon,
                          g.monitor_count);

    HWND h = target ? target : desktop_get_focused();
    ManagedWindow *mw = h ? window_find(h) : NULL;
    if (!mw) return luaL_error(L, "no managed window to move");

    window_follow_monitor(mw, mon);
    tile_current();
    return 0;
}

static int lua_mshell_window_to_monitor(lua_State *L) {
    HWND target; int spec;
    split_target_and_spec(L, &target, &spec);
    return window_move_to_monitor(L, target, spec);
}

static int lua_mshell_window_resize(lua_State *L) {
    HWND target; int spec;
    split_target_and_spec(L, &target, &spec);

    if (lua_isstring(L, spec)) {
        Action a = direction_action(L, lua_tostring(L, spec),
                                    ACTION_RESIZE_LEFT, ACTION_RESIZE_DOWN,
                                    ACTION_RESIZE_UP, ACTION_RESIZE_RIGHT);
        execute_action_on(a, target, 0, NULL, NULL, NULL);
        return 0;
    }

    luaL_checktype(L, spec, LUA_TTABLE);
    HWND h = target ? target : desktop_get_focused();
    if (!h) return luaL_error(L, "no window to resize");

    RECT r = {0};
    if (!window_frame_rect(h, &r)) return luaL_error(L, "cannot read that window");

    lua_getfield(L, spec, "x");      int x = (int)luaL_optinteger(L, -1, r.left);
    lua_getfield(L, spec, "y");      int y = (int)luaL_optinteger(L, -1, r.top);
    lua_getfield(L, spec, "width");  int w = (int)luaL_optinteger(L, -1, r.right - r.left);
    lua_getfield(L, spec, "height"); int hh = (int)luaL_optinteger(L, -1, r.bottom - r.top);
    lua_pop(L, 4);

    window_set_pos(h, x, y, w, hh, 0);
    return 0;
}

static int lua_mshell_window_float(lua_State *L) {
    HWND target; int spec;
    split_target_and_spec(L, &target, &spec);

    HWND h = target ? target : desktop_get_focused();
    if (!h) return luaL_error(L, "no window to float");

    if (lua_isnoneornil(L, spec)) {
        const ManagedWindow *mw = window_find(h);
        lua_pushboolean(L, mw && mw->is_floating);
        return 1;
    }

    window_set_floating(h, (bool)lua_toboolean(L, spec));
    tile_current();
    return 0;
}

static int lua_mshell_window_fullscreen(lua_State *L) {
    HWND target; int spec;
    split_target_and_spec(L, &target, &spec);

    const char *mode = luaL_optstring(L, spec, "window");
    Action a;
    if (strcmp(mode, "window") == 0)       a = ACTION_FULLSCREEN;
    else if (strcmp(mode, "content") == 0) a = ACTION_FULLSCREEN_CONTENT;
    else if (strcmp(mode, "both") == 0)    a = ACTION_FULLSCREEN_BOTH;
    else return luaL_error(L, "fullscreen mode must be \"window\", \"content\" "
                              "or \"both\"");

    execute_action_on(a, target, 0, NULL, NULL, NULL);
    return 0;
}

static int lua_mshell_window_center(lua_State *L) {
    HWND h = opt_window(L, 1);
    if (!h) h = desktop_get_focused();
    if (!h) return luaL_error(L, "no window to center");
    window_center_float(h);
    return 0;
}

static int lua_mshell_window_promote(lua_State *L) {
    HWND h = opt_window(L, 1);
    if (!h) h = desktop_get_focused();
    if (!h) return luaL_error(L, "no window to promote");
    window_promote(h);
    tile_current();
    return 0;
}

static bool window_matches_filter(lua_State *L, int fidx, HWND hwnd) {
    if (fidx == 0) return true;

    const ManagedWindow *mw = window_find(hwnd);

    lua_getfield(L, fidx, "desktop");
    if (lua_isstring(L, -1)) {
        wchar_t want[DESKTOP_NAME_MAX];
        u8_to_w(lua_tostring(L, -1), want, DESKTOP_NAME_MAX);
        const Desktop *d = mw ? desktop_by_id(mw->desktop_id) : NULL;
        if (!d || !desktop_name_eq(d->name, want)) { lua_pop(L, 1); return false; }
    }
    lua_pop(L, 1);

    lua_getfield(L, fidx, "monitor");
    if (lua_isinteger(L, -1)) {
        if (!mw || mw->monitor != (int)lua_tointeger(L, -1)) { lua_pop(L, 1); return false; }
    }
    lua_pop(L, 1);

    lua_getfield(L, fidx, "floating");
    if (lua_isboolean(L, -1)) {
        bool want = (bool)lua_toboolean(L, -1);
        if (!mw || mw->is_floating != want) { lua_pop(L, 1); return false; }
    }
    lua_pop(L, 1);

    lua_getfield(L, fidx, "class");
    if (lua_isstring(L, -1)) {
        wchar_t pat[256], cls[256] = {0};
        u8_to_w(lua_tostring(L, -1), pat, 256);
        GetClassNameW(hwnd, cls, 256);
        if (!wildcard_match(pat, cls)) { lua_pop(L, 1); return false; }
    }
    lua_pop(L, 1);

    lua_getfield(L, fidx, "process");
    if (lua_isstring(L, -1)) {
        wchar_t pat[MAX_PATH], path[MAX_PATH] = {0};
        u8_to_w(lua_tostring(L, -1), pat, MAX_PATH);
        window_process_path(hwnd, path, MAX_PATH);
        const wchar_t *proc = path;
        for (const wchar_t *p = path; *p; p++)
            if (*p == L'\\' || *p == L'/') proc = p + 1;
        if (!wildcard_match(pat, proc)) { lua_pop(L, 1); return false; }
    }
    lua_pop(L, 1);

    return true;
}

static int lua_mshell_window_list(lua_State *L) {
    int fidx = lua_istable(L, 1) ? 1 : 0;

    lua_createtable(L, g.managed_count, 0);
    int n = 0;
    for (int i = 0; i < g.managed_count; i++) {
        HWND h = g.managed[i].hwnd;
        if (!h || !IsWindow(h)) continue;
        if (!window_matches_filter(L, fidx, h)) continue;
        push_window(L, h);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static int lua_mshell_desktop_get(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    wchar_t     wname[DESKTOP_NAME_MAX];
    u8_to_w(name, wname, DESKTOP_NAME_MAX);

    int slot = desktop_slot_by_name(wname);
    if (slot < 0) { lua_pushnil(L); return 1; }

    lua_createtable(L, 0, 9);
    push_desktop_fields(L, &g.desktops[slot]);
    return 1;
}

static int lua_mshell_desktop_windows(lua_State *L) {
    const Desktop *dt = NULL;
    if (lua_isstring(L, 1)) {
        wchar_t wname[DESKTOP_NAME_MAX];
        u8_to_w(lua_tostring(L, 1), wname, DESKTOP_NAME_MAX);
        int slot = desktop_slot_by_name(wname);
        if (slot >= 0) dt = &g.desktops[slot];
    } else {
        dt = desktop_current();
    }
    if (!dt) { lua_createtable(L, 0, 0); return 1; }

    lua_createtable(L, dt->count, 0);
    int n = 0;
    for (int i = 0; i < dt->count; i++) {
        if (!dt->windows[i] || !IsWindow(dt->windows[i])) continue;
        push_window(L, dt->windows[i]);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static int lua_mshell_monitor_current(lua_State *L) {
    int i = g.focused_monitor;
    if (i < 0 || i >= g.monitor_count) { lua_pushnil(L); return 1; }
    push_monitor_fields(L, i);
    return 1;
}

static int lua_mshell_monitor_focus(lua_State *L) {
    int mon = (int)luaL_checkinteger(L, 1);
    if (mon < 0 || mon >= g.monitor_count)
        return luaL_error(L, "no monitor %d (there are %d)", mon,
                          g.monitor_count);
    focus_monitor_at(mon);
    return 0;
}

static int lua_mshell_layout_get(lua_State *L) {
    const Desktop *dt = desktop_current();
    lua_pushstring(L, layout_to_name(dt ? dt->layout : LAYOUT_TILING));
    return 1;
}

static const wchar_t *display_device_arg(lua_State *L, int idx, wchar_t *buf,
                                         int cap) {
    if (idx >= 1 && lua_isstring(L, idx)) {
        u8_to_w(lua_tostring(L, idx), buf, cap);
        return buf;
    }
    int i = g.focused_monitor;
    if (i < 0 || i >= g.monitor_count) return NULL;
    return g.monitors[i].device;
}

static int lua_mshell_display_modes(lua_State *L) {
    wchar_t        buf[CCHDEVICENAME];
    const wchar_t *dev = display_device_arg(L, 1, buf, CCHDEVICENAME);
    if (!dev || !dev[0]) { lua_createtable(L, 0, 0); return 1; }

    DisplayMode modes[128];
    int         n = display_modes(dev, modes, 128);

    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 3);
        set_int(L, "width",   modes[i].width);
        set_int(L, "height",  modes[i].height);
        set_int(L, "refresh", modes[i].refresh);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int lua_mshell_display_set_mode(lua_State *L) {
    int            opts = lua_istable(L, 1) ? 1 : 2;
    wchar_t        buf[CCHDEVICENAME];
    const wchar_t *dev = display_device_arg(L, opts == 1 ? 0 : 1, buf,
                                            CCHDEVICENAME);
    if (!dev || !dev[0]) return luaL_error(L, "no display to set");

    luaL_checktype(L, opts, LUA_TTABLE);
    DisplayMode want = {0};
    lua_getfield(L, opts, "width");   want.width   = (int)luaL_optinteger(L, -1, 0);
    lua_getfield(L, opts, "height");  want.height  = (int)luaL_optinteger(L, -1, 0);
    lua_getfield(L, opts, "refresh"); want.refresh = (int)luaL_optinteger(L, -1, 0);
    lua_getfield(L, opts, "rotation");
    int rotation = (int)luaL_optinteger(L, -1, ROTATE_KEEP);
    lua_pop(L, 4);

    lua_pushboolean(L, display_set_mode(dev, &want, rotation));
    return 1;
}

static int lua_mshell_exec(lua_State *L) {
    const char *cmd  = luaL_checkstring(L, 1);
    const char *args = luaL_optstring(L, 2, NULL);
    const char *cwd  = luaL_optstring(L, 3, NULL);

    wchar_t *wcmd  = u8_to_w_dup(cmd);
    wchar_t *wargs = (args && args[0]) ? u8_to_w_dup(args) : NULL;
    wchar_t *wcwd  = (cwd  && cwd[0])  ? u8_to_w_dup(cwd)  : NULL;
    if (!wcmd) { free(wargs); free(wcwd); return luaL_error(L, "out of memory"); }

    bool ok = spawn_command(wcmd, wargs, wcwd, L"lua");
    free(wcmd); free(wargs); free(wcwd);

    lua_pushboolean(L, ok);
    return 1;
}

static int window_method_action(lua_State *L, Action a) {
    execute_action_on(a, check_window(L, 1), 0, NULL, NULL, NULL);
    return 0;
}

static int lua_window_close(lua_State *L)    { return window_method_action(L, ACTION_CLOSE); }
static int lua_window_kill(lua_State *L)     { return window_method_action(L, ACTION_KILL); }
static int lua_window_minimize(lua_State *L) { return window_method_action(L, ACTION_MINIMIZE); }
static int lua_window_restore(lua_State *L)  { return window_method_action(L, ACTION_RESTORE); }

static const luaL_Reg window_methods[] = {
    { "focus",      lua_mshell_window_focus },
    { "move",       lua_mshell_window_move },
    { "resize",     lua_mshell_window_resize },
    { "float",      lua_mshell_window_float },
    { "fullscreen", lua_mshell_window_fullscreen },
    { "center",     lua_mshell_window_center },
    { "promote",    lua_mshell_window_promote },
    { "close",      lua_window_close },
    { "kill",       lua_window_kill },
    { "minimize",   lua_window_minimize },
    { "restore",    lua_window_restore },
    { NULL, NULL }
};

static int l_action_call(lua_State *L) {
    int             si = (int)lua_tointeger(L, lua_upvalueindex(1));
    const ApiEntry *e  = api_spec_at(si);
    if (!e) return luaL_error(L, "unknown action");

    if ((e->flags & API_CONFIG_ONLY) && g.lua_running)
        return luaL_error(L, "mshell.%s can only be called while the config is "
                             "loading, not from a binding or callback", e->path);
    if ((e->flags & API_RUNTIME_ONLY) && !g.lua_running)
        return luaL_error(L, "mshell.%s can only be called at runtime, from a "
                             "binding or a callback", e->path);

    HWND target = to_window(L, 1);
    if (!(e->flags & API_TAKES_WINDOW)) target = NULL;
    if (target && !IsWindow(target))    target = NULL;

    int first = api_payload_index(e, to_window(L, 1) != NULL);

    int      arg     = 0;
    wchar_t *command = NULL;
    if (e->flags & API_PAYLOAD_STR) {
        command = u8_to_w_dup(luaL_checkstring(L, first));
        if (!command) return luaL_error(L, "out of memory");
    } else if (e->flags & API_PAYLOAD_NUM) {
        arg = (int)luaL_optinteger(L, first, 1);
    }

    execute_action_on(e->action, target, arg, command, NULL, NULL);
    free(command);
    return 0;
}

typedef struct { const char *path; lua_CFunction fn; } ApiImpl;

static const ApiImpl api_impl[] = {
    { "keys.bind",                lua_mshell_bind },
    { "keys.submap",              lua_mshell_submap },
    { "keys.leader",              lua_mshell_set_leader },
    { "keys.block_system",        lua_mshell_block_system_keys },

    { "window.get",               lua_mshell_get_focused_window },
    { "window.list",              lua_mshell_window_list },
    { "window.rule",              lua_mshell_rule },
    { "window.focus",             lua_mshell_window_focus },
    { "window.move",              lua_mshell_window_move },
    { "window.move.to_monitor",   lua_mshell_window_to_monitor },
    { "window.resize",            lua_mshell_window_resize },
    { "window.float",             lua_mshell_window_float },
    { "window.fullscreen",        lua_mshell_window_fullscreen },
    { "window.center",            lua_mshell_window_center },
    { "window.promote",           lua_mshell_window_promote },
    { "window.policy.float",      lua_mshell_set_float_policy },
    { "window.policy.hide",       lua_mshell_set_hide_policy },
    { "window.policy.fullscreen", lua_mshell_set_fullscreen_policy },
    { "window.policy.placement",  lua_mshell_set_float_placement },
    { "window.policy.minimize",   lua_mshell_set_minimize_policy },
    { "window.min_size",          lua_mshell_set_min_window_size },
    { "window.manage_owned",      lua_mshell_set_manage_owned },
    { "window.float_on_top",      lua_mshell_set_float_on_top },

    { "desktop.list",             lua_mshell_get_desktops },
    { "desktop.current",          lua_mshell_get_current_desktop },
    { "desktop.get",              lua_mshell_desktop_get },
    { "desktop.windows",          lua_mshell_desktop_windows },
    { "desktop.to_monitor",       lua_mshell_desktop_to_monitor },
    { "desktop.rule",             lua_mshell_desktop_rule },
    { "desktop.attach",           lua_mshell_set_attach },

    { "monitor.list",             lua_mshell_get_monitors },
    { "monitor.current",          lua_mshell_monitor_current },
    { "monitor.focus",            lua_mshell_monitor_focus },
    { "monitor.rule",             lua_mshell_monitor_rule },

    { "display.modes",            lua_mshell_display_modes },
    { "display.set_mode",         lua_mshell_display_set_mode },

    { "layout.set",               lua_mshell_set_layout },
    { "layout.get",               lua_mshell_layout_get },
    { "layout.master.ratio",      lua_mshell_set_master_ratio },
    { "layout.master.count",      lua_mshell_set_nmaster },
    { "layout.gaps",              lua_mshell_set_gaps },
    { "layout.smart_gaps",        lua_mshell_set_smart_gaps },

    { "bar.setup",                lua_mshell_set_bar },
    { "whichkey.setup",           lua_mshell_set_whichkey },
    { "mouse.setup",              lua_mshell_set_mouse_tbl },
    { "notify",                   lua_mshell_notify },
    { "notify.setup",             lua_mshell_set_notify },

    { "appearance.border",        lua_mshell_set_border },
    { "appearance.background",    lua_mshell_set_background },
    { "appearance.dim",           lua_mshell_set_dim },
    { "appearance.animation",     lua_mshell_set_animation },
    { "appearance.smart_borders", lua_mshell_set_smart_borders },
    { "appearance.urgency",       lua_mshell_set_urgency },

    { "exec",                     lua_mshell_exec },
    { "exec.startup",             lua_mshell_spawn },
    { "exec.setenv",              lua_mshell_setenv },

    { "log",                      lua_mshell_log },
    { "log.level",                lua_mshell_set_log_level },
    { "log.verbose",              lua_mshell_set_verbose },

    { "config.auto_reload",       lua_mshell_set_auto_reload },
    { "config.update_check",      lua_mshell_set_update_check },

    { "on",                       lua_mshell_on },
    { NULL, NULL }
};

static lua_CFunction api_impl_for(const char *path) {
    for (const ApiImpl *i = api_impl; i->path; i++)
        if (strcmp(i->path, path) == 0) return i->fn;
    return NULL;
}

static bool api_has_children(const char *path) {
    size_t n = strlen(path);
    for (const ApiEntry *e = api_spec; e->path; e++) {
        if (strncmp(e->path, path, n) == 0 && e->path[n] == '.') return true;
    }
    return false;
}

static void api_push_parent(lua_State *L, int root, const char *path) {
    lua_pushvalue(L, root);

    const char *p = path;
    for (;;) {
        const char *dot = strchr(p, '.');
        if (!dot) return;

        lua_pushlstring(L, p, (size_t)(dot - p));
        lua_pushvalue(L, -1);
        lua_rawget(L, -3);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_insert(L, -3);
            lua_rawset(L, -4);
        } else {
            lua_remove(L, -2);
        }
        lua_remove(L, -2);
        p = dot + 1;
    }
}

static void api_register_entry(lua_State *L, int root, int index) {
    const ApiEntry *e = api_spec_at(index);

    lua_CFunction impl = api_impl_for(e->path);
    bool          is_action = (e->kind == API_ACTION);
    bool          wants_table = (e->kind == API_NAMESPACE) ||
                                (e->flags & API_CALLABLE) ||
                                api_has_children(e->path);

    if (!impl && !is_action && e->kind != API_NAMESPACE) {
        log_err(L"[lua] api_spec: no implementation for mshell.%hs", e->path);
        return;
    }

    api_push_parent(L, root, e->path);
    const char *dot  = strrchr(e->path, '.');
    const char *leaf = dot ? dot + 1 : e->path;

    if (!wants_table) {
        if (impl) {
            lua_pushcfunction(L, impl);
        } else {
            lua_pushinteger(L, index);
            lua_pushcclosure(L, l_action_call, 1);
        }
        lua_setfield(L, -2, leaf);
        lua_pop(L, 1);
        return;
    }

    lua_getfield(L, -1, leaf);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); }

    lua_newtable(L);
    if (is_action) {
        lua_pushinteger(L, index);
        lua_setfield(L, -2, MSHELL_SPEC_KEY);
    }
    if (impl || is_action) {
        if (impl) lua_pushcfunction(L, impl);
        else    { lua_pushinteger(L, index); lua_pushcclosure(L, l_action_call, 1); }
        lua_pushcclosure(L, l_call_self, 1);
        lua_setfield(L, -2, "__call");
    }
    lua_setmetatable(L, -2);

    lua_setfield(L, -2, leaf);
    lua_pop(L, 1);
}

static int lua_mshell_missing(lua_State *L) {
    const char *k = lua_tostring(L, 2);
    if (k) {
        const char *now = api_removed_replacement(k);
        if (now)
            return luaL_error(L, "mshell.%s was removed — use mshell.%s", k, now);
    }
    lua_pushnil(L);
    return 1;
}

void lua_register_api(lua_State *L) {
    luaL_newmetatable(L, MSHELL_WINDOW_MT);
    lua_pushcfunction(L, lua_window_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_window_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, lua_window_tostring); lua_setfield(L, -2, "__tostring");
    lua_newtable(L);
    for (const luaL_Reg *m = window_methods; m->name; m++) {
        lua_pushcfunction(L, m->func);
        lua_setfield(L, -2, m->name);
    }
    lua_setfield(L, -2, "methods");
    lua_pop(L, 1);

    lua_newtable(L);
    int root = lua_gettop(L);
    for (int i = 0; i < api_spec_count(); i++) api_register_entry(L, root, i);

    lua_newtable(L);
    lua_pushcfunction(L, lua_mshell_missing);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, root);

    lua_setglobal(L, "mshell");
}
