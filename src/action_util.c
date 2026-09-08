#include "mshell.h"
#include "actions.h"

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

int resolve_target(Desktop *dt, int from, Action action, bool cycle_prev) {
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

void focus_monitor(int delta) {
    if (g.monitor_count < 2) return;

    int cur = g.focused_monitor;
    if (cur < 0 || cur >= g.monitor_count) cur = 0;

    int m = (((cur + delta) % g.monitor_count) + g.monitor_count)
            % g.monitor_count;
    focus_monitor_at(m);
}

bool parse_desktop_monitor(const wchar_t *command, int arg,
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

void move_focused_to_monitor(int delta) {
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

void float_nudge(ManagedWindow *mw, Action action, bool resize) {
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

void adjust_cfact(HWND focus, float delta) {
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

