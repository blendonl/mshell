#pragma once

#define ACTION_TABLE(X)                                                    \
    X(ACTION_LUA_CALL,               act_lua_call)                         \
    X(ACTION_SPAWN,                  act_spawn)                            \
                                                                           \
    X(ACTION_FOCUS_LEFT,             act_focus_left)                       \
    X(ACTION_FOCUS_DOWN,             act_focus_down)                       \
    X(ACTION_FOCUS_UP,               act_focus_up)                         \
    X(ACTION_FOCUS_RIGHT,            act_focus_right)                      \
    X(ACTION_FOCUS_NEXT,             act_focus_next)                       \
    X(ACTION_FOCUS_PREV,             act_focus_prev)                       \
                                                                           \
    X(ACTION_MOVE_LEFT,              act_move_left)                        \
    X(ACTION_MOVE_DOWN,              act_move_down)                        \
    X(ACTION_MOVE_UP,                act_move_up)                          \
    X(ACTION_MOVE_RIGHT,             act_move_right)                       \
                                                                           \
    X(ACTION_RESIZE_LEFT,            act_resize_left)                      \
    X(ACTION_RESIZE_DOWN,            act_resize_down)                      \
    X(ACTION_RESIZE_UP,              act_resize_up)                        \
    X(ACTION_RESIZE_RIGHT,           act_resize_right)                     \
                                                                           \
    X(ACTION_TOGGLE_ALWAYS_ON_TOP,   act_toggle_always_on_top)             \
    X(ACTION_LAST_WINDOW,            act_last_window)                      \
                                                                           \
    X(ACTION_SWITCH_DESKTOP,         act_switch_desktop)                   \
    X(ACTION_MOVE_TO_DESKTOP,        act_move_to_desktop)                  \
    X(ACTION_LAST_DESKTOP,           act_last_desktop)                     \
    X(ACTION_NEXT_DESKTOP,           act_next_desktop)                     \
    X(ACTION_PREV_DESKTOP,           act_prev_desktop)                     \
                                                                           \
    X(ACTION_FOCUS_MONITOR_NEXT,     act_focus_monitor_next)               \
    X(ACTION_FOCUS_MONITOR_PREV,     act_focus_monitor_prev)               \
    X(ACTION_MOVE_TO_MONITOR_NEXT,   act_move_to_monitor_next)             \
    X(ACTION_MOVE_TO_MONITOR_PREV,   act_move_to_monitor_prev)             \
    X(ACTION_DESKTOP_TO_MONITOR,     act_desktop_to_monitor)               \
                                                                           \
    X(ACTION_CLOSE,                  act_close)                            \
    X(ACTION_KILL,                   act_kill)                             \
    X(ACTION_MINIMIZE,               act_minimize)                         \
    X(ACTION_RESTORE,                act_restore)                          \
    X(ACTION_TOGGLE_STICKY,          act_toggle_sticky)                    \
    X(ACTION_MARK_SCRATCHPAD,        act_mark_scratchpad)                  \
    X(ACTION_TOGGLE_SCRATCHPAD,      act_toggle_scratchpad)                \
    X(ACTION_ZOOM,                   act_zoom)                             \
    X(ACTION_TOGGLE_FLOAT,           act_toggle_float)                     \
                                                                           \
    X(ACTION_FULLSCREEN,             act_fullscreen)                       \
    X(ACTION_FULLSCREEN_CONTENT,     act_fullscreen_content)               \
    X(ACTION_FULLSCREEN_BOTH,        act_fullscreen_both)                  \
                                                                           \
    X(ACTION_LAYOUT_TILING,          act_layout_tiling)                    \
    X(ACTION_LAYOUT_MONOCLE,         act_layout_monocle)                   \
    X(ACTION_LAYOUT_GRID,            act_layout_grid)                      \
    X(ACTION_LAYOUT_SPIRAL,          act_layout_spiral)                    \
    X(ACTION_LAYOUT_CENTERED,        act_layout_centered)                  \
    X(ACTION_LAYOUT_BSTACK,          act_layout_bstack)                    \
    X(ACTION_LAYOUT_COLUMNS,         act_layout_columns)                   \
    X(ACTION_LAYOUT_BSP,             act_layout_bsp)                       \
    X(ACTION_CYCLE_LAYOUT,           act_cycle_layout)                     \
                                                                           \
    X(ACTION_INC_NMASTER,            act_inc_nmaster)                      \
    X(ACTION_DEC_NMASTER,            act_dec_nmaster)                      \
    X(ACTION_INC_CFACT,              act_inc_cfact)                        \
    X(ACTION_DEC_CFACT,              act_dec_cfact)                        \
    X(ACTION_RESET_CFACT,            act_reset_cfact)                      \
    X(ACTION_PROMOTE_MASTER,         act_promote_master)                   \
    X(ACTION_INC_MASTER,             act_inc_master)                       \
    X(ACTION_DEC_MASTER,             act_dec_master)                       \
                                                                           \
    X(ACTION_SPLIT_H,                act_split_h)                          \
    X(ACTION_SPLIT_V,                act_split_v)                          \
    X(ACTION_ROTATE_SPLIT,           act_rotate_split)                     \
    X(ACTION_TOGGLE_TABBED,          act_toggle_tabbed)                    \
    X(ACTION_TOGGLE_STACKED,         act_toggle_stacked)                   \
    X(ACTION_CONTAINER_NEXT,         act_container_next)                   \
    X(ACTION_CONTAINER_PREV,         act_container_prev)                   \
    X(ACTION_SPLIT_GROW,             act_split_grow)                       \
    X(ACTION_SPLIT_SHRINK,           act_split_shrink)                     \
                                                                           \
    X(ACTION_LOCK,                   act_lock)                             \
    X(ACTION_LOGOFF,                 act_logoff)                           \
    X(ACTION_REBOOT,                 act_reboot)                           \
    X(ACTION_SHUTDOWN,               act_shutdown)                         \
    X(ACTION_SLEEP,                  act_sleep)                            \
    X(ACTION_HIBERNATE,              act_hibernate)                        \
                                                                           \
    X(ACTION_VOLUME_UP,              act_volume_up)                        \
    X(ACTION_VOLUME_DOWN,            act_volume_down)                      \
    X(ACTION_VOLUME_MUTE,            act_volume_mute)                      \
    X(ACTION_MEDIA_PLAY,             act_media_play)                       \
    X(ACTION_MEDIA_NEXT,             act_media_next)                       \
    X(ACTION_MEDIA_PREV,             act_media_prev)                       \
    X(ACTION_MEDIA_STOP,             act_media_stop)                       \
                                                                           \
    X(ACTION_SCREENSHOT,             act_screenshot)                       \
    X(ACTION_SCREENSHOT_WINDOW,      act_screenshot_window)                \
    X(ACTION_NOTIFY,                 act_notify)                           \
    X(ACTION_LAUNCHER,               act_launcher)                         \
    X(ACTION_JUMP_URGENT,            act_jump_urgent)                      \
                                                                           \
    X(ACTION_TOGGLE_BAR,             act_toggle_bar)                       \
    X(ACTION_BAR_TOP,                act_bar_top)                          \
    X(ACTION_BAR_FLOATING,           act_bar_floating)                     \
                                                                           \
    X(ACTION_TOGGLE_HDR,             act_toggle_hdr)                       \
    X(ACTION_CYCLE_REFRESH,          act_cycle_refresh)                    \
    X(ACTION_CYCLE_ROTATION,         act_cycle_rotation)                   \
    X(ACTION_TOGGLE_PORTRAIT,        act_toggle_portrait)                  \
                                                                           \
    X(ACTION_RELOAD,                 act_reload)                           \
    X(ACTION_QUIT,                   act_quit)                             \
    X(ACTION_UPDATE,                 act_update)                           \
    X(ACTION_RESTART_HELPER,         act_restart_helper)                   \
    X(ACTION_PANIC,                  act_panic)
