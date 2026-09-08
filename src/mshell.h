#pragma once

#ifndef MSHELL_VERSION
#define MSHELL_VERSION "dev"
#endif

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
#include "desktop_list.h"
#include "api_spec.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#ifndef MOD_LWIN
#define MOD_LWIN MOD_WIN
#endif

#define WM_MSHELL_ACTION  (WM_APP + 1)

#define MSHELL_INPUT_TAG  ((ULONG_PTR)0x6D736831)

#define TIMER_CRASHLOOP_HEALTHY  1
#define TIMER_FOLLOW_MOUSE       2
#define TIMER_ANIM               3
#define TIMER_RESINK             4
#define TIMER_SINK_VERIFY        5
#define FOLLOW_MOUSE_MS          120
#define RESINK_RETRY_MS          500
#define RESINK_RETRIES           6
#define SINK_VERIFY_MS           250
#define SINK_WALK_MAX            512
#define ZORDER_WALK_MAX          512
#define WM_MSHELL_SUBMAP  (WM_APP + 2)
#define WM_MSHELL_CONFIG_CHANGED  (WM_APP + 3)

#define WM_MSHELL_IPC             (WM_APP + 4)

#define WM_MSHELL_MOUSE           (WM_APP + 5)

#define WM_MSHELL_CAPTURE_KEY     (WM_APP + 6)

#define WM_MSHELL_UPDATE          (WM_APP + 7)

#define WM_MSHELL_RESTART         (WM_APP + 8)

#define MAX_DESKTOPS              32
#define MAX_WINDOWS_PER_DESKTOP   256
#define FOCUS_HIST_MAX            8
#define MAX_BINDINGS_PER_MAP      256
#define MAX_KEYMAPS               32
#define MAX_RULES                 128
#define MAX_DESKTOP_RULES         64
#define MAX_STARTUP_COMMANDS      32
#define SPAWN_ARGS_MAX            512
#define MAX_MANAGED_WINDOWS       512
#define MAX_MONITORS              8
#define MAX_MONITOR_RULES         8
#define DESKTOP_NAME_MAX          64

#define DEFAULT_INNER_GAP         4
#define DEFAULT_OUTER_GAP         4
#define DEFAULT_BORDER_WIDTH      2
#define DEFAULT_MASTER_RATIO      0.6f
#define DEFAULT_NMASTER           1
#define DEFAULT_START_DESKTOP     L"1"
#define DEFAULT_MIN_WIN_W         120
#define DEFAULT_MIN_WIN_H         80
#define DEFAULT_BORDER_COLOR      RGB(0xff, 0xff, 0xff)
#define DEFAULT_BACKGROUND_COLOR  RGB(0x00, 0x00, 0x00)

#define DEFAULT_BAR_HEIGHT        28
#define DEFAULT_BAR_BG            RGB(0x1e, 0x1e, 0x2e)
#define DEFAULT_BAR_FG            RGB(0xcd, 0xd6, 0xf4)
#define DEFAULT_BAR_ACCENT        RGB(0x7a, 0xa2, 0xf7)
#define DEFAULT_BAR_DIM           RGB(0x6c, 0x70, 0x86)

typedef enum {
    BAR_MODE_TOP_BAR = 0,
    BAR_MODE_FLOATING,
} BarMode;
#define DEFAULT_BAR_MODE          BAR_MODE_TOP_BAR

#define BAR_MOD_DESKTOPS      0x1
#define BAR_MOD_LAYOUT        0x2
#define BAR_MOD_TITLE         0x4
#define BAR_MOD_CLOCK         0x8
#define BAR_MOD_NOTIFICATIONS 0x10
#define BAR_MOD_DEFAULT   (BAR_MOD_DESKTOPS | BAR_MOD_LAYOUT | \
                           BAR_MOD_TITLE    | BAR_MOD_CLOCK  | \
                           BAR_MOD_NOTIFICATIONS)

#define DEFAULT_WHICHKEY_DELAY    150
#define DEFAULT_WHICHKEY_BG       RGB(0x1e, 0x1e, 0x2e)
#define DEFAULT_WHICHKEY_FG       RGB(0xcd, 0xd6, 0xf4)
#define DEFAULT_WHICHKEY_KEY_FG   RGB(0x7a, 0xa2, 0xf7)
#define DEFAULT_WHICHKEY_BORDER   RGB(0x7a, 0xa2, 0xf7)

#define WHICHKEY_MAX_ROWS         64
#define DEFAULT_WHICHKEY_MARGIN   -1
#define DEFAULT_WHICHKEY_PADDING  14
#define DEFAULT_WHICHKEY_ROW_GAP  6
#define DEFAULT_WHICHKEY_COL_GAP  30
#define DEFAULT_WHICHKEY_KEY_GAP  10
#define DEFAULT_WHICHKEY_HDR_GAP  8
#define DEFAULT_WHICHKEY_MAX_ROWS 12
#define DEFAULT_WHICHKEY_FONT     L"Segoe UI"
#define DEFAULT_WHICHKEY_FONT_SIZE 18
#define DEFAULT_WHICHKEY_BORDER_W 1
#define DEFAULT_WHICHKEY_OPACITY  235

typedef enum {
    LAYOUT_TILING = 0,
    LAYOUT_MONOCLE,
    LAYOUT_GRID,
    LAYOUT_SPIRAL,
    LAYOUT_CENTERED,
    LAYOUT_BSTACK,
    LAYOUT_COLUMNS,
    LAYOUT_BSP,
    LAYOUT_COUNT
} Layout;

typedef enum {
    SPLIT_V = 0,
    SPLIT_H,
    SPLIT_TABBED,
    SPLIT_STACKED,
} SplitMode;

typedef void (*TreeEmitFn)(HWND hwnd, RECT area, void *ctx);

typedef enum {
    NOTIFY_INFO = 0,
    NOTIFY_WARN,
    NOTIFY_ERROR,
} NotifyKind;

typedef struct {
    RECT  area;
    int   inner;
    float master_ratio;
    int   n_master;
    HWND  focus;
} LayoutParams;

typedef enum {
    RULE_MANAGE = 0,
    RULE_FLOAT,
    RULE_IGNORE,
} RuleAction;

typedef enum {
    FLOAT_RULES = 0,
    FLOAT_NEVER,
} FloatPolicy;

typedef enum {
    HIDE_CLOAK = 0,
    HIDE_SHOWWINDOW,
} HidePolicy;

typedef enum {
    FLOAT_PLACE_CENTER = 0,
    FLOAT_PLACE_NONE,
} FloatPlacement;

typedef enum {
    FS_OFF = 0,
    FS_WINDOW,
    FS_CONTENT,
    FS_BOTH,
} FullscreenMode;

typedef enum {
    WK_POS_BOTTOM = 0,
    WK_POS_TOP,
    WK_POS_CENTER,
    WK_POS_LEFT,
    WK_POS_RIGHT,
    WK_POS_TOP_LEFT,
    WK_POS_TOP_RIGHT,
    WK_POS_BOTTOM_LEFT,
    WK_POS_BOTTOM_RIGHT,
} WhichKeyPos;

typedef struct KeyMap KeyMap;

typedef struct {
    DWORD    mod_flags;
    DWORD    vk;
    Action   action;
    int      arg;
    KeyMap  *submap;
    wchar_t *command;
    wchar_t *args;
    wchar_t *cwd;
    wchar_t *desc;
    bool     terminal;
} KeyBinding;

struct KeyMap {
    wchar_t    *name;
    KeyBinding *bindings;
    int         count;
    int         capacity;
    bool        persist;
    DWORD       exit_vk;
};

typedef struct {
    HWND      hwnd;
    int       desktop_id;
    int       monitor;
    wchar_t   monitor_device[CCHDEVICENAME];
    bool      is_floating;
    bool      tracked_only;
    bool      no_ring;
    bool      no_decor;
    bool      fullscreen;
    bool      center_float;
    bool      decorations_stripped;
    bool      decor_strip_refused;
    LONG_PTR  orig_style;
    LONG_PTR  orig_exstyle;
    float     cfact;
    RECT      applied_rect;
    bool      has_applied;
    ULONGLONG snap_first_at;
    int       snap_tries;
    int       dpi_settle_left;

    FullscreenMode fs_mode;
    bool      app_fullscreen;
    RECT      fs_prev_rect;
    bool      fs_has_prev;
    bool      needs_helper;
    bool      place_refused;
    bool      always_on_top;
    bool      layout_hidden;
    bool      urgent;
    bool      sticky;
    bool      scratchpad;
    bool      app_hidden;
    bool      user_hidden;
    bool      vis_deferred;
    bool      wm_hidden;
    bool      cloaked;
    bool      sunk;
    bool      stashed;
    RECT      stash_rect;
    bool      needs_repaint;
    bool      made_topmost;
} ManagedWindow;

typedef struct {
    int     id;
    wchar_t name[DESKTOP_NAME_MAX];

    HWND   windows[MAX_WINDOWS_PER_DESKTOP];
    int    count;
    int    focused;

    HWND   focus_hist[FOCUS_HIST_MAX];
    int    focus_hist_n;

    Layout  layout;
    float   master_ratio;
    int     n_master;
    int     inner_gap;
    int     outer_gap;
    bool    float_all;
    int     monitor;
    int     monitor_override;
    wchar_t app[MAX_PATH];
    wchar_t app_args[SPAWN_ARGS_MAX];
    wchar_t app_cwd[MAX_PATH];

    bool   app_pending;
} Desktop;

typedef struct {
    wchar_t name_match[DESKTOP_NAME_MAX];

    wchar_t app[MAX_PATH];
    wchar_t app_args[SPAWN_ARGS_MAX];
    wchar_t app_cwd[MAX_PATH];

    bool    set_float;    bool   float_all;
    bool    set_layout;   Layout layout;
    bool    set_ratio;    float  master_ratio;
    bool    set_nmaster;  int    n_master;
    bool    set_gaps;     int    inner_gap, outer_gap;
    bool    set_monitor;  int    monitor;
} DesktopRule;

typedef enum {
    LUA_EVENT_WINDOW_OPEN = 0,
    LUA_EVENT_WINDOW_CLOSE,
    LUA_EVENT_DESKTOP_SWITCH,
    LUA_EVENT_FOCUS,
    LUA_EVENT_COUNT
} LuaEvent;

#define MAX_LUA_HOOKS 32

typedef struct {
    LuaEvent event;
    int      ref;
} LuaHook;

typedef struct {
    wchar_t *cmd;
    wchar_t *args;
    wchar_t *cwd;
} StartupCommand;

typedef struct {
    HMONITOR handle;
    RECT     full;
    RECT     work_area;
    UINT     dpi;

    wchar_t  device[CCHDEVICENAME];

    int      inner_gap, outer_gap;
    int      n_master;
    float    master_ratio;
    Layout   layout;
} Monitor;

typedef struct {
    int width, height;
    int refresh;
} DisplayMode;

enum { HDR_UNSUPPORTED = -1, HDR_OFF = 0, HDR_ON = 1 };

enum { ROTATE_KEEP = -1, ROTATE_0 = 0, ROTATE_90 = 90, ROTATE_180 = 180,
       ROTATE_270 = 270 };

typedef struct {
    wchar_t device[CCHDEVICENAME];
    int     index;
    bool    set_gaps;    int   inner_gap, outer_gap;
    bool    set_nmaster; int   n_master;
    bool    set_ratio;   float master_ratio;
    bool    set_layout;  Layout layout;

    bool    set_resolution; int width, height;
    bool    set_refresh;    int refresh;
    bool    set_hdr;        bool hdr;
    bool    set_rotation;   int rotation;
    bool    set_primary;    bool primary;
    bool    set_position;   int pos_x, pos_y;
} MonitorRule;

typedef struct {
    wchar_t    class_match[256];
    wchar_t    process_match[256];
    wchar_t    path_match[MAX_PATH];
    wchar_t    title_match[256];
    bool       set_dialog;
    bool       dialog;
    RuleAction action;
    bool       no_ring;
    bool       no_decor;
    bool       fullscreen;

    wchar_t    desktop[DESKTOP_NAME_MAX];
    bool       set_monitor;  int monitor;
    bool       set_geometry;
    int        x, y, w, h;
    bool       set_center;   bool center;
    bool       start_fullscreen;
} WindowRule;

typedef struct {
    wchar_t  start_desktop[DESKTOP_NAME_MAX];

    DesktopRule desktop_rules[MAX_DESKTOP_RULES];
    int         desktop_rule_count;

    KeyMap   keymaps[MAX_KEYMAPS];
    int      keymap_count;
    KeyMap  *leader_map;

    WindowRule rules[MAX_RULES];
    int        rule_count;

    MonitorRule monitor_rules[MAX_MONITOR_RULES];
    int      monitor_rule_count;

    StartupCommand startup_commands[MAX_STARTUP_COMMANDS];
    int       startup_count;

    LuaHook   lua_hooks[MAX_LUA_HOOKS];
    int       lua_hook_count;

    bool      auto_reload;

    int      inner_gap;
    int      outer_gap;
    bool     smart_gaps;
    bool     smart_borders;
    int      border_width;
    COLORREF border_color;
    COLORREF border_color_float;
    COLORREF border_color_urgent;
    int      corner_pref;
    COLORREF background_color;

    bool     bar_enabled;
    BarMode  bar_mode;
    bool     bar_bottom;
    int      bar_height;
    unsigned bar_modules;
    COLORREF bar_bg, bar_fg, bar_accent, bar_dim;

    int      anim_ms;
    bool     dim_enabled;
    COLORREF dim_color;
    BYTE     dim_alpha;
    bool     update_check;
    bool     minimize_never;
    bool     urgency_enabled;
    bool     notify_enabled;
    bool     notify_desktop;
    bool     whichkey_enabled;
    int      whichkey_delay;
    COLORREF whichkey_bg;
    COLORREF whichkey_fg;
    COLORREF whichkey_key_fg;
    COLORREF whichkey_border;
    WhichKeyPos whichkey_pos;
    int      whichkey_margin;
    float    whichkey_max_w;
    float    whichkey_max_h;
    int      whichkey_max_rows;
    int      whichkey_padding;
    int      whichkey_row_gap;
    int      whichkey_col_gap;
    int      whichkey_key_gap;
    int      whichkey_hdr_gap;
    wchar_t  whichkey_font[LF_FACESIZE];
    int      whichkey_font_size;
    int      whichkey_border_w;
    BYTE     whichkey_opacity;
    bool     whichkey_rounded;

    FloatPolicy  float_policy;
    HidePolicy   hide_policy;
    FullscreenMode fullscreen_policy;
    AttachPolicy attach_policy;
    FloatPlacement float_placement;
    bool     manage_owned;
    bool     float_on_top;
    int      min_win_w;
    int      min_win_h;

    Layout   default_layout;
    float    default_master_ratio;
    int      default_nmaster;

    bool     mouse_enabled;
    bool     mouse_follow;
    bool     mouse_warp;
    bool     mouse_mod_drag;
    int      mouse_speed;
    int      mouse_accel;
    int      mouse_swap;

    bool     block_system_keys;
} MShellConfig;

typedef struct {
    MShellConfig cfg;

    Desktop  desktops[MAX_DESKTOPS];
    int      desktop_count;
    int      current_desktop_id;

    int      monitor_desktop[MAX_MONITORS];
    int      next_desktop_id;

    wchar_t  last_desktop[DESKTOP_NAME_MAX];

    KeyMap  *root_map;
    KeyMap  *current_map;

    ManagedWindow managed[MAX_MANAGED_WINDOWS];
    int           managed_count;

    HWND     bar_windows[MAX_MONITORS];

    SplitMode next_split;

    bool     running;
    int      suppress_depth;
    HWND     message_window;
    HWND     background_window;
    HWND     border_window;
    HWND     whichkey_window;
    HWND     notify_window;
    HWND     launcher_window;
    bool     launcher_open;
    HWINEVENTHOOK statechange_hook;

    HHOOK         kb_hook;
    HHOOK         mouse_hook;
    HWINEVENTHOOK win_event_hook;
    HWINEVENTHOOK location_hook;
    HWINEVENTHOOK foreground_hook;
    HWINEVENTHOOK minimize_hook;
    HWINEVENTHOOK movesize_hook;

    HWND     mod_drag_hwnd;
    HWND     drag_hwnd;
    POINT    drag_start;

    Monitor  monitors[MAX_MONITORS];
    int      monitor_count;
    int      primary_monitor;
    int      focused_monitor;

    RECT     work_area;

    wchar_t   config_path[MAX_PATH];
    char      config_error[512];
    lua_State *L;

    unsigned  config_gen;

    bool      lua_running;

    HINSTANCE hinst;

    bool     test_mode;
    bool     panicked;
    bool     safe_mode;
    bool     elevated;

} MShell;

extern MShell g;

int  WINAPI   WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine, int nCmdShow);
LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void     resolve_config_path(wchar_t *out, size_t out_len);

void     monitors_update(void);
void     monitors_apply_rules(void);

void     update_work_area(void);
int      monitor_of_window(HWND hwnd);

BOOL     spi_set_broadcast(UINT action, UINT ui_param, PVOID pv_param);

UINT     monitor_dpi(int mon);
UINT     monitor_dpi_of(HMONITOR handle);

bool     display_current_mode(const wchar_t *device, DisplayMode *out);
int      display_modes(const wchar_t *device, DisplayMode *out, int max);
bool     display_set_mode(const wchar_t *device, const DisplayMode *want,
                          int rotation);

int      display_rotation(const wchar_t *device);
const wchar_t *rotation_name(int rotation);

int      display_hdr_state(const wchar_t *device);
bool     display_hdr_set(const wchar_t *device, bool on);

void     displays_apply_rules(bool force);

void     display_toggle_hdr(int mon);
void     display_cycle_refresh(int mon, int dir);
void     display_cycle_rotation(int mon, int dir);
void     display_toggle_portrait(int mon);
void     display_list(void);

bool     kb_init(void);
void     kb_shutdown(void);
LRESULT CALLBACK kb_hook_proc(int nCode, WPARAM wParam, LPARAM lParam);

void     kb_locks_init(void);
void     kb_lock(void);
void     kb_unlock(void);
void     kb_reset_state(void);

DWORD    key_name_to_vk(const char *name);
const char *vk_to_key_name(DWORD vk);
DWORD    mod_name_to_flag(const char *name);
Action   action_name_to_enum(const char *name);
const char *action_enum_to_name(Action action);
void     execute_action(Action action, int arg, const wchar_t *command,
                        const wchar_t *args, const wchar_t *cwd);
void     focus_monitor_at(int mon);
void     execute_action_on(Action action, HWND target, int arg,
                           const wchar_t *command, const wchar_t *args,
                           const wchar_t *cwd);

bool     spawn_command(const wchar_t *cmd, const wchar_t *args,
                       const wchar_t *cwd, const wchar_t *ctx);

bool     kb_take_pending(unsigned seq, Action *action, int *arg,
                         wchar_t *cmd, size_t cmd_cap,
                         wchar_t *args, size_t args_cap,
                         wchar_t *cwd, size_t cwd_cap, int *count);

bool     action_is_repeatable(Action action);
KeyMap  *keymap_new(const wchar_t *name, bool persist);
void     keymap_add_binding(KeyMap *map, DWORD mods, DWORD vk,
                            Action action, int arg, KeyMap *submap,
                            const wchar_t *command, const wchar_t *args,
                            const wchar_t *cwd, const wchar_t *desc,
                            bool terminal);

bool     window_is_manageable(HWND hwnd);

void     window_process_path(HWND hwnd, wchar_t *out, size_t out_len);
bool     window_is_dialog(HWND hwnd);
const WindowRule *window_rule_lookup(HWND hwnd);

bool     wildcard_match(const wchar_t *pat, const wchar_t *str);
void     window_manage(HWND hwnd);
void     window_unmanage(HWND hwnd);
void     window_strip_decorations(HWND hwnd);
void     window_restore_decorations(HWND hwnd);
void     window_kill(HWND hwnd);
void     window_close(HWND hwnd);
void     window_focus(HWND hwnd);

void     window_focus_none(void);
HWND     window_get_focused(void);
ManagedWindow *window_find(HWND hwnd);
bool     window_set_monitor(ManagedWindow *mw, int mon);

int      window_home_monitor(const ManagedWindow *mw);
bool     window_follow_monitor(ManagedWindow *mw, int mon);
void     window_float_moved(ManagedWindow *mw);
void     window_manage_existing(void);
void     window_restore_all_decorations(void);

void     window_uncloak_strays(void);

void     window_recover_frames(void);

void     window_restore_all_visibility(void);

void     window_hide(ManagedWindow *mw);
void     window_show(ManagedWindow *mw);

bool     window_on_screen(const ManagedWindow *mw);
void     window_set_floating(HWND hwnd, bool floating);
void     window_promote(HWND hwnd);
void     window_center_float(HWND hwnd);

bool     window_rescue_offscreen(ManagedWindow *mw);
bool     window_clamp_into_monitor(ManagedWindow *mw, int mon);
void     window_enforce_zorder(void);
void     window_resink(void);
int      window_sunk_count(void);
void     window_verify_sink(void);
void     window_verify_visibility(void);
void     window_verify_placement(void);
void     window_raise_floats(void);
bool     window_frame_rect(HWND hwnd, RECT *out);

RECT     window_adjust_for_frame(HWND hwnd, RECT want);

void     window_apply_fullscreen(HWND hwnd);
void     window_reassert_rule(HWND hwnd);

void     window_set_fullscreen(HWND hwnd, FullscreenMode mode);
void     window_park_over_monitor(HWND hwnd);
bool     window_covers_monitor(HWND hwnd);

bool     layout_tree_run(Desktop *dt, int monitor, RECT area,
                         TreeEmitFn emit, void *ctx);
void     layout_tree_set_split(SplitMode mode);
void     layout_tree_rotate(void);
void     layout_tree_set_container(SplitMode mode);
void     layout_tree_cycle_container(int delta);
void     layout_tree_resize(float delta);
void     layout_tree_forget(int desktop_id);

bool     anim_begin(HWND hwnd, RECT from, RECT to);
void     anim_tick(void);
bool     anim_is_animating(HWND hwnd);
void     anim_cancel(HWND hwnd);
void     anim_cancel_all(void);
bool     anim_dim_init(void);
void     anim_dim_shutdown(void);
void     anim_dim_refresh(void);
void     tile_current(void);

void     desktop_init(void);

bool     desktop_name_ok(const wchar_t *name);

int      desktop_slot_by_name(const wchar_t *name);
int      desktop_slot_by_id(int id);
Desktop *desktop_by_id(int id);
int      desktop_current_slot(void);

HWND     desktop_focused_of(const Desktop *dt);

int      desktop_on_monitor(int mon);
int      desktop_monitor_showing(int id);
bool     desktop_is_visible(int id);
int      desktop_monitor_of_window(const ManagedWindow *mw);
void     desktop_place_on_monitor(int id, int mon);
int      desktop_target_monitor(const Desktop *dt);
void     desktop_sync_current(void);
Desktop *desktop_current(void);

int      desktop_ensure(const wchar_t *name);

void     desktop_switch(const wchar_t *name);
void     desktop_switch_last(void);
void     desktop_cycle(int delta);
void     desktop_move_window(HWND hwnd, const wchar_t *name);
bool     desktop_add_window(HWND hwnd, int slot);
void     desktop_remove_window(HWND hwnd);
int      desktop_of_window(HWND hwnd);
void     desktop_focus_update(HWND hwnd);
HWND     desktop_last_window(void);

void     system_lock(void);
void     system_logoff(void);
void     system_reboot(void);
void     system_shutdown(void);
void     system_sleep(void);
void     system_hibernate(void);
void     system_media_key(Action action);

void     screenshot_screen(void);
void     screenshot_window(void);

bool     notify_init(void);
void     notify_shutdown(void);
void     notify_show(const wchar_t *text, NotifyKind kind, int ms);

typedef struct {
    wchar_t    text[256];
    NotifyKind kind;
} NotifyItem;

int      notify_recent(NotifyItem *out, int max);

void     notify_resurface(void);

COLORREF notify_kind_color(NotifyKind kind);

bool     launcher_init(void);
void     launcher_shutdown(void);
void     launcher_open(void);
void     launcher_close(void);
void     launcher_key(DWORD vk, wchar_t ch);
bool     launcher_spawn_mrun(void);
#define NOTIFY_TEXT_CAP 512

void     desktop_gc(int slot);

void     desktop_apply_rules(int slot);
void     desktop_monitors_changed(void);
bool     desktop_set_monitor(int slot, int mon);
void     desktop_reapply(void);
HWND     desktop_get_focused(void);
void     desktop_launch_app_if_empty(int slot);

bool     border_init(void);
void     border_shutdown(void);
void     border_refresh(void);
void     border_hide(void);

bool     background_init(void);
void     background_shutdown(void);
void     background_update(void);

void     helper_init(void);
void     helper_shutdown(void);
void     helper_restart_async(void);
bool     helper_available(void);
bool     helper_set_window_pos(HWND hwnd, int x, int y, int w, int h, UINT flags);
bool     helper_set_topmost(HWND hwnd, bool on);
bool     helper_set_cloak(HWND hwnd, bool on);
bool     helper_close_window(HWND hwnd);

typedef enum {
    PLACE_OK = 0,
    PLACE_VIA_HELPER,
    PLACE_REFUSED,
} PlaceResult;

PlaceResult window_set_pos(HWND hwnd, int x, int y, int w, int h, UINT flags);

PlaceResult window_apply_rect(ManagedWindow *mw, RECT want, UINT flags);

bool window_placement_crosses_dpi(HWND hwnd, RECT want);
PlaceResult window_place_settled(HWND hwnd, RECT want, UINT flags);

void     mouse_drag_begin(HWND hwnd);
void     mouse_poll_focus(void);
bool     mouse_mod_drag_event(WPARAM msg, POINT pt, bool mod_held);

void     mouse_warp_focus(void);
void     mouse_mod_drag_apply(int dx, int dy);
void     mouse_sync_hook(void);
void     mouse_drag_end(HWND hwnd);

void     mouse_sync_pointer(void);
void     mouse_restore_pointer(void);

bool     bar_init(void);
void     bar_shutdown(void);

void     bar_reconfigure(void);

void     bar_reserve_work_area(void);

void     bar_refresh(void);

void     bar_toggle(void);

void     bar_set_mode(BarMode mode);

bool     bar_owns_notifications(void);

bool     whichkey_init(void);
void     whichkey_shutdown(void);
void     whichkey_notify(void);
void     whichkey_hide(void);

bool     events_init(void);
void     events_sync_urgency(void);
void     events_shutdown(void);
void CALLBACK events_win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                     LONG idObject, LONG idChild,
                                     DWORD idEventThread, DWORD dwmsEventTime);

void     config_apply_defaults(MShellConfig *c);
bool     config_load(const wchar_t *path);
void     config_reload(void);
bool     config_init(void);
void     config_load_builtin(void);
void     config_shutdown(void);

void     config_watch_sync(void);
void     config_watch_stop(void);
void     config_on_file_changed(unsigned generation);

bool     ipc_client_try(int *exit_code);

void     console_print(const char *s);

int      tweaks_apply(const wchar_t *group);
int      tweaks_revert(const wchar_t *group);
void     tweaks_list(void);
void     tweaks_emit_reg(const wchar_t *group, bool undo);

void     update_check_async(void);

void     update_install_async(void);

void     update_clear_staged_image(void);

void     ipc_start(void);
void     ipc_stop(void);
void     ipc_handle_request(void *req);

const char *layout_to_name(Layout l);

void     lua_register_api(lua_State *L);

void     lua_run_ref(int ref);

void     lua_fire(LuaEvent ev, HWND hwnd, const wchar_t *name);

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

static inline void hwnd_swap(HWND *a, HWND *b) {
    HWND t = *a; *a = *b; *b = t;
}

static inline int window_index_of(HWND hwnd) {
    for (int i = 0; i < g.managed_count; i++) {
        if (g.managed[i].hwnd == hwnd) return i;
    }
    return -1;
}

static inline int desktop_index_of(const Desktop *dt, HWND hwnd) {
    if (!dt) return -1;
    for (int i = 0; i < dt->count; i++) {
        if (dt->windows[i] == hwnd) return i;
    }
    return -1;
}

static inline bool window_is_alive(HWND hwnd) {
    return IsWindow(hwnd) != 0;
}

static inline bool window_is_screen_fullscreen(const ManagedWindow *mw) {
    return mw && (mw->fs_mode == FS_WINDOW || mw->fs_mode == FS_BOTH ||
                  mw->app_fullscreen);
}

static inline bool window_hidden_by_showwindow(const ManagedWindow *mw) {
    return mw && mw->wm_hidden && !mw->cloaked && !mw->sunk && !mw->stashed;
}

static inline bool window_is_float_tier(const ManagedWindow *mw) {
    return mw && mw->is_floating && !mw->tracked_only;
}

static inline void events_suppress_begin(void) { g.suppress_depth++; }
static inline void events_suppress_end(void)   { if (g.suppress_depth > 0) g.suppress_depth--; }
static inline bool events_suppressed(void)     { return g.suppress_depth > 0; }
