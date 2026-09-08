#pragma once

#include <stdbool.h>
#include <stddef.h>

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
    ACTION_DESKTOP_TO_MONITOR,

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

    /* Show/hide the status bar. Worth a binding mostly for floating mode,
     * where the panel sits over the middle of the screen and wanting it out of
     * the way for a moment is the normal case. */
    ACTION_TOGGLE_BAR,

    /* Switch the bar between its two modes without toggling visibility. */
    ACTION_BAR_TOP,
    ACTION_BAR_FLOATING,

    /* The physical display, on the focused monitor. Bindable because these are
     * the display settings people change per task rather than once — HDR only
     * while a game is up, a lower refresh rate on battery, a secondary turned
     * on its side to read — and with no Explorer there is no Settings page a
     * keystroke away. `cycle_refresh` and `cycle_rotation` take +1 / -1 like
     * the other steppers. */
    ACTION_TOGGLE_HDR,
    ACTION_CYCLE_REFRESH,
    ACTION_CYCLE_ROTATION,
    ACTION_TOGGLE_PORTRAIT,

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

    /* Fetch the latest GitHub release and hand it to its own install.bat.
     * Unlike the daily check this one applies — see update.c for why a
     * keystroke is the thing that makes that difference. */
    ACTION_UPDATE,

    /* Last resort: start Explorer alongside us and stop swallowing keys, so a
     * machine whose shell is misbehaving stays usable without Task Manager.
     * Deliberately does not exit — quitting as the shell is what logs you out. */
    ACTION_PANIC,

    /* Stop and restart mshelld's logon task, so the helper on disk becomes the
     * helper that is running. An update does this through install.bat; this is
     * the same thing for a binary put in place by hand, or a helper that has
     * stopped answering. */
    ACTION_RESTART_HELPER,

    ACTION_COUNT
} Action;

typedef enum {
    API_ACTION = 0,
    API_METHOD,
    API_SETTER,
    API_QUERY,
    API_NAMESPACE,
} ApiKind;

enum {
    API_CONFIG_ONLY  = 1 << 0,
    API_RUNTIME_ONLY = 1 << 1,
    API_REPEATABLE   = 1 << 2,
    API_TAKES_WINDOW = 1 << 3,
    API_PAYLOAD_STR  = 1 << 4,
    API_PAYLOAD_NUM  = 1 << 5,
    API_CALLABLE     = 1 << 6,
};

typedef struct {
    const char *path;
    const char *legacy_name;
    Action      action;
    ApiKind     kind;
    unsigned    flags;
    const char *doc;
    const char *luals_params;
    const char *luals_returns;
} ApiEntry;

typedef struct {
    const char *removed;
    const char *replacement;
} ApiRemoved;

extern const ApiEntry   api_spec[];
extern const ApiRemoved api_removed[];

const char *api_removed_replacement(const char *name);

int  api_spec_count(void);

const ApiEntry *api_spec_at(int index);
const ApiEntry *api_spec_by_path(const char *path);
const ApiEntry *api_spec_by_legacy(const char *name);
const ApiEntry *api_spec_by_action(Action action);

Action      api_action_from_name(const char *name);
const char *api_action_path(Action action);
bool        api_action_repeatable(Action action);

int api_payload_index(const ApiEntry *e, bool first_arg_is_window);
