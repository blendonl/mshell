/*
 * keyboard.c — low-level keyboard hook, submap state machine, action dispatch
 *
 * The WH_KEYBOARD_LL hook runs on its OWN dedicated thread (see kb_init), NOT
 * the main message thread. This is deliberate and important: a low-level hook
 * fires on the thread that installed it, during that thread's message pump. If
 * the hook shared the main thread, then every time the main thread ran a focus
 * change / tiling pass the hook could not fire — and while the Win key is held
 * it autorepeats a stream of Win-down events that MUST be swallowed. A missed
 * (starved) Win-down gets processed by the OS, and once the hook resumes it
 * swallows the Win-up, leaving the OS convinced Win is still held (bare "k"
 * then fires Win+K). A dedicated thread that does nothing but pump the hook can
 * never be starved, so no Win event is ever missed.
 */

#include "mshell.h"

/* ===========================================================================
 * Keymap lock + dedicated hook thread
 *
 * Because the hook now runs on a separate thread, its reads of the keymaps race
 * with a config reload rebuilding them on the main thread. A single critical
 * section serialises the two. It is held only for the fast keymap lookup in the
 * hook and for the (rare, user-initiated) reload — never around focus/tiling.
 * =========================================================================== */
static CRITICAL_SECTION g_kb_cs;
static bool             g_kb_cs_ready = false;
static HANDLE           g_kb_thread   = NULL;
static DWORD            g_kb_thread_id = 0;
static HANDLE           g_kb_ready_evt = NULL;

void kb_locks_init(void) {
    if (!g_kb_cs_ready) {
        InitializeCriticalSection(&g_kb_cs);
        g_kb_cs_ready = true;
    }
}
void kb_lock(void)   { if (g_kb_cs_ready) EnterCriticalSection(&g_kb_cs); }
void kb_unlock(void) { if (g_kb_cs_ready) LeaveCriticalSection(&g_kb_cs); }

/* ===========================================================================
 * Key-name → virtual-key lookup
 * =========================================================================== */
typedef struct {
    const char *name;
    DWORD       vk;
} KeyNameEntry;

static const KeyNameEntry key_names[] = {
    /* letters */
    {"a", 'A'}, {"b", 'B'}, {"c", 'C'}, {"d", 'D'}, {"e", 'E'},
    {"f", 'F'}, {"g", 'G'}, {"h", 'H'}, {"i", 'I'}, {"j", 'J'},
    {"k", 'K'}, {"l", 'L'}, {"m", 'M'}, {"n", 'N'}, {"o", 'O'},
    {"p", 'P'}, {"q", 'Q'}, {"r", 'R'}, {"s", 'S'}, {"t", 'T'},
    {"u", 'U'}, {"v", 'V'}, {"w", 'W'}, {"x", 'X'}, {"y", 'Y'}, {"z", 'Z'},

    /* digits */
    {"0", '0'}, {"1", '1'}, {"2", '2'}, {"3", '3'}, {"4", '4'},
    {"5", '5'}, {"6", '6'}, {"7", '7'}, {"8", '8'}, {"9", '9'},

    /* punctuation */
    {"`",  VK_OEM_3},     {"~",  VK_OEM_3},
    {"-",  VK_OEM_MINUS}, {"_",  VK_OEM_MINUS},
    {"=",  VK_OEM_PLUS},  {"+",  VK_OEM_PLUS},
    {"[",  VK_OEM_4},     {"{",  VK_OEM_4},
    {"]",  VK_OEM_6},     {"}",  VK_OEM_6},
    {"\\", VK_OEM_5},     {"|",  VK_OEM_5},
    {";",  VK_OEM_1},     {":",  VK_OEM_1},
    {"'",  VK_OEM_7},     {"\"", VK_OEM_7},
    {",",  VK_OEM_COMMA}, {"<",  VK_OEM_COMMA},
    {".",  VK_OEM_PERIOD},{">",  VK_OEM_PERIOD},
    {"/",  VK_OEM_2},     {"?",  VK_OEM_2},

    /* navigation */
    {"Left",      VK_LEFT},
    {"Right",     VK_RIGHT},
    {"Up",        VK_UP},
    {"Down",      VK_DOWN},
    {"Home",      VK_HOME},
    {"End",       VK_END},
    {"PageUp",    VK_PRIOR},
    {"PageDown",  VK_NEXT},

    /* special */
    {"Return",    VK_RETURN},
    {"Enter",     VK_RETURN},
    {"Space",     VK_SPACE},
    {"Tab",       VK_TAB},
    {"Escape",    VK_ESCAPE},
    {"Esc",       VK_ESCAPE},
    {"Backspace", VK_BACK},
    {"Delete",    VK_DELETE},
    {"Insert",    VK_INSERT},
    {"PrintScreen", VK_SNAPSHOT},
    {"Pause",     VK_PAUSE},
    {"CapsLock",  VK_CAPITAL},

    /* Media and browser keys, so a keyboard that HAS them can rebind them —
     * bare, without a modifier, since that is how they are pressed. The
     * volume and media ACTIONS are the other direction: they synthesise these
     * same keys for a keyboard that lacks them. */
    {"VolumeUp",   VK_VOLUME_UP},
    {"VolumeDown", VK_VOLUME_DOWN},
    {"VolumeMute", VK_VOLUME_MUTE},
    {"MediaPlay",  VK_MEDIA_PLAY_PAUSE},
    {"MediaNext",  VK_MEDIA_NEXT_TRACK},
    {"MediaPrev",  VK_MEDIA_PREV_TRACK},
    {"MediaStop",  VK_MEDIA_STOP},
    {"BrowserBack",    VK_BROWSER_BACK},
    {"BrowserForward", VK_BROWSER_FORWARD},
    {"BrowserRefresh", VK_BROWSER_REFRESH},
    {"BrowserHome",    VK_BROWSER_HOME},
    {"NumLock",   VK_NUMLOCK},
    {"ScrollLock", VK_SCROLL},

    /* function keys */
    {"F1",  VK_F1},  {"F2",  VK_F2},  {"F3",  VK_F3},  {"F4",  VK_F4},
    {"F5",  VK_F5},  {"F6",  VK_F6},  {"F7",  VK_F7},  {"F8",  VK_F8},
    {"F9",  VK_F9},  {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},

    /* numpad */
    {"Numpad0", VK_NUMPAD0}, {"Numpad1", VK_NUMPAD1}, {"Numpad2", VK_NUMPAD2},
    {"Numpad3", VK_NUMPAD3}, {"Numpad4", VK_NUMPAD4}, {"Numpad5", VK_NUMPAD5},
    {"Numpad6", VK_NUMPAD6}, {"Numpad7", VK_NUMPAD7}, {"Numpad8", VK_NUMPAD8},
    {"Numpad9", VK_NUMPAD9},

    {NULL, 0}
};

DWORD key_name_to_vk(const char *name) {
    for (const KeyNameEntry *e = key_names; e->name; e++) {
        if (_stricmp(e->name, name) == 0) return e->vk;
    }
    /* fallback: if name is a single ASCII char, use uppercase VK */
    if (name[0] && !name[1]) {
        return (DWORD)toupper((unsigned char)name[0]);
    }
    return 0;
}

/* Reverse of key_name_to_vk: the display name for a virtual-key, or NULL if we
 * don't have one. Returns the FIRST table entry for the vk, so aliases resolve
 * to the canonical label ("Return" not "Enter", "a" not the bare char). Used by
 * the which-key hint to print each binding's key. */
const char *vk_to_key_name(DWORD vk) {
    for (const KeyNameEntry *e = key_names; e->name; e++) {
        if (e->vk == vk) return e->name;
    }
    return NULL;
}

/* ===========================================================================
 * Action-name → enum lookup
 * =========================================================================== */
typedef struct {
    const char *name;
    Action      action;
} ActionNameEntry;

static const ActionNameEntry action_names[] = {
    {"focus_left",       ACTION_FOCUS_LEFT},
    {"focus_down",       ACTION_FOCUS_DOWN},
    {"focus_up",         ACTION_FOCUS_UP},
    {"focus_right",      ACTION_FOCUS_RIGHT},
    {"focus_next",       ACTION_FOCUS_NEXT},
    {"focus_prev",       ACTION_FOCUS_PREV},
    {"move_left",        ACTION_MOVE_LEFT},
    {"move_down",        ACTION_MOVE_DOWN},
    {"move_up",          ACTION_MOVE_UP},
    {"move_right",       ACTION_MOVE_RIGHT},
    {"switch_desktop",   ACTION_SWITCH_DESKTOP},
    {"move_to_desktop",  ACTION_MOVE_TO_DESKTOP},
    {"last_desktop",     ACTION_LAST_DESKTOP},
    {"next_desktop",     ACTION_NEXT_DESKTOP},
    {"prev_desktop",     ACTION_PREV_DESKTOP},
    {"focus_monitor_next", ACTION_FOCUS_MONITOR_NEXT},
    {"focus_monitor_prev", ACTION_FOCUS_MONITOR_PREV},
    {"move_to_monitor_next", ACTION_MOVE_TO_MONITOR_NEXT},
    {"move_to_monitor_prev", ACTION_MOVE_TO_MONITOR_PREV},
    {"close",            ACTION_CLOSE},
    {"kill",             ACTION_KILL},
    {"minimize",         ACTION_MINIMIZE},
    {"restore",          ACTION_RESTORE},
    {"toggle_sticky",    ACTION_TOGGLE_STICKY},
    {"mark_scratchpad",  ACTION_MARK_SCRATCHPAD},
    {"toggle_scratchpad",ACTION_TOGGLE_SCRATCHPAD},
    {"toggle_always_on_top", ACTION_TOGGLE_ALWAYS_ON_TOP},
    {"last_window",      ACTION_LAST_WINDOW},
    {"toggle_float",     ACTION_TOGGLE_FLOAT},
    {"resize_left",      ACTION_RESIZE_LEFT},
    {"resize_down",      ACTION_RESIZE_DOWN},
    {"resize_up",        ACTION_RESIZE_UP},
    {"resize_right",     ACTION_RESIZE_RIGHT},
    {"fullscreen",         ACTION_FULLSCREEN},
    {"fullscreen_content", ACTION_FULLSCREEN_CONTENT},
    {"fullscreen_both",    ACTION_FULLSCREEN_BOTH},
    {"layout_tiling",    ACTION_LAYOUT_TILING},
    {"layout_monocle",   ACTION_LAYOUT_MONOCLE},
    {"layout_grid",      ACTION_LAYOUT_GRID},
    {"layout_spiral",    ACTION_LAYOUT_SPIRAL},
    {"layout_centered",  ACTION_LAYOUT_CENTERED},
    {"layout_bstack",    ACTION_LAYOUT_BSTACK},
    {"layout_columns",   ACTION_LAYOUT_COLUMNS},
    {"cycle_layout",     ACTION_CYCLE_LAYOUT},
    {"promote_master",   ACTION_PROMOTE_MASTER},
    {"zoom",             ACTION_ZOOM},
    {"inc_master",       ACTION_INC_MASTER},
    {"dec_master",       ACTION_DEC_MASTER},
    {"inc_nmaster",      ACTION_INC_NMASTER},
    {"dec_nmaster",      ACTION_DEC_NMASTER},
    {"inc_cfact",        ACTION_INC_CFACT},
    {"dec_cfact",        ACTION_DEC_CFACT},
    {"reset_cfact",      ACTION_RESET_CFACT},
    {"enter_submap",     ACTION_ENTER_SUBMAP},
    {"spawn",            ACTION_SPAWN},
    {"reload",           ACTION_RELOAD},
    {"quit",             ACTION_QUIT},
    {"update",           ACTION_UPDATE},
    {"panic",            ACTION_PANIC},
    {"lock",             ACTION_LOCK},
    {"logoff",           ACTION_LOGOFF},
    {"reboot",           ACTION_REBOOT},
    {"shutdown",         ACTION_SHUTDOWN},
    {"sleep",            ACTION_SLEEP},
    {"hibernate",        ACTION_HIBERNATE},
    {"volume_up",        ACTION_VOLUME_UP},
    {"volume_down",      ACTION_VOLUME_DOWN},
    {"volume_mute",      ACTION_VOLUME_MUTE},
    {"media_play",       ACTION_MEDIA_PLAY},
    {"media_next",       ACTION_MEDIA_NEXT},
    {"media_prev",       ACTION_MEDIA_PREV},
    {"media_stop",       ACTION_MEDIA_STOP},
    {"screenshot",       ACTION_SCREENSHOT},
    {"screenshot_window", ACTION_SCREENSHOT_WINDOW},
    {"notify",           ACTION_NOTIFY},
    {"jump_urgent",      ACTION_JUMP_URGENT},
    {"launcher",         ACTION_LAUNCHER},
    {"toggle_bar",       ACTION_TOGGLE_BAR},
    {"toggle_hdr",       ACTION_TOGGLE_HDR},
    {"cycle_refresh",    ACTION_CYCLE_REFRESH},
    {"split_h",          ACTION_SPLIT_H},
    {"split_v",          ACTION_SPLIT_V},
    {"rotate_split",     ACTION_ROTATE_SPLIT},
    {"toggle_tabbed",    ACTION_TOGGLE_TABBED},
    {"toggle_stacked",   ACTION_TOGGLE_STACKED},
    {"container_next",   ACTION_CONTAINER_NEXT},
    {"container_prev",   ACTION_CONTAINER_PREV},
    {"split_grow",       ACTION_SPLIT_GROW},
    {"split_shrink",     ACTION_SPLIT_SHRINK},
    {"layout_bsp",       ACTION_LAYOUT_BSP},
    {NULL, ACTION_NONE}
};

Action action_name_to_enum(const char *name) {
    for (const ActionNameEntry *e = action_names; e->name; e++) {
        if (_stricmp(e->name, name) == 0) return e->action;
    }
    return ACTION_NONE;
}

/* Reverse of action_name_to_enum: the canonical name for an action, or NULL.
 * The which-key hint uses this as the label for bindings that aren't spawn /
 * enter_submap (which it formats specially). */
const char *action_enum_to_name(Action action) {
    for (const ActionNameEntry *e = action_names; e->name; e++) {
        if (e->action == action) return e->name;
    }
    return NULL;
}

/* ===========================================================================
 * Modifier-name → flag
 * =========================================================================== */
DWORD mod_name_to_flag(const char *name) {
    if (_stricmp(name, "LWin")  == 0) return MOD_LWIN;
    if (_stricmp(name, "RWin")  == 0) return MOD_LWIN;
    if (_stricmp(name, "Win")   == 0) return MOD_LWIN;
    if (_stricmp(name, "Shift") == 0) return MOD_SHIFT;
    if (_stricmp(name, "Ctrl")  == 0) return MOD_CONTROL;
    if (_stricmp(name, "Alt")   == 0) return MOD_ALT;
    return 0;
}

/* ===========================================================================
 * KeyMap allocation / manipulation
 * =========================================================================== */
KeyMap *keymap_new(const wchar_t *name, bool persist) {
    if (g.keymap_count >= MAX_KEYMAPS) return NULL;

    KeyMap *km = &g.keymaps[g.keymap_count++];
    km->name     = _wcsdup(name);
    km->capacity = 64;
    km->bindings = (KeyBinding *)calloc((size_t)km->capacity, sizeof(KeyBinding));
    km->count    = 0;
    km->persist  = persist;
    km->exit_vk  = 0;        /* 0 => Escape; a custom key is set by lua_api */
    return km;
}

void keymap_add_binding(KeyMap *map, DWORD mods, DWORD vk,
                        Action action, int arg, KeyMap *submap,
                        const wchar_t *command, const wchar_t *args,
                        const wchar_t *cwd, const wchar_t *desc,
                        bool terminal) {
    if (!map) return;

    /* grow the array if it's full */
    if (map->count >= map->capacity) {
        int new_cap = map->capacity * 2;
        KeyBinding *grown = (KeyBinding *)realloc(
            map->bindings, (size_t)new_cap * sizeof(KeyBinding));
        if (!grown) return;   /* out of memory — drop the binding */
        map->bindings = grown;
        map->capacity = new_cap;
    }

    /* Warn about a chord bound twice in the same map. keymap_find returns the
     * FIRST match, so the later binding can never fire — silently, which reads
     * as "that keybind doesn't work". Not an error: shadowing is harmless as
     * long as it is visible, and failing the load would break configs that
     * already have a duplicate. */
    for (int i = 0; i < map->count; i++) {
        if (map->bindings[i].vk == vk && map->bindings[i].mod_flags == mods) {
            log_err(L"config: %ls: mods=0x%X vk=0x%02X is already bound — the "
                    L"later binding is SHADOWED and will never fire",
                    map->name ? map->name : L"?", (unsigned)mods, (unsigned)vk);
            break;
        }
    }

    KeyBinding *kb = &map->bindings[map->count++];
    kb->mod_flags = mods;
    kb->vk        = vk;
    kb->action    = action;
    kb->arg       = arg;
    kb->submap    = submap;
    kb->command   = command ? _wcsdup(command) : NULL;
    kb->args      = (args && args[0]) ? _wcsdup(args) : NULL;
    kb->cwd       = (cwd  && cwd[0])  ? _wcsdup(cwd)  : NULL;
    kb->desc      = (desc && desc[0]) ? _wcsdup(desc) : NULL;
    kb->terminal  = terminal;
}

/* ===========================================================================
 * Find a binding in a keymap
 * =========================================================================== */
static KeyBinding *keymap_find(KeyMap *map, DWORD mods, DWORD vk) {
    if (!map) return NULL;
    for (int i = 0; i < map->count; i++) {
        KeyBinding *kb = &map->bindings[i];
        if (kb->vk == vk && kb->mod_flags == mods) return kb;
    }
    return NULL;
}

/* ===========================================================================
 * Modifier state — tracked MANUALLY from the events the hook observes.
 *
 * We cannot use GetAsyncKeyState/GetKeyState here: we swallow the Win key
 * (return 1), and a key blocked by a low-level hook is not reflected in those
 * APIs. A global WH_KEYBOARD_LL hook sees every keystroke, so we just watch
 * the modifier key up/down events ourselves. This is the standard approach.
 * =========================================================================== */
static bool mod_lwin, mod_shift, mod_ctrl, mod_alt;

/* Per-side state behind the three combined flags above. Held separately so
 * that releasing one Shift while the other is still down doesn't report the
 * modifier as released. */
static bool mod_lshift, mod_rshift, mod_lctrl, mod_rctrl, mod_lalt, mod_ralt;

/* Win-tap detection. Win is both a held modifier (Win+key chords, still live at
 * the root map) AND a leader: a *bare* tap — Win pressed and released with no
 * other key in between — enters the leader map (whichever submap the config
 * chose via mshell.set_leader). `win_used` records whether any key was seen
 * while Win was held; if not, the Win-up is a bare tap. Reset on a
 * fresh Win-down, set by the first non-Win key observed while Win is held. */
/* Pending vim-style repeat count, built from digits typed inside a submap and
 * consumed by the next action. Hook-thread state, guarded by kb_lock like the
 * map pointer it sits beside. */
static int s_count;

static bool win_used;

static DWORD current_mods(void) {
    DWORD m = 0;
    if (mod_lwin)  m |= MOD_LWIN;
    if (mod_shift) m |= MOD_SHIFT;
    if (mod_ctrl)  m |= MOD_CONTROL;
    if (mod_alt)   m |= MOD_ALT;
    return m;
}

/* Which-key bridge. The submap hint must be drawn from the MAIN thread (GDI),
 * but g.current_map flips here on the hook thread — so whenever the active map
 * changes we only POST a nudge and let the main thread (whichkey_notify) do the
 * drawing. The static guard suppresses a post on every root-map keystroke (the
 * common case, where the active map doesn't actually change). The LPARAM is a
 * hint; whichkey_notify re-reads the authoritative map under the lock anyway.
 * Callers must hold kb_lock (this reads g.current_map). */
static KeyMap *g_wk_last_map = (KeyMap *)-1;
static void notify_submap(void) {
    KeyMap *m = (g.current_map && g.current_map != g.root_map) ? g.current_map : NULL;
    if (m == g_wk_last_map) return;      /* no change — nothing to redraw */
    g_wk_last_map = m;
    if (g.message_window)
        PostMessageW(g.message_window, WM_MSHELL_SUBMAP, 0, (LPARAM)m);
}

/* Re-sync input state after returning from a secure desktop (session unlock,
 * fast-user-switch, UAC prompt, RDP reconnect). While the secure desktop is up
 * our hook does not run, so key-up events for anything held at lock time —
 * notably Win, e.g. after an idle-timeout lock or a Win+L we couldn't block —
 * never reach us, and the modifier would otherwise stay stuck "down" in our
 * tracking. main.c calls this on WM_WTSSESSION_CHANGE. */
void kb_reset_state(void) {
    mod_lwin = mod_shift = mod_ctrl = mod_alt = false;
    mod_lshift = mod_rshift = mod_lctrl = mod_rctrl = false;
    mod_lalt = mod_ralt = false;
    win_used = false;
    kb_lock();
    s_count = 0;              /* a half-typed count must not survive this */
    g.current_map = g.root_map;
    notify_submap();          /* drop the hint if a submap was showing */
    kb_unlock();
}

/* ===========================================================================
 * Pending-action ring — how a keystroke reaches the main thread.
 *
 * The hook must stay fast: doing SetForegroundWindow / ShellExecute, or ANY
 * logging or blocking call, inline can blow LowLevelHooksTimeout, which makes
 * Windows drop the hook for an event and leaves the swallowed Win key stuck
 * (the OS sees the Win-down but the hook still eats the Win-up). So the hook
 * only records what to do and posts; all heavy work happens on the main thread.
 *
 * What is recorded is the action BY VALUE, not the KeyBinding it came from.
 * Posting the pointer was a use-after-free: a config reload frees every
 * binding (config_free_owned), and two keystrokes can sit in the queue at once
 * — press Win+Shift+R and then any bound key before the main thread drains,
 * and the reload runs first, freeing the second message's binding out from
 * under it. Key autorepeat alone gets you there. FIFO ordering does not help,
 * because the reload is itself one of the queued actions.
 *
 * The sequence number travels in lParam and is stored in the slot too, so a
 * burst deep enough to lap the ring is detected on read rather than silently
 * executing whatever overwrote it.
 * =========================================================================== */
#define KB_PENDING_N 64            /* power of two — index is seq & (N-1) */

typedef struct {
    unsigned seq;                  /* 0 = never written */
    unsigned gen;                  /* g.config_gen when this was dispatched */
    Action   action;
    int      arg;
    wchar_t  command[MAX_PATH];
    wchar_t  args[SPAWN_ARGS_MAX];
    wchar_t  cwd[MAX_PATH];
    int      count;                /* vim-style repeat; 0 or 1 = once */
} PendingAction;

/* Bounded copy that does not zero-pad the destination (unlike wcsncpy). Used
 * inside the hook, so it stays cheap: it writes exactly what is there. */
static void pend_copy(wchar_t *dst, size_t cap, const wchar_t *src) {
    if (!cap) return;
    if (!src) { dst[0] = L'\0'; return; }
    size_t n = wcslen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = L'\0';
}

static PendingAction s_pending[KB_PENDING_N];
static unsigned      s_pend_seq = 1;   /* seq 0 is reserved for "empty slot" */

/* Called from the hook, which already holds kb_lock (see kb_hook_proc). */
static void dispatch(KeyBinding *b, DWORD vk, DWORD mods) {
    if (!g.message_window || !b) return;

    unsigned       seq = s_pend_seq++;
    PendingAction *p   = &s_pending[seq & (KB_PENDING_N - 1)];

    p->action = b->action;
    p->arg    = b->arg;
    p->gen    = g.config_gen;
    pend_copy(p->command, MAX_PATH,        b->command);
    pend_copy(p->args,    SPAWN_ARGS_MAX,  b->args);
    pend_copy(p->cwd,     MAX_PATH,        b->cwd);
    p->count  = s_count;
    s_count   = 0;   /* consumed by this action, whatever it turns out to be */
    p->seq = seq;   /* last: the slot is only valid once fully written */

    /* wParam still carries the triggering key (low word) + modifiers (high
     * word) purely so the main thread can log the match — the hook itself must
     * never touch I/O. */
    PostMessageW(g.message_window, WM_MSHELL_ACTION,
                 (WPARAM)((vk & 0xFFFF) | ((mods & 0xFFFF) << 16)),
                 (LPARAM)seq);
}

/* Main-thread side: copy out the action `seq` refers to. False when the ring
 * lapped before we got here, which is the only way to lose a keystroke and is
 * worth saying out loud. */
bool kb_take_pending(unsigned seq, Action *action, int *arg,
                     wchar_t *cmd, size_t cmd_cap,
                     wchar_t *args, size_t args_cap,
                     wchar_t *cwd, size_t cwd_cap, int *count) {
    bool ok = false;

    kb_lock();
    PendingAction *p = &s_pending[seq & (KB_PENDING_N - 1)];
    bool stale_lua = false;
    if (p->seq == seq) {
        /* A Lua binding carries a registry ref, which belongs to the lua_State
         * that created it. If a reload landed between the keypress and now,
         * that state is closed and the ref would index into a different one —
         * so drop it. Everything else was copied by value and is still safe to
         * run, so only Lua calls are gated on the generation. */
        if (p->action == ACTION_LUA_CALL && p->gen != g.config_gen) {
            stale_lua = true;
        } else {
            *action = p->action;
            *arg    = p->arg;
            pend_copy(cmd,  cmd_cap,  p->command);
            pend_copy(args, args_cap, p->args);
            pend_copy(cwd,  cwd_cap,  p->cwd);
            *count = p->count;
            ok = true;
        }
    }
    kb_unlock();

    if (stale_lua)
        log_w(L"input: dropped a Lua keybind queued before the config reload");
    else if (!ok)
        log_err(L"input: dropped a keybind action — more than %d were queued "
                L"before the main thread could run them", KB_PENDING_N);
    return ok;
}

/* ===========================================================================
 * WH_KEYBOARD_LL hook procedure — fast, no blocking calls
 * =========================================================================== */
LRESULT CALLBACK kb_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
    DWORD vk = kb->vkCode;

    /* Our own synthetic keystroke, injected by window_focus() to claim
     * foreground rights (see claim_foreground_rights() in window.c). Let it
     * through untouched and before anything else: swallowing it would deny us
     * the "last input event" credit that is the whole point of sending it, and
     * running it through the modifier tracking below would corrupt our idea of
     * which keys the user is holding. */
    if (kb->dwExtraInfo == MSHELL_INPUT_TAG)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    /* Panic mode: pass everything, bind nothing. Checked here rather than by
     * unhooking so the state is reversible from a reload — and because
     * unhooking from the hook thread while inside the callback is not something
     * to do. Win, Alt+Tab and the Start menu all work again from this point. */
    if (g.panicked)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    /* ---- launcher capture mode ----
     * Checked before the modifier tracking and before every keymap: while the
     * launcher is open it owns the keyboard completely, which is the only way
     * to type into a window that never takes focus.
     *
     * Translation happens HERE rather than in the launcher because ToUnicodeEx
     * needs the keyboard state as it was at this keystroke — by the time the
     * main thread runs, Shift may already be up.
     *
     * Escape is handled by the launcher itself (it closes), so nothing special
     * is needed here; the modifier keys are passed to the tracker below so a
     * Shift held for a capital letter does not desynchronise our idea of what
     * is down. */
    if (g.launcher_open) {
        if (!down) return 1;            /* swallow key-ups too */

        switch (vk) {
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
        case VK_LMENU: case VK_RMENU: case VK_MENU:
            break;                      /* fall through to modifier tracking */
        default: {
            BYTE   ks[256];
            wchar_t buf[8] = {0};
            wchar_t ch = 0;
            if (GetKeyboardState(ks)) {
                /* Our hook has swallowed these, so GetKeyboardState does not
                 * know about them — tell it what we are tracking. */
                ks[VK_SHIFT] = mod_shift ? 0x80 : 0;
                int n = ToUnicodeEx(vk, (UINT)((lParam >> 16) & 0xFF), ks,
                                    buf, 8, 0, GetKeyboardLayout(0));
                if (n == 1) ch = buf[0];
            }
            PostMessageW(g.message_window, WM_MSHELL_CAPTURE_KEY,
                         (WPARAM)vk, (LPARAM)ch);
            return 1;
        }
        }
    }


    /* Any non-Win key seen while Win is held means the Win press was USED (a
     * chord, or a keystroke handled by a modal submap), so the eventual Win-up
     * is not a bare tap. Covers ordinary keys and the modifier keys below. */
    if (down && mod_lwin && vk != VK_LWIN && vk != VK_RWIN)
        win_used = true;

    /* ---- modifier tracking + Win capture ----
     * These touch only file-static modifier flags (and, on Win release, the
     * current-map pointer under the lock); they never read the keymaps, so they
     * return immediately without taking the lock for the common case. */
    switch (vk) {
    case VK_LWIN: case VK_RWIN:
        if (down) {
            if (!mod_lwin) win_used = false;   /* fresh press: tap candidate */
            mod_lwin = true;
            return 1;                          /* swallow Win-DOWN entirely */
        }
        /* Win release. A bare tap (nothing pressed while held) toggles the
         * leader map: from the root it enters the configured leader (if any);
         * from within any submap it backs out to the root. When the tap did land
         * a chord/modal key, win_used is set and we leave the current map alone. */
        mod_lwin = false;
        kb_lock();
        if (!win_used) {
            if (g.current_map == g.root_map) {
                if (g.leader_map) g.current_map = g.leader_map;
            } else {
                g.current_map = g.root_map;
            }
            s_count = 0;              /* leaving the map abandons its count */
        }
        notify_submap();            /* show/hide the hint as the map changes */
        kb_unlock();
        /* Swallow every Win-DOWN (Win is a pure modifier / leader), but let the
         * Win-UP reach the OS. A handful of reserved shortcuts — Win+L, Win+G,
         * Win+Tab — are detected BELOW this hook and latch the Win key "down" in
         * the OS no matter what we return here; if we also eat the Win-up, the
         * OS stays convinced Win is held and bare "k" then fires Win+K. Passing
         * the up through clears that latch. It is a no-op in the normal case:
         * the OS never saw our swallowed Win-down, so a lone up does nothing and
         * never opens the Start menu (that needs an OS-visible down + up with no
         * key between). */
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    /* Left and right are tracked separately and OR'd, so releasing one while
     * the other is still held does not clear the modifier. A single flag made
     * Shift+Shift+release read as "shift is up" while it was still down. */
    case VK_LSHIFT:   mod_lshift = down; mod_shift = mod_lshift || mod_rshift;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_RSHIFT:   mod_rshift = down; mod_shift = mod_lshift || mod_rshift;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_SHIFT:    mod_shift = down;   /* synthetic/unsided */
                      return CallNextHookEx(NULL, nCode, wParam, lParam);

    case VK_LCONTROL: mod_lctrl = down; mod_ctrl = mod_lctrl || mod_rctrl;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_RCONTROL: mod_rctrl = down; mod_ctrl = mod_lctrl || mod_rctrl;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_CONTROL:  mod_ctrl = down;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);

    case VK_LMENU:    mod_lalt = down; mod_alt = mod_lalt || mod_ralt;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_RMENU:    mod_ralt = down; mod_alt = mod_lalt || mod_ralt;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    case VK_MENU:     mod_alt = down;
                      return CallNextHookEx(NULL, nCode, wParam, lParam);
    }

    /* Only act on key-down for ordinary keys. */
    if (!down)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    DWORD mods = current_mods();

    /* Everything below reads/mutates the keymaps, which a config reload may be
     * rebuilding on the main thread — so hold the keymap lock. Single exit via
     * `done` keeps it balanced; `pass` defers CallNextHookEx until after the
     * lock is released (no blocking call while holding the lock). */
    LRESULT result = 1;      /* default: swallow */
    bool    pass   = false;  /* true => CallNextHookEx after unlock */

    kb_lock();

    KeyMap *map   = g.current_map ? g.current_map : g.root_map;
    bool    modal = (map != g.root_map);

    /* ---- modal submap: intercept every key, Win not required ----
     * We're "inside" a submap, reached either by a bare Win tap (the leader) or
     * by a held Win+key that ran enter_submap. Submap bindings are stored with
     * no Win, so match on the modifiers minus Win. The map's `persist` flag,
     * not the Win key, decides whether we stay. */
    if (modal) {
        DWORD kmods   = mods & ~MOD_LWIN;
        DWORD exit_vk = map->exit_vk ? map->exit_vk : VK_ESCAPE;
        KeyBinding *b = keymap_find(map, kmods, vk);

        /* ---- vim-style counts ----
         * A digit typed inside a submap builds a repeat count for the next
         * action: "3j" focuses down three times.
         *
         * It only applies to a digit the map does NOT bind, which is what keeps
         * it from breaking any existing config — the shipped one puts the
         * desktops on 1..9 inside the `go` map, and those keep working exactly
         * as before because the binding is found first. A leading 0 is likewise
         * left alone, so a desktop named "0" stays reachable; 0 only counts once
         * a count is already being built ("10j").
         *
         * Root-map chords are deliberately excluded: they need Win held, and
         * "Win+3 Win+j" is not a gesture anyone wants. Counts belong to the
         * bare-key mode, which is what a submap is. */
        if (!b && kmods == 0 && vk >= '0' && vk <= '9' &&
            (s_count > 0 || vk > '0')) {
            int d = (int)(vk - '0');
            s_count = (s_count > 99) ? 999 : s_count * 10 + d;  /* cap, no wrap */
            goto done;                    /* swallowed; the map is unchanged */
        }

        if (map->persist) {
            /* Persisting: the exit key leaves (checked first so it can't be
             * shadowed by a binding); a bound key fires and, unless flagged
             * terminal, keeps us here; any other key is ignored — you stay. */
            if (vk == exit_vk) {
                g.current_map = g.root_map;
                s_count = 0;          /* abandon a half-typed count too */
            } else if (b) {
                if (b->action == ACTION_ENTER_SUBMAP) {
                    g.current_map = b->submap;
                } else {
                    dispatch(b, vk, mods);
                    if (b->terminal) g.current_map = g.root_map;
                }
            }
        } else {
            /* Unpersisting: the next key disables the map no matter what.
             * Entering a nested submap is the one move that keeps us modal;
             * a bound key fires then drops to root; anything else just drops. */
            if (b && b->action == ACTION_ENTER_SUBMAP) {
                g.current_map = b->submap;
            } else {
                if (b) dispatch(b, vk, mods);
                g.current_map = g.root_map;
            }
        }
        goto done;                       /* modal mode consumes every key */
    }

    /* ================= at the root map ================= */

    /* ---- block leftover Windows system shortcuts ----
     * Win+* combos are already dead (we ate the Win key). These are the
     * remaining OS shortcuts a low-level hook CAN intercept. We deliberately
     * never touch Ctrl+Shift+Esc (Task Manager) — it's the recovery hatch —
     * and Ctrl+Alt+Del is a kernel SAS that no hook can block. (Modal submaps,
     * above, swallow every key already, so this only needs to run at root.) */
    if (g.block_system_keys) {
        bool ctrl_shift_esc = (vk == VK_ESCAPE) && mod_ctrl && mod_shift;
        if (!ctrl_shift_esc) {
            if (mod_alt && (vk == VK_TAB ||      /* Alt+Tab   task switch  */
                            vk == VK_ESCAPE ||   /* Alt+Esc   cycle windows*/
                            vk == VK_SPACE))     /* Alt+Space system menu  */
                goto done;                       /* swallow */
            if (mod_ctrl && vk == VK_ESCAPE)     /* Ctrl+Esc  open Start   */
                goto done;                       /* swallow */
        }
    }

    /* Root bindings are Win+key chords; without Win, a keystroke is plain
     * typing and passes through untouched. */
    if (!mod_lwin) { pass = true; goto done; }

    {
        /* NB: no logging in the hook — it must never block (see file header and
         * dispatch()). The match is logged on the main thread in the
         * WM_MSHELL_ACTION handler instead. */
        KeyBinding *b = keymap_find(map, mods, vk);
        if (b) {
            if (b->action == ACTION_ENTER_SUBMAP) {
                g.current_map = b->submap;   /* enter the modal submap */
            } else {
                dispatch(b, vk, mods);
                if (b->terminal) g.current_map = g.root_map;
            }
        }
        /* Matched, or Win + unbound key: swallow either way so the leftover
         * Windows shortcut can't fire (result stays 1). */
    }

done:
    notify_submap();     /* enter/leave submap → refresh the hint (guarded) */
    kb_unlock();
    if (pass)
        return CallNextHookEx(NULL, nCode, wParam, lParam);
    return result;
}

/* ===========================================================================
 * Directional navigation helpers
 *
 * Focus/move by direction picks the geometrically nearest window in that
 * direction (by window-rect centers), which matches what the eye expects in
 * grid and master-stack layouts. When nothing lies in the requested direction
 * we fall back to simple prev/next cycling so a keypress is never a no-op.
 * =========================================================================== */
typedef enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN } Direction;

static Direction action_to_dir(Action a) {
    switch (a) {
    case ACTION_FOCUS_LEFT:  case ACTION_MOVE_LEFT:  return DIR_LEFT;
    case ACTION_FOCUS_RIGHT: case ACTION_MOVE_RIGHT: return DIR_RIGHT;
    case ACTION_FOCUS_UP:    case ACTION_MOVE_UP:    return DIR_UP;
    default:                                          return DIR_DOWN;
    }
}

static bool center_of(HWND hwnd, POINT *out) {
    RECT r;
    if (!hwnd || !IsWindow(hwnd) || !GetWindowRect(hwnd, &r)) return false;
    out->x = (r.left + r.right) / 2;
    out->y = (r.top + r.bottom) / 2;
    return true;
}

/* Index (into dt->windows) of the nearest window in `dir` from `from`, or -1. */
static int neighbor_in_dir(Desktop *dt, int from, Direction dir) {
    POINT fc;
    if (from < 0 || from >= dt->count || !center_of(dt->windows[from], &fc))
        return -1;

    int  best = -1;
    long best_score = 0;
    for (int i = 0; i < dt->count; i++) {
        if (i == from) continue;
        POINT c;
        if (!center_of(dt->windows[i], &c)) continue;

        long dx = c.x - fc.x, dy = c.y - fc.y;
        bool ok = false;
        switch (dir) {
        case DIR_LEFT:  ok = dx < 0 && labs(dx) >= labs(dy); break;
        case DIR_RIGHT: ok = dx > 0 && labs(dx) >= labs(dy); break;
        case DIR_UP:    ok = dy < 0 && labs(dy) >= labs(dx); break;
        case DIR_DOWN:  ok = dy > 0 && labs(dy) >= labs(dx); break;
        }
        if (!ok) continue;

        long score = dx * dx + dy * dy;   /* squared distance */
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

/* Resolve a directional target, cycling as a fallback. `cycle_prev` decides
 * which way the fallback goes. Returns an index into dt->windows. */
static int resolve_target(Desktop *dt, int from, Action action, bool cycle_prev) {
    int target = -1;
    if (dt->layout != LAYOUT_MONOCLE)
        target = neighbor_in_dir(dt, from, action_to_dir(action));
    if (target < 0)
        target = cycle_prev ? (from - 1 + dt->count) % dt->count
                            : (from + 1) % dt->count;
    return target;
}

/* ===========================================================================
 * Multi-monitor helpers
 * =========================================================================== */

/* Move focus to a window on the next/prev monitor that has one. */
static void focus_monitor(int delta) {
    if (g.monitor_count < 2) return;
    Desktop *dt = desktop_current();
    HWND focus  = desktop_get_focused();
    int  cur    = focus ? monitor_of_window(focus) : g.focused_monitor;

    for (int step = 1; step <= g.monitor_count; step++) {
        int m = (((cur + delta * step) % g.monitor_count) + g.monitor_count)
                % g.monitor_count;
        for (int i = 0; i < dt->count; i++) {
            HWND h = dt->windows[i];
            if (!h || !IsWindow(h)) continue;
            if (monitor_of_window(h) == m) {
                dt->focused = i;
                g.focused_monitor = m;
                window_focus(h);
                if (dt->layout == LAYOUT_MONOCLE) tile_current();
                return;
            }
        }
    }
}

/* Reassign the focused window to the next/prev monitor and re-tile both. */
static void move_focused_to_monitor(int delta) {
    if (g.monitor_count < 2) return;
    HWND focus = desktop_get_focused();
    ManagedWindow *mw = window_find(focus);
    if (!mw) return;

    int cur = mw->monitor;
    if (cur < 0 || cur >= g.monitor_count) cur = 0;
    window_set_monitor(mw, (((cur + delta) % g.monitor_count) +
                            g.monitor_count) % g.monitor_count);
    g.focused_monitor = mw->monitor;
    tile_current();
    if (focus) window_focus(focus);
}

/* ---------------------------------------------------------------------------
 * Move or resize a FLOATING window by one step.
 *
 * The step is a design-pixel constant scaled to the window's own monitor, so
 * one press covers the same visible distance on a 100% and a 200% display.
 * Resizing pulls the edge the direction names: resize_left narrows, resize_
 * right widens, which reads the same way the tiled inc/dec_master pair does.
 *
 * Geometry is written through window_set_pos (the helper-aware wrapper) and
 * recorded in applied_rect, so the drift detector recognises the move as ours
 * and does not treat it as the window escaping.
 * --------------------------------------------------------------------------- */
#define FLOAT_STEP 40   /* design px at 96 DPI */

static void float_nudge(ManagedWindow *mw, Action action, bool resize) {
    RECT r;
    if (!mw || !window_frame_rect(mw->hwnd, &r)) return;

    int step = MulDiv(FLOAT_STEP, (int)monitor_dpi(mw->monitor), 96);
    int dx = 0, dy = 0;
    switch (action) {
    case ACTION_MOVE_LEFT:  case ACTION_RESIZE_LEFT:  dx = -step; break;
    case ACTION_MOVE_RIGHT: case ACTION_RESIZE_RIGHT: dx =  step; break;
    case ACTION_MOVE_UP:    case ACTION_RESIZE_UP:    dy = -step; break;
    case ACTION_MOVE_DOWN:  case ACTION_RESIZE_DOWN:  dy =  step; break;
    default: return;
    }

    int w = r.right - r.left, h = r.bottom - r.top;
    int x = r.left,           y = r.top;

    if (resize) {
        w += dx;
        h += dy;
        /* Never let a window be resized into something unclickable. */
        if (w < g.min_win_w) w = g.min_win_w;
        if (h < g.min_win_h) h = g.min_win_h;
    } else {
        x += dx;
        y += dy;
    }

    RECT want = { x, y, x + w, y + h };
    RECT adj  = window_adjust_for_frame(mw->hwnd, want);
    window_set_pos(mw->hwnd, adj.left, adj.top,
                   adj.right - adj.left, adj.bottom - adj.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);

    mw->applied_rect = want;
    mw->has_applied  = true;
    window_set_monitor(mw, monitor_of_window(mw->hwnd));
    border_refresh();
}

/* ---------------------------------------------------------------------------
 * Which actions a repeat count applies to.
 *
 * A whitelist rather than a blacklist, deliberately: the failure mode of
 * guessing wrong is "3q quit three times" or "3<Return> launched three
 * terminals", so a new action has to opt in rather than inherit repetition it
 * was never considered for. Everything here is motion, ordering or sizing —
 * idempotent-ish, visibly reversible, and meaningless to do once when you asked
 * for five.
 * --------------------------------------------------------------------------- */
bool action_is_repeatable(Action action) {
    switch (action) {
    case ACTION_FOCUS_LEFT:  case ACTION_FOCUS_DOWN:
    case ACTION_FOCUS_UP:    case ACTION_FOCUS_RIGHT:
    case ACTION_FOCUS_NEXT:  case ACTION_FOCUS_PREV:
    case ACTION_MOVE_LEFT:   case ACTION_MOVE_DOWN:
    case ACTION_MOVE_UP:     case ACTION_MOVE_RIGHT:
    case ACTION_RESIZE_LEFT: case ACTION_RESIZE_DOWN:
    case ACTION_RESIZE_UP:   case ACTION_RESIZE_RIGHT:
    case ACTION_NEXT_DESKTOP: case ACTION_PREV_DESKTOP:
    case ACTION_FOCUS_MONITOR_NEXT: case ACTION_FOCUS_MONITOR_PREV:
    case ACTION_MOVE_TO_MONITOR_NEXT: case ACTION_MOVE_TO_MONITOR_PREV:
    case ACTION_INC_MASTER:  case ACTION_DEC_MASTER:
    case ACTION_INC_NMASTER: case ACTION_DEC_NMASTER:
    case ACTION_INC_CFACT:   case ACTION_DEC_CFACT:
    case ACTION_CYCLE_LAYOUT:
    case ACTION_VOLUME_UP:   case ACTION_VOLUME_DOWN:
        return true;
    default:
        return false;
    }
}

/* Grow/shrink the focused window within its stack (cfact). */
static void adjust_cfact(HWND focus, float delta) {
    ManagedWindow *mw = window_find(focus);
    if (!mw) return;
    float c = mw->cfact; if (c <= 0.f) c = 1.f;
    mw->cfact = clamp_f(c + delta, 0.25f, 4.0f);
    tile_current();
}

/* ===========================================================================
 * Action dispatch
 * =========================================================================== */
/* ===========================================================================
 * The one place mshell launches a program.
 *
 * ShellExecuteW rather than CreateProcessW on purpose: it resolves PATH (so a
 * bare "firefox.exe" works) and it can start a .lnk shortcut, which several
 * apps require because their real binary sits in a versioned folder and moves
 * between updates. Arguments must therefore be a separate string — that is the
 * shape of the API — which is exactly why they could not be expressed before.
 * =========================================================================== */
bool spawn_command(const wchar_t *cmd, const wchar_t *args,
                   const wchar_t *cwd, const wchar_t *ctx) {
    if (!cmd || !cmd[0]) {
        log_err(L"%ls: nothing to launch (empty command)", ctx ? ctx : L"spawn");
        return false;
    }

    const wchar_t *params = (args && args[0]) ? args : NULL;
    /* NULL means "inherit ours", which for a Winlogon-launched shell is
     * C:\Windows\system32 — rarely what you want, but never surprising. */
    const wchar_t *dir    = (cwd && cwd[0]) ? cwd : NULL;

    INT_PTR code = (INT_PTR)ShellExecuteW(NULL, L"open", cmd, params,
                                          dir, SW_SHOWNORMAL);
    if (code <= 32) {   /* the documented failure range */
        log_err(L"%ls: FAILED to launch '%ls'%ls%ls (code %lld) — not on PATH "
                L"or not installed? Try a full path.",
                ctx ? ctx : L"spawn", cmd,
                params ? L" " : L"", params ? params : L"",
                (long long)code);
        return false;
    }

    log_w(L"%ls: launched '%ls'%ls%ls", ctx ? ctx : L"spawn", cmd,
          params ? L" " : L"", params ? params : L"");
    return true;
}

void execute_action(Action action, int arg, const wchar_t *command,
                    const wchar_t *args, const wchar_t *cwd) {
    Desktop *dt  = desktop_current();
    HWND     focus = desktop_get_focused();
    int      fi   = dt->focused;   /* focused index in desktop array */

    log_w(L"execute_action: action=%d arg=%d (desktop '%ls', %d windows)",
          (int)action, arg, dt->name, dt->count);

    switch (action) {

    /* -- spawn a program ----------------------------------------------- */
    /* -- run a Lua function from the config ----------------------------
     * `arg` is a registry ref. It was checked against the config generation
     * before we got here (kb_take_pending), so the VM it belongs to is the
     * live one. */
    case ACTION_LUA_CALL:
        lua_run_ref(arg);
        break;

    case ACTION_SPAWN:
        if (command && command[0]) {
            /* Launch detached; the WinEvent hook picks the new window up and
             * tiles it when it appears. */
            spawn_command(command, args, cwd, L"keybind");
        } else {
            log_w(L"spawn: no command set on binding");
        }
        break;

    /* -- focus movement (directional, vim keys) ------------------------ */
    case ACTION_FOCUS_LEFT:
    case ACTION_FOCUS_DOWN:
    case ACTION_FOCUS_UP:
    case ACTION_FOCUS_RIGHT: {
        if (dt->count < 2) {
            log_w(L"  focus: only %d window(s) — nothing to move to", dt->count);
            break;
        }
        bool prev = (action == ACTION_FOCUS_LEFT || action == ACTION_FOCUS_UP);
        dt->focused = resolve_target(dt, fi, action, prev);
        window_focus(dt->windows[dt->focused]);
        /* Monocle only shows the focused window — re-tile to reveal it. */
        if (dt->layout == LAYOUT_MONOCLE) tile_current();
        break;
    }

    /* -- focus cycling (next/prev, layout-independent) ----------------- */
    case ACTION_FOCUS_NEXT:
    case ACTION_FOCUS_PREV: {
        if (dt->count < 2) break;
        bool prev = (action == ACTION_FOCUS_PREV);
        dt->focused = prev ? (fi - 1 + dt->count) % dt->count
                           : (fi + 1) % dt->count;
        window_focus(dt->windows[dt->focused]);
        if (dt->layout == LAYOUT_MONOCLE) tile_current();
        break;
    }

    /* -- window swap (shift + vim), or a literal move when floating -----
     * A tiled window has no position of its own — the layout owns it — so
     * "move" there means swapping places with the neighbour in that direction.
     * A floating window does have one, and the same keys should move it, which
     * is what they now do. */
    case ACTION_MOVE_LEFT:
    case ACTION_MOVE_DOWN:
    case ACTION_MOVE_UP:
    case ACTION_MOVE_RIGHT: {
        ManagedWindow *mw = focus ? window_find(focus) : NULL;
        if (mw && mw->is_floating) {
            float_nudge(mw, action, false);
            break;
        }
        if (dt->layout != LAYOUT_MONOCLE && dt->count > 1 && focus) {
            bool prev = (action == ACTION_MOVE_LEFT || action == ACTION_MOVE_UP);
            int target = resolve_target(dt, fi, action, prev);
            hwnd_swap(&dt->windows[fi], &dt->windows[target]);
            dt->focused = target;
            tile_current();
        }
        break;
    }

    /* -- resize a floating window --------------------------------------
     * Tiled geometry belongs to the layout (inc_master / cfact are its knobs),
     * so this is deliberately a no-op on a tiled window rather than quietly
     * doing something else. */
    case ACTION_RESIZE_LEFT:
    case ACTION_RESIZE_DOWN:
    case ACTION_RESIZE_UP:
    case ACTION_RESIZE_RIGHT: {
        ManagedWindow *mw = focus ? window_find(focus) : NULL;
        if (mw && mw->is_floating) float_nudge(mw, action, true);
        break;
    }

    /* -- always on top --------------------------------------------------
     * The z-order pass owns the actual promotion, so this only flips the
     * intent and asks for a pass. */
    case ACTION_TOGGLE_ALWAYS_ON_TOP: {
        ManagedWindow *mw = focus ? window_find(focus) : NULL;
        if (mw) {
            mw->always_on_top = !mw->always_on_top;
            log_msg(LOG_INFO, L"always-on-top %ls",
                    mw->always_on_top ? L"on" : L"off");
            window_enforce_zorder();
        }
        break;
    }

    /* -- back to the previously focused window --------------------------
     * The window-level counterpart of last_desktop, and a toggle for the same
     * reason: focusing the old window pushes the current one to the front of
     * the history, so pressing it twice returns you. */
    case ACTION_LAST_WINDOW: {
        HWND prev = desktop_last_window();
        if (prev) {
            desktop_focus_update(prev);
            window_focus(prev);
            if (dt->layout == LAYOUT_MONOCLE) tile_current();
        }
        break;
    }

    /* -- desktop switching ---------------------------------------------
     * The target is a NAME (in `command`), and it does not have to exist:
     * switching to a name nothing is using creates that desktop. */
    case ACTION_SWITCH_DESKTOP:
        if (command && command[0]) desktop_switch(command);
        break;

    case ACTION_MOVE_TO_DESKTOP:
        if (command && command[0] && focus) desktop_move_window(focus, command);
        break;

    /* Back to the desktop we came from; press twice to end up where you were. */
    case ACTION_LAST_DESKTOP:
        desktop_switch_last();
        break;

    /* Step through the desktops that exist right now — how you reach one you
     * created on the fly and never bound a key to. */
    case ACTION_NEXT_DESKTOP: desktop_cycle(+1); break;
    case ACTION_PREV_DESKTOP: desktop_cycle(-1); break;

    /* -- monitors ------------------------------------------------------ */
    case ACTION_FOCUS_MONITOR_NEXT:   focus_monitor(+1); break;
    case ACTION_FOCUS_MONITOR_PREV:   focus_monitor(-1); break;
    case ACTION_MOVE_TO_MONITOR_NEXT: move_focused_to_monitor(+1); break;
    case ACTION_MOVE_TO_MONITOR_PREV: move_focused_to_monitor(-1); break;

    /* -- window lifetime ----------------------------------------------- */
    case ACTION_CLOSE:
        if (focus) window_close(focus);
        break;

    case ACTION_KILL:
        if (focus) window_kill(focus);
        break;

    case ACTION_MINIMIZE:
        if (focus && dt->count > 0) {
            ShowWindow(focus, SW_MINIMIZE);
            /* dt->focused still names the window we just minimized. Hand the
             * keyboard to the next sibling that is actually on screen —
             * otherwise focus sits on something invisible and every
             * directional keybind computes from the wrong place. */
            for (int i = 1; i <= dt->count; i++) {
                int  j = (fi + i) % dt->count;
                HWND h = dt->windows[j];
                if (h && IsWindow(h) && !IsIconic(h)) {
                    dt->focused = j;
                    window_focus(h);
                    break;
                }
            }
            tile_current();
        }
        break;

    /* There is no taskbar under mshell, so a minimized window has nothing to
     * click and this is the only way back. Takes the first one in the
     * desktop's window order, so repeating it walks through them. */
    case ACTION_RESTORE:
        for (int i = 0; i < dt->count; i++) {
            HWND h = dt->windows[i];
            if (h && IsWindow(h) && IsIconic(h)) {
                ShowWindow(h, SW_RESTORE);
                dt->focused = i;
                tile_current();
                window_focus(h);
                break;
            }
        }
        break;

    /* -- sticky: follow me to every desktop ---------------------------- */
    case ACTION_TOGGLE_STICKY: {
        ManagedWindow *mw = window_find(focus);
        if (!mw) break;
        mw->sticky = !mw->sticky;
        log_err(L"sticky: %p is %ls", (void *)focus,
                mw->sticky ? L"now on every desktop" : L"back on one desktop");
        bar_refresh();
        break;
    }

    /* -- scratchpad ----------------------------------------------------
     * A single window you summon anywhere and dismiss again — a terminal, a
     * notes app. There is no spawn-and-track magic: you mark a window you
     * already have, which is predictable and needs no guessing about which
     * window a launch produced. */
    case ACTION_MARK_SCRATCHPAD: {
        ManagedWindow *mw = window_find(focus);
        if (!mw) break;
        /* Only one at a time — a second mark moves the role. */
        for (int i = 0; i < g.managed_count; i++) g.managed[i].scratchpad = false;
        mw->scratchpad  = true;
        mw->is_floating = true;   /* it overlays, it does not join the grid */
        window_set_floating(focus, true);
        log_err(L"scratchpad: %p is now the scratchpad window", (void *)focus);
        tile_current();
        break;
    }

    case ACTION_TOGGLE_SCRATCHPAD: {
        ManagedWindow *sp = NULL;
        for (int i = 0; i < g.managed_count; i++)
            if (g.managed[i].scratchpad) { sp = &g.managed[i]; break; }

        if (!sp) {
            log_err(L"scratchpad: none marked yet — focus a window and use "
                    L"the 'mark_scratchpad' action first");
            break;
        }
        if (!IsWindow(sp->hwnd)) { sp->scratchpad = false; break; }

        bool here    = (sp->desktop_id == g.current_desktop_id);
        bool showing = here && window_on_screen(sp);

        events_suppress_begin();
        if (showing) {
            window_hide(sp);
        } else {
            /* Summon it onto the desktop you are looking at, rather than
             * making you go to where it happens to live. */
            sp->desktop_id = g.current_desktop_id;
            sp->app_hidden = false;
            window_show(sp);
        }
        events_suppress_end();

        if (!showing) {
            SetWindowPos(sp->hwnd, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            window_focus(sp->hwnd);
        }
        tile_current();
        break;
    }

    /* -- zoom: dwm's swap-with-master ----------------------------------
     * Unlike promote_master, this is a TOGGLE. From the stack it swaps you into
     * the master slot; pressed again from master it swaps you back out to where
     * the old master went, so the pair alternates. */
    case ACTION_ZOOM:
        if (dt->count > 1 && focus) {
            int other = (fi == 0) ? 1 : 0;
            hwnd_swap(&dt->windows[fi], &dt->windows[other]);
            dt->focused = other;
            tile_current();
            window_focus(dt->windows[other]);
        }
        break;

    /* -- float --------------------------------------------------------- */
    case ACTION_TOGGLE_FLOAT: {
        ManagedWindow *mw = window_find(focus);
        if (!mw) break;
        if (mw->tracked_only) {
            /* A tracked window is floating by construction, so the toggle's
             * first job is promotion into full management — which lands it in
             * the grid unless a rule or the desktop floats it. Not gated on
             * FLOAT_NEVER: promoting is not floating. */
            window_promote(focus);
            tile_current();
        } else if (g.float_policy != FLOAT_NEVER) {
            /* FLOAT_NEVER means every window stays tiled — ignore the toggle. */
            window_set_floating(focus, !mw->is_floating);
            tile_current();
        }
        break;
    }

    /* -- fullscreen ---------------------------------------------------- */
    case ACTION_FULLSCREEN:
        if (focus) window_set_fullscreen(focus, FS_WINDOW);
        break;

    case ACTION_FULLSCREEN_CONTENT:
        if (focus) window_set_fullscreen(focus, FS_CONTENT);
        break;

    case ACTION_FULLSCREEN_BOTH:
        if (focus) window_set_fullscreen(focus, FS_BOTH);
        break;

    /* -- layout -------------------------------------------------------- */
    case ACTION_LAYOUT_TILING:   dt->layout = LAYOUT_TILING;   tile_current(); session_save(); break;
    case ACTION_LAYOUT_MONOCLE:  dt->layout = LAYOUT_MONOCLE;  tile_current(); session_save(); break;
    case ACTION_LAYOUT_GRID:     dt->layout = LAYOUT_GRID;     tile_current(); session_save(); break;
    case ACTION_LAYOUT_SPIRAL:   dt->layout = LAYOUT_SPIRAL;   tile_current(); session_save(); break;
    case ACTION_LAYOUT_CENTERED: dt->layout = LAYOUT_CENTERED; tile_current(); session_save(); break;
    case ACTION_LAYOUT_BSTACK:   dt->layout = LAYOUT_BSTACK;   tile_current(); session_save(); break;
    case ACTION_LAYOUT_COLUMNS:  dt->layout = LAYOUT_COLUMNS;  tile_current(); session_save(); break;

    case ACTION_CYCLE_LAYOUT: {
        /* The cycle covers the DYNAMIC layouts — the seven that are a pure
         * function of the window list, where cycling past one you did not want
         * costs nothing. BSP is not one of them: its structure is the record of
         * where you were each time a window opened, so arriving in it by
         * pressing Space one time too many drops you into a tree you did not
         * build, and pressing Space again abandons it. It has its own binding
         * (`layout_bsp`, and the `b` submap) for the same reason.
         *
         * Wrapping on LAYOUT_BSP rather than LAYOUT_COUNT is what excludes it,
         * and it is why the enum keeps BSP last before LAYOUT_COUNT. A desktop
         * already in BSP still cycles OUT — into tiling, the first of the
         * dynamic ones — so the key is never a dead end. */
        Layout next = (Layout)(dt->layout + 1);
        if (next >= LAYOUT_BSP) next = LAYOUT_TILING;
        dt->layout = next;
        tile_current();
        session_save();
        break;
    }

    /* -- number of master windows -------------------------------------- */
    case ACTION_INC_NMASTER:
        dt->n_master = clamp_i(dt->n_master + 1, 1, dt->count > 0 ? dt->count : 1);
        tile_current();
        session_save();
        break;

    case ACTION_DEC_NMASTER:
        dt->n_master = clamp_i(dt->n_master - 1, 1, 20);
        tile_current();
        session_save();
        break;

    /* -- per-window size within the stack (cfact) ---------------------- */
    case ACTION_INC_CFACT:  adjust_cfact(focus, +0.10f); break;
    case ACTION_DEC_CFACT:  adjust_cfact(focus, -0.10f); break;
    case ACTION_RESET_CFACT: {
        ManagedWindow *mw = window_find(focus);
        if (mw) { mw->cfact = 1.0f; tile_current(); }
        break;
    }

    case ACTION_PROMOTE_MASTER:
        if (focus && fi > 0 && dt->count > 1) {
            /* move focused window to index 0 (master) */
            HWND f = dt->windows[fi];
            memmove(&dt->windows[1], &dt->windows[0],
                    (size_t)fi * sizeof(HWND));
            dt->windows[0]  = f;
            dt->focused     = 0;
            tile_current();
        }
        break;

    case ACTION_INC_MASTER:
        dt->master_ratio = clamp_f(dt->master_ratio + 0.05f, 0.2f, 0.9f);
        tile_current();
        session_save();
        break;

    case ACTION_DEC_MASTER:
        dt->master_ratio = clamp_f(dt->master_ratio - 0.05f, 0.2f, 0.9f);
        tile_current();
        session_save();
        break;

    /* ACTION_ENTER_SUBMAP is deliberately absent: entering a submap is decided
     * and applied by the hook itself (all three of its paths assign
     * g.current_map directly), so it is never dispatched here. The case that
     * used to sit here was unreachable — and wrote g.current_map from THIS
     * thread with no kb_lock, racing the hook that owns it. */

    /* -- meta ---------------------------------------------------------- */
    case ACTION_RELOAD:
        config_reload();   /* clears panic mode, wherever the reload came from */
        tile_current();
        whichkey_hide();   /* colors/enabled may have changed; drop any stale hint */
        log_w(L"Config reloaded");
        break;

    /* -- session / power ------------------------------------------------ */
    case ACTION_LOCK:      system_lock();      break;
    case ACTION_LOGOFF:    system_logoff();    break;
    case ACTION_REBOOT:    system_reboot();    break;
    case ACTION_SHUTDOWN:  system_shutdown();  break;
    case ACTION_SLEEP:     system_sleep();     break;
    case ACTION_HIBERNATE: system_hibernate(); break;

    /* -- media ----------------------------------------------------------- */
    case ACTION_VOLUME_UP:   case ACTION_VOLUME_DOWN:
    case ACTION_VOLUME_MUTE: case ACTION_MEDIA_PLAY:
    case ACTION_MEDIA_NEXT:  case ACTION_MEDIA_PREV:
    case ACTION_MEDIA_STOP:
        system_media_key(action);
        break;

    /* -- screenshots ----------------------------------------------------- */
    case ACTION_SCREENSHOT:        screenshot_screen(); break;
    case ACTION_SCREENSHOT_WINDOW: screenshot_window(); break;

    case ACTION_NOTIFY:
        if (command && command[0]) notify_show(command, NOTIFY_INFO, 4000);
        break;

    /* Go to whatever asked for attention, wherever it is — including a desktop
     * you are not on, which is the case the flag exists for. */
    case ACTION_LAUNCHER:
        launcher_open();
        break;

    case ACTION_TOGGLE_BAR:
        bar_toggle();
        break;

    /* The focused monitor, not the primary: on a two-monitor desk the display
     * you want HDR on is the one you are looking at. */
    case ACTION_TOGGLE_HDR:
        display_toggle_hdr(g.focused_monitor);
        break;

    case ACTION_CYCLE_REFRESH:
        display_cycle_refresh(g.focused_monitor, arg >= 0 ? +1 : -1);
        break;

    /* -- manual (BSP) tiling ------------------------------------------- */
    case ACTION_SPLIT_H: layout_tree_set_split(SPLIT_H); break;
    case ACTION_SPLIT_V: layout_tree_set_split(SPLIT_V); break;
    case ACTION_ROTATE_SPLIT:   layout_tree_rotate();    break;
    case ACTION_TOGGLE_TABBED:  layout_tree_set_container(SPLIT_TABBED);  break;
    case ACTION_TOGGLE_STACKED: layout_tree_set_container(SPLIT_STACKED); break;
    case ACTION_CONTAINER_NEXT: layout_tree_cycle_container(+1); break;
    case ACTION_CONTAINER_PREV: layout_tree_cycle_container(-1); break;
    case ACTION_SPLIT_GROW:     layout_tree_resize(+0.05f); break;
    case ACTION_SPLIT_SHRINK:   layout_tree_resize(-0.05f); break;
    case ACTION_LAYOUT_BSP:     dt->layout = LAYOUT_BSP; tile_current(); session_save(); break;

    case ACTION_JUMP_URGENT: {
        for (int i = 0; i < g.managed_count; i++) {
            ManagedWindow *mw = &g.managed[i];
            if (!mw->urgent || !IsWindow(mw->hwnd)) continue;

            Desktop *d = desktop_by_id(mw->desktop_id);
            if (d && mw->desktop_id != g.current_desktop_id)
                desktop_switch(d->name);

            desktop_focus_update(mw->hwnd);
            window_focus(mw->hwnd);
            break;
        }
        break;
    }

    case ACTION_QUIT:
        g.running = false;
        PostQuitMessage(0);
        break;

    /* Returns at once: the fetch and the install run on their own thread and
     * report by notification, because this one must not block the thread that
     * services keybinds for the length of a download. */
    case ACTION_UPDATE:
        update_install_async();
        break;

    /* -- panic --------------------------------------------------------
     * The escape hatch that does not require Task Manager. Explorer comes up
     * alongside us (it coexists fine — that is exactly what --test mode is),
     * every window we hid is given back, and the keyboard hook stops swallowing
     * anything, so Win, Alt+Tab and the Start menu all work again.
     *
     * It deliberately does NOT quit: as the shell, exiting ends the session,
     * which is the outcome someone reaching for a panic key is trying to avoid.
     * mshell stays running and can be restored with reload, or quit
     * deliberately once you have a desktop to land on. */
    case ACTION_PANIC:
        /* No keybinding can undo this — the hook stops matching, which is the
         * point — so the way back is a reload from outside: `mshell.exe --msg
         * reload`, or just saving init.lua if auto-reload is on. */
        log_err(L"PANIC: starting explorer.exe and releasing the keyboard. "
                L"mshell is still running but no longer binding any key. "
                L"To undo: run `mshell.exe --msg reload`, or save your "
                L"init.lua if auto-reload is enabled.");
        window_restore_all_visibility();
        launcher_close();               /* never leave the keyboard captured */
        g.panicked = true;              /* checked by the hook, first thing */
        kb_reset_state();               /* drop any half-held modifier */
        whichkey_hide();
        spawn_command(L"explorer.exe", NULL, NULL, L"panic");
        break;

    default:
        break;
    }
}

/* ===========================================================================
 * Init / shutdown — the hook lives on its own thread
 *
 * A WH_KEYBOARD_LL hook fires on the thread that installed it, serviced by that
 * thread's message pump. We give it a thread that does nothing else, so it is
 * never starved by focus/tiling work on the main thread (which is what let a
 * held-Win autorepeat leak and stick — see the file header).
 * =========================================================================== */
/* ---------------------------------------------------------------------------
 * WH_MOUSE_LL — only while Mod+drag is enabled.
 *
 * It shares the keyboard hook's thread, so it is on the same LowLevelHooksTimeout
 * budget: the first thing it does for a non-modifier-held event is return. That
 * matters because this fires on every pixel of every mouse movement, and the
 * thread it runs on is the one that has to answer keystrokes.
 * --------------------------------------------------------------------------- */
static LRESULT CALLBACK mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    /* Cheapest possible early-out: unless a drag is in flight, nothing here is
     * interesting without the modifier. mod_lwin is the hook thread's own
     * state, already tracked for the keyboard. */
    if (!g.mod_drag_hwnd && !mod_lwin)
        return CallNextHookEx(NULL, nCode, wParam, lParam);

    MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;
    if (mouse_mod_drag_event(wParam, ms->pt, mod_lwin))
        return 1;                       /* swallow: the app must not see it */

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

/* Install or remove the mouse hook to match the setting. Must run ON the hook
 * thread, so it is driven by a posted message rather than called directly. */
static void mouse_sync_hook_here(void) {
    if (g.mouse_mod_drag && !g.mouse_hook) {
        g.mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc,
                                         g.hinst, 0);
        if (g.mouse_hook) log_msg(LOG_INFO, L"mouse: Mod+drag on");
        else log_msg(LOG_WARN, L"SetWindowsHookEx(WH_MOUSE_LL) failed: %lu",
                     GetLastError());
    } else if (!g.mouse_mod_drag && g.mouse_hook) {
        UnhookWindowsHookEx(g.mouse_hook);
        g.mouse_hook    = NULL;
        g.mod_drag_hwnd = NULL;
        log_msg(LOG_INFO, L"mouse: Mod+drag off");
    }
}

/* Ask the hook thread to reconcile its mouse hook with the config. */
void mouse_sync_hook(void) {
    if (g_kb_thread_id) PostThreadMessageW(g_kb_thread_id, WM_MSHELL_MOUSE, 0, 0);
}

static DWORD WINAPI kb_thread_proc(LPVOID param) {
    (void)param;

    /* Run this pump at the top scheduling priority. A WH_KEYBOARD_LL event is
     * silently dropped by the OS — passed straight to the next hook / the app,
     * with our return value ignored — if the thread that installed it is not
     * scheduled to service it within LowLevelHooksTimeout (default 300 ms). A
     * fullscreen game commonly runs its own threads at an elevated priority and
     * saturates the CPU/GPU, which starves a NORMAL-priority thread for far
     * longer than that. The result is every keybind dying for as long as the
     * game is running: the hook is never uninstalled (so it comes back the
     * instant the game closes, with no restart), it is just repeatedly timed
     * out. A dedicated thread only escapes starvation by *mshell's own* work,
     * not by a higher-priority game — so lift it above that game. This thread
     * does nothing but a fast keymap lookup, so TIME_CRITICAL here costs no real
     * CPU while guaranteeing it wakes ahead of the game to swallow each key. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    /* Install the hook FROM this thread so its callbacks run here. */
    g.kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook_proc, g.hinst, 0);

    /* Let kb_init() know whether the install succeeded before it returns. */
    if (g_kb_ready_evt) SetEvent(g_kb_ready_evt);

    if (!g.kb_hook) {
        log_w(L"SetWindowsHookEx(WH_KEYBOARD_LL) failed: %lu", GetLastError());
        return 1;
    }

    /* Dedicated pump: nothing here but servicing the hook. kb_shutdown() posts
     * WM_QUIT to break out. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* A thread message (hwnd == NULL) is ours: the only one is the request
         * to reconcile the mouse hook, which has to happen on this thread
         * because that is where its callbacks must run. */
        if (!msg.hwnd && msg.message == WM_MSHELL_MOUSE) {
            mouse_sync_hook_here();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.mouse_hook) {
        UnhookWindowsHookEx(g.mouse_hook);
        g.mouse_hook = NULL;
    }
    if (g.kb_hook) {
        UnhookWindowsHookEx(g.kb_hook);
        g.kb_hook = NULL;
    }
    return 0;
}

bool kb_init(void) {
    kb_locks_init();   /* idempotent — also called from WinMain before config */

    g_kb_ready_evt = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual reset */

    g_kb_thread = CreateThread(NULL, 0, kb_thread_proc, NULL, 0, &g_kb_thread_id);
    if (!g_kb_thread) {
        log_w(L"kb_init: CreateThread failed: %lu", GetLastError());
        return false;
    }

    /* Wait until the thread has attempted the hook install, so we can report
     * success/failure synchronously like before. */
    if (g_kb_ready_evt)
        WaitForSingleObject(g_kb_ready_evt, 5000);

    return g.kb_hook != NULL;
}

void kb_shutdown(void) {
    if (g_kb_thread) {
        PostThreadMessageW(g_kb_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(g_kb_thread, 2000);
        CloseHandle(g_kb_thread);
        g_kb_thread = NULL;
    }
    /* The hook is normally unhooked inside the thread; make sure. */
    if (g.kb_hook) {
        UnhookWindowsHookEx(g.kb_hook);
        g.kb_hook = NULL;
    }
    if (g_kb_ready_evt) {
        CloseHandle(g_kb_ready_evt);
        g_kb_ready_evt = NULL;
    }
    if (g_kb_cs_ready) {
        DeleteCriticalSection(&g_kb_cs);
        g_kb_cs_ready = false;
    }
}
