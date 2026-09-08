#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ACTION_NONE = 0,

    ACTION_FOCUS_LEFT,
    ACTION_FOCUS_DOWN,
    ACTION_FOCUS_UP,
    ACTION_FOCUS_RIGHT,
    ACTION_FOCUS_NEXT,
    ACTION_FOCUS_PREV,

    ACTION_MOVE_LEFT,
    ACTION_MOVE_DOWN,
    ACTION_MOVE_UP,
    ACTION_MOVE_RIGHT,

    ACTION_SWITCH_DESKTOP,
    ACTION_MOVE_TO_DESKTOP,
    ACTION_LAST_DESKTOP,
    ACTION_NEXT_DESKTOP,
    ACTION_PREV_DESKTOP,

    ACTION_FOCUS_MONITOR_NEXT,
    ACTION_FOCUS_MONITOR_PREV,
    ACTION_MOVE_TO_MONITOR_NEXT,
    ACTION_MOVE_TO_MONITOR_PREV,
    ACTION_DESKTOP_TO_MONITOR,

    ACTION_CLOSE,
    ACTION_KILL,
    ACTION_MINIMIZE,
    ACTION_RESTORE,
    ACTION_TOGGLE_STICKY,
    ACTION_MARK_SCRATCHPAD,
    ACTION_TOGGLE_SCRATCHPAD,
    ACTION_TOGGLE_ALWAYS_ON_TOP,
    ACTION_LAST_WINDOW,

    ACTION_TOGGLE_FLOAT,

    ACTION_RESIZE_LEFT,
    ACTION_RESIZE_DOWN,
    ACTION_RESIZE_UP,
    ACTION_RESIZE_RIGHT,

    ACTION_FULLSCREEN,
    ACTION_FULLSCREEN_CONTENT,
    ACTION_FULLSCREEN_BOTH,

    ACTION_LAYOUT_TILING,
    ACTION_LAYOUT_MONOCLE,
    ACTION_LAYOUT_GRID,
    ACTION_LAYOUT_SPIRAL,
    ACTION_LAYOUT_CENTERED,
    ACTION_LAYOUT_BSTACK,
    ACTION_LAYOUT_COLUMNS,
    ACTION_CYCLE_LAYOUT,
    ACTION_PROMOTE_MASTER,
    ACTION_ZOOM,
    ACTION_INC_MASTER,
    ACTION_DEC_MASTER,
    ACTION_INC_NMASTER,
    ACTION_DEC_NMASTER,
    ACTION_INC_CFACT,
    ACTION_DEC_CFACT,
    ACTION_RESET_CFACT,

    ACTION_ENTER_SUBMAP,

    ACTION_SPAWN,

    ACTION_LUA_CALL,

    ACTION_LOCK,
    ACTION_LOGOFF,
    ACTION_REBOOT,
    ACTION_SHUTDOWN,
    ACTION_SLEEP,
    ACTION_HIBERNATE,

    ACTION_VOLUME_UP,
    ACTION_VOLUME_DOWN,
    ACTION_VOLUME_MUTE,
    ACTION_MEDIA_PLAY,
    ACTION_MEDIA_NEXT,
    ACTION_MEDIA_PREV,
    ACTION_MEDIA_STOP,

    ACTION_SCREENSHOT,
    ACTION_SCREENSHOT_WINDOW,

    ACTION_NOTIFY,

    ACTION_JUMP_URGENT,

    ACTION_LAUNCHER,

    ACTION_TOGGLE_BAR,

    ACTION_BAR_TOP,
    ACTION_BAR_FLOATING,

    ACTION_TOGGLE_HDR,
    ACTION_CYCLE_REFRESH,
    ACTION_CYCLE_ROTATION,
    ACTION_TOGGLE_PORTRAIT,

    ACTION_SPLIT_H,
    ACTION_SPLIT_V,
    ACTION_ROTATE_SPLIT,
    ACTION_TOGGLE_TABBED,
    ACTION_TOGGLE_STACKED,
    ACTION_CONTAINER_NEXT,
    ACTION_CONTAINER_PREV,
    ACTION_SPLIT_GROW,
    ACTION_SPLIT_SHRINK,
    ACTION_LAYOUT_BSP,

    ACTION_RELOAD,
    ACTION_QUIT,

    ACTION_UPDATE,

    ACTION_PANIC,

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
