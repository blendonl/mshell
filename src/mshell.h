#pragma once

/*
 * mshell — keyboard-driven window manager shell replacement for Windows
 *
 * Replaces explorer.exe as the Windows shell. No taskbar, no systray,
 * no desktop icons. Just a tiling window manager driven entirely by keyboard.
 */

/* Version string is injected by the Makefile (-DMSHELL_VERSION); fall back to
 * "dev" for ad-hoc builds. */
#ifndef MSHELL_VERSION
#define MSHELL_VERSION "dev"
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>

/* ---------------------------------------------------------------------------
 * Lua forward declarations — need the real headers at compile time
 * --------------------------------------------------------------------------- */
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* ---------------------------------------------------------------------------
 * Modifier flags
 *
 * MOD_ALT (0x01), MOD_CONTROL (0x02), MOD_SHIFT (0x04) and MOD_WIN (0x08)
 * come from winuser.h. We alias MOD_LWIN to MOD_WIN for readability — we
 * treat both Win keys identically.
 * --------------------------------------------------------------------------- */
#ifndef MOD_LWIN
#define MOD_LWIN MOD_WIN
#endif

/* ---------------------------------------------------------------------------
 * Custom window messages
 *
 * The keyboard hook posts WM_MSHELL_ACTION to the message window so heavy
 * work (focus changes, tiling, spawning) runs on the main thread instead of
 * inside the time-critical low-level hook. lParam is a KeyBinding*.
 *
 * WM_MSHELL_SUBMAP is posted (also from the hook) whenever the active keymap
 * changes, so the main thread can show/hide the which-key hint — GDI must run
 * on the main thread, but g.current_map flips on the hook thread.
 *
 * WM_MSHELL_CONFIG_CHANGED is posted by the config-watcher thread once the
 * config directory goes quiet after a write, so the reload itself runs on the
 * main thread (config_load takes kb_lock and re-tiles — neither is safe off
 * the main thread). wParam is the watcher generation, so stale posts from a
 * watcher we already replaced are dropped.
 * --------------------------------------------------------------------------- */
#define WM_MSHELL_ACTION  (WM_APP + 1)

/* ---------------------------------------------------------------------------
 * Marker stamped into the dwExtraInfo of the synthetic keystroke window_focus()
 * injects to claim foreground rights. The keyboard hook recognises it and lets
 * it straight through instead of running it past the keymaps.
 * --------------------------------------------------------------------------- */
#define MSHELL_INPUT_TAG  ((ULONG_PTR)0x6D736831)   /* 'msh1' */
#define WM_MSHELL_SUBMAP  (WM_APP + 2)
#define WM_MSHELL_CONFIG_CHANGED  (WM_APP + 3)

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define MAX_DESKTOPS              32    /* how many may be ALIVE at once         */
#define MAX_WINDOWS_PER_DESKTOP   256
#define MAX_BINDINGS_PER_MAP      256
#define MAX_KEYMAPS               32
#define MAX_RULES                 128
#define MAX_DESKTOP_RULES         64
#define MAX_STARTUP_COMMANDS      32
#define SPAWN_ARGS_MAX            512   /* command-line arguments for a spawn */
#define MAX_MANAGED_WINDOWS       512
#define MAX_MONITORS              8
#define DESKTOP_NAME_MAX          64    /* incl. NUL; a desktop is never unnamed */

#define DEFAULT_INNER_GAP         4
#define DEFAULT_OUTER_GAP         4
#define DEFAULT_BORDER_WIDTH      2
#define DEFAULT_MASTER_RATIO      0.6f
#define DEFAULT_NMASTER           1
#define DEFAULT_START_DESKTOP     L"1"   /* the desktop you land on at startup   */
#define DEFAULT_MIN_WIN_W         120     /* below this a window is left floating  */
#define DEFAULT_MIN_WIN_H         80
#define DEFAULT_BORDER_COLOR      RGB(0xff, 0xff, 0xff)  /* focused-window ring   */
#define DEFAULT_BACKGROUND_COLOR  RGB(0x00, 0x00, 0x00)  /* desktop backdrop      */

/* which-key submap hint (all overridable via mshell.set_whichkey{}) */
#define DEFAULT_WHICHKEY_DELAY    150                    /* ms; 0 = show instantly */
#define DEFAULT_WHICHKEY_BG       RGB(0x1e, 0x1e, 0x2e)  /* panel background      */
#define DEFAULT_WHICHKEY_FG       RGB(0xcd, 0xd6, 0xf4)  /* action-label text     */
#define DEFAULT_WHICHKEY_KEY_FG   RGB(0x7a, 0xa2, 0xf7)  /* key + header accent   */
#define DEFAULT_WHICHKEY_BORDER   RGB(0x7a, 0xa2, 0xf7)  /* panel outline         */

/* ---------------------------------------------------------------------------
 * Action enum — every possible WM operation
 * --------------------------------------------------------------------------- */
typedef enum {
    ACTION_NONE = 0,

    /* focus movement */
    ACTION_FOCUS_LEFT,
    ACTION_FOCUS_DOWN,
    ACTION_FOCUS_UP,
    ACTION_FOCUS_RIGHT,
    ACTION_FOCUS_NEXT,
    ACTION_FOCUS_PREV,

    /* window movement / swap */
    ACTION_MOVE_LEFT,
    ACTION_MOVE_DOWN,
    ACTION_MOVE_UP,
    ACTION_MOVE_RIGHT,

    /* desktop — the target is a NAME, carried in KeyBinding.command */
    ACTION_SWITCH_DESKTOP,
    ACTION_MOVE_TO_DESKTOP,
    ACTION_LAST_DESKTOP,      /* back to the desktop we came from (toggle) */
    ACTION_NEXT_DESKTOP,      /* step through the desktops that exist now  */
    ACTION_PREV_DESKTOP,

    /* monitors (multi-monitor) */
    ACTION_FOCUS_MONITOR_NEXT,
    ACTION_FOCUS_MONITOR_PREV,
    ACTION_MOVE_TO_MONITOR_NEXT,
    ACTION_MOVE_TO_MONITOR_PREV,

    /* window lifetime */
    ACTION_CLOSE,
    ACTION_KILL,
    ACTION_MINIMIZE,
    ACTION_RESTORE,    /* un-minimize one — there is no taskbar to click */

    /* floating */
    ACTION_TOGGLE_FLOAT,

    /* fullscreen — one per FullscreenMode; each binding toggles its own mode */
    ACTION_FULLSCREEN,           /* the window covers its monitor            */
    ACTION_FULLSCREEN_CONTENT,   /* the app's fullscreen stays in the tile   */
    ACTION_FULLSCREEN_BOTH,      /* the app's fullscreen covers the monitor  */

    /* layout */
    ACTION_LAYOUT_TILING,
    ACTION_LAYOUT_MONOCLE,
    ACTION_LAYOUT_GRID,
    ACTION_LAYOUT_SPIRAL,
    ACTION_LAYOUT_CENTERED,
    ACTION_LAYOUT_BSTACK,
    ACTION_LAYOUT_COLUMNS,
    ACTION_CYCLE_LAYOUT,
    ACTION_PROMOTE_MASTER,
    ACTION_INC_MASTER,
    ACTION_DEC_MASTER,
    ACTION_INC_NMASTER,
    ACTION_DEC_NMASTER,
    ACTION_INC_CFACT,       /* grow focused window within its stack   */
    ACTION_DEC_CFACT,       /* shrink focused window within its stack */
    ACTION_RESET_CFACT,

    /* submap navigation */
    ACTION_ENTER_SUBMAP,

    /* spawn a program (command carried in KeyBinding.command) */
    ACTION_SPAWN,

    /* meta */
    ACTION_RELOAD,
    ACTION_QUIT,

    ACTION_COUNT
} Action;

/* ---------------------------------------------------------------------------
 * Layout enum
 * --------------------------------------------------------------------------- */
typedef enum {
    LAYOUT_TILING = 0,   /* master-stack (master left, stack right)          */
    LAYOUT_MONOCLE,      /* single fullscreen window                         */
    LAYOUT_GRID,         /* equal grid                                       */
    LAYOUT_SPIRAL,       /* fibonacci / dwindle                              */
    LAYOUT_CENTERED,     /* centered master, stack split left+right          */
    LAYOUT_BSTACK,       /* bottom-stack (master top, stack row below)       */
    LAYOUT_COLUMNS,      /* equal vertical columns                           */
    LAYOUT_COUNT
} Layout;

/* ---------------------------------------------------------------------------
 * Rule action
 * --------------------------------------------------------------------------- */
typedef enum {
    RULE_MANAGE = 0,
    RULE_FLOAT,
    RULE_IGNORE,
} RuleAction;

/* ---------------------------------------------------------------------------
 * Float policy — how aggressively we tile
 *   FLOAT_RULES : honor RULE_FLOAT and Win+f (default)
 *   FLOAT_NEVER : force every managed window into the tiling grid; rule
 *                 "float" is downgraded to "manage" and toggle_float is a no-op
 * --------------------------------------------------------------------------- */
typedef enum {
    FLOAT_RULES = 0,
    FLOAT_NEVER,
} FloatPolicy;

/* ---------------------------------------------------------------------------
 * Fullscreen mode — what "fullscreen" means for one window.
 *
 * Two different things can go fullscreen, and they are independent: the
 * *window* (geometry, which mshell owns) and the app's own *content*
 * fullscreen (YouTube's fullscreen button, F11 in a browser — the app switches
 * its own UI and resizes itself to the display). The modes are every useful
 * combination of the two:
 *
 *   FS_OFF      the layout owns the window. An app that fullscreens itself is
 *               handled by g.fullscreen_policy.
 *   FS_WINDOW   mshell parks the window over its monitor's full bounds. The app
 *               is never told anything — its ordinary UI is simply as big as
 *               the display. For apps with no fullscreen mode of their own.
 *   FS_CONTENT  the window keeps its tile and is pinned there, so an app that
 *               fullscreens itself renders its fullscreen UI *inside* the
 *               window: a fullscreen video fills the tile, not the screen.
 *   FS_BOTH     the window covers the monitor AND mshell stops policing its
 *               geometry, so the app's own fullscreen covers the display —
 *               fullscreen the way it behaves outside a tiling WM.
 * --------------------------------------------------------------------------- */
typedef enum {
    FS_OFF = 0,
    FS_WINDOW,
    FS_CONTENT,
    FS_BOTH,
} FullscreenMode;

/* ---------------------------------------------------------------------------
 * Attach policy — where a newly-managed window lands in the window order
 *   ATTACH_END    : append (becomes last stack window) — the historical default
 *   ATTACH_MASTER : insert at index 0 (becomes master, dwm-style)
 *   ATTACH_AFTER  : insert right after the focused window
 * --------------------------------------------------------------------------- */
typedef enum {
    ATTACH_END = 0,
    ATTACH_MASTER,
    ATTACH_AFTER,
} AttachPolicy;

/* ---------------------------------------------------------------------------
 * Forward-declare KeyMap (used by KeyBinding)
 * --------------------------------------------------------------------------- */
typedef struct KeyMap KeyMap;

/* ---------------------------------------------------------------------------
 * KeyBinding — one entry in a keymap
 * --------------------------------------------------------------------------- */
typedef struct {
    DWORD    mod_flags;   /* MOD_LWIN | MOD_SHIFT | MOD_CONTROL | MOD_ALT */
    DWORD    vk;          /* virtual-key code (e.g. 'H', VK_RETURN)         */
    Action   action;      /* what to do                                      */
    int      arg;         /* desktop index, submap index, …                  */
    KeyMap  *submap;      /* target submap for ACTION_ENTER_SUBMAP           */
    wchar_t *command;     /* program to launch for ACTION_SPAWN (owned)      */
    wchar_t *args;        /* its command-line arguments, or NULL (owned)     */
    bool     terminal;    /* return to root map after this action fires      */
} KeyBinding;

/* ---------------------------------------------------------------------------
 * KeyMap — a named layer of bindings (root map, or a submap)
 *
 * Submaps come in two flavours, chosen by `persist`:
 *   persist == false  (unpersisting, the default): the very next keystroke
 *                     disables the map — a bound key fires its action, any
 *                     other key is swallowed, and either way we drop back to
 *                     the root map. One-shot.
 *   persist == true   (persisting): the map stays active; each bound key fires
 *                     and you remain in the map. It is left only by the exit
 *                     key (`exit_vk`, or Escape when exit_vk == 0). A custom
 *                     exit key REPLACES Escape — Escape is then just an ordinary
 *                     binding if the map binds it, and otherwise does nothing.
 * --------------------------------------------------------------------------- */
struct KeyMap {
    wchar_t    *name;
    KeyBinding *bindings;
    int         count;
    int         capacity;
    bool        persist;   /* true: stay until the exit key; false: one-shot */
    DWORD       exit_vk;   /* persisting map's exit key; 0 => Escape (default) */
};

/* ---------------------------------------------------------------------------
 * ManagedWindow — our record for every window we manage
 * --------------------------------------------------------------------------- */
typedef struct {
    HWND      hwnd;
    int       desktop_id;            /* Desktop.id — stable across reordering */
    int       monitor;               /* which monitor it is tiled on        */
    bool      is_floating;           /* exempt from tiling                  */
    bool      no_ring;               /* suppress the focus ring (games)     */
    bool      no_decor;              /* strip the frame even while floating,
                                      * and never add the thin WS_BORDER    */
    bool      fullscreen;            /* while floating: cover the monitor   */
    bool      decorations_stripped;  /* did we strip WS_CAPTION etc?        */
    LONG_PTR  orig_style;            /* style before stripping              */
    LONG_PTR  orig_exstyle;          /* extended style before stripping     */
    float     cfact;                 /* size factor within its stack (1.0)  */
    RECT      applied_rect;          /* last frame rect we assigned         */
    bool      has_applied;           /* applied_rect is valid               */

    /* --- fullscreen (see FullscreenMode) --- */
    FullscreenMode fs_mode;          /* explicit mode set from a keybinding  */
    bool      app_fullscreen;        /* the app fullscreened itself and the
                                      * policy let it keep the display       */
    RECT      fs_prev_rect;          /* pre-fullscreen rect of a FLOATING
                                      * window (a tiled one is re-tiled)     */
    bool      fs_has_prev;           /* fs_prev_rect is valid                */
    bool      app_hidden;            /* the APP hid this window (minimise-to-
                                      * tray), as opposed to mshell hiding it
                                      * for a desktop switch or monocle. Such a
                                      * window leaves the layout and must not be
                                      * shown again until the app shows it —
                                      * see the EVENT_OBJECT_HIDE handler.     */
    bool      made_topmost;          /* WE put it in the topmost band to keep
                                      * always-on-top windows off a fullscreen
                                      * one — so only WE take it back out. A
                                      * window that was already topmost by its
                                      * own choice is never flagged, and so is
                                      * never demoted out from under its app. */
} ManagedWindow;

/* ---------------------------------------------------------------------------
 * Desktop — one virtual desktop.
 *
 * A desktop IS its name. There is no fixed set and no index addressing: the
 * name is what the config, the keybindings and the log all use, and it is
 * either a word ("web") or a number ("1") — a number is just a name whose
 * characters happen to be digits, not an index into anything.
 *
 * Desktops are DYNAMIC. Only the one you are standing on is guaranteed to
 * exist; switching to a name that has none creates it, and a desktop that runs
 * out of windows while you are not on it is destroyed. So `g.desktops` is a
 * live set that grows and shrinks under you, and the array is re-sorted on
 * every insert (see desktop_sort_cmp) — which is why nothing outside desktop.c
 * may hold on to a slot index across a create/destroy. `id` is the handle that
 * survives: unique, never reused, and what ManagedWindow.desktop_id refers to.
 * --------------------------------------------------------------------------- */
typedef struct {
    int     id;                      /* stable identity (see above); > 0     */
    wchar_t name[DESKTOP_NAME_MAX];  /* never empty for a live desktop       */

    HWND   windows[MAX_WINDOWS_PER_DESKTOP];
    int    count;
    int    focused;         /* index into windows[] of the active window  */

    /* --- settings: global defaults first, then every matching DesktopRule,
     *     applied at creation and re-applied on config reload --- */
    Layout  layout;
    float   master_ratio;
    int     n_master;       /* number of windows in the master area       */
    bool    float_all;      /* windows opened here start floating         */
    int     monitor;        /* pinned display, or -1 for "wherever it opens" */
    wchar_t app[MAX_PATH];  /* auto-launch while empty; "" = none         */
    wchar_t app_args[SPAWN_ARGS_MAX];  /* its arguments; "" = none        */

    bool   app_pending;     /* an `app` auto-launch is in flight          */
} Desktop;

/* ---------------------------------------------------------------------------
 * DesktopRule — match a desktop NAME → settings for it
 *
 * The desktop analogue of WindowRule, and the only place per-desktop behaviour
 * is configured. `name_match` is the same case-insensitive wildcard pattern
 * window rules use (`*` any run, `?` one character), so it names one desktop
 * ("web"), a family ("game-*"), or all of them ("*").
 *
 * Rules LAYER rather than compete: every rule whose pattern matches is applied,
 * in declaration order, and each one overwrites only the fields it actually
 * set. So a `"*"` rule sets the house style and a specific rule below it
 * overrides a field or two. The `set_*` flags are what make that possible —
 * without them an omitted field would be indistinguishable from a zero.
 * --------------------------------------------------------------------------- */
typedef struct {
    wchar_t name_match[DESKTOP_NAME_MAX];   /* "" matches every desktop      */

    wchar_t app[MAX_PATH];                  /* "" = don't touch the app      */
    wchar_t app_args[SPAWN_ARGS_MAX];       /* arguments for it; "" = none   */

    bool    set_float;    bool   float_all;
    bool    set_layout;   Layout layout;
    bool    set_ratio;    float  master_ratio;
    bool    set_nmaster;  int    n_master;
    bool    set_monitor;  int    monitor;
} DesktopRule;

/* ---------------------------------------------------------------------------
 * StartupCommand — one mshell.spawn() from the config, run once at startup.
 *
 * The arguments are a separate string rather than part of the command because
 * that is how ShellExecuteW takes them, and splitting a single string back
 * apart is ambiguous the moment a path contains a space — which on Windows is
 * most of them.
 * --------------------------------------------------------------------------- */
typedef struct {
    wchar_t *cmd;    /* owned */
    wchar_t *args;   /* owned; NULL when none were given */
} StartupCommand;

/* ---------------------------------------------------------------------------
 * Monitor — one physical display
 * --------------------------------------------------------------------------- */
typedef struct {
    HMONITOR handle;
    RECT     full;          /* full monitor bounds                        */
    RECT     work_area;     /* usable area (== full when we are the shell) */
} Monitor;

/* ---------------------------------------------------------------------------
 * WindowRule — match criteria → action
 *
 * Every string criterion is an empty-means-any, case-insensitive wildcard
 * pattern: `*` matches any run of characters, `?` a single one, and `/` and `\`
 * compare equal so a path can be written either way. A pattern with no wildcard
 * in it is therefore just an exact match, which is what plain class/process
 * rules have always been.
 *
 * `set_dialog` adds the one criterion that is not a name: whether Windows
 * itself treats the window as a dialog (see window_is_dialog). File pickers,
 * message boxes and permission prompts carry no class or process of their own —
 * they are the *host app's* process wearing a common-dialog class — so naming
 * them is impossible and asking what they are is the only way to match them.
 * --------------------------------------------------------------------------- */
typedef struct {
    wchar_t    class_match[256];      /* empty = match any class            */
    wchar_t    process_match[256];    /* empty = match any process (exe)    */
    wchar_t    path_match[MAX_PATH];  /* empty = match any full image path  */
    bool       set_dialog;            /* false = don't care what it is      */
    bool       dialog;                /* required window_is_dialog() answer */
    RuleAction action;
    bool       no_ring;             /* don't draw the focus ring around it   */
    bool       no_decor;            /* strip the frame, add no border at all */
    bool       fullscreen;          /* while floating: cover the monitor     */
} WindowRule;

/* ---------------------------------------------------------------------------
 * Global shell state — single instance accessed by all modules
 * --------------------------------------------------------------------------- */
typedef struct {
    /* --- desktops (dynamic; see the Desktop comment) --- */
    Desktop  desktops[MAX_DESKTOPS];
    int      desktop_count;      /* how many are alive right now             */
    int      current_desktop_id; /* the one you are on — by id, not by slot  */
    int      next_desktop_id;    /* monotonic id source; never wraps in a session */

    /* "Go back" and "start here" are both stored as NAMES rather than as a
     * desktop, because neither is guaranteed to exist: the desktop you came
     * from is destroyed behind you when you leave it empty, and last_desktop
     * then re-creates it by name. */
    wchar_t  last_desktop[DESKTOP_NAME_MAX];
    wchar_t  start_desktop[DESKTOP_NAME_MAX];

    /* --- desktop rules --- */
    DesktopRule desktop_rules[MAX_DESKTOP_RULES];
    int         desktop_rule_count;

    /* --- keybinding --- */
    KeyMap   keymaps[MAX_KEYMAPS];
    int      keymap_count;
    KeyMap  *root_map;        /* always points to keymaps[0]          */
    KeyMap  *current_map;     /* active map (root or a submap)        */
    KeyMap  *leader_map;      /* entered by a bare Win tap; set from Lua by
                               * mshell.set_leader, or NULL if never called  */

    /* --- managed windows --- */
    ManagedWindow managed[MAX_MANAGED_WINDOWS];
    int           managed_count;

    /* --- rules --- */
    WindowRule rules[MAX_RULES];
    int        rule_count;

    /* --- appearance --- */
    int      inner_gap;       /* gap between adjacent tiled windows           */
    int      outer_gap;       /* margin between the screen edge and windows   */
    bool     smart_gaps;      /* drop all gaps when a monitor has one window  */
    int      border_width;    /* focused-window ring thickness (0 = off)      */
    COLORREF border_color;    /* focused-window ring color                    */
    COLORREF background_color;/* solid desktop backdrop color                 */

    /* --- which-key submap hint --- */
    bool     whichkey_enabled;   /* show a hint panel when a submap is active  */
    int      whichkey_delay;     /* ms before it appears (0 = instant)         */
    COLORREF whichkey_bg;        /* panel background                           */
    COLORREF whichkey_fg;        /* action-label text                          */
    COLORREF whichkey_key_fg;    /* key + header accent                        */
    COLORREF whichkey_border;    /* panel outline                              */

    /* --- tiling policy --- */
    FloatPolicy  float_policy;   /* FLOAT_RULES (default) or FLOAT_NEVER      */
    FullscreenMode fullscreen_policy; /* what an app that fullscreens ITSELF
                                       * gets when the window has no explicit
                                       * mode: FS_CONTENT = keep it in its tile
                                       * (default), FS_BOTH = give it the
                                       * monitor until it leaves fullscreen   */
    AttachPolicy attach_policy;  /* where new windows land in the order       */
    bool     manage_owned;    /* also tile owned windows (dialogs); risky     */
    bool     float_on_top;    /* keep floating windows above tiled ones       */
    int      min_win_w;       /* ignore windows narrower than this            */
    int      min_win_h;       /* ignore windows shorter than this             */

    /* --- per-desktop defaults (seed new desktops; set from config) --- */
    Layout   default_layout;
    float    default_master_ratio;
    int      default_nmaster;

    /* --- input --- */
    bool     block_system_keys; /* swallow Alt+Tab, Ctrl+Esc, Alt+Space… */

    /* --- lifecycle --- */
    bool     running;
    int      suppress_depth;  /* >0 while we reposition windows ourselves    */
    HWND     message_window;  /* hidden top-level window for the message pump */
    HWND     background_window;/* bottom-most solid-color desktop backdrop    */
    HWND     border_window;   /* layered overlay marking the focused window   */
    HWND     whichkey_window; /* layered overlay: submap hint ("which-key")   */

    /* --- hooks --- */
    HHOOK         kb_hook;
    /* Four narrow WinEvent hooks rather than one wide one. The ranges between
     * them (REORDER, OBJECT_FOCUS, SELECTION*, STATECHANGE, and most of the
     * system range) fire constantly across every process on the machine and
     * nothing here handles them, so they are deliberately not subscribed to. */
    HWINEVENTHOOK win_event_hook;    /* EVENT_OBJECT_CREATE..HIDE            */
    HWINEVENTHOOK location_hook;     /* EVENT_OBJECT_LOCATIONCHANGE only     */
    HWINEVENTHOOK foreground_hook;   /* EVENT_SYSTEM_FOREGROUND (separate id
                                      * range — see events_init())           */
    HWINEVENTHOOK minimize_hook;     /* EVENT_SYSTEM_MINIMIZESTART/END — also
                                      * in the system range, so also its own  */

    /* --- monitors (re-queried on display change) --- */
    Monitor  monitors[MAX_MONITORS];
    int      monitor_count;
    int      primary_monitor;
    int      focused_monitor;

    /* --- primary-monitor work area (kept for fallbacks / single-monitor) --- */
    RECT     work_area;

    /* --- config --- */
    wchar_t   config_path[MAX_PATH];
    bool      auto_reload;    /* reload when the config file is saved         */
    StartupCommand startup_commands[MAX_STARTUP_COMMANDS];
    int       startup_count;
    lua_State *L;             /* live Lua VM (rebuilt on reload)      */

    /* --- instance --- */
    HINSTANCE hinst;

    /* --- run mode --- */
    bool     test_mode;       /* running alongside explorer, not as shell */
    bool     elevated;        /* we hold an elevated token. The config is then
                               * effectively administrator-level code, so
                               * auto-reload (which would execute it silently on
                               * any write) is disabled — see config_watch_sync */

    /* --- logging (DebugView + %TEMP%\mshell.log) --- */
    bool     verbose;
    FILE    *logfile;
} MShell;

/* ---------------------------------------------------------------------------
 * The one-and-only global instance
 * --------------------------------------------------------------------------- */
extern MShell g;

/* ===========================================================================
 * Prototypes — main.c
 * =========================================================================== */
int  WINAPI   WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, int nCmdShow);
LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* Where the config file lives: %APPDATA%\mshell\init.lua, falling back to
 * config\init.lua next to the .exe. Re-run on every reload, so creating the
 * AppData config takes effect with Win+Shift+R — no sign-out needed. */
void     resolve_config_path(wchar_t *out, size_t out_len);

/* monitors */
void     monitors_update(void);           /* (re)enumerate physical displays */
int      monitor_of_window(HWND hwnd);    /* index into g.monitors, or 0      */

/* Effective DPI of a monitor (96 when unknown). mshell is per-monitor DPI
 * aware, so coordinates are physical pixels and anything drawn at a fixed size
 * must scale itself — MulDiv(px, monitor_dpi(m), 96). */
UINT     monitor_dpi(int mon);

/* ===========================================================================
 * Prototypes — keyboard.c
 * =========================================================================== */
bool     kb_init(void);
void     kb_shutdown(void);
LRESULT CALLBACK kb_hook_proc(int nCode, WPARAM wParam, LPARAM lParam);

/* Keymap lock — guards g.keymaps / g.current_map / g.root_map, which the hook
 * (now on its own thread) reads while a config reload rebuilds them on the main
 * thread. kb_locks_init() must run once before the first config load. */
void     kb_locks_init(void);
void     kb_lock(void);
void     kb_unlock(void);
void     kb_reset_state(void);   /* clear stuck modifiers after a session unlock */

DWORD    key_name_to_vk(const char *name);
const char *vk_to_key_name(DWORD vk);       /* reverse of key_name_to_vk (or NULL) */
DWORD    mod_name_to_flag(const char *name);
Action   action_name_to_enum(const char *name);
const char *action_enum_to_name(Action action); /* reverse lookup (or NULL)        */
void     execute_action(Action action, int arg, const wchar_t *command,
                        const wchar_t *args);

/* Launch `cmd` with `args` (either may be NULL/empty). ShellExecuteW, so PATH
 * is resolved and .lnk shortcuts work — which is why arguments have to be a
 * separate string rather than baked into the command. `ctx` names the caller
 * ("startup", "keybind", a desktop name) in the failure message, because a
 * program that silently never appears is indistinguishable from mshell
 * ignoring the request. Returns false and logs when the launch fails. */
bool     spawn_command(const wchar_t *cmd, const wchar_t *args,
                       const wchar_t *ctx);

/* Copy out the action a WM_MSHELL_ACTION message refers to (its lParam is the
 * sequence number). The hook records actions BY VALUE rather than posting the
 * KeyBinding pointer, because a config reload frees every binding while another
 * keystroke may still be sitting in the queue behind it. False if the ring
 * lapped before the main thread drained it. */
bool     kb_take_pending(unsigned seq, Action *action, int *arg,
                         wchar_t *cmd, size_t cmd_cap,
                         wchar_t *args, size_t args_cap);
KeyMap  *keymap_new(const wchar_t *name, bool persist);
void     keymap_add_binding(KeyMap *map, DWORD mods, DWORD vk,
                            Action action, int arg, KeyMap *submap,
                            const wchar_t *command, const wchar_t *args,
                            bool terminal);

/* ===========================================================================
 * Prototypes — window.c
 * =========================================================================== */
bool     window_is_manageable(HWND hwnd);

/* Full image path of the process owning a window ("" if it can't be read). */
void     window_process_path(HWND hwnd, wchar_t *out, size_t out_len);
bool     window_is_dialog(HWND hwnd);   /* file picker, message box, prompt … */
const WindowRule *window_rule_lookup(HWND hwnd);  /* matched rule, or NULL */

/* Case-insensitive wildcard match — `*` any run, `?` one character, `/` == `\`.
 * A pattern with no wildcard in it is an exact match. Window rules match their
 * class/process/path with it and desktop rules match names with it, so both
 * kinds of rule take exactly the same patterns. */
bool     wildcard_match(const wchar_t *pat, const wchar_t *str);
void     window_manage(HWND hwnd);
void     window_unmanage(HWND hwnd);
void     window_strip_decorations(HWND hwnd);
void     window_restore_decorations(HWND hwnd);
void     window_kill(HWND hwnd);
void     window_close(HWND hwnd);
void     window_focus(HWND hwnd);
HWND     window_get_focused(void);
ManagedWindow *window_find(HWND hwnd);
void     window_manage_existing(void);
void     window_restore_all_decorations(void);

/* Re-show everything mshell hid (other desktops, monocle's non-focused
 * windows). A hidden window has no taskbar button and no Alt+Tab entry, so
 * without this they are unreachable once mshell exits. Call it before
 * window_restore_all_decorations(). */
void     window_restore_all_visibility(void);
void     window_set_floating(HWND hwnd, bool floating);
void     window_enforce_zorder(void);     /* backdrop at bottom, floats on top */
bool     window_frame_rect(HWND hwnd, RECT *out);  /* DWM visible-frame bounds  */

/* Expand a desired *visible* frame into the window rect SetWindowPos wants,
 * compensating for DWM's invisible resize border. Used by the tiler and by the
 * fullscreen placement below, so both land pixel-accurate. */
RECT     window_adjust_for_frame(HWND hwnd, RECT want);

/* Rule-driven geometry/chrome for floating windows (games). Both are no-ops
 * unless the window's rule asked for them, so callers don't have to check. */
void     window_apply_fullscreen(HWND hwnd);  /* park it over its whole monitor */
void     window_reassert_rule(HWND hwnd);     /* re-strip / re-place after drift */

/* Fullscreen (see FullscreenMode). window_set_fullscreen TOGGLES: asking for
 * the mode a window is already in turns fullscreen off, any other mode switches
 * straight over. The other two are the primitives it and the tiler share. */
void     window_set_fullscreen(HWND hwnd, FullscreenMode mode);
void     window_park_over_monitor(HWND hwnd); /* full monitor bounds, no gaps   */
bool     window_covers_monitor(HWND hwnd);    /* does its frame reach every edge? */

/* ===========================================================================
 * Prototypes — tiling.c
 * =========================================================================== */
void     tile_desktop(int slot);   /* slot, not id — see desktop.c prototypes */
void     tile_current(void);

/* ===========================================================================
 * Prototypes — desktop.c
 *
 * Two ways to name a desktop, and they are not interchangeable:
 *   - a NAME (const wchar_t *) is what the config and the user speak. Passing
 *     one to switch/move CREATES the desktop if it doesn't exist yet.
 *   - a SLOT (int, an index into g.desktops) is desktop.c's own currency and is
 *     valid only until the next create/destroy. Never store one.
 * `id` sits between them: stable, and what a window remembers.
 * =========================================================================== */
void     desktop_init(void);             /* create the start desktop, once */

/* Is this a usable desktop name? Non-empty, no whitespace, fits the buffer. */
bool     desktop_name_ok(const wchar_t *name);

int      desktop_slot_by_name(const wchar_t *name);  /* -1 when not alive */
int      desktop_slot_by_id(int id);                 /* -1 when not alive */
Desktop *desktop_by_id(int id);                      /* NULL when not alive */
int      desktop_current_slot(void);
Desktop *desktop_current(void);          /* never NULL once desktop_init ran */

/* Look the name up, creating the desktop when it isn't alive. Returns its slot,
 * or -1 if the name is unusable or MAX_DESKTOPS are already alive. */
int      desktop_ensure(const wchar_t *name);

void     desktop_switch(const wchar_t *name);
void     desktop_switch_last(void);      /* back to g.last_desktop (no-op if same) */
void     desktop_cycle(int delta);       /* +1/-1 through the live desktops */
void     desktop_move_window(HWND hwnd, const wchar_t *name);
void     desktop_add_window(HWND hwnd, int slot);
void     desktop_remove_window(HWND hwnd);
int      desktop_of_window(HWND hwnd);   /* desktop id, or 0 when unmanaged */
void     desktop_focus_update(HWND hwnd);

/* Destroy `slot` if it is empty and not the one you are on. Call after anything
 * that can empty a desktop; it decides for itself whether there's work to do. */
void     desktop_gc(int slot);

void     desktop_apply_rules(int slot);  /* defaults, then every matching rule */
void     desktop_monitors_changed(void); /* re-resolve monitor pins after a
                                          * display was added or removed      */
void     desktop_reapply(void);           /* re-assert show/hide + tiling after reload */
HWND     desktop_get_focused(void);
void     desktop_launch_app_if_empty(int slot); /* spawn the desktop's `app` */

/* ===========================================================================
 * Prototypes — border.c / background.c (focus ring + desktop backdrop)
 * =========================================================================== */
bool     border_init(void);
void     border_shutdown(void);
void     border_refresh(void);            /* redraw ring around the focused window */
void     border_hide(void);

bool     background_init(void);
void     background_shutdown(void);
void     background_update(void);         /* resize/repaint on display change      */

/* ===========================================================================
 * Prototypes — whichkey.c (submap hint popup)
 * =========================================================================== */
bool     whichkey_init(void);
void     whichkey_shutdown(void);
void     whichkey_notify(void);   /* re-read the active map; show/hide/schedule it */
void     whichkey_hide(void);

/* ===========================================================================
 * Prototypes — events.c
 * =========================================================================== */
bool     events_init(void);
void     events_shutdown(void);
void CALLBACK events_win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD idEventThread, DWORD dwmsEventTime);

/* ===========================================================================
 * Prototypes — config.c / lua_api.c
 * =========================================================================== */
bool     config_load(const wchar_t *path);
void     config_reload(void);
bool     config_init(void);
void     config_load_builtin(void);   /* minimal built-in keymap when config fails */
void     config_shutdown(void);

/* Auto-reload: a watcher thread on the config's directory. config_watch_sync()
 * starts/stops/retargets it to match g.auto_reload and the current config path
 * (cheap no-op when neither changed); config_on_file_changed() is the main-
 * thread handler for WM_MSHELL_CONFIG_CHANGED. */
void     config_watch_sync(void);
void     config_watch_stop(void);
void     config_on_file_changed(unsigned generation);

void     lua_register_api(lua_State *L);

/* ===========================================================================
 * Prototypes — util (inline helpers defined below)
 * =========================================================================== */

/* clamp a value between lo and hi */
static inline int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* swap two HWNDs */
static inline void hwnd_swap(HWND *a, HWND *b) {
    HWND t = *a; *a = *b; *b = t;
}

/* Find a ManagedWindow by HWND — returns index or -1.
 *
 * Yes, this is a linear scan, and yes, it is called from inside loops. Leave it
 * alone: at the window counts that actually occur (a few dozen) a whole tiling
 * pass costs a few thousand integer comparisons, which is far below anything
 * measurable. It would only be worth indexing if MAX_MANAGED_WINDOWS were
 * raised by orders of magnitude. */
static inline int window_index_of(HWND hwnd) {
    for (int i = 0; i < g.managed_count; i++) {
        if (g.managed[i].hwnd == hwnd) return i;
    }
    return -1;
}

/* is the window still alive? */
static inline bool window_is_alive(HWND hwnd) {
    return IsWindow(hwnd) != 0;
}

/* Is this window currently covering its whole monitor — either because a
 * keybinding put it there (FS_WINDOW / FS_BOTH) or because the app fullscreened
 * itself and the policy let it? Such a window is outside the layout: the tiler
 * skips it, the others tile underneath, and the z-order pass raises it.
 * FS_CONTENT is deliberately NOT here — that mode keeps its tile. */
static inline bool window_is_screen_fullscreen(const ManagedWindow *mw) {
    return mw && (mw->fs_mode == FS_WINDOW || mw->fs_mode == FS_BOTH ||
                  mw->app_fullscreen);
}

/* WinEvent suppression is a nesting counter, not a flag: repositioning a
 * window can trigger nested tiling passes, and a bare bool would let the
 * inner pass re-enable events while the outer pass is still moving windows. */
static inline void events_suppress_begin(void) { g.suppress_depth++; }
static inline void events_suppress_end(void)   { if (g.suppress_depth > 0) g.suppress_depth--; }
static inline bool events_suppressed(void)     { return g.suppress_depth > 0; }

/* Debug logging. GUI-subsystem apps have no usable stderr, so we emit via
 * OutputDebugStringW — watch it live with Sysinternals DebugView (run as
 * admin, enable "Capture Global Win32") or any attached debugger. */
static inline void log_vw(const wchar_t *fmt, va_list ap) {
    wchar_t buf[1024];
    _vsnwprintf(buf, 1023, fmt, ap);
    buf[1023] = L'\0';
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    if (g.logfile) {
        fputws(buf, g.logfile);
        fputwc(L'\n', g.logfile);
        fflush(g.logfile);
    }
}

/* Chatty tracing — per-keystroke matches, tiling passes, focus changes. Costs
 * nothing unless --verbose, and would drown the log if it were always on. */
static inline void log_w(const wchar_t *fmt, ...) {
    if (!g.verbose) return;
    va_list ap;
    va_start(ap, fmt);
    log_vw(fmt, ap);
    va_end(ap);
}

/* Failures the user has to be able to diagnose — ALWAYS written, --verbose or
 * not. As the Windows shell there is no console, no taskbar and no tray to
 * report into, so the log file is the only channel there is; a rejected config
 * that logged nothing is indistinguishable from "mshell ignored my keybinds".
 * Reserve this for things that are actually wrong (config load failed, fallback
 * keymap in use, a spawn that didn't launch) so the file stays a few lines long
 * in normal operation. */
static inline void log_err(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vw(fmt, ap);
    va_end(ap);
}
