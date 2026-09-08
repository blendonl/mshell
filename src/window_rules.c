#include "mshell.h"
#include "window_internal.h"

static void get_process_path(HWND hwnd, wchar_t *out, size_t out_len) {
    out[0] = L'\0';

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;

    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return;

    DWORD size = (DWORD)out_len;
    if (!QueryFullProcessImageNameW(hp, 0, out, &size)) out[0] = L'\0';
    CloseHandle(hp);
}

void window_process_path(HWND hwnd, wchar_t *out, size_t out_len) {
    get_process_path(hwnd, out, out_len);
}

static const wchar_t *path_basename(const wchar_t *path) {
    const wchar_t *back = wcsrchr(path, L'\\');
    const wchar_t *fwd  = wcsrchr(path, L'/');
    const wchar_t *sep  = (back > fwd) ? back : fwd;
    return sep ? sep + 1 : path;
}

static void get_class_name(HWND hwnd, wchar_t *out, size_t out_len) {
    GetClassNameW(hwnd, out, (int)out_len);
}

bool window_is_dialog(HWND hwnd) {
    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if ((style & WS_CAPTION) != WS_CAPTION) return false;

    wchar_t cls[256] = {0};
    get_class_name(hwnd, cls, 256);
    if (_wcsicmp(cls, L"#32770") == 0) return true;

    if (GetWindow(hwnd, GW_OWNER) != NULL) return true;

    return (exstyle & WS_EX_DLGMODALFRAME) && !(style & WS_MAXIMIZEBOX);
}

static bool any_dialog_rule(void) {
    for (int i = 0; i < g.rule_count; i++)
        if (g.rules[i].set_dialog) return true;
    return false;
}

typedef enum {
    ADOPT_NO = 0,
    ADOPT_TRACK,
    ADOPT_FULL,
} AdoptTier;

static AdoptTier window_adopt_tier(HWND hwnd, const WindowRule **rule_out) {
    const WindowRule *rule      = NULL;
    bool              looked_up = false;

    if (rule_out) *rule_out = NULL;

    if (!IsWindowVisible(hwnd))   return ADOPT_NO;

    if (!IsWindowEnabled(hwnd))   return ADOPT_TRACK;

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return ADOPT_NO;

    if (!g.manage_owned && GetWindow(hwnd, GW_OWNER) != NULL) {
        if (!any_dialog_rule()) return ADOPT_TRACK;
        rule = window_rule_lookup(hwnd);
        looked_up = true;
        if (rule && rule->action == RULE_IGNORE) return ADOPT_NO;
        if (!rule || !rule->set_dialog || !rule->dialog)
            return ADOPT_TRACK;
    }

    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (exstyle & WS_EX_TOOLWINDOW) return ADOPT_NO;

    if (style & WS_CHILD) return ADOPT_NO;

    wchar_t cls[256];
    get_class_name(hwnd, cls, 256);
    static const wchar_t *ignore_classes[] = {
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"TrayNotifyWnd",
        L"NotifyIconOverflowWindow",
        L"Windows.UI.Core.CoreWindow",
        L"ForegroundStaging",
        L"MultitaskingViewFrame",
        L"XamlExplorerHostIslandWindow",
        L"mshell_Background",
        L"mshell_FocusBorder",
        L"mshell_MessageWindow",
        L"mshell_Bar",
        L"mshell_WhichKey",
        L"mshell_Notify",
        L"mshell_Launcher",
        L"mrun_Window",
        L"mshell_Dim",
        NULL
    };
    for (const wchar_t **p = ignore_classes; *p; p++) {
        if (_wcsicmp(cls, *p) == 0) return ADOPT_NO;
    }

    if (!IsIconic(hwnd)) {
        RECT r;
        if (!GetWindowRect(hwnd, &r)) return ADOPT_TRACK;
        int w = r.right  - r.left;
        int h = r.bottom - r.top;
        if (w < g.min_win_w || h < g.min_win_h) return ADOPT_TRACK;
    }

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked))) && cloaked)
        return ADOPT_NO;

    if (!looked_up) rule = window_rule_lookup(hwnd);
    if (rule_out) *rule_out = rule;
    if (rule) return rule->action != RULE_IGNORE ? ADOPT_FULL : ADOPT_NO;

    if ((style & WS_POPUP) && !(style & WS_CAPTION) && !(style & WS_SIZEBOX))
        return ADOPT_NO;

    return ADOPT_FULL;
}

bool window_is_manageable(HWND hwnd) {
    return window_adopt_tier(hwnd, NULL) == ADOPT_FULL;
}

const WindowRule *window_rule_lookup(HWND hwnd) {
    if (g.rule_count <= 0) return NULL;

    wchar_t cls[256]       = {0};
    wchar_t path[MAX_PATH] = {0};
    get_class_name(hwnd, cls, 256);
    get_process_path(hwnd, path, MAX_PATH);
    const wchar_t *proc = path_basename(path);

    int is_dlg = -1;

    wchar_t title[256];
    int     have_title = 0;

    for (int i = 0; i < g.rule_count; i++) {
        WindowRule *r = &g.rules[i];

        if (r->class_match[0]   && !wildcard_match(r->class_match, cls))    continue;
        if (r->process_match[0] && !wildcard_match(r->process_match, proc)) continue;
        if (r->path_match[0]    && !wildcard_match(r->path_match, path))    continue;
        if (r->title_match[0]) {
            if (!have_title) {
                title[0] = L'\0';
                DWORD_PTR res = 0;
                if (SendMessageTimeoutW(hwnd, WM_GETTEXT, 256, (LPARAM)title,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &res))
                    title[255] = L'\0';
                have_title = 1;
            }
            if (!wildcard_match(r->title_match, title)) continue;
        }

        if (r->set_dialog) {
            if (is_dlg < 0) is_dlg = window_is_dialog(hwnd) ? 1 : 0;
            if (r->dialog != (is_dlg == 1)) continue;
        }

        return r;
    }

    return NULL;
}

static void window_grant_full(ManagedWindow *mw, const WindowRule *rule) {
    HWND hwnd = mw->hwnd;

    mw->no_ring    = rule ? rule->no_ring    : false;
    mw->no_decor   = rule ? rule->no_decor   : false;
    mw->fullscreen = rule ? rule->fullscreen : false;
    mw->center_float = (rule && rule->set_center)
                       ? rule->center
                       : (g.float_placement == FLOAT_PLACE_CENTER);

    window_apply_flat(hwnd);

    Desktop *dt = desktop_by_id(mw->desktop_id);
    mw->is_floating =
        (rule && rule->action == RULE_FLOAT && g.float_policy != FLOAT_NEVER)
        || (dt && dt->float_all);

    if (mw->is_floating) {
        if (mw->no_decor) window_strip_decorations(hwnd);
    } else {
        window_strip_decorations(hwnd);
    }

    if (rule && rule->start_fullscreen) mw->fs_mode = FS_WINDOW;
}

void window_manage(HWND hwnd) {
    if (window_index_of(hwnd) >= 0) return;

    const WindowRule *rule = NULL;
    AdoptTier tier = window_adopt_tier(hwnd, &rule);
    if (tier == ADOPT_NO) return;

    if (g.managed_count >= MAX_MANAGED_WINDOWS) return;

    int slot = desktop_current_slot();
    if (rule && rule->desktop[0]) {
        int target = desktop_ensure(rule->desktop);
        if (target >= 0) slot = target;
        else log_msg(LOG_WARN, L"rule: cannot place a window on '%ls' — the "
                               L"name is unusable or there are too many "
                               L"desktops", rule->desktop);
    }
    if (g.desktops[slot].count >= MAX_WINDOWS_PER_DESKTOP) {
        log_err(L"desktop: '%ls' already holds %d windows, which is the "
                L"maximum — leaving %p unmanaged", g.desktops[slot].name,
                MAX_WINDOWS_PER_DESKTOP, (void *)hwnd);
        return;
    }

    int desk_id = g.desktops[slot].id;

    ManagedWindow *mw = &g.managed[g.managed_count++];
    memset(mw, 0, sizeof(*mw));
    mw->hwnd       = hwnd;
    mw->desktop_id = desk_id;
    mw->cfact      = 1.0f;
    window_claim_saved_frame(mw);

    if (tier == ADOPT_TRACK) {
        mw->tracked_only = true;
        mw->is_floating  = true;
        mw->no_ring      = true;
        window_set_monitor(mw, monitor_of_window(hwnd));
    } else {
        window_grant_full(mw, rule);

        {
            const Desktop *dt = desktop_by_id(desk_id);
            int mon = monitor_of_window(hwnd);
            if (rule && rule->set_monitor) mon = rule->monitor;

            int shown = desktop_monitor_showing(desk_id);
            if (shown >= 0)
                mon = shown;
            else if (dt && dt->monitor >= 0 && dt->monitor < g.monitor_count)
                mon = dt->monitor;

            window_set_monitor(mw, mon);
        }

        if (mw->is_floating && rule && rule->set_geometry) {
            RECT want = { rule->x, rule->y, rule->x + rule->w,
                          rule->y + rule->h };
            window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
            window_rescue_offscreen(mw);
        } else if (mw->is_floating) {
            window_center_float(hwnd);
        }
    }

    desktop_add_window(hwnd, desktop_slot_by_id(desk_id));

    if (!desktop_is_visible(desk_id)) {
        events_suppress_begin();
        window_hide(mw);
        events_suppress_end();
    }

    tile_current();

    window_park_float_if_fullscreen(mw);

    {
        const Desktop *dt = desktop_by_id(desk_id);
        log_w(L"%ls: %p (desktop '%ls', float=%d, no_decor=%d, fullscreen=%d)",
              tier == ADOPT_TRACK ? L"Tracking" : L"Managed",
              (void *)hwnd, dt ? dt->name : L"?", mw->is_floating,
              mw->no_decor, mw->fullscreen);
    }

    lua_fire(LUA_EVENT_WINDOW_OPEN, hwnd, NULL);
}

void window_promote(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->tracked_only) return;

    mw->tracked_only = false;
    window_grant_full(mw, window_rule_lookup(hwnd));

    mw->has_applied = false;

    window_park_float_if_fullscreen(mw);

    log_w(L"promoted to full management: %p (float=%d)", (void *)hwnd,
          mw->is_floating);
}

void window_unmanage(HWND hwnd) {
    int idx = window_index_of(hwnd);
    if (idx < 0) return;

    lua_fire(LUA_EVENT_WINDOW_CLOSE, hwnd, NULL);

    idx = window_index_of(hwnd);
    if (idx < 0) return;

    int desk_id = g.managed[idx].desktop_id;

    window_restore_flat(hwnd);
    window_restore_decorations(hwnd);

    if (g.managed[idx].made_topmost && IsWindow(hwnd)) {
        g.managed[idx].made_topmost = false;
        window_set_band(hwnd, HWND_NOTOPMOST, false);
    }

    desktop_remove_window(hwnd);

    g.managed[idx] = g.managed[g.managed_count - 1];
    g.managed_count--;

    bool was_current = desktop_is_visible(desk_id);
    desktop_gc(desktop_slot_by_id(desk_id));

    tile_current();

    if (was_current) {
        HWND next = desktop_get_focused();
        if (next) window_focus(next);
        else      border_hide();
    }
}

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lp) {
    (void)lp;
    window_manage(hwnd);
    return TRUE;
}

void window_manage_existing(void) {
    window_recover_frames();
    window_uncloak_strays();
    EnumWindows(enum_windows_proc, 0);
}
