/*
 * lua_api.c — C functions exposed to the Lua config.
 *
 * Every function follows the Lua C API convention:
 *   static int func(lua_State *L)
 * Returns the number of values pushed onto the Lua stack.
 *
 * These are called DURING config loading only; the hot path never
 * touches Lua.
 */

#include "mshell.h"

/* ===========================================================================
 * UTF-8 → wide conversion.
 *
 * Lua strings reach us as UTF-8 bytes. mbstowcs() would interpret them in the
 * process's (C) locale and mangle anything non-ASCII, so we go through
 * MultiByteToWideChar with CP_UTF8 everywhere instead.
 * =========================================================================== */
static void u8_to_w(const char *s, wchar_t *out, int out_count) {
    if (!s || out_count <= 0) { if (out_count > 0) out[0] = L'\0'; return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, out_count);
    if (n <= 0) out[out_count - 1] = L'\0', out[0] = L'\0';
}

/* Allocate a wide copy of a UTF-8 string (caller frees). NULL on OOM. */
static wchar_t *u8_to_w_dup(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return _wcsdup(L"");
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* Raises unless we are inside a config load — see its definition below. Every
 * config-building entry point starts with it. */
static void reject_at_runtime(lua_State *L, const char *fn);

/* Push a wide string onto the Lua stack as UTF-8. */
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

/* t[key] = <string>, t[key] = <int>, t[key] = <bool> on the table at the top. */
static void set_str (lua_State *L, const char *k, const wchar_t *v) {
    push_wstr(L, v);            lua_setfield(L, -2, k);
}
static void set_int (lua_State *L, const char *k, lua_Integer v) {
    lua_pushinteger(L, v);      lua_setfield(L, -2, k);
}
static void set_bool(lua_State *L, const char *k, bool v) {
    lua_pushboolean(L, v);      lua_setfield(L, -2, k);
}

/* ===========================================================================
 * Helper: find a keymap by name
 * =========================================================================== */
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

/* ===========================================================================
 * A desktop argument is a NAME.
 *
 * Desktops are created on demand, so there is nothing to resolve against and
 * no such thing as an unknown desktop: whatever name a binding carries either
 * already exists or comes into being the moment the key is pressed. All this
 * does is normalise and validate — a number is accepted and used as the name it
 * spells, so `switch_desktop 3` means the desktop called "3" (which is what
 * Win+3 lands on) rather than an index into anything.
 *
 * Returns a fresh wide string the caller owns.
 * =========================================================================== */
static wchar_t *desktop_arg_name(lua_State *L, const char *ctx, bool has_num,
                                 int num_arg, const char *str_arg) {
    char buf[32];
    if (has_num) {
        snprintf(buf, sizeof buf, "%d", num_arg);
        str_arg = buf;
    }
    if (!str_arg || !str_arg[0])
        luaL_error(L, "%s: needs a desktop name, e.g. %s \"web\" or %s \"1\"",
                   ctx, ctx, ctx);

    wchar_t wname[DESKTOP_NAME_MAX];
    u8_to_w(str_arg, wname, DESKTOP_NAME_MAX);
    if (!desktop_name_ok(wname))
        luaL_error(L, "%s: '%s' is not a usable desktop name — it must be "
                      "non-empty, contain no spaces, and be under %d characters",
                   ctx, str_arg, DESKTOP_NAME_MAX);

    wchar_t *out = _wcsdup(wname);
    if (!out) luaL_error(L, "out of memory");
    return out;
}

/* Enum → layout name; the exact spellings layout_from_name accepts.
 * Non-static: ipc.c reports the same names, and two copies of this switch
 * would drift the moment a layout is added. */
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

/* Layout name → enum. Shared by set_layout and desktop_rule so both spell the
 * layouts identically. Returns false (leaving *out alone) on an unknown name. */
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

/* ===========================================================================
 * Resolve an action name (+ optional numeric/string payload) into a full
 * binding and append it to `map`. Shared by mshell.bind and mshell.submap so
 * submaps are as expressive as root bindings (spawn, switch_desktop, nesting).
 *
 * Raises a Lua error (longjmp) on bad input; never returns in that case.
 * =========================================================================== */
static void add_resolved_binding(lua_State *L, KeyMap *map, DWORD mods, DWORD vk,
                                 const char *action_str, bool has_num, int num_arg,
                                 const char *str_arg, const char *args_str,
                                 const char *cwd_str, const char *desc_str,
                                 bool default_terminal) {
    int      arg      = 0;
    KeyMap  *submap   = NULL;
    wchar_t *command  = NULL;
    wchar_t *cmd_args = NULL;
    wchar_t *cmd_cwd  = NULL;
    /* A which-key label the config chose, overriding the one derived from the
     * action. "browser" reads better in a hint panel than "spawn firefox.exe". */
    wchar_t *desc_str_w = (desc_str && desc_str[0]) ? u8_to_w_dup(desc_str) : NULL;
    bool     terminal = default_terminal;
    Action   action;

    if (strcmp(action_str, "enter_submap") == 0) {
        action = ACTION_ENTER_SUBMAP;
        if (!str_arg) luaL_error(L, "enter_submap requires a submap name");
        submap = find_keymap(str_arg);
        if (!submap)
            luaL_error(L, "submap not found: %s (define it before entering it)",
                       str_arg);
        arg = (int)(submap - g.keymaps);
        terminal = false;   /* entering a submap is never terminal */
    } else if (strcmp(action_str, "spawn") == 0) {
        action = ACTION_SPAWN;
        if (!str_arg) luaL_error(L, "spawn requires a command string");
        command = u8_to_w_dup(str_arg);
        if (args_str && args_str[0]) cmd_args = u8_to_w_dup(args_str);
        if (cwd_str  && cwd_str[0])  cmd_cwd  = u8_to_w_dup(cwd_str);
    } else {
        if (args_str)
            luaL_error(L, "%s takes no arguments string — only spawn does",
                       action_str);
        if (cwd_str)
            luaL_error(L, "%s takes no working directory — only spawn does",
                       action_str);
        action = action_name_to_enum(action_str);
        if (action == ACTION_NONE) luaL_error(L, "unknown action: %s", action_str);

        if (action == ACTION_NOTIFY) {
            /* The message rides in `command`, the same slot spawn and the
             * desktop actions use — there is one string slot, and a notify has
             * exactly one string. */
            if (!str_arg) luaL_error(L, "notify requires a message string");
            command = u8_to_w_dup(str_arg);
        } else if (action == ACTION_SWITCH_DESKTOP ||
                   action == ACTION_MOVE_TO_DESKTOP) {
            /* Desktop actions carry a NAME, not an index — in `command`, the
             * same slot spawn uses, since a name is a string and there is no
             * number to put in `arg`. */
            command = desktop_arg_name(L, action_str, has_num, num_arg, str_arg);
        } else if (has_num) {
            arg = num_arg;
        } else if (str_arg) {
            arg = atoi(str_arg);
        }
    }

    keymap_add_binding(map, mods, vk, action, arg, submap, command, cmd_args,
                       cmd_cwd, desc_str_w, terminal);
    free(command);    /* keymap_add_binding takes its own copies */
    free(cmd_args);
    free(cmd_cwd);
    free(desc_str_w);
}

/* ===========================================================================
 * A binding's payload: either a plain string, or {command, arguments}.
 *
 * Arguments have to be a separate string because ShellExecuteW takes them
 * separately, and splitting one string back apart is ambiguous as soon as a
 * path contains a space — which on Windows is most of them. Reading the pair
 * off the stack is factored out here so mshell.bind and mshell.submap accept
 * exactly the same shapes.
 *
 * Returns how many values it pushed. *str and *args point into those, so the
 * caller must lua_pop() that many only after it is finished with them.
 * =========================================================================== */
static int read_payload(lua_State *L, int idx, bool *has_num, int *num_arg,
                        const char **str, const char **args,
                        const char **cwd, const char **desc) {
    *has_num = false; *num_arg = 0; *str = NULL; *args = NULL; *cwd = NULL;
    *desc = NULL;

    if (lua_isnumber(L, idx)) {
        *has_num = true;
        *num_arg = (int)lua_tointeger(L, idx);
        return 0;
    }
    if (lua_isstring(L, idx)) {
        *str = lua_tostring(L, idx);
        return 0;
    }
    if (lua_istable(L, idx)) {
        int t = lua_absindex(L, idx);   /* idx may be relative; pushing shifts it */
        lua_rawgeti(L, t, 1);
        lua_rawgeti(L, t, 2);
        /* Third slot is the working directory. Also accepts the named form,
         * because {"cmd", nil, "C:\\x"} to give only a cwd reads badly. */
        lua_rawgeti(L, t, 3);
        if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, t, "cwd"); }
        /* Named only: a fourth positional slot after cwd would be unreadable. */
        lua_getfield(L, t, "desc");
        *str  = lua_isstring(L, -4) ? lua_tostring(L, -4) : NULL;
        *args = lua_isstring(L, -3) ? lua_tostring(L, -3) : NULL;
        *cwd  = lua_isstring(L, -2) ? lua_tostring(L, -2) : NULL;
        *desc = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
        return 4;
    }
    return 0;
}

/* Parse a {"LWin", "Shift"} style modifier table at `idx` into a flag mask. */
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

/* ===========================================================================
 * mshell.bind(mods, key, action [, arg [, terminal]])
 *
 *   mods   — table of modifier strings, e.g. {"LWin", "Shift"}
 *   key    — key name string, e.g. "h", "Return"
 *   action — action name string, e.g. "focus_left", "spawn", "enter_submap"
 *   arg    — optional int (switch_desktop) or string (command / submap name)
 * =========================================================================== */
static int lua_mshell_bind(lua_State *L) {
    reject_at_runtime(L, "bind");
    int nargs = lua_gettop(L);

    DWORD mods = parse_mods(L, 1);

    const char *key_str = luaL_checkstring(L, 2);
    DWORD vk = key_name_to_vk(key_str);
    if (vk == 0) return luaL_error(L, "unknown key: %s", key_str);

    /* A function instead of an action name: keep a reference to it and bind a
     * call. The ref lives in the registry of the CURRENT lua_State, which is
     * torn down wholesale on reload — so there is nothing to release by hand,
     * and dispatch checks the config generation before ever using it. */
    if (lua_isfunction(L, 3)) {
        if (!g.root_map) return luaL_error(L, "root keymap not initialized");

        bool terminal = true;
        if (nargs >= 4 && lua_isboolean(L, 4)) terminal = (bool)lua_toboolean(L, 4);

        lua_pushvalue(L, 3);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);

        keymap_add_binding(g.root_map, mods, vk, ACTION_LUA_CALL, ref,
                           NULL, NULL, NULL, NULL, NULL, terminal);
        return 0;
    }

    const char *action_str = luaL_checkstring(L, 3);

    /* Payload: a number, a string, or {command, arguments} for spawn. */
    bool        has_num = false;
    int         num_arg = 0;
    const char *str_arg = NULL, *args_str = NULL, *cwd_str = NULL;
    const char *desc_str = NULL;
    int         pushed  = 0;
    if (nargs >= 4)
        pushed = read_payload(L, 4, &has_num, &num_arg, &str_arg, &args_str,
                              &cwd_str, &desc_str);

    bool terminal = true;   /* most actions return to root after firing */
    if (nargs >= 5 && lua_isboolean(L, 5)) terminal = (bool)lua_toboolean(L, 5);

    if (!g.root_map) return luaL_error(L, "root keymap not initialized");

    add_resolved_binding(L, g.root_map, mods, vk, action_str,
                         has_num, num_arg, str_arg, args_str, cwd_str,
                         desc_str, terminal);
    lua_pop(L, pushed);
    return 0;
}

/* ===========================================================================
 * mshell.submap(name, bindings [, opts])
 *
 *   name      — submap name. Pass it to mshell.set_leader to make a bare Win tap
 *               enter this map (nothing is special about any particular name).
 *   bindings  — table of key → action strings
 *   opts      — optional table:
 *                 persist = true   stay in the map; leave only via the exit key
 *                                  (default false = one-shot: the next key,
 *                                  bound or not, drops you back to root)
 *                 exit    = "q"    persisting map's exit key; REPLACES Escape
 *                                  (Escape is the exit only when this is omitted)
 *                 sticky  = true   backward-compatible alias for persist = true
 * =========================================================================== */
static int lua_mshell_submap(lua_State *L) {
    reject_at_runtime(L, "submap");
    const char *name    = luaL_checkstring(L, 1);
    bool        persist = false;
    DWORD       exit_vk = 0;

    /* Parse opts (table at index 3, optional).
     *   persist = true|false  choose the flavour (default false = one-shot)
     *   sticky  = true        backward-compatible alias for persist = true
     *   exit    = "q"         key that leaves a persisting map; it REPLACES
     *                         Escape (Escape stays special only when omitted) */
    if (lua_gettop(L) >= 3 && lua_istable(L, 3)) {
        lua_getfield(L, 3, "persist");
        if (lua_isboolean(L, -1)) persist = (bool)lua_toboolean(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 3, "sticky");   /* alias, checked after persist */
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

    /* An exit key only means something for a persisting map (an unpersisting one
     * leaves on the very next key). Flag the mismatch rather than ignore it. */
    if (exit_vk && !persist)
        return luaL_error(L, "submap '%s': 'exit' needs a persisting submap "
                             "(set persist = true)", name);

    /* Convert name to wide */
    wchar_t wname[256];
    u8_to_w(name, wname, 256);

    KeyMap *km = keymap_new(wname, persist);
    if (!km) return luaL_error(L, "too many keymaps");
    km->exit_vk = exit_vk;

    /* Submap bindings carry no modifier: a submap is modal, matched on bare keys
     * (the hook strips Win before matching). Terminal semantics: an unpersisting
     * map's actions return to root after one keypress; a persisting map's stay
     * in the submap (its exit key leaves). */
    bool term = !persist;

    /* Parse bindings table (index 2). Each value is either:
     *   key = "action"                 — simple action
     *   key = {"action", arg|command}  — action with a payload (spawn, desktop,
     *                                     enter_submap, …) */
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        /* lua_tostring on the KEY would be undefined behaviour here: it
         * converts the value in place, and mutating a key mid-traversal can
         * corrupt lua_next's iteration. Numeric keys — an array-style table, or
         * an explicit [1] = ... — are the case that would hit it, so check the
         * type instead of coercing, and say so rather than skipping silently. */
        if (lua_type(L, -2) != LUA_TSTRING)
            return luaL_error(L, "submap '%s': keys must be key-name strings "
                                 "(e.g. h = \"focus_left\"); got a %s key",
                              name, luaL_typename(L, -2));

        const char *key_str = lua_tostring(L, -2);
        DWORD vk = key_str ? key_name_to_vk(key_str) : 0;
        if (vk == 0)
            return luaL_error(L, "submap '%s': unknown key '%s'", name,
                              key_str ? key_str : "?");

        if (lua_isfunction(L, -1)) {
            /*  key = function() ... end  */
            lua_pushvalue(L, -1);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            keymap_add_binding(km, 0, vk, ACTION_LUA_CALL, ref,
                               NULL, NULL, NULL, NULL, NULL, term);
        } else if (lua_isstring(L, -1)) {
            /*  key = "action"  */
            add_resolved_binding(L, km, 0, vk, lua_tostring(L, -1),
                                 false, 0, NULL, NULL, NULL, NULL, term);
        } else if (lua_istable(L, -1)) {
            /*  key = {"action", payload}  — payload may itself be
             *  {command, arguments} for spawn. */
            int  t = lua_absindex(L, -1);
            lua_rawgeti(L, t, 1);
            const char *action_str = lua_isstring(L, -1) ? lua_tostring(L, -1)
                                                         : NULL;
            if (!action_str)
                return luaL_error(L, "submap '%s': binding for '%s' must start "
                                     "with an action name", name, key_str);

            lua_rawgeti(L, t, 2);
            bool        has_num; int num_arg;
            const char *str_arg, *args_str, *cwd_str, *desc_str;
            int pushed = read_payload(L, -1, &has_num, &num_arg,
                                      &str_arg, &args_str, &cwd_str, &desc_str);

            add_resolved_binding(L, km, 0, vk, action_str,
                                 has_num, num_arg, str_arg, args_str, cwd_str,
                                 desc_str, term);

            lua_pop(L, pushed + 2);  /* payload parts + payload + action */
        } else {
            return luaL_error(L, "submap '%s': binding for '%s' must be an "
                                 "action name, {action, payload}, or a "
                                 "function; got %s",
                              name, key_str, luaL_typename(L, -1));
        }

        lua_pop(L, 1);  /* pop value, keep key for lua_next */
    }

    return 0;
}

/* ===========================================================================
 * mshell.set_leader(name) — choose the submap a bare Win tap enters.
 *
 * There is no built-in leader map: without this call a Win tap does nothing, and
 * the name is entirely the config's choice. The named submap must already be
 * defined (define it with mshell.submap first, exactly like enter_submap), and
 * a persisting map is the usual choice so the leader stays until Escape.
 * =========================================================================== */
static int lua_mshell_set_leader(lua_State *L) {
    reject_at_runtime(L, "set_leader");
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

/* ===========================================================================
 * mshell.set_gaps(inner [, outer]) — independent inner/outer gaps.
 *   inner: gap between adjacent tiled windows
 *   outer: margin between the screen edge and the outermost windows
 *          (defaults to inner if omitted)
 * =========================================================================== */
static int lua_mshell_set_gaps(lua_State *L) {
    int inner = (int)luaL_checkinteger(L, 1);
    int outer = (int)luaL_optinteger(L, 2, inner);
    g.inner_gap = clamp_i(inner, 0, 100);
    g.outer_gap = clamp_i(outer, 0, 100);
    return 0;
}

/* mshell.set_smart_gaps(enabled) — drop gaps when a monitor holds one window */
static int lua_mshell_set_smart_gaps(lua_State *L) {
    g.smart_gaps = lua_toboolean(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.block_system_keys(enabled)
 *   true  (default) — swallow Alt+Tab, Alt+Esc, Alt+Space, Ctrl+Esc.
 *   false           — let those Windows shortcuts through.
 *   (Win+* is always blocked; Ctrl+Alt+Del can never be blocked.)
 * =========================================================================== */
static int lua_mshell_block_system_keys(lua_State *L) {
    g.block_system_keys = lua_toboolean(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.set_border(width, color)
 *   color is an integer, e.g. 0x333333
 * =========================================================================== */
static COLORREF rgb_from_lua(unsigned c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static int lua_mshell_set_border(lua_State *L) {
    /* Two shapes, because the positional one was here first and configs use it:
     *
     *   set_border(width [, color])
     *   set_border{ width = 2, focused = 0xffffff, floating = 0x89b4fa,
     *               urgent = 0xf38ba8, corners = "square" }
     *
     * The table form is how the per-state colours are reachable; floating and
     * urgent both default to the focused colour, so an existing config that
     * only ever set one sees no change. */
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "width");
        if (lua_isnumber(L, -1))
            g.border_width = clamp_i((int)lua_tointeger(L, -1), 0, 10);
        lua_pop(L, 1);

        lua_getfield(L, 1, "focused");
        if (lua_isnumber(L, -1)) {
            g.border_color = rgb_from_lua((unsigned)lua_tointeger(L, -1));
            /* Seed the others so naming only `focused` still means one colour
             * everywhere, exactly as the positional form did. */
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
            if      (!strcmp(c, "square"))  g.corner_pref = 1;  /* DONOTROUND  */
            else if (!strcmp(c, "round"))   g.corner_pref = 2;  /* ROUND       */
            else if (!strcmp(c, "small"))   g.corner_pref = 3;  /* ROUNDSMALL  */
            else if (!strcmp(c, "default")) g.corner_pref = 0;  /* DEFAULT     */
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

/* ===========================================================================
 * mshell.set_background(color)
 *   color is an integer 0xRRGGBB — the solid desktop backdrop color
 * =========================================================================== */
static int lua_mshell_set_background(lua_State *L) {
    unsigned c = (unsigned)luaL_checkinteger(L, 1);
    /* Accept 0xRRGGBB and store as COLORREF (0x00BBGGRR). */
    g.background_color = RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    return 0;
}

/* ===========================================================================
 * mshell.set_auto_reload(enabled)
 *   true (default) — watch this config's folder and reload when you save it.
 *   false          — reload only on the "reload" action (Win+Shift+R).
 *   Takes effect at the end of the load that sets it, so flipping it off and
 *   saving is the last thing auto-reload does.
 * =========================================================================== */
static int lua_mshell_set_auto_reload(lua_State *L) {
    g.auto_reload = lua_toboolean(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.set_verbose(enabled)
 *   Kept for configs that already call it: true means DEBUG, false means the
 *   INFO default. set_log_level is the finer-grained knob.
 * =========================================================================== */
static int lua_mshell_set_verbose(lua_State *L) {
    log_set_level(lua_toboolean(L, 1) ? LOG_DEBUG : LOG_INFO);
    return 0;
}

/* ===========================================================================
 * mshell.set_log_level("error"|"warn"|"info"|"debug"|"trace")
 *   Raising the level widens the log. "debug" is what --verbose gives you.
 * =========================================================================== */
static int lua_mshell_set_log_level(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    LogLevel lvl;
    if (!log_level_from_name(name, &lvl))
        return luaL_error(L, "unknown log level: %s "
                             "(error|warn|info|debug|trace)", name);
    log_set_level(lvl);
    return 0;
}

/* ===========================================================================
 * mshell.set_start_desktop(name) — the desktop you land on at startup.
 *
 * There is no desktop count to configure: desktops are created when you switch
 * to them and destroyed when you leave them empty, so at startup exactly one
 * exists and this names it. Defaults to "1".
 * =========================================================================== */
static int lua_mshell_set_start_desktop(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);

    wchar_t wname[DESKTOP_NAME_MAX];
    u8_to_w(name, wname, DESKTOP_NAME_MAX);
    if (!desktop_name_ok(wname))
        return luaL_error(L, "set_start_desktop: '%s' is not a usable desktop "
                             "name — it must be non-empty, contain no spaces, "
                             "and be under %d characters", name, DESKTOP_NAME_MAX);

    wcscpy(g.start_desktop, wname);
    return 0;
}

/* ===========================================================================
 * mshell.desktop_rule(pattern, opts) — what a desktop does.
 *
 *   pattern — the desktop NAME this applies to, as a case-insensitive wildcard
 *             pattern: "web" for one desktop, "game-*" for a family, "*" for
 *             every desktop.
 *   opts    — table, any subset of:
 *               app          = "firefox.exe"  open this when you enter the
 *                              desktop and it has no windows (and again after
 *                              you close the last one and come back)
 *               float        = true           windows opened here start
 *                              floating instead of tiled
 *               layout       = "monocle"      this desktop's layout, overriding
 *                              set_layout — same names set_layout takes
 *               master_ratio = 0.6            master area size, 0.2 .. 0.9
 *               nmaster      = 1              windows in the master area
 *               monitor      = 1              pin the desktop to a display
 *                              (0-based); its windows tile there and switching
 *                              to it takes the focus there
 *
 * Rules LAYER: every rule whose pattern matches is applied in declaration
 * order, each overriding only the fields it names. So a "*" rule sets the house
 * style and specific ones adjust a field or two:
 *
 *   mshell.desktop_rule("*",    { layout = "tiling" })
 *   mshell.desktop_rule("chat", { layout = "monocle", app = "discord.exe" })
 *
 * Rules are matched when a desktop is created and re-applied on config reload,
 * so editing one takes effect on desktops that already exist — including a
 * layout you changed at runtime, which goes back to what the rule says.
 * =========================================================================== */
static int lua_mshell_desktop_rule(lua_State *L) {
    reject_at_runtime(L, "desktop_rule");
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

    /* app = "firefox.exe"          — launch it with no arguments
     * app = {"wt.exe", "-p Ubuntu"} — launch it with arguments */
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
        /* Third slot, or a named cwd = — the same shapes a spawn payload
         * accepts, so the two do not have to be remembered separately. */
        lua_rawgeti(L, t, 3);
        if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_getfield(L, t, "cwd"); }
        if (lua_isstring(L, -1))
            u8_to_w(lua_tostring(L, -1), r->app_cwd, MAX_PATH);
        lua_pop(L, 3);
    }
    lua_pop(L, 1);

    /* gaps = 8  or  gaps = {inner, outer} — the same two shapes set_gaps
     * takes, so there is one thing to remember rather than two. */
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

/* ===========================================================================
 * mshell.set_master_ratio(ratio)
 *   ratio is a float between 0.2 and 0.9
 *
 * The default for every desktop; a desktop_rule's master_ratio overrides it.
 * Live desktops pick it up on the reapply that follows a reload, so there is
 * nothing to update here.
 * =========================================================================== */
static int lua_mshell_set_master_ratio(lua_State *L) {
    g.default_master_ratio = clamp_f((float)luaL_checknumber(L, 1), 0.2f, 0.9f);
    return 0;
}

/* ===========================================================================
 * mshell.set_nmaster(n) — default windows in the master area (>= 1)
 * =========================================================================== */
static int lua_mshell_set_nmaster(lua_State *L) {
    g.default_nmaster = clamp_i((int)luaL_checkinteger(L, 1), 1, 20);
    return 0;
}

/* ===========================================================================
 * mshell.set_layout(name) — default layout for every desktop.
 *   "tiling" | "monocle" | "grid" | "spiral" | "centered" | "bstack" | "columns"
 * =========================================================================== */
static int lua_mshell_set_layout(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if (!layout_from_name(s, &g.default_layout))
        return luaL_error(L, "unknown layout: %s", s);
    return 0;
}

/* ===========================================================================
 * Tiling policy knobs
 * =========================================================================== */

/* mshell.set_float_policy("rules" | "never") */
static int lua_mshell_set_float_policy(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "never") == 0) g.float_policy = FLOAT_NEVER;
    else if (strcmp(s, "rules") == 0) g.float_policy = FLOAT_RULES;
    else return luaL_error(L, "float_policy must be 'rules' or 'never'");
    return 0;
}

/* mshell.set_fullscreen_policy("contain" | "monitor")
 *
 * What an app that fullscreens ITSELF gets — YouTube's fullscreen button, F11
 * in a browser — when the window has no explicit mode from a keybinding:
 *   "contain" (default) keeps the window in its tile, so the app's fullscreen
 *             content fills the window instead of the screen;
 *   "monitor" lets it cover the display, dropping it out of the layout until it
 *             leaves fullscreen again.
 * The per-window actions (fullscreen_content / fullscreen_both) override this. */
static int lua_mshell_set_fullscreen_policy(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "contain") == 0) g.fullscreen_policy = FS_CONTENT;
    else if (strcmp(s, "monitor") == 0) g.fullscreen_policy = FS_BOTH;
    else return luaL_error(L, "fullscreen_policy must be 'contain' or 'monitor'");
    return 0;
}

/* mshell.set_float_placement("center" | "none")
 *
 * Where a window that is NOT tiled ends up:
 *   "center" (default) in the middle of its monitor's work area — both the
 *            window that opens floating (a rule, or a floating desktop) and the
 *            one toggle_float just took out of the grid;
 *   "none"   wherever the app opened it, which is what every release before
 *            this setting existed did.
 * Only the position: the size stays whatever the app asked for, clamped to the
 * work area. A rule's `geometry` names an exact rect and beats this, a rule's
 * `fullscreen` parks over the whole monitor and beats it, and `center` in a
 * rule's opts overrides it per app in either direction. */
static int lua_mshell_set_float_placement(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "center") == 0) g.float_placement = FLOAT_PLACE_CENTER;
    else if (strcmp(s, "none")   == 0) g.float_placement = FLOAT_PLACE_NONE;
    else return luaL_error(L, "float_placement must be 'center' or 'none'");
    return 0;
}

/* mshell.set_attach("end" | "master" | "after") */
static int lua_mshell_set_attach(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    if      (strcmp(s, "end")    == 0) g.attach_policy = ATTACH_END;
    else if (strcmp(s, "master") == 0) g.attach_policy = ATTACH_MASTER;
    else if (strcmp(s, "after")  == 0) g.attach_policy = ATTACH_AFTER;
    else return luaL_error(L, "attach must be 'end', 'master', or 'after'");
    return 0;
}

/* mshell.set_manage_owned(enabled) — also tile owned/dialog windows (risky) */
static int lua_mshell_set_manage_owned(lua_State *L) {
    g.manage_owned = lua_toboolean(L, 1);
    return 0;
}

/* mshell.set_float_on_top(enabled) — keep floating windows above tiled ones.
 * On by default; pass false to let a float sink behind the window you focus. */
static int lua_mshell_set_float_on_top(lua_State *L) {
    g.float_on_top = lua_toboolean(L, 1);
    return 0;
}

/* mshell.set_min_window_size(w, h) — windows smaller than this are ignored */
static int lua_mshell_set_min_window_size(lua_State *L) {
    g.min_win_w = clamp_i((int)luaL_checkinteger(L, 1), 0, 100000);
    g.min_win_h = clamp_i((int)luaL_checkinteger(L, 2), 0, 100000);
    return 0;
}

/* ===========================================================================
 * mshell.rule(match, action [, opts])
 *
 *   match  — table with optional "class", "process", "path" and/or "title"
 *            keys. Each is
 *            a case-insensitive wildcard pattern (`*` any run, `?` one char,
 *            `/` == `\`), so one rule can cover a whole game library:
 *              class   = "UnityWndClass"
 *              process = "eldenring.exe"
 *              path    = [[C:\SteamLibrary\steamapps\common\*]]
 *            A pattern with no wildcard is an exact match, as before. All the
 *            keys given must match; rules are tried in declaration order and
 *            the first match wins, so put specific rules above broad ones.
 *
 *            "dialog" is the one key that is not a pattern:
 *              dialog = true    match only windows Windows itself treats as a
 *                               dialog — file pickers (Open / Save As / Select
 *                               Folder), message boxes, print and properties
 *                               sheets, permission and credential prompts
 *              dialog = false   match only windows that are NOT one of those
 *            A file picker carries no class or process of its own — it is the
 *            host app's process wearing a common-dialog class — so this is the
 *            only way to name one. It is also the only match that can pull in
 *            an *owned* window, which mshell otherwise leaves untouched: a
 *            `dialog` rule is the config saying "manage these, floating", so
 *            the picker hides with its desktop instead of hovering over every
 *            desktop you switch to.
 *   action — "manage", "float", or "ignore"
 *   opts   — optional table of extra behavior:
 *              ring       = false  don't draw the focus ring around the window
 *                                  (see set_border)
 *              decorate   = false  strip the frame and add no border at all —
 *                                  floating windows normally keep their own
 *                                  chrome, this makes them borderless
 *              fullscreen = true   park it over the whole monitor it opened on
 *                                  (full bounds, no gaps); floating only
 *              center     = false  don't centre this app's floating windows —
 *                                  leave them where the app opens them (see
 *                                  set_float_placement). true forces centring
 *                                  under a config that turned it off
 *            The three together are the game preset: never tiled, borderless,
 *            covering the display, with no ring painted over its edges.
 * =========================================================================== */
static int lua_mshell_rule(lua_State *L) {
    reject_at_runtime(L, "rule");
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

    /* Often the only thing that separates two windows of one app — a
     * picture-in-picture player, a splash screen — where class and process are
     * identical. Matched against the title at the moment the window is adopted;
     * a title that changes later does not re-run the rules. */
    lua_getfield(L, 1, "title");
    if (lua_isstring(L, -1)) {
        u8_to_w(lua_tostring(L, -1), r->title_match, 256);
    }
    lua_pop(L, 1);

    /* --- where the window goes, as opposed to how it looks ---
     * These live in the OPTS table (third argument), beside ring/decorate,
     * because they describe what to do with a match rather than what to match. */
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

        /* geometry = {x, y, w, h} — only meaningful for a floating window,
         * since a tiled one's rect belongs to the layout. */
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

        /* center = false — this app places its own floating windows well
         * enough (a picture-in-picture player parked in a corner), so leave
         * them alone. center = true opts one app in under a config that
         * centres nothing. Only a real boolean counts: a missing key means
         * "whatever set_float_placement says", which is not the same as false.
         * Redundant beside `geometry`, which is already an exact rect. */
        lua_getfield(L, 3, "center");
        if (lua_isboolean(L, -1)) {
            r->set_center = true;
            r->center     = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }

    /* Only an actual boolean opts in — a missing key means "don't care", which
     * is not the same as `dialog = false` ("match everything that isn't one"). */
    lua_getfield(L, 1, "dialog");
    if (lua_isboolean(L, -1)) {
        r->set_dialog = true;
        r->dialog     = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    /* Optional opts table (arg 3). Absent fields keep their zeroed defaults, so
     * `ring`/`decorate` only take effect when the config explicitly passes
     * false, and `fullscreen` only when it explicitly passes true. */
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

/* ===========================================================================
 * mshell.spawn(command [, arguments]) — run this at startup.
 *
 *   mshell.spawn("alacritty.exe")
 *   mshell.spawn("wt.exe", "-p Ubuntu")
 *
 * Arguments are a separate string, not part of the command, because that is how
 * ShellExecuteW takes them — and splitting one string apart is ambiguous the
 * moment a path contains a space. Launching by bare name works (PATH is
 * resolved) and so does a .lnk shortcut.
 * =========================================================================== */
static int lua_mshell_spawn(lua_State *L) {
    reject_at_runtime(L, "spawn");
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

/* ===========================================================================
 * mshell.set_update_check(enabled)
 *
 * Off by default: it is a network request, and a window manager should not make
 * one nobody asked for. Notify-only in any case — nothing is ever downloaded or
 * applied, because a bad automatic update to the SHELL is a black screen at
 * sign-in with no desktop to fix it from. See update.c.
 * =========================================================================== */
static int lua_mshell_set_update_check(lua_State *L) {
    g.update_check = lua_toboolean(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.set_animation(ms)   |   mshell.set_dim{ enabled=, color= }
 *
 * Both off by default. They are the two features that cost frames rather than
 * bytes, and a tiling WM's appeal is that windows are where you put them
 * instantly — so motion is something you ask for.
 *
 * Dimming does NOT make anybody's window layered. See anim.c: that changes how
 * another process's window is composited, and can leave a GPU-accelerated app
 * rendering black. It is a scrim per monitor with the focused window punched
 * out of its region instead.
 * =========================================================================== */
static int lua_mshell_set_animation(lua_State *L) {
    int ms = (int)luaL_checkinteger(L, 1);
    /* Capped: past a fifth of a second the window manager feels laggy rather
     * than animated, and the tiler is placing windows behind the motion. */
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

    /* Opacity is how strong the dimming looks; it is the layered alpha of the
     * scrim rather than anything applied to a window. */
    lua_getfield(L, 1, "opacity");
    if (lua_isnumber(L, -1))
        g.dim_alpha = (BYTE)clamp_i((int)lua_tointeger(L, -1), 0, 255);
    lua_pop(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.set_mouse(enabled)  |  mshell.set_mouse{ drag_swap=, follow=,
 *                                                 mod_drag= }
 *
 * follow (focus-follows-mouse) is polled on a timer, not hooked — a
 * WH_MOUSE_LL hook fires on every pixel of movement, on the thread that has to
 * answer the keyboard hook inside LowLevelHooksTimeout.
 *
 * mod_drag is the one part that genuinely needs that hook, because it has to
 * swallow the button-down. Hence opt-in, and hence the hook existing only while
 * it is on.
 * =========================================================================== */
static int lua_mshell_set_mouse_tbl(lua_State *L) {
    reject_at_runtime(L, "set_mouse");
    /* Bare boolean is the original form: dragging a TILED window onto another
     * swaps them. A tiled window cannot really be moved (the layout owns its
     * geometry), so the drag is interpreted rather than obeyed. */
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

    lua_getfield(L, 1, "mod_drag");
    if (!lua_isnil(L, -1)) g.mouse_mod_drag = (bool)lua_toboolean(L, -1);
    lua_pop(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.monitor_rule(which, opts)
 *
 *   which — a device-name pattern ("\\\\.\\DISPLAY2", "*DISPLAY2") or a
 *           0-based index.
 *   opts  — gaps, nmaster, master_ratio, layout.
 *
 * Prefer the name form. An index is easier to write and is also what changes
 * when a display is unplugged: the rest renumber, and "monitor 1 uses columns"
 * quietly starts describing a different screen. The name survives that.
 *
 * A desktop spans every display (per-monitor tags were declined in 0.11.0 and
 * the README says why), so these describe the DISPLAY's habits — a vertical
 * secondary that always wants columns, an ultrawide that wants a wider master.
 * Rules layer like desktop rules; a desktop's own override still wins.
 * =========================================================================== */
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

    g.monitor_rule_count++;
    return 0;
}

/* ===========================================================================
 * mshell.set_minimize_policy("allow" | "never")
 *
 * "never" restores a window the moment it is minimized. Off by default, and
 * deliberately: 0.8.0 ADDED minimize/restore precisely so a window could be got
 * out of the way when there is no taskbar to retrieve it from. This is for
 * people who would rather nothing ever vanish.
 *
 * An app hiding itself to the tray is exempt either way — that is the app's
 * own window management, not a minimize, and fighting it would re-break
 * closing Discord or Steam to the tray.
 * =========================================================================== */
static int lua_mshell_set_minimize_policy(lua_State *L) {
    const char *p = luaL_checkstring(L, 1);
    if      (!strcmp(p, "allow")) g.minimize_never = false;
    else if (!strcmp(p, "never")) g.minimize_never = true;
    else return luaL_error(L, "set_minimize_policy: expected \"allow\" or "
                              "\"never\", got '%s'", p);
    return 0;
}

/* ===========================================================================
 * mshell.set_urgency(enabled)
 *
 * Off by default, and deliberately: noticing that a window flashed for
 * attention costs a hook on EVENT_OBJECT_STATECHANGE, which fires for every
 * control on the system. 0.8.0 narrowed the object-event range specifically to
 * stop that traffic, so turning it back on is a choice with a cost attached.
 * =========================================================================== */
static int lua_mshell_set_urgency(lua_State *L) {
    g.urgency_enabled = lua_toboolean(L, 1);
    return 0;
}

/* ===========================================================================
 * mshell.notify(text [, kind [, ms]])
 *
 *   kind — "info" (default), "warn" or "error"; picks the accent colour.
 *
 * Unlike most of the API this is callable at RUNTIME as well as config time —
 * it is the one thing an event handler most obviously wants, and it touches no
 * state a reload is rebuilding.
 * =========================================================================== */
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

/* ===========================================================================
 * mshell.set_notify{ enabled = true, desktop_switch = false }
 *
 * desktop_switch is off by default because the status bar already lists every
 * live desktop with the current one marked — a toast saying the same thing is
 * redundant unless the bar is disabled.
 * =========================================================================== */
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

/* ===========================================================================
 * mshell.setenv(name, value)
 *
 * The environment half of "spawn with an environment". Deliberately
 * process-wide rather than per-spawn: children inherit our block, so one call
 * covers every launch — a keybinding, a startup program and a desktop's `app`
 * alike — and there is no per-binding storage to keep in step. Passing nil
 * removes the variable.
 *
 * It changes OUR environment, which is the mechanism, so keep it to things a
 * child should see (PATH additions, EDITOR, a toolchain root).
 * =========================================================================== */
static int lua_mshell_setenv(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!name[0]) return luaL_error(L, "setenv: the name is empty");
    if (strchr(name, '='))
        return luaL_error(L, "setenv: '%s' contains '=', which cannot be part "
                             "of a variable name", name);

    wchar_t wname[256];
    u8_to_w(name, wname, 256);

    if (lua_isnoneornil(L, 2)) {
        SetEnvironmentVariableW(wname, NULL);   /* NULL deletes it */
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

/* ===========================================================================
 * State queries — what mshell can tell the config about right now.
 *
 * WHEN THESE ARE USEFUL matters, because the config normally runs exactly once,
 * at startup, in this order:
 *
 *     monitors enumerated  ->  config loaded  ->  first desktop created
 *
 * So mshell.get_monitors() is populated during the initial load, but the
 * desktop and window queries are not — no desktop exists yet, and nothing is
 * managed. They return an empty table / nil rather than inventing anything.
 *
 * They come into their own on a RELOAD (Win+Shift+R, or saving the file), when
 * the full runtime state is live — and from a keybinding bound to a Lua
 * function, which runs whenever you press it.
 * =========================================================================== */

/* mshell.get_monitors() -> array of
 *   { x, y, width, height, work_x, work_y, work_width, work_height,
 *     dpi, scale, primary, focused }
 * Geometry is in physical pixels (mshell is per-monitor DPI aware); `scale` is
 * the convenience form of dpi/96. */
static int lua_mshell_get_monitors(lua_State *L) {
    lua_createtable(L, g.monitor_count, 0);

    for (int i = 0; i < g.monitor_count; i++) {
        const Monitor *m   = &g.monitors[i];
        UINT           dpi = monitor_dpi(i);

        lua_createtable(L, 0, 12);
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

        lua_rawseti(L, -2, i + 1);   /* Lua arrays are 1-based */
    }
    return 1;
}

/* Fill in one desktop's table (assumed on top of the stack). */
static void push_desktop_fields(lua_State *L, const Desktop *d) {
    set_str (L, "name",         d->name);
    set_bool(L, "current",      d->id == g.current_desktop_id);
    set_int (L, "windows",      d->count);
    lua_pushstring(L, layout_to_name(d->layout)); lua_setfield(L, -2, "layout");
    lua_pushnumber(L, d->master_ratio); lua_setfield(L, -2, "master_ratio");
    set_int (L, "nmaster",      d->n_master);
    set_bool(L, "float",        d->float_all);
    if (d->monitor >= 0) set_int(L, "monitor", d->monitor);   /* nil = unpinned */
    if (d->app[0])       set_str(L, "app",     d->app);
}

/* mshell.get_desktops() -> array of desktop tables, in the order the cycling
 * actions step through them (numeric names first, then alphabetical). Empty
 * during the initial config load — see the note above. */
static int lua_mshell_get_desktops(lua_State *L) {
    lua_createtable(L, g.desktop_count, 0);
    for (int i = 0; i < g.desktop_count; i++) {
        lua_createtable(L, 0, 9);
        push_desktop_fields(L, &g.desktops[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* mshell.get_current_desktop() -> desktop table, or nil if none exists yet. */
static int lua_mshell_get_current_desktop(lua_State *L) {
    int slot = desktop_slot_by_id(g.current_desktop_id);
    if (slot < 0) { lua_pushnil(L); return 1; }

    lua_createtable(L, 0, 9);
    push_desktop_fields(L, &g.desktops[slot]);
    return 1;
}

/* mshell.get_focused_window() -> table or nil:
 *   { title, class, process, path, floating, fullscreen, monitor, desktop }
 * `process` is the bare exe name, the same thing a rule's `process` matches. */
static int lua_mshell_get_focused_window(lua_State *L) {
    HWND hwnd = desktop_get_focused();
    if (!hwnd || !IsWindow(hwnd)) { lua_pushnil(L); return 1; }

    wchar_t title[256] = {0}, cls[256] = {0}, path[MAX_PATH] = {0};
    GetWindowTextW(hwnd, title, 256);
    GetClassNameW(hwnd, cls, 256);
    window_process_path(hwnd, path, MAX_PATH);

    const wchar_t *proc = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') proc = p + 1;

    lua_createtable(L, 0, 8);
    set_str(L, "title",   title);
    set_str(L, "class",   cls);
    set_str(L, "process", proc);
    set_str(L, "path",    path);

    const ManagedWindow *mw = window_find(hwnd);
    if (mw) {
        set_bool(L, "floating",   mw->is_floating);
        set_bool(L, "fullscreen", window_is_screen_fullscreen(mw));
        set_int (L, "monitor",    mw->monitor);
        const Desktop *d = desktop_by_id(mw->desktop_id);
        if (d) set_str(L, "desktop", d->name);
    }
    return 1;
}

/* ===========================================================================
 * mshell.log(message)
 *   prints to stderr / DebugView
 * =========================================================================== */
static int lua_mshell_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    /* log_err, not log_w: a config that calls mshell.log() is deliberately
     * reporting something (a skipped optional app, a branch it took), and those
     * notes are worth nothing if they only appear under --verbose. */
    log_err(L"[lua] %hs", msg);
    return 0;
}

/* ===========================================================================
 * mshell.set_bar(opts) — the status bar.
 *   opts — table, any subset of:
 *     enabled  = true|false      show it at all                  (default true)
 *     position = "top"|"bottom"                                  (default top)
 *     height   = 28              design pixels at 96 DPI, scaled per monitor
 *     bg / fg / accent / dim = 0xRRGGBB
 *     modules  = {"desktops", "layout", "title", "clock"}   drawn left to right
 *
 * One bar per monitor, all showing the same thing — a desktop in mshell spans
 * every display, so there is no per-monitor desktop list to show. The bar
 * reserves its strip out of each monitor's work area, so tiled windows sit
 * below it while a fullscreen window still covers it.
 * =========================================================================== */
static int lua_mshell_set_bar(lua_State *L) {
    reject_at_runtime(L, "set_bar");
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "enabled");
    if (!lua_isnil(L, -1)) g.bar_enabled = lua_toboolean(L, -1);
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

    /* An explicit list REPLACES the default set, so leaving one out turns it
     * off — `modules = {"desktops"}` is how you get a bar with nothing else. */
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
            else {
                lua_pop(L, 2);
                return luaL_error(L, "set_bar: unknown module '%s' (expected "
                                     "desktops, layout, title or clock)",
                                  m ? m : "?");
            }
            lua_pop(L, 1);
        }
        g.bar_modules = mods;
    }
    lua_pop(L, 1);

    return 0;
}

/* ===========================================================================
 * mshell.set_whichkey(opts) — the submap hint ("which-key") panel.
 *   opts — table, any subset of:
 *     enabled = true|false   show the hint at all         (default true)
 *     delay   = <ms>         pause before it appears; 0 = instant (default 150)
 *     bg      = 0xRRGGBB     panel background
 *     fg      = 0xRRGGBB     action-label text
 *     key_fg  = 0xRRGGBB     key + header accent
 *     border  = 0xRRGGBB     panel outline
 * =========================================================================== */
static int lua_mshell_set_whichkey(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "enabled");
    if (!lua_isnil(L, -1)) g.whichkey_enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "delay");
    if (lua_isnumber(L, -1))
        g.whichkey_delay = clamp_i((int)lua_tointeger(L, -1), 0, 5000);
    lua_pop(L, 1);

    /* 0xRRGGBB → COLORREF, for each color field that is present. */
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

/* ===========================================================================
 * Call a config-supplied Lua function, by registry ref, on the main thread.
 *
 * Three things this has to get right:
 *
 *   - It must not unwind. These run from inside keybind dispatch and WinEvent
 *     handling; a raw lua_error there would longjmp out through C frames that
 *     own locks and suppression counters. lua_pcall keeps it contained and the
 *     error becomes a log line, which is also what the user needs to see.
 *   - It must not re-enter. A callback that switches desktops would fire the
 *     desktop callback, and so on. The guard makes the inner call a no-op.
 *   - It marks the window in which the config-building API is off limits:
 *     rebuilding keymaps while the hook thread may be reading them is exactly
 *     what config_load takes kb_lock for, and a callback holds no such lock.
 * =========================================================================== */
void lua_run_ref(int ref) {
    if (!g.L || ref == LUA_NOREF || ref == LUA_REFNIL) return;

    if (g.lua_running) {
        /* log_w, not log_err: nesting is legitimate to attempt — a handler that
         * switches desktops would fire the desktop event — and being refused is
         * the protection working, not a mistake the user has to fix. */
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

/* ===========================================================================
 * mshell.on(event, fn) — run fn when something happens.
 *
 *   "window_open"    a window came under management
 *   "window_close"   one is about to leave it
 *   "desktop_switch" the visible desktop changed
 *   "focus"          the focused window changed
 *
 * The handler receives one table describing the event: the window for the
 * window and focus events, the desktop for a switch. Several handlers may be
 * registered for the same event and run in registration order.
 * =========================================================================== */
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
        /* Name every valid option: a typo here otherwise reads as "my handler
         * never runs", with nothing to compare against. */
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

/* Push the argument table an event handler receives. */
static void push_event_arg(lua_State *L, LuaEvent ev, HWND hwnd,
                           const wchar_t *name) {
    if (ev == LUA_EVENT_DESKTOP_SWITCH) {
        int slot = desktop_slot_by_id(g.current_desktop_id);
        lua_createtable(L, 0, 10);
        if (slot >= 0) push_desktop_fields(L, &g.desktops[slot]);
        if (name && name[0]) { push_wstr(L, name); lua_setfield(L, -2, "from"); }
        return;
    }

    /* Window events. Described the same way get_focused_window() does, so a
     * handler can be written once and used for either. */
    if (!hwnd || !IsWindow(hwnd)) { lua_pushnil(L); return; }

    wchar_t title[256] = {0}, cls[256] = {0}, path[MAX_PATH] = {0};
    GetWindowTextW(hwnd, title, 256);
    GetClassNameW(hwnd, cls, 256);
    window_process_path(hwnd, path, MAX_PATH);

    const wchar_t *proc = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') proc = p + 1;

    lua_createtable(L, 0, 8);
    set_str(L, "title",   title);
    set_str(L, "class",   cls);
    set_str(L, "process", proc);
    set_str(L, "path",    path);

    const ManagedWindow *mw = window_find(hwnd);
    if (mw) {
        set_bool(L, "floating",   mw->is_floating);
        set_bool(L, "fullscreen", window_is_screen_fullscreen(mw));
        set_int (L, "monitor",    mw->monitor);
        const Desktop *d = desktop_by_id(mw->desktop_id);
        if (d) set_str(L, "desktop", d->name);
    }
}

void lua_fire(LuaEvent ev, HWND hwnd, const wchar_t *name) {
    if (!g.L || g.lua_hook_count == 0) return;   /* the overwhelmingly common case */
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

/* The config-building calls are only meaningful while the config is loading.
 * Called at the top of each of them; raises rather than corrupting live state
 * if a callback tries to rebuild the configuration from underneath the hook. */
static void reject_at_runtime(lua_State *L, const char *fn) {
    if (g.lua_running)
        luaL_error(L, "mshell.%s can only be called while the config is "
                      "loading, not from a binding or callback", fn);
}

/* ===========================================================================
 * Register all functions into the Lua VM
 * =========================================================================== */
void lua_register_api(lua_State *L) {
    static const luaL_Reg api[] = {
        {"bind",            lua_mshell_bind},
        {"submap",          lua_mshell_submap},
        {"set_leader",      lua_mshell_set_leader},
        {"set_gaps",        lua_mshell_set_gaps},
        {"set_smart_gaps",  lua_mshell_set_smart_gaps},
        {"set_border",      lua_mshell_set_border},
        {"set_background",  lua_mshell_set_background},
                {"set_bar",         lua_mshell_set_bar},
{"set_whichkey",    lua_mshell_set_whichkey},
        {"set_start_desktop", lua_mshell_set_start_desktop},
        {"desktop_rule",    lua_mshell_desktop_rule},
        {"set_master_ratio",lua_mshell_set_master_ratio},
        {"set_nmaster",     lua_mshell_set_nmaster},
        {"set_layout",      lua_mshell_set_layout},
        {"set_float_policy",lua_mshell_set_float_policy},
        {"set_fullscreen_policy",lua_mshell_set_fullscreen_policy},
        {"set_float_placement",lua_mshell_set_float_placement},
        {"set_attach",      lua_mshell_set_attach},
        {"set_mouse",       lua_mshell_set_mouse_tbl},
        {"set_animation",   lua_mshell_set_animation},
        {"set_update_check",lua_mshell_set_update_check},
        {"set_dim",         lua_mshell_set_dim},
        {"set_manage_owned",lua_mshell_set_manage_owned},
        {"set_float_on_top",lua_mshell_set_float_on_top},
        {"set_min_window_size",lua_mshell_set_min_window_size},
        {"set_auto_reload", lua_mshell_set_auto_reload},
        {"set_verbose",     lua_mshell_set_verbose},
        {"set_log_level",   lua_mshell_set_log_level},
        {"block_system_keys",lua_mshell_block_system_keys},
        {"rule",            lua_mshell_rule},
        {"spawn",           lua_mshell_spawn},
        {"setenv",          lua_mshell_setenv},
        {"set_urgency",     lua_mshell_set_urgency},
        {"monitor_rule",    lua_mshell_monitor_rule},
        {"set_minimize_policy", lua_mshell_set_minimize_policy},
        {"notify",          lua_mshell_notify},
        {"set_notify",      lua_mshell_set_notify},
        {"log",             lua_mshell_log},
        {"on",              lua_mshell_on},

        /* --- state queries (see the note above their definitions: only the
         *     monitor list is populated during the FIRST config load) --- */
        {"get_monitors",        lua_mshell_get_monitors},
        {"get_desktops",        lua_mshell_get_desktops},
        {"get_current_desktop", lua_mshell_get_current_desktop},
        {"get_focused_window",  lua_mshell_get_focused_window},

        {NULL, NULL}
    };

    /* Register all functions into the global "mshell" table */
    luaL_newlib(L, api);
    lua_setglobal(L, "mshell");
}
