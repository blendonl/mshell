#include "mshell.h"
#include "actions.h"
#include "action_table.h"


typedef struct {
    HWND           target;
    int            arg;
    const wchar_t *command, *args, *cwd;
    Desktop       *dt;
    HWND           focus;
    int            fi;
} ActionCtx;

typedef void (*ActionFn)(const ActionCtx *);

static void act_lua_call(const ActionCtx *c) {
    lua_run_ref(c->arg);
}

static void act_spawn(const ActionCtx *c) {
    if (c->command && c->command[0])
        spawn_command(c->command, c->args, c->cwd, L"keybind");
    else
        log_w(L"spawn: no command set on binding");
}

static void focus_dir(const ActionCtx *c, Action action) {
    Desktop *dt = c->dt;
    if (dt->count < 2) {
        log_w(L"  focus: only %d window(s) — nothing to move to", dt->count);
        return;
    }
    bool prev = (action == ACTION_FOCUS_LEFT || action == ACTION_FOCUS_UP);
    dt->focused = resolve_target(dt, c->fi, action, prev);
    window_focus(dt->windows[dt->focused]);
    if (dt->layout == LAYOUT_MONOCLE) tile_current();
}

static void act_focus_left (const ActionCtx *c) { focus_dir(c, ACTION_FOCUS_LEFT);  }
static void act_focus_down (const ActionCtx *c) { focus_dir(c, ACTION_FOCUS_DOWN);  }
static void act_focus_up   (const ActionCtx *c) { focus_dir(c, ACTION_FOCUS_UP);    }
static void act_focus_right(const ActionCtx *c) { focus_dir(c, ACTION_FOCUS_RIGHT); }

static void focus_cycle(const ActionCtx *c, bool prev) {
    Desktop *dt = c->dt;
    if (dt->count < 2) return;
    dt->focused = prev ? (c->fi - 1 + dt->count) % dt->count
                       : (c->fi + 1) % dt->count;
    window_focus(dt->windows[dt->focused]);
    if (dt->layout == LAYOUT_MONOCLE) tile_current();
}

static void act_focus_next(const ActionCtx *c) { focus_cycle(c, false); }
static void act_focus_prev(const ActionCtx *c) { focus_cycle(c, true);  }

static void move_dir(const ActionCtx *c, Action action) {
    Desktop       *dt = c->dt;
    ManagedWindow *mw = c->focus ? window_find(c->focus) : NULL;
    if (mw && mw->is_floating) {
        float_nudge(mw, action, false);
        return;
    }
    if (dt->layout != LAYOUT_MONOCLE && dt->count > 1 && c->focus) {
        bool prev = (action == ACTION_MOVE_LEFT || action == ACTION_MOVE_UP);
        int target = resolve_target(dt, c->fi, action, prev);
        hwnd_swap(&dt->windows[c->fi], &dt->windows[target]);
        dt->focused = target;
        tile_current();
    }
}

static void act_move_left (const ActionCtx *c) { move_dir(c, ACTION_MOVE_LEFT);  }
static void act_move_down (const ActionCtx *c) { move_dir(c, ACTION_MOVE_DOWN);  }
static void act_move_up   (const ActionCtx *c) { move_dir(c, ACTION_MOVE_UP);    }
static void act_move_right(const ActionCtx *c) { move_dir(c, ACTION_MOVE_RIGHT); }

static void resize_dir(const ActionCtx *c, Action action) {
    ManagedWindow *mw = c->focus ? window_find(c->focus) : NULL;
    if (mw && mw->is_floating) float_nudge(mw, action, true);
}

static void act_resize_left (const ActionCtx *c) { resize_dir(c, ACTION_RESIZE_LEFT);  }
static void act_resize_down (const ActionCtx *c) { resize_dir(c, ACTION_RESIZE_DOWN);  }
static void act_resize_up   (const ActionCtx *c) { resize_dir(c, ACTION_RESIZE_UP);    }
static void act_resize_right(const ActionCtx *c) { resize_dir(c, ACTION_RESIZE_RIGHT); }

static void act_toggle_always_on_top(const ActionCtx *c) {
    ManagedWindow *mw = c->focus ? window_find(c->focus) : NULL;
    if (!mw) return;
    mw->always_on_top = !mw->always_on_top;
    log_msg(LOG_INFO, L"always-on-top %ls", mw->always_on_top ? L"on" : L"off");
    window_enforce_zorder();
}

static void act_last_window(const ActionCtx *c) {
    HWND prev = desktop_last_window();
    if (!prev) return;
    desktop_focus_update(prev);
    window_focus(prev);
    if (c->dt->layout == LAYOUT_MONOCLE) tile_current();
}

static void act_switch_desktop(const ActionCtx *c) {
    if (c->command && c->command[0]) desktop_switch(c->command);
}

static void act_move_to_desktop(const ActionCtx *c) {
    if (c->command && c->command[0] && c->focus)
        desktop_move_window(c->focus, c->command);
}

static void act_last_desktop(const ActionCtx *c) { desktop_switch_last(); }
static void act_next_desktop(const ActionCtx *c) { desktop_cycle(+1); }
static void act_prev_desktop(const ActionCtx *c) { desktop_cycle(-1); }

static void act_focus_monitor_next(const ActionCtx *c) { focus_monitor(+1); }
static void act_focus_monitor_prev(const ActionCtx *c) { focus_monitor(-1); }

static void act_move_to_monitor_next(const ActionCtx *c) { move_focused_to_monitor(+1); }
static void act_move_to_monitor_prev(const ActionCtx *c) { move_focused_to_monitor(-1); }

static void act_desktop_to_monitor(const ActionCtx *c) {
    int     slot, mon;
    wchar_t unknown[DESKTOP_NAME_MAX];
    if (!parse_desktop_monitor(c->command, c->arg, &slot, &mon,
                               unknown, DESKTOP_NAME_MAX)) {
        if (unknown[0]) {
            wchar_t msg[DESKTOP_NAME_MAX + 64];
            _snwprintf(msg, DESKTOP_NAME_MAX + 63,
                       L"desktop_to_monitor: no desktop named '%ls' "
                       L"exists right now", unknown);
            msg[DESKTOP_NAME_MAX + 63] = L'\0';
            notify_show(msg, NOTIFY_WARN, 3000);
        } else {
            notify_show(L"desktop_to_monitor: expected a monitor index, "
                        L"optionally after a desktop name",
                        NOTIFY_WARN, 3000);
        }
        return;
    }
    if (!desktop_set_monitor(slot, mon)) {
        wchar_t msg[96];
        _snwprintf(msg, 96, L"monitor %d does not exist — %d attached",
                   mon, g.monitor_count);
        msg[95] = L'\0';
        notify_show(msg, NOTIFY_WARN, 3000);
    }
}

static void act_close(const ActionCtx *c) { if (c->focus) window_close(c->focus); }
static void act_kill (const ActionCtx *c) { if (c->focus) window_kill(c->focus);  }

static void act_minimize(const ActionCtx *c) {
    Desktop *dt = c->dt;
    if (!c->focus || dt->count <= 0) return;

    ShowWindow(c->focus, SW_MINIMIZE);
    for (int i = 1; i <= dt->count; i++) {
        int  j = (c->fi + i) % dt->count;
        HWND h = dt->windows[j];
        if (h && IsWindow(h) && !IsIconic(h)) {
            dt->focused = j;
            window_focus(h);
            break;
        }
    }
    tile_current();
}

static void act_restore(const ActionCtx *c) {
    Desktop *dt = c->dt;
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
}

static void act_toggle_sticky(const ActionCtx *c) {
    ManagedWindow *mw = window_find(c->focus);
    if (!mw) return;
    mw->sticky = !mw->sticky;
    log_err(L"sticky: %p is %ls", (void *)c->focus,
            mw->sticky ? L"now on every desktop" : L"back on one desktop");
    bar_refresh();
}

static void act_mark_scratchpad(const ActionCtx *c) {
    ManagedWindow *mw = window_find(c->focus);
    if (!mw) return;
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *old = &g.managed[i];
        if (!old->scratchpad) continue;
        old->scratchpad = false;
        if (!old->user_hidden) continue;
        old->user_hidden = false;
        if (desktop_is_visible(old->desktop_id)) {
            events_suppress_begin();
            window_show(old);
            events_suppress_end();
        }
    }
    mw->scratchpad  = true;
    mw->is_floating = true;
    window_set_floating(c->focus, true);
    log_err(L"scratchpad: %p is now the scratchpad window", (void *)c->focus);
    tile_current();
}

static void act_toggle_scratchpad(const ActionCtx *c) {
    ManagedWindow *sp = NULL;
    for (int i = 0; i < g.managed_count; i++)
        if (g.managed[i].scratchpad) { sp = &g.managed[i]; break; }

    if (!sp) {
        log_err(L"scratchpad: none marked yet — focus a window and use "
                L"the 'mark_scratchpad' action first");
        return;
    }
    if (!IsWindow(sp->hwnd)) { sp->scratchpad = false; return; }

    bool here    = desktop_is_visible(sp->desktop_id);
    bool showing = here && window_on_screen(sp);

    if (!showing) {
        desktop_move_window(sp->hwnd, desktop_current()->name);
        sp = window_find(sp->hwnd);
        if (!sp) return;
    }

    events_suppress_begin();
    if (showing) {
        window_hide(sp);
        sp->user_hidden = true;
    } else {
        sp->user_hidden = false;
        sp->app_hidden  = false;
        window_show(sp);
    }
    events_suppress_end();

    if (!showing) {
        SetWindowPos(sp->hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        window_focus(sp->hwnd);
    }
    tile_current();
}

static void act_zoom(const ActionCtx *c) {
    Desktop *dt = c->dt;
    if (dt->count <= 1 || !c->focus) return;
    int other = (c->fi == 0) ? 1 : 0;
    hwnd_swap(&dt->windows[c->fi], &dt->windows[other]);
    dt->focused = other;
    tile_current();
    window_focus(dt->windows[other]);
}

static void act_toggle_float(const ActionCtx *c) {
    ManagedWindow *mw = window_find(c->focus);
    if (!mw) return;
    if (mw->tracked_only) {
        window_promote(c->focus);
        tile_current();
    } else if (g.float_policy != FLOAT_NEVER) {
        window_set_floating(c->focus, !mw->is_floating);
        tile_current();
    }
}

static void fullscreen(const ActionCtx *c, FullscreenMode mode) {
    if (c->focus) window_set_fullscreen(c->focus, mode);
}

static void act_fullscreen        (const ActionCtx *c) { fullscreen(c, FS_WINDOW);  }
static void act_fullscreen_content(const ActionCtx *c) { fullscreen(c, FS_CONTENT); }
static void act_fullscreen_both   (const ActionCtx *c) { fullscreen(c, FS_BOTH);    }

static void set_layout(const ActionCtx *c, Layout layout) {
    c->dt->layout = layout;
    tile_current();
}

static void act_layout_tiling  (const ActionCtx *c) { set_layout(c, LAYOUT_TILING);   }
static void act_layout_monocle (const ActionCtx *c) { set_layout(c, LAYOUT_MONOCLE);  }
static void act_layout_grid    (const ActionCtx *c) { set_layout(c, LAYOUT_GRID);     }
static void act_layout_spiral  (const ActionCtx *c) { set_layout(c, LAYOUT_SPIRAL);   }
static void act_layout_centered(const ActionCtx *c) { set_layout(c, LAYOUT_CENTERED); }
static void act_layout_bstack  (const ActionCtx *c) { set_layout(c, LAYOUT_BSTACK);   }
static void act_layout_columns (const ActionCtx *c) { set_layout(c, LAYOUT_COLUMNS);  }
static void act_layout_bsp     (const ActionCtx *c) { set_layout(c, LAYOUT_BSP);      }

static void act_cycle_layout(const ActionCtx *c) {
    Layout next = (Layout)(c->dt->layout + 1);
    if (next >= LAYOUT_BSP) next = LAYOUT_TILING;
    set_layout(c, next);
}

static void act_inc_nmaster(const ActionCtx *c) {
    Desktop *dt = c->dt;
    dt->n_master = clamp_i(dt->n_master + 1, 1, dt->count > 0 ? dt->count : 1);
    tile_current();
}

static void act_dec_nmaster(const ActionCtx *c) {
    c->dt->n_master = clamp_i(c->dt->n_master - 1, 1, 20);
    tile_current();
}

static void act_inc_cfact(const ActionCtx *c) { adjust_cfact(c->focus, +0.10f); }
static void act_dec_cfact(const ActionCtx *c) { adjust_cfact(c->focus, -0.10f); }

static void act_reset_cfact(const ActionCtx *c) {
    ManagedWindow *mw = window_find(c->focus);
    if (mw) { mw->cfact = 1.0f; tile_current(); }
}

static void act_promote_master(const ActionCtx *c) {
    Desktop *dt = c->dt;
    if (!c->focus || c->fi <= 0 || dt->count <= 1) return;
    HWND f = dt->windows[c->fi];
    memmove(&dt->windows[1], &dt->windows[0], (size_t)c->fi * sizeof(HWND));
    dt->windows[0] = f;
    dt->focused    = 0;
    tile_current();
}

static void act_inc_master(const ActionCtx *c) {
    c->dt->master_ratio = clamp_f(c->dt->master_ratio + 0.05f, 0.2f, 0.9f);
    tile_current();
}

static void act_dec_master(const ActionCtx *c) {
    c->dt->master_ratio = clamp_f(c->dt->master_ratio - 0.05f, 0.2f, 0.9f);
    tile_current();
}

static void act_split_h       (const ActionCtx *c) { layout_tree_set_split(SPLIT_H); }
static void act_split_v       (const ActionCtx *c) { layout_tree_set_split(SPLIT_V); }
static void act_rotate_split  (const ActionCtx *c) { layout_tree_rotate(); }
static void act_toggle_tabbed (const ActionCtx *c) { layout_tree_set_container(SPLIT_TABBED); }
static void act_toggle_stacked(const ActionCtx *c) { layout_tree_set_container(SPLIT_STACKED); }
static void act_container_next(const ActionCtx *c) { layout_tree_cycle_container(+1); }
static void act_container_prev(const ActionCtx *c) { layout_tree_cycle_container(-1); }
static void act_split_grow    (const ActionCtx *c) { layout_tree_resize(+0.05f); }
static void act_split_shrink  (const ActionCtx *c) { layout_tree_resize(-0.05f); }

static void act_lock     (const ActionCtx *c) { system_lock();      }
static void act_logoff   (const ActionCtx *c) { system_logoff();    }
static void act_reboot   (const ActionCtx *c) { system_reboot();    }
static void act_shutdown (const ActionCtx *c) { system_shutdown();  }
static void act_sleep    (const ActionCtx *c) { system_sleep();     }
static void act_hibernate(const ActionCtx *c) { system_hibernate(); }

static void act_volume_up  (const ActionCtx *c) { system_media_key(ACTION_VOLUME_UP);   }
static void act_volume_down(const ActionCtx *c) { system_media_key(ACTION_VOLUME_DOWN); }
static void act_volume_mute(const ActionCtx *c) { system_media_key(ACTION_VOLUME_MUTE); }
static void act_media_play (const ActionCtx *c) { system_media_key(ACTION_MEDIA_PLAY);  }
static void act_media_next (const ActionCtx *c) { system_media_key(ACTION_MEDIA_NEXT);  }
static void act_media_prev (const ActionCtx *c) { system_media_key(ACTION_MEDIA_PREV);  }
static void act_media_stop (const ActionCtx *c) { system_media_key(ACTION_MEDIA_STOP);  }

static void act_screenshot       (const ActionCtx *c) { screenshot_screen(); }
static void act_screenshot_window(const ActionCtx *c) { screenshot_window(); }

static void act_notify(const ActionCtx *c) {
    if (c->command && c->command[0])
        notify_show(c->command, NOTIFY_INFO, 4000);
}

static void act_launcher(const ActionCtx *c) {
    if (!launcher_spawn_mrun()) launcher_open();
}

static void act_jump_urgent(const ActionCtx *c) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!mw->urgent || !IsWindow(mw->hwnd)) continue;

        Desktop *d = desktop_by_id(mw->desktop_id);
        if (d && !desktop_is_visible(mw->desktop_id))
            desktop_switch(d->name);

        desktop_focus_update(mw->hwnd);
        window_focus(mw->hwnd);
        break;
    }
}

static void act_toggle_bar  (const ActionCtx *c) { bar_toggle(); }
static void act_bar_top     (const ActionCtx *c) { bar_set_mode(BAR_MODE_TOP_BAR);  }
static void act_bar_floating(const ActionCtx *c) { bar_set_mode(BAR_MODE_FLOATING); }

static void act_toggle_hdr(const ActionCtx *c) {
    display_toggle_hdr(g.focused_monitor);
}

static void act_cycle_refresh(const ActionCtx *c) {
    display_cycle_refresh(g.focused_monitor, c->arg >= 0 ? +1 : -1);
}

static void act_cycle_rotation(const ActionCtx *c) {
    display_cycle_rotation(g.focused_monitor, c->arg >= 0 ? +1 : -1);
}

static void act_toggle_portrait(const ActionCtx *c) {
    display_toggle_portrait(g.focused_monitor);
}

static void act_reload(const ActionCtx *c) {
    config_reload();
    tile_current();
    whichkey_hide();
    log_w(L"Config reloaded");
}

static void act_quit(const ActionCtx *c) {
    g.running = false;
    PostQuitMessage(0);
}

static void act_update(const ActionCtx *c) { update_install_async(); }

static void act_restart_helper(const ActionCtx *c) { helper_restart_async(); }

static void act_panic(const ActionCtx *c) {
    log_err(L"PANIC: starting explorer.exe and releasing the keyboard. "
            L"mshell is still running but no longer binding any key. "
            L"To undo: run `mshell.exe --msg reload`, or save your "
            L"init.lua if auto-reload is enabled.");
    window_restore_all_visibility();
    launcher_close();
    g.panicked = true;
    kb_reset_state();
    whichkey_hide();
    spawn_command(L"explorer.exe", NULL, NULL, L"panic");
}

#define ACTION_ROW(action, fn) [action] = fn,
static const ActionFn action_table[ACTION_COUNT] = { ACTION_TABLE(ACTION_ROW) };
#undef ACTION_ROW

void execute_action(Action action, int arg, const wchar_t *command,
                    const wchar_t *args, const wchar_t *cwd) {
    execute_action_on(action, NULL, arg, command, args, cwd);
}

void execute_action_on(Action action, HWND target, int arg,
                       const wchar_t *command, const wchar_t *args,
                       const wchar_t *cwd) {
    if (action <= ACTION_NONE || action >= ACTION_COUNT) return;

    if (target && !IsWindow(target)) target = NULL;

    Desktop *dt = NULL;
    if (target) {
        int id = desktop_of_window(target);
        if (id) dt = desktop_by_id(id);
    }
    if (!dt) { dt = desktop_current(); }

    HWND focus = target ? target : desktop_get_focused();
    int  fi    = target ? desktop_index_of(dt, target) : dt->focused;
    if (fi < 0) fi = dt->focused;

    log_w(L"execute_action: action=%d arg=%d (desktop '%ls', %d windows)",
          (int)action, arg, dt->name, dt->count);

    ActionFn fn = action_table[action];
    if (!fn) return;

    ActionCtx ctx = { target, arg, command, args, cwd, dt, focus, fi };
    fn(&ctx);
}
