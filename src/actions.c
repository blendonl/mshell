#include "mshell.h"

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

        long score = dx * dx + dy * dy;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

static int resolve_target(Desktop *dt, int from, Action action, bool cycle_prev) {
    int target = -1;
    if (dt->layout != LAYOUT_MONOCLE)
        target = neighbor_in_dir(dt, from, action_to_dir(action));
    if (target < 0)
        target = cycle_prev ? (from - 1 + dt->count) % dt->count
                            : (from + 1) % dt->count;
    return target;
}

void focus_monitor_at(int mon) {
    if (mon < 0 || mon >= g.monitor_count) return;

    g.focused_monitor = mon;
    desktop_sync_current();

    HWND f = desktop_on_monitor(mon) ? desktop_get_focused() : NULL;
    if (f) window_focus(f);
    else   window_focus_none();

    g.focused_monitor = mon;
    desktop_sync_current();

    if (desktop_current()->layout == LAYOUT_MONOCLE) tile_current();
    border_refresh();
    bar_refresh();
    mouse_warp_focus();
}

static void focus_monitor(int delta) {
    if (g.monitor_count < 2) return;

    int cur = g.focused_monitor;
    if (cur < 0 || cur >= g.monitor_count) cur = 0;

    int m = (((cur + delta) % g.monitor_count) + g.monitor_count)
            % g.monitor_count;
    focus_monitor_at(m);
}

static bool parse_desktop_monitor(const wchar_t *command, int arg,
                                  int *slot, int *mon,
                                  wchar_t *unknown, size_t unknown_cap) {
    *slot = desktop_current_slot();
    *mon  = arg;
    if (unknown_cap) unknown[0] = L'\0';
    if (!command || !command[0]) return true;

    const wchar_t *p = command;
    while (*p == L' ') p++;
    const wchar_t *name = p;
    while (*p && *p != L' ') p++;
    size_t len = (size_t)(p - name);
    while (*p == L' ') p++;

    if (!*p) {
        if (!len || (name[0] != L'-' && (name[0] < L'0' || name[0] > L'9')))
            return false;
        *mon = _wtoi(name);
        return true;
    }

    if (!len || len >= DESKTOP_NAME_MAX) return false;
    wchar_t dtname[DESKTOP_NAME_MAX];
    memcpy(dtname, name, len * sizeof(wchar_t));
    dtname[len] = L'\0';

    int s = desktop_slot_by_name(dtname);
    if (s < 0) {
        if (unknown_cap) {
            wcsncpy(unknown, dtname, unknown_cap - 1);
            unknown[unknown_cap - 1] = L'\0';
        }
        return false;
    }
    *slot = s;
    *mon  = _wtoi(p);
    return true;
}

static void move_focused_to_monitor(int delta) {
    if (g.monitor_count < 2) return;

    HWND focus = desktop_get_focused();
    if (!focus) return;

    int cur = g.focused_monitor;
    if (cur < 0 || cur >= g.monitor_count) cur = 0;
    int m = (((cur + delta) % g.monitor_count) + g.monitor_count)
            % g.monitor_count;

    Desktop *dst = desktop_by_id(desktop_on_monitor(m));
    if (!dst) {
        notify_show(L"that display is not showing a desktop — switch to one "
                    L"there first", NOTIFY_WARN, 2500);
        return;
    }

    desktop_move_window(focus, dst->name);
}

#define FLOAT_STEP 40

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
        if (w < g.min_win_w) w = g.min_win_w;
        if (h < g.min_win_h) h = g.min_win_h;
    } else {
        x += dx;
        y += dy;
    }

    RECT want = { x, y, x + w, y + h };
    window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
    window_set_monitor(mw, monitor_of_window(mw->hwnd));
    border_refresh();
}

bool action_is_repeatable(Action action) {
    return api_action_repeatable(action);
}

static void adjust_cfact(HWND focus, float delta) {
    ManagedWindow *mw = window_find(focus);
    if (!mw) return;
    float c = mw->cfact; if (c <= 0.f) c = 1.f;
    mw->cfact = clamp_f(c + delta, 0.25f, 4.0f);
    tile_current();
}

bool spawn_command(const wchar_t *cmd, const wchar_t *args,
                   const wchar_t *cwd, const wchar_t *ctx) {
    if (!cmd || !cmd[0]) {
        log_err(L"%ls: nothing to launch (empty command)", ctx ? ctx : L"spawn");
        return false;
    }

    const wchar_t *params = (args && args[0]) ? args : NULL;
    const wchar_t *dir    = (cwd && cwd[0]) ? cwd : NULL;

    INT_PTR code = (INT_PTR)ShellExecuteW(NULL, L"open", cmd, params,
                                          dir, SW_SHOWNORMAL);
    if (code <= 32) {
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
    execute_action_on(action, NULL, arg, command, args, cwd);
}

void execute_action_on(Action action, HWND target, int arg,
                       const wchar_t *command, const wchar_t *args,
                       const wchar_t *cwd) {
    if (target && !IsWindow(target)) target = NULL;

    Desktop *dt    = NULL;
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

    switch (action) {

    case ACTION_LUA_CALL:
        lua_run_ref(arg);
        break;

    case ACTION_SPAWN:
        if (command && command[0]) {
            spawn_command(command, args, cwd, L"keybind");
        } else {
            log_w(L"spawn: no command set on binding");
        }
        break;

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
        if (dt->layout == LAYOUT_MONOCLE) tile_current();
        break;
    }

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

    case ACTION_RESIZE_LEFT:
    case ACTION_RESIZE_DOWN:
    case ACTION_RESIZE_UP:
    case ACTION_RESIZE_RIGHT: {
        ManagedWindow *mw = focus ? window_find(focus) : NULL;
        if (mw && mw->is_floating) float_nudge(mw, action, true);
        break;
    }

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

    case ACTION_LAST_WINDOW: {
        HWND prev = desktop_last_window();
        if (prev) {
            desktop_focus_update(prev);
            window_focus(prev);
            if (dt->layout == LAYOUT_MONOCLE) tile_current();
        }
        break;
    }

    case ACTION_SWITCH_DESKTOP:
        if (command && command[0]) desktop_switch(command);
        break;

    case ACTION_MOVE_TO_DESKTOP:
        if (command && command[0] && focus) desktop_move_window(focus, command);
        break;

    case ACTION_LAST_DESKTOP:
        desktop_switch_last();
        break;

    case ACTION_NEXT_DESKTOP: desktop_cycle(+1); break;
    case ACTION_PREV_DESKTOP: desktop_cycle(-1); break;

    case ACTION_FOCUS_MONITOR_NEXT:   focus_monitor(+1); break;
    case ACTION_FOCUS_MONITOR_PREV:   focus_monitor(-1); break;
    case ACTION_MOVE_TO_MONITOR_NEXT: move_focused_to_monitor(+1); break;
    case ACTION_MOVE_TO_MONITOR_PREV: move_focused_to_monitor(-1); break;

    case ACTION_DESKTOP_TO_MONITOR: {
        int     slot, mon;
        wchar_t unknown[DESKTOP_NAME_MAX];
        if (!parse_desktop_monitor(command, arg, &slot, &mon,
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
            break;
        }
        if (!desktop_set_monitor(slot, mon)) {
            wchar_t msg[96];
            _snwprintf(msg, 96, L"monitor %d does not exist — %d attached",
                       mon, g.monitor_count);
            msg[95] = L'\0';
            notify_show(msg, NOTIFY_WARN, 3000);
        }
        break;
    }

    case ACTION_CLOSE:
        if (focus) window_close(focus);
        break;

    case ACTION_KILL:
        if (focus) window_kill(focus);
        break;

    case ACTION_MINIMIZE:
        if (focus && dt->count > 0) {
            ShowWindow(focus, SW_MINIMIZE);
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

    case ACTION_TOGGLE_STICKY: {
        ManagedWindow *mw = window_find(focus);
        if (!mw) break;
        mw->sticky = !mw->sticky;
        log_err(L"sticky: %p is %ls", (void *)focus,
                mw->sticky ? L"now on every desktop" : L"back on one desktop");
        bar_refresh();
        break;
    }

    case ACTION_MARK_SCRATCHPAD: {
        ManagedWindow *mw = window_find(focus);
        if (!mw) break;
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

        bool here    = desktop_is_visible(sp->desktop_id);
        bool showing = here && window_on_screen(sp);

        if (!showing) {
            desktop_move_window(sp->hwnd, desktop_current()->name);
            sp = window_find(sp->hwnd);
            if (!sp) break;
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
        break;
    }

    case ACTION_ZOOM:
        if (dt->count > 1 && focus) {
            int other = (fi == 0) ? 1 : 0;
            hwnd_swap(&dt->windows[fi], &dt->windows[other]);
            dt->focused = other;
            tile_current();
            window_focus(dt->windows[other]);
        }
        break;

    case ACTION_TOGGLE_FLOAT: {
        ManagedWindow *mw = window_find(focus);
        if (!mw) break;
        if (mw->tracked_only) {
            window_promote(focus);
            tile_current();
        } else if (g.float_policy != FLOAT_NEVER) {
            window_set_floating(focus, !mw->is_floating);
            tile_current();
        }
        break;
    }

    case ACTION_FULLSCREEN:
        if (focus) window_set_fullscreen(focus, FS_WINDOW);
        break;

    case ACTION_FULLSCREEN_CONTENT:
        if (focus) window_set_fullscreen(focus, FS_CONTENT);
        break;

    case ACTION_FULLSCREEN_BOTH:
        if (focus) window_set_fullscreen(focus, FS_BOTH);
        break;

    case ACTION_LAYOUT_TILING:   dt->layout = LAYOUT_TILING;   tile_current(); break;
    case ACTION_LAYOUT_MONOCLE:  dt->layout = LAYOUT_MONOCLE;  tile_current(); break;
    case ACTION_LAYOUT_GRID:     dt->layout = LAYOUT_GRID;     tile_current(); break;
    case ACTION_LAYOUT_SPIRAL:   dt->layout = LAYOUT_SPIRAL;   tile_current(); break;
    case ACTION_LAYOUT_CENTERED: dt->layout = LAYOUT_CENTERED; tile_current(); break;
    case ACTION_LAYOUT_BSTACK:   dt->layout = LAYOUT_BSTACK;   tile_current(); break;
    case ACTION_LAYOUT_COLUMNS:  dt->layout = LAYOUT_COLUMNS;  tile_current(); break;

    case ACTION_CYCLE_LAYOUT: {
        Layout next = (Layout)(dt->layout + 1);
        if (next >= LAYOUT_BSP) next = LAYOUT_TILING;
        dt->layout = next;
        tile_current();
        break;
    }

    case ACTION_INC_NMASTER:
        dt->n_master = clamp_i(dt->n_master + 1, 1, dt->count > 0 ? dt->count : 1);
        tile_current();
        break;

    case ACTION_DEC_NMASTER:
        dt->n_master = clamp_i(dt->n_master - 1, 1, 20);
        tile_current();
        break;

    case ACTION_INC_CFACT:  adjust_cfact(focus, +0.10f); break;
    case ACTION_DEC_CFACT:  adjust_cfact(focus, -0.10f); break;
    case ACTION_RESET_CFACT: {
        ManagedWindow *mw = window_find(focus);
        if (mw) { mw->cfact = 1.0f; tile_current(); }
        break;
    }

    case ACTION_PROMOTE_MASTER:
        if (focus && fi > 0 && dt->count > 1) {
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
        break;

    case ACTION_DEC_MASTER:
        dt->master_ratio = clamp_f(dt->master_ratio - 0.05f, 0.2f, 0.9f);
        tile_current();
        break;

    case ACTION_RELOAD:
        config_reload();
        tile_current();
        whichkey_hide();
        log_w(L"Config reloaded");
        break;

    case ACTION_LOCK:      system_lock();      break;
    case ACTION_LOGOFF:    system_logoff();    break;
    case ACTION_REBOOT:    system_reboot();    break;
    case ACTION_SHUTDOWN:  system_shutdown();  break;
    case ACTION_SLEEP:     system_sleep();     break;
    case ACTION_HIBERNATE: system_hibernate(); break;

    case ACTION_VOLUME_UP:   case ACTION_VOLUME_DOWN:
    case ACTION_VOLUME_MUTE: case ACTION_MEDIA_PLAY:
    case ACTION_MEDIA_NEXT:  case ACTION_MEDIA_PREV:
    case ACTION_MEDIA_STOP:
        system_media_key(action);
        break;

    case ACTION_SCREENSHOT:        screenshot_screen(); break;
    case ACTION_SCREENSHOT_WINDOW: screenshot_window(); break;

    case ACTION_NOTIFY:
        if (command && command[0]) notify_show(command, NOTIFY_INFO, 4000);
        break;

    case ACTION_LAUNCHER:
        if (!launcher_spawn_mrun()) launcher_open();
        break;

    case ACTION_TOGGLE_BAR:
        bar_toggle();
        break;

    case ACTION_BAR_TOP:      bar_set_mode(BAR_MODE_TOP_BAR);  break;
    case ACTION_BAR_FLOATING: bar_set_mode(BAR_MODE_FLOATING); break;

    case ACTION_TOGGLE_HDR:
        display_toggle_hdr(g.focused_monitor);
        break;

    case ACTION_CYCLE_REFRESH:
        display_cycle_refresh(g.focused_monitor, arg >= 0 ? +1 : -1);
        break;

    case ACTION_CYCLE_ROTATION:
        display_cycle_rotation(g.focused_monitor, arg >= 0 ? +1 : -1);
        break;

    case ACTION_TOGGLE_PORTRAIT:
        display_toggle_portrait(g.focused_monitor);
        break;

    case ACTION_SPLIT_H: layout_tree_set_split(SPLIT_H); break;
    case ACTION_SPLIT_V: layout_tree_set_split(SPLIT_V); break;
    case ACTION_ROTATE_SPLIT:   layout_tree_rotate();    break;
    case ACTION_TOGGLE_TABBED:  layout_tree_set_container(SPLIT_TABBED);  break;
    case ACTION_TOGGLE_STACKED: layout_tree_set_container(SPLIT_STACKED); break;
    case ACTION_CONTAINER_NEXT: layout_tree_cycle_container(+1); break;
    case ACTION_CONTAINER_PREV: layout_tree_cycle_container(-1); break;
    case ACTION_SPLIT_GROW:     layout_tree_resize(+0.05f); break;
    case ACTION_SPLIT_SHRINK:   layout_tree_resize(-0.05f); break;
    case ACTION_LAYOUT_BSP:     dt->layout = LAYOUT_BSP; tile_current(); break;

    case ACTION_JUMP_URGENT: {
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
        break;
    }

    case ACTION_QUIT:
        g.running = false;
        PostQuitMessage(0);
        break;

    case ACTION_UPDATE:
        update_install_async();
        break;

    case ACTION_RESTART_HELPER:
        helper_restart_async();
        break;

    case ACTION_PANIC:
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
        break;

    default:
        break;
    }
}
