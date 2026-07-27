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

/* The same string as a wide literal, for the places that compare or format it
 * as one. The two-step is how a macro argument gets stringified through L. */
#define MSHELL_WIDEN2(x) L##x
#define MSHELL_WIDEN(x)  MSHELL_WIDEN2(x)
#define MSHELL_VERSION_W MSHELL_WIDEN(MSHELL_VERSION)

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

#include "log.h"

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

/* Timer ids on the message window. The first one there has ever been — the
 * message pump previously handled no WM_TIMER at all. */
#define TIMER_CRASHLOOP_HEALTHY  1
#define TIMER_FOLLOW_MOUSE       2
#define TIMER_ANIM               3
#define FOLLOW_MOUSE_MS          120   /* human-speed; see mouse.c */
#define WM_MSHELL_SUBMAP  (WM_APP + 2)
#define WM_MSHELL_CONFIG_CHANGED  (WM_APP + 3)

/* Posted by the IPC pipe thread with an IpcRequest*. The command runs on the
 * main thread because everything it can touch is main-thread state; the pipe
 * thread waits on the request's event and then writes the reply. */
#define WM_MSHELL_IPC             (WM_APP + 4)

/* Posted by the mouse hook during a Mod+drag: wParam/lParam are the cursor's
 * offset from where the drag started. Applied on the main thread, because
 * SetWindowPos from the hook thread runs inside the input timeout. */
#define WM_MSHELL_MOUSE           (WM_APP + 5)

/* Posted by the hook while the launcher is capturing: wParam is the virtual
 * key, lParam the character it produced (0 for a non-text key). The hook
 * translates rather than the launcher, because ToUnicodeEx needs the keyboard
 * state as it was at the moment of the keystroke. */
#define WM_MSHELL_CAPTURE_KEY     (WM_APP + 6)

/* Posted by the update thread with an owned wide string to show. Raised on the
 * main thread because every overlay is. */
#define WM_MSHELL_UPDATE          (WM_APP + 7)

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define MAX_DESKTOPS              32    /* how many may be ALIVE at once         */
#define MAX_WINDOWS_PER_DESKTOP   256
#define FOCUS_HIST_MAX            8     /* per-desktop focus history depth */
#define MAX_BINDINGS_PER_MAP      256
#define MAX_KEYMAPS               32
#define MAX_RULES                 128
#define MAX_DESKTOP_RULES         64
#define MAX_STARTUP_COMMANDS      32
#define SPAWN_ARGS_MAX            512   /* command-line arguments for a spawn */
#define MAX_MANAGED_WINDOWS       512
#define MAX_MONITORS              8
#define MAX_MONITOR_RULES         8
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

/* status bar (all overridable via mshell.set_bar{}) */
#define DEFAULT_BAR_HEIGHT        28                     /* design px at 96 DPI */
#define DEFAULT_BAR_BG            RGB(0x1e, 0x1e, 0x2e)
#define DEFAULT_BAR_FG            RGB(0xcd, 0xd6, 0xf4)
#define DEFAULT_BAR_ACCENT        RGB(0x7a, 0xa2, 0xf7)  /* the current desktop */
#define DEFAULT_BAR_DIM           RGB(0x6c, 0x70, 0x86)  /* the other desktops  */

/* Which sections the bar draws, left to right. */
#define BAR_MOD_DESKTOPS  0x1
#define BAR_MOD_LAYOUT    0x2
#define BAR_MOD_TITLE     0x4
#define BAR_MOD_CLOCK     0x8
#define BAR_MOD_DEFAULT   (BAR_MOD_DESKTOPS | BAR_MOD_LAYOUT | \
                           BAR_MOD_TITLE    | BAR_MOD_CLOCK)

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
    ACTION_TOGGLE_STICKY,      /* show this window on every desktop        */
    ACTION_MARK_SCRATCHPAD,    /* make this window the scratchpad          */
    ACTION_TOGGLE_SCRATCHPAD,  /* summon / dismiss it                      */
    ACTION_TOGGLE_ALWAYS_ON_TOP, /* keep it in the topmost band            */
    ACTION_LAST_WINDOW,        /* back to the previously focused window    */

    /* floating */
    ACTION_TOGGLE_FLOAT,

    /* Resize a FLOATING window from the keyboard. A tiled window's geometry
     * belongs to the layout — inc_master / cfact are its equivalents — so these
     * are deliberately no-ops on one. Moving a float needs no new action: the
     * move_* group already means "move this window", and does so literally when
     * the window is floating rather than swapping it in the tile order. */
    ACTION_RESIZE_LEFT,
    ACTION_RESIZE_DOWN,
    ACTION_RESIZE_UP,
    ACTION_RESIZE_RIGHT,

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
    ACTION_ZOOM,            /* dwm-style: swap with master, and back again */
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

    /* call a Lua function from the config (registry ref carried in
     * KeyBinding.arg — valid only for the lua_State that created it, which is
     * why dispatch stamps the config generation alongside it) */
    ACTION_LUA_CALL,

    /* Session / power. Replacing Explorer removes every other route to these:
     * there is no Start menu, and `quit` is not a substitute because exiting as
     * the shell ends the session however you meant it. */
    ACTION_LOCK,
    ACTION_LOGOFF,
    ACTION_REBOOT,
    ACTION_SHUTDOWN,
    ACTION_SLEEP,
    ACTION_HIBERNATE,

    /* Media. A keyboard with dedicated volume keys already works (Windows
     * handles those below our hook); one without had no route to volume at all,
     * since every Win+key belongs to mshell. */
    ACTION_VOLUME_UP,
    ACTION_VOLUME_DOWN,
    ACTION_VOLUME_MUTE,
    ACTION_MEDIA_PLAY,
    ACTION_MEDIA_NEXT,
    ACTION_MEDIA_PREV,
    ACTION_MEDIA_STOP,

    /* Screenshots. PrintScreen is remapped to Snip by a shell setting, and
     * Win+Shift+S is a Win chord and therefore ours — so without these there
     * is no screenshot at all. */
    ACTION_SCREENSHOT,          /* the whole virtual screen */
    ACTION_SCREENSHOT_WINDOW,   /* just the focused window  */

    /* Show a message on screen. The text rides in `command`, so this works
     * over --msg as well as from a binding. */
    ACTION_NOTIFY,

    /* Go to the window that asked for attention. */
    ACTION_JUMP_URGENT,

    /* Open the launcher (see launcher.c for why it captures the keyboard). */
    ACTION_LAUNCHER,

    /* Manual (BSP) tiling. split_h / split_v state where the NEXT window goes
     * rather than acting immediately — there is nothing to split until one
     * arrives. */
    ACTION_SPLIT_H,
    ACTION_SPLIT_V,
    ACTION_ROTATE_SPLIT,      /* flip the split holding the focused window   */
    ACTION_TOGGLE_TABBED,     /* make that split a tabbed container, or back */
    ACTION_TOGGLE_STACKED,
    ACTION_CONTAINER_NEXT,    /* show the container's other child            */
    ACTION_CONTAINER_PREV,
    ACTION_SPLIT_GROW,        /* resize the split holding the focus          */
    ACTION_SPLIT_SHRINK,
    ACTION_LAYOUT_BSP,

    /* meta */
    ACTION_RELOAD,
    ACTION_QUIT,

    /* Last resort: start Explorer alongside us and stop swallowing keys, so a
     * machine whose shell is misbehaving stays usable without Task Manager.
     * Deliberately does not exit — quitting as the shell is what logs you out. */
    ACTION_PANIC,

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
    LAYOUT_BSP,          /* manual splits + containers — see layout_tree.c   */
    LAYOUT_COUNT
} Layout;

/* How a split node divides its area, or refuses to. TABBED and STACKED show one
 * child at a time instead of dividing, which is what makes them containers. */
typedef enum {
    SPLIT_V = 0,      /* side by side  */
    SPLIT_H,          /* one above the other */
    SPLIT_TABBED,
    SPLIT_STACKED,
} SplitMode;

/* How layout_tree_run hands a placement back — the tiler's emit(), without
 * layout_tree.c having to know what a Placement is. */
typedef void (*TreeEmitFn)(HWND hwnd, RECT area, void *ctx);

/* ---------------------------------------------------------------------------
 * On-screen notification severity — picks the accent stripe's colour.
 * --------------------------------------------------------------------------- */
typedef enum {
    NOTIFY_INFO = 0,
    NOTIFY_WARN,
    NOTIFY_ERROR,
} NotifyKind;

/* ---------------------------------------------------------------------------
 * Resolved layout parameters for ONE monitor's slice of a desktop.
 *
 * The layout functions used to read dt->n_master / dt->master_ratio directly
 * and take the gap-inset area as a separate argument, which meant every knob
 * was per-desktop by construction: there was nowhere for a per-monitor value to
 * come from. Resolving them into this struct first — once, in tile_monitor —
 * is what lets a later per-desktop or per-monitor override exist without
 * touching a single layout function, and it makes the layouts themselves pure
 * functions of their input.
 * --------------------------------------------------------------------------- */
typedef struct {
    RECT  area;          /* the slice to fill, already inset for gaps       */
    int   inner;         /* inner gap in effect (0 under smart gaps)        */
    float master_ratio;
    int   n_master;
} LayoutParams;

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
 * Hide policy — HOW a window is taken off the screen.
 *
 * Virtual desktops, monocle and the scratchpad all work by removing a window
 * from view, and there are two ways to do that. They are not equivalent:
 *
 *   HIDE_CLOAK (default)
 *       DwmSetWindowAttribute(DWMWA_CLOAK). The window keeps WS_VISIBLE and
 *       keeps its DWM redirection surface — it goes on rendering, DWM simply
 *       stops compositing it onto the screen. This is precisely the mechanism
 *       Windows' own virtual desktops use, so every application is already
 *       tested against it.
 *
 *   HIDE_SHOWWINDOW
 *       ShowWindow(SW_HIDE), which is what mshell used to do unconditionally.
 *       Clearing WS_VISIBLE makes DWM tear the window's redirection surface
 *       down, and apps that render off the UI thread — anything Chromium or
 *       Electron based, WPF, Qt on D3D — additionally see it as "occluded" and
 *       shut their compositor down. On the way back a fresh, EMPTY surface is
 *       created and nothing asks the app to present into it, so the window
 *       comes back BLACK until something forces a repaint. Kept as an escape
 *       hatch for a setup where cloaking misbehaves; the show path forces the
 *       repaint that this mode needs.
 * --------------------------------------------------------------------------- */
typedef enum {
    HIDE_CLOAK = 0,
    HIDE_SHOWWINDOW,
} HidePolicy;

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
    wchar_t *cwd;         /* working directory to start it in, or NULL (owned)*/
    wchar_t *desc;        /* which-key label, or NULL to derive one (owned)   */
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
    /* The display it was on, by name. An index is meaningless across a
     * hotplug — the survivors renumber — so on replug this is what puts the
     * window back where it was rather than wherever the index now points. */
    wchar_t   monitor_device[CCHDEVICENAME];
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
    bool      needs_helper;          /* a placement for this window was refused
                                      * locally and went via mshelld.exe. Kept
                                      * so later passes skip the batch for it —
                                      * one refused window would fail the whole
                                      * DeferWindowPos group.                 */
    bool      always_on_top;         /* user asked for the topmost band      */
    bool      layout_hidden;         /* the LAYOUT hid it (monocle, or the
                                      * unshown side of a tabbed container) —
                                      * distinct from app_hidden, which is the
                                      * app hiding itself to the tray        */
    bool      urgent;                /* flashed for attention while elsewhere */
    bool      sticky;                /* follows you to every desktop         */
    bool      scratchpad;            /* the scratchpad window (see ACTION_
                                      * TOGGLE_SCRATCHPAD); hidden when away  */
    bool      app_hidden;            /* the APP hid this window (minimise-to-
                                      * tray), as opposed to mshell hiding it
                                      * for a desktop switch or monocle. Such a
                                      * window leaves the layout and must not be
                                      * shown again until the app shows it —
                                      * see the EVENT_OBJECT_HIDE handler.     */
    bool      wm_hidden;             /* MSHELL has it off the screen: another
                                      * desktop, monocle, a stowed scratchpad.
                                      * Tracked rather than derived, because
                                      * under HIDE_CLOAK the window is still
                                      * WS_VISIBLE and IsWindowVisible can no
                                      * longer answer the question.            */
    bool      cloaked;               /* ...and we did it by cloaking. Recorded
                                      * per window so a policy change mid-
                                      * session still un-hides the way the
                                      * window was hidden.                     */
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

    /* Most-recently-focused first, [0] being the current window. Kept per
     * desktop because "the window I was just in" means the one on the desktop
     * you are standing on; a global list would send last_window somewhere you
     * cannot see. HWNDs rather than indices: windows[] is memmove'd on every
     * insert and removal, so an index would silently come to mean another
     * window. Dead entries are skipped at read time instead of being pruned
     * eagerly, since a window closing is exactly when the list is consulted. */
    HWND   focus_hist[FOCUS_HIST_MAX];
    int    focus_hist_n;

    /* --- settings: global defaults first, then every matching DesktopRule,
     *     applied at creation and re-applied on config reload --- */
    Layout  layout;
    float   master_ratio;
    int     n_master;       /* number of windows in the master area       */
    /* Gap overrides, -1 meaning "use the global". Per desktop rather than
     * global-only because the desktops that want different gaps are usually
     * the ones with a different job — a full-bleed video desktop against a
     * roomy editing one. */
    int     inner_gap;
    int     outer_gap;
    bool    float_all;      /* windows opened here start floating         */
    int     monitor;        /* pinned display, or -1 for "wherever it opens" */
    wchar_t app[MAX_PATH];  /* auto-launch while empty; "" = none         */
    wchar_t app_args[SPAWN_ARGS_MAX];  /* its arguments; "" = none        */
    wchar_t app_cwd[MAX_PATH];         /* its working directory; "" = ours */

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
    wchar_t app_cwd[MAX_PATH];              /* working directory; "" = ours  */

    bool    set_float;    bool   float_all;
    bool    set_layout;   Layout layout;
    bool    set_ratio;    float  master_ratio;
    bool    set_nmaster;  int    n_master;
    bool    set_gaps;     int    inner_gap, outer_gap;
    bool    set_monitor;  int    monitor;
} DesktopRule;

/* ---------------------------------------------------------------------------
 * Events a config can hook with mshell.on(name, fn).
 *
 * Deliberately few. Each one fires from inside window or desktop bookkeeping,
 * so every handler is main-thread work happening between the user pressing a
 * key and the screen updating — a slow one is felt directly. These are the
 * points where a config plausibly needs to react to something it did not do
 * itself; anything more fine-grained belongs behind the IPC channel instead.
 * --------------------------------------------------------------------------- */
typedef enum {
    LUA_EVENT_WINDOW_OPEN = 0,   /* a window came under management        */
    LUA_EVENT_WINDOW_CLOSE,      /* one is about to leave it              */
    LUA_EVENT_DESKTOP_SWITCH,    /* the visible desktop changed           */
    LUA_EVENT_FOCUS,             /* the focused window changed            */
    LUA_EVENT_COUNT
} LuaEvent;

#define MAX_LUA_HOOKS 32

typedef struct {
    LuaEvent event;
    int      ref;    /* registry ref, owned by the current lua_State */
} LuaHook;

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
    wchar_t *cwd;    /* owned; NULL to inherit ours */
} StartupCommand;

/* ---------------------------------------------------------------------------
 * Monitor — one physical display
 * --------------------------------------------------------------------------- */
typedef struct {
    HMONITOR handle;
    RECT     full;          /* full monitor bounds                        */
    RECT     work_area;     /* usable area (== full when we are the shell) */
    UINT     dpi;

    /* A STABLE identity, unlike the index or the HMONITOR — both of which
     * change when a display is unplugged and plugged back in. This is what lets
     * a per-monitor override, and a window's home, survive a hotplug instead of
     * silently landing on whichever display took the vacated slot. */
    wchar_t  device[CCHDEVICENAME];

    /* Per-monitor overrides, -1 / LAYOUT_COUNT meaning "inherit". A desktop
     * spans every display here (0.11.0 declined per-monitor tags and said why),
     * so these describe the DISPLAY's habits: a vertical secondary that always
     * wants columns, an ultrawide that wants a different master ratio. */
    int      inner_gap, outer_gap;
    int      n_master;
    float    master_ratio;   /* <= 0 = inherit */
    Layout   layout;         /* LAYOUT_COUNT = inherit */
} Monitor;

/* One monitor's configured overrides, matched by device name. Kept separate
 * from Monitor because a config is loaded before the displays it names are
 * necessarily attached, and has to survive them coming and going. */
typedef struct {
    wchar_t device[CCHDEVICENAME];   /* wildcard pattern, or "" for index form */
    int     index;                   /* -1 when matched by name instead        */
    bool    set_gaps;    int   inner_gap, outer_gap;
    bool    set_nmaster; int   n_master;
    bool    set_ratio;   float master_ratio;
    bool    set_layout;  Layout layout;
} MonitorRule;

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
    /* Empty = match any title. Often the ONLY thing that separates two windows
     * of one app — a picture-in-picture player, a splash screen, a specific
     * dialog — where class and process are identical. Note a title is the one
     * criterion that CHANGES while the window lives, so this matches whatever
     * it says at the moment the window is adopted. */
    wchar_t    title_match[256];
    bool       set_dialog;            /* false = don't care what it is      */
    bool       dialog;                /* required window_is_dialog() answer */
    RuleAction action;
    bool       no_ring;             /* don't draw the focus ring around it   */
    bool       no_decor;            /* strip the frame, add no border at all */
    bool       fullscreen;          /* while floating: cover the monitor     */

    /* --- where the window goes, rather than how it looks ---
     * Before these, a window always landed on whichever desktop you happened to
     * be looking at; "open Slack on chat" was expressible only as a desktop
     * rule's auto-launch, which is a different thing (it fires when you ARRIVE
     * on an empty desktop, not when the app opens). */
    wchar_t    desktop[DESKTOP_NAME_MAX];  /* "" = the current desktop        */
    bool       set_monitor;  int monitor;  /* pin to a display                */
    bool       set_geometry;               /* fixed rect for a FLOATING window */
    int        x, y, w, h;
    bool       start_fullscreen;           /* claim the monitor on open        */
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
    /* Per-state ring colours. A tiled window and a floating one behave
     * differently enough — one obeys the layout, the other does not — that
     * telling them apart at a glance is worth a colour. Urgent is a window that
     * asked for attention while you were elsewhere. Both default to
     * border_color, so a config that never sets them sees no change. */
    COLORREF border_color_float;
    COLORREF border_color_urgent;
    /* DWMWA_WINDOW_CORNER_PREFERENCE for MANAGED windows. Square by default
     * because a tiled grid with rounded corners has gaps at every junction that
     * the gap setting did not ask for. */
    int      corner_pref;     /* DWMWCP_* */
    COLORREF background_color;/* solid desktop backdrop color                 */

    /* --- status bar ---
     * With no taskbar there is otherwise nothing on screen telling you which
     * desktop you are on — and since desktops are created and destroyed as you
     * use them, the set itself is invisible without this. */
    bool     bar_enabled;
    bool     bar_bottom;      /* false = top edge (default), true = bottom     */
    int      bar_height;      /* design pixels at 96 DPI; scaled per monitor   */
    unsigned bar_modules;     /* BAR_MOD_* bitmask, drawn left to right        */
    COLORREF bar_bg, bar_fg, bar_accent, bar_dim;
    HWND     bar_windows[MAX_MONITORS];   /* one per display, or NULL          */

    /* --- which-key submap hint --- */
    /* Urgency tracking is OPT-IN because it costs a hook on
     * EVENT_OBJECT_STATECHANGE, which fires for every control on the system.
     * 0.8.0 deliberately narrowed the object range to stop exactly that
     * traffic, so switching it back on is the user's call, not the default. */
    /* "never" restores a window the moment it is minimized. Off by default
     * because 0.8.0 added minimize/restore precisely so a window COULD be got
     * out of the way when there is no taskbar, and 0.11.0 made minimise-to-tray
     * work — this un-does both for people who would rather no window ever
     * vanish. App-initiated tray hides are exempt either way. */
    SplitMode next_split;     /* direction the next BSP insertion uses     */

    /* Both off by default: they are the two features that cost frames rather
     * than bytes, and a tiling WM's appeal is that windows are where you put
     * them instantly. 0 disables the animation entirely. */
    int      anim_ms;         /* movement duration; 0 = place instantly     */
    bool     dim_enabled;     /* scrim over everything but the focused window */
    COLORREF dim_color;
    BYTE     dim_alpha;       /* the scrim's alpha, not any window's       */
    bool     update_check;    /* ask GitHub, at most daily, whether a newer
                               * release exists. Notify-only — see update.c  */
    bool     minimize_never;
    bool     urgency_enabled;
    bool     notify_enabled;     /* show mshell's own on-screen notifications  */
    bool     notify_desktop;     /* announce a desktop switch (the bar usually
                                  * already lists them, so off by default)      */
    bool     whichkey_enabled;   /* show a hint panel when a submap is active  */
    int      whichkey_delay;     /* ms before it appears (0 = instant)         */
    COLORREF whichkey_bg;        /* panel background                           */
    COLORREF whichkey_fg;        /* action-label text                          */
    COLORREF whichkey_key_fg;    /* key + header accent                        */
    COLORREF whichkey_border;    /* panel outline                              */

    /* --- tiling policy --- */
    FloatPolicy  float_policy;   /* FLOAT_RULES (default) or FLOAT_NEVER      */
    HidePolicy   hide_policy;    /* HIDE_CLOAK (default) or HIDE_SHOWWINDOW   */
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
    HWND     notify_window;   /* layered overlay: mshell's own toasts         */
    HWND     launcher_window; /* layered overlay: the app launcher            */
    /* While true the keyboard hook is in CAPTURE MODE: every key is translated
     * and forwarded to the launcher, and nothing reaches the keymaps or the
     * foreground app. Read on the hook thread, written on the main one — a
     * single bool, and the failure mode of a stale read is one keystroke going
     * the other way. Escape and the panic action both clear it. */
    bool     launcher_open;
    HWINEVENTHOOK statechange_hook;  /* only while urgency_enabled */

    /* --- hooks --- */
    HHOOK         kb_hook;
    HHOOK         mouse_hook;   /* WH_MOUSE_LL, only while mouse_mod_drag */
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
    HWINEVENTHOOK movesize_hook;     /* EVENT_SYSTEM_MOVESIZESTART/END        */

    /* --- mouse ---
     * A tiled window cannot really be "moved": the layout owns its geometry.
     * So a drag is interpreted instead — dropped onto another tile, the two
     * swap; dragged along the master split, the ratio follows. drag_hwnd is the
     * window a drag is in progress on, NULL when none. */
    bool     mouse_enabled;      /* drag a tile onto another to swap them     */
    bool     mouse_follow;       /* focus follows the pointer (polled)        */
    bool     mouse_mod_drag;     /* Win+drag moves / Win+right-drag resizes a
                                  * FLOATING window. Opt-in: it is the one part
                                  * that needs a WH_MOUSE_LL hook, on the thread
                                  * that must also answer the keyboard hook. */
    HWND     mod_drag_hwnd;      /* window being Mod+dragged, or NULL          */
    HWND     drag_hwnd;
    POINT    drag_start;

    /* --- monitors (re-queried on display change) --- */
    Monitor  monitors[MAX_MONITORS];
    MonitorRule monitor_rules[MAX_MONITOR_RULES];
    int      monitor_rule_count;
    int      monitor_count;
    int      primary_monitor;
    int      focused_monitor;

    /* --- primary-monitor work area (kept for fallbacks / single-monitor) --- */
    RECT     work_area;

    /* --- config --- */
    wchar_t   config_path[MAX_PATH];
    /* The last config load failure, as Lua reported it (UTF-8). The log is not
     * always the right place to look — `--check` prints this straight to the
     * console the user is standing in front of. */
    char      config_error[512];
    bool      auto_reload;    /* reload when the config file is saved         */
    StartupCommand startup_commands[MAX_STARTUP_COMMANDS];
    int       startup_count;
    lua_State *L;             /* live Lua VM (rebuilt on reload)      */

    /* Bumped on every successful config load. A Lua registry ref belongs to the
     * lua_State that created it, and a reload closes that state — so a
     * keystroke queued before a reload must not call a ref through the new one.
     * Actions carry the generation they were dispatched under. */
    unsigned  config_gen;

    /* Handlers registered with mshell.on(). Refs belong to g.L and die with it,
     * so a reload only has to reset the count. */
    LuaHook   lua_hooks[MAX_LUA_HOOKS];
    int       lua_hook_count;

    /* True while a Lua callback is running on the main thread. Guards against
     * re-entering Lua from inside itself, and marks the window in which the
     * config-mutating API calls are not allowed. */
    bool      lua_running;

    /* --- instance --- */
    HINSTANCE hinst;

    /* --- run mode --- */
    bool     test_mode;       /* running alongside explorer, not as shell */
    bool     panicked;        /* ACTION_PANIC fired: the keyboard hook passes
                               * everything through and Explorer is running
                               * alongside us. Cleared by a config reload. */
    bool     safe_mode;       /* repeated fast restarts detected: init.lua is
                               * skipped this run in favour of the built-in
                               * keymap, because a config that crashes us at
                               * startup otherwise loops forever and there is no
                               * shell left to fix it from */
    bool     elevated;        /* we hold an elevated token. The config is then
                               * effectively administrator-level code, so
                               * auto-reload (which would execute it silently on
                               * any write) is disabled — see config_watch_sync */

    /* Logging state lives in log.c, not here: mshelld.exe shares that
     * translation unit and has no MShell to hang it off. */
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
void     monitors_update(void);
void     monitors_apply_rules(void);  /* per-display overrides, by device name */           /* (re)enumerate physical displays */

/* Re-enumerate displays AND re-apply the bar's reservation, then refresh the
 * cached primary work area. Anything that changes the display set, the DPI, or
 * the bar's geometry must go through this before tiling. */
void     update_work_area(void);
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
                        const wchar_t *args, const wchar_t *cwd);

/* Launch `cmd` with `args` (either may be NULL/empty). ShellExecuteW, so PATH
 * is resolved and .lnk shortcuts work — which is why arguments have to be a
 * separate string rather than baked into the command. `ctx` names the caller
 * ("startup", "keybind", a desktop name) in the failure message, because a
 * program that silently never appears is indistinguishable from mshell
 * ignoring the request. Returns false and logs when the launch fails. */
bool     spawn_command(const wchar_t *cmd, const wchar_t *args,
                       const wchar_t *cwd, const wchar_t *ctx);

/* Copy out the action a WM_MSHELL_ACTION message refers to (its lParam is the
 * sequence number). The hook records actions BY VALUE rather than posting the
 * KeyBinding pointer, because a config reload frees every binding while another
 * keystroke may still be sitting in the queue behind it. False if the ring
 * lapped before the main thread drained it. */
bool     kb_take_pending(unsigned seq, Action *action, int *arg,
                         wchar_t *cmd, size_t cmd_cap,
                         wchar_t *args, size_t args_cap,
                         wchar_t *cwd, size_t cwd_cap, int *count);

/* Is repeating this action meaningful and safe? Counts repeat motion and sizing;
 * they must never turn one keystroke into three shutdowns or three spawns. */
bool     action_is_repeatable(Action action);
KeyMap  *keymap_new(const wchar_t *name, bool persist);
void     keymap_add_binding(KeyMap *map, DWORD mods, DWORD vk,
                            Action action, int arg, KeyMap *submap,
                            const wchar_t *command, const wchar_t *args,
                            const wchar_t *cwd, const wchar_t *desc,
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

/* Focus nothing — hand the foreground to the backdrop and hide the focus ring.
 * Call it where there is no window to focus (an empty desktop): a CLOAKED
 * window keeps the foreground, so without this the window you just left would
 * still be taking your keystrokes, invisibly. Replaces the bare border_hide()
 * such places used to do. */
void     window_focus_none(void);
HWND     window_get_focused(void);
ManagedWindow *window_find(HWND hwnd);
/* Assign a window's display AND remember it by name — see the definition for
 * why an index alone does not survive a hotplug. */
bool     window_set_monitor(ManagedWindow *mw, int mon);
void     window_manage_existing(void);
void     window_restore_all_decorations(void);

/* Uncloak windows a previous mshell died without releasing. Called by
 * window_manage_existing() before it enumerates, because is_manageable rejects
 * cloaked windows and would otherwise leave them stranded and invisible. */
void     window_uncloak_strays(void);

/* Re-show everything mshell hid (other desktops, monocle's non-focused
 * windows). A hidden window has no taskbar button and no Alt+Tab entry, so
 * without this they are unreachable once mshell exits. Call it before
 * window_restore_all_decorations(). */
void     window_restore_all_visibility(void);

/* ---------------------------------------------------------------------------
 * Taking a window off the screen, and putting it back.
 *
 * The single door for every "mshell removes this window from view" — desktop
 * switches, monocle, the scratchpad — so that WHICH mechanism is used
 * (see HidePolicy) is decided in exactly one place. Both are idempotent, so
 * callers may hide a window that is already hidden.
 *
 * Neither touches a window the APP hid (app_hidden): that one is not ours.
 * --------------------------------------------------------------------------- */
void     window_hide(ManagedWindow *mw);
void     window_show(ManagedWindow *mw);

/* Is this window actually on the screen right now? Under HIDE_CLOAK a hidden
 * window is still WS_VISIBLE, so IsWindowVisible() alone answers "yes" for
 * every window on every desktop. */
bool     window_on_screen(const ManagedWindow *mw);
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

/* ---------------------------------------------------------------------------
 * Prototypes — layout_tree.c (manual/BSP tiling and tabbed containers)
 *
 * The tree does NOT own the windows: Desktop.windows[] remains the membership
 * store and the tree is an index synchronised to it on every pass. See the
 * file header for why.
 * --------------------------------------------------------------------------- */
void     layout_tree_run(Desktop *dt, RECT area, TreeEmitFn emit, void *ctx);
void     layout_tree_set_split(SplitMode mode);
void     layout_tree_rotate(void);
void     layout_tree_set_container(SplitMode mode);
void     layout_tree_cycle_container(int delta);
void     layout_tree_resize(float delta);
void     layout_tree_forget(int desktop_id);

/* ---------------------------------------------------------------------------
 * Prototypes — anim.c (movement animation, unfocused-window dimming)
 * --------------------------------------------------------------------------- */
bool     anim_begin(HWND hwnd, RECT from, RECT to);
void     anim_tick(void);
bool     anim_is_animating(HWND hwnd);
void     anim_cancel_all(void);
bool     anim_dim_init(void);
void     anim_dim_shutdown(void);
void     anim_dim_refresh(void);
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
HWND     desktop_last_window(void);      /* previous focus on THIS desktop */

/* ---------------------------------------------------------------------------
 * Prototypes — system.c (session/power actions, media keys)
 * --------------------------------------------------------------------------- */
void     system_lock(void);
void     system_logoff(void);
void     system_reboot(void);
void     system_shutdown(void);
void     system_sleep(void);
void     system_hibernate(void);
void     system_media_key(Action action);

/* ---------------------------------------------------------------------------
 * Prototypes — screenshot.c
 * --------------------------------------------------------------------------- */
void     screenshot_screen(void);
void     screenshot_window(void);

/* ---------------------------------------------------------------------------
 * Prototypes — notify.c (mshell's OWN on-screen messages; deliberately not a
 * host for other applications' notifications — see the file header)
 * --------------------------------------------------------------------------- */
bool     notify_init(void);
void     notify_shutdown(void);
void     notify_show(const wchar_t *text, NotifyKind kind, int ms);

/* ---------------------------------------------------------------------------
 * Prototypes — launcher.c
 * --------------------------------------------------------------------------- */
bool     launcher_init(void);
void     launcher_shutdown(void);
void     launcher_open(void);
void     launcher_close(void);
void     launcher_key(DWORD vk, wchar_t ch);
#define NOTIFY_TEXT_CAP 512   /* matches notify.c's per-toast buffer */

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
 * Prototypes — helper.c (the privileged helper, mshelld.exe)
 *
 * Optional and absent by default. mshell attempts every placement itself and
 * only forwards the ones Windows refuses because the target belongs to a
 * higher-integrity process (UIPI) — so with no helper running, behaviour is
 * exactly what it was: such windows float instead of tiling.
 * =========================================================================== */
void     helper_init(void);
void     helper_shutdown(void);
bool     helper_available(void);
bool     helper_set_window_pos(HWND hwnd, int x, int y, int w, int h, UINT flags);

/* SetWindowPos that falls back to the helper when the local call is refused.
 * Use this for placement; the raw API is still right for our own overlays. */
bool     window_set_pos(HWND hwnd, int x, int y, int w, int h, UINT flags);

/* ===========================================================================
 * Prototypes — session.c
 *
 * Per-desktop layout / master-ratio / master-count, remembered by desktop NAME
 * across restarts. Window placement is deliberately NOT saved: an HWND means
 * nothing next boot, and guessing from titles would scatter your windows.
 * =========================================================================== */
void     session_load(void);              /* read the file; call once at start */
void     session_apply(Desktop *dt);      /* from desktop_apply_rules          */
void     session_save(void);              /* whenever a saved value changes    */
const wchar_t *session_start_desktop(void);  /* last desktop, or NULL          */

/* ===========================================================================
 * Prototypes — bar.c (status bar)
 * =========================================================================== */
/* Mouse drag handling (events.c). A tiled drag is not a move — see the note on
 * g.drag_hwnd. */
void     mouse_drag_begin(HWND hwnd);
void     mouse_poll_focus(void);
bool     mouse_mod_drag_event(WPARAM msg, POINT pt, bool mod_held);
void     mouse_mod_drag_apply(int dx, int dy);
void     mouse_sync_hook(void);   /* install/remove the WH_MOUSE_LL hook */
void     mouse_drag_end(HWND hwnd);

bool     bar_init(void);
void     bar_shutdown(void);

/* Create/destroy/reposition the per-monitor bar windows to match the current
 * config and display set. Call after a config load and on a display change. */
void     bar_reconfigure(void);

/* Subtract the bar from each monitor's work_area so the tiler lays out beneath
 * it. Called from update_work_area(), right after monitors_update(). Fullscreen
 * windows use monitor .full and so still cover the bar, which is correct. */
void     bar_reserve_work_area(void);

/* Rebuild the bar text and repaint if it changed. Cheap to call often — it
 * compares against what is already displayed and does nothing when equal. */
void     bar_refresh(void);

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
void     events_sync_urgency(void);   /* opt-in STATECHANGE hook on/off */
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

/* ===========================================================================
 * Prototypes — ipc.c (control a running mshell from the command line)
 * =========================================================================== */

/* Handle --msg / --query. True if this invocation was a client command, in
 * which case the shell must not start. Call first in WinMain. */
bool     ipc_client_try(int *exit_code);

/* Write a line to the PARENT console. mshell is a GUI-subsystem binary with no
 * console of its own, so this is the only way a command-line invocation can say
 * anything; it is a no-op when there is no parent console. */
void     console_print(const char *s);

/* ---------------------------------------------------------------------------
 * Prototypes — tweaks.c (registry tweaks with a real backup/restore)
 * --------------------------------------------------------------------------- */
int      tweaks_apply(const wchar_t *group);
int      tweaks_revert(const wchar_t *group);
void     tweaks_list(void);
void     tweaks_emit_reg(const wchar_t *group, bool undo);

/* ---------------------------------------------------------------------------
 * Prototypes — update.c
 * --------------------------------------------------------------------------- */
void     update_check_async(void);

void     ipc_start(void);   /* begin serving the per-session named pipe */
void     ipc_stop(void);
void     ipc_handle_request(void *req);   /* WM_MSHELL_IPC — main thread only */

/* Canonical name of a layout — the same spelling set_layout accepts. */
const char *layout_to_name(Layout l);

void     lua_register_api(lua_State *L);

/* Call a config-supplied Lua function by registry ref, on the main thread.
 * Wrapped in lua_pcall and guarded against re-entry, so an error in the config
 * is logged rather than unwound through a WinEvent callback. No-op if no VM is
 * live. */
void     lua_run_ref(int ref);

/* Fire every handler registered for `ev`. `hwnd` is the window the event is
 * about (NULL when it isn't about one) and `name` the desktop name for
 * DESKTOP_SWITCH. Cheap and safe to call when no config registered anything —
 * it returns immediately. Main thread only. */
void     lua_fire(LuaEvent ev, HWND hwnd, const wchar_t *name);

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

/* Logging lives in log.c / log.h, which mshell.h includes at the top.
 *
 * log_err() and log_w() are still the two names to reach for and still mean
 * what they did: log_err is ERROR and is always written — as the shell there is
 * no console, taskbar or tray, so the file is the only channel, and a rejected
 * config that logged nothing is indistinguishable from "mshell ignored my
 * keybinds". log_w is DEBUG: per-keystroke matches, tiling passes, focus
 * changes, off until the level is raised. Reserve log_err for things that are
 * actually wrong so the file stays a few lines long in normal operation, and
 * use log_msg(LOG_INFO, ...) / log_msg(LOG_WARN, ...) for the middle ground
 * that previously had to borrow one of the two. */
