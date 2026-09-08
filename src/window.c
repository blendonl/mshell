#include "mshell.h"
#include "layout_math.h"
#include "overlay.h"

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

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

static bool window_set_band(HWND hwnd, HWND after, bool topmost);

static void window_park_float_if_fullscreen(ManagedWindow *mw);

static bool rect_clamp_into_monitor(RECT *r, int mon);
static void window_float_keep_reachable(ManagedWindow *mw);

static bool window_sink_intact(void);

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
    for (int i = 0; i < g.cfg.rule_count; i++)
        if (g.cfg.rules[i].set_dialog) return true;
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

    if (!g.cfg.manage_owned && GetWindow(hwnd, GW_OWNER) != NULL) {
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
        if (w < g.cfg.min_win_w || h < g.cfg.min_win_h) return ADOPT_TRACK;
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
    if (g.cfg.rule_count <= 0) return NULL;

    wchar_t cls[256]       = {0};
    wchar_t path[MAX_PATH] = {0};
    get_class_name(hwnd, cls, 256);
    get_process_path(hwnd, path, MAX_PATH);
    const wchar_t *proc = path_basename(path);

    int is_dlg = -1;

    wchar_t title[256];
    int     have_title = 0;

    for (int i = 0; i < g.cfg.rule_count; i++) {
        WindowRule *r = &g.cfg.rules[i];

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

bool window_set_monitor(ManagedWindow *mw, int mon) {
    if (!mw) return false;
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;
    if (mon < 0 || mon >= g.monitor_count) return false;

    bool changed = (mw->monitor != mon);
    mw->monitor = mon;
    if (changed) mw->has_applied = false;

    wcsncpy(mw->monitor_device, g.monitors[mon].device, CCHDEVICENAME - 1);
    mw->monitor_device[CCHDEVICENAME - 1] = L'\0';
    return changed;
}

ManagedWindow *window_find(HWND hwnd) {
    int idx = window_index_of(hwnd);
    return (idx >= 0) ? &g.managed[idx] : NULL;
}

static void window_apply_flat(HWND hwnd) {
    DWORD corner = (DWORD)g.cfg.corner_pref;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));
    BOOL disable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disable, sizeof(disable));
}

static void window_restore_flat(HWND hwnd) {
    DWORD corner = DWMWCP_DEFAULT;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));
    BOOL disable = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disable, sizeof(disable));
}

static ATOM s_prop_style, s_prop_exstyle;

static void frame_props_init(void) {
    if (!s_prop_style)   s_prop_style   = GlobalAddAtomW(L"mshell.orig_style");
    if (!s_prop_exstyle) s_prop_exstyle = GlobalAddAtomW(L"mshell.orig_exstyle");
}

#define PROP_ORIG_STYLE   MAKEINTATOM(s_prop_style)
#define PROP_ORIG_EXSTYLE MAKEINTATOM(s_prop_exstyle)

static bool window_app_draws_own_frame(HWND hwnd) {
    if (IsIconic(hwnd)) return false;

    RECT  window_rect, client_rect;
    POINT client_origin = { 0, 0 };
    if (!GetWindowRect(hwnd, &window_rect)) return false;
    if (!GetClientRect(hwnd, &client_rect)) return false;
    if (client_rect.right <= client_rect.left ||
        client_rect.bottom <= client_rect.top) return false;
    if (!ClientToScreen(hwnd, &client_origin)) return false;

    return client_origin.y - window_rect.top <= 1;
}

void window_strip_decorations(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (!(style & WS_CAPTION)) return;

    if (!mw->orig_style) {
        mw->orig_style   = style;
        mw->orig_exstyle = exstyle;
    }

    LONG_PTR strip = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    if (!window_app_draws_own_frame(hwnd)) strip |= WS_THICKFRAME;
    style &= ~strip;

    style &= ~WS_BORDER;

    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    exstyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exstyle);

    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    bool stripped = !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION);
    if (!stripped && !mw->decor_strip_refused) {
        mw->decor_strip_refused = true;
        log_msg(LOG_WARN, L"could not strip decorations from %p — the window "
                          L"belongs to a higher-integrity process and keeps its "
                          L"title bar", (void *)hwnd);
    }
    mw->decorations_stripped = stripped;

    if (stripped) {
        frame_props_init();
        SetPropW(hwnd, PROP_ORIG_STYLE,   (HANDLE)mw->orig_style);
        SetPropW(hwnd, PROP_ORIG_EXSTYLE, (HANDLE)mw->orig_exstyle);
    }
}

static void window_claim_saved_frame(ManagedWindow *mw) {
    frame_props_init();

    HANDLE style = GetPropW(mw->hwnd, PROP_ORIG_STYLE);
    if (!style) return;

    mw->orig_style           = (LONG_PTR)style;
    mw->orig_exstyle         = (LONG_PTR)GetPropW(mw->hwnd, PROP_ORIG_EXSTYLE);
    mw->decorations_stripped = true;
}

void window_restore_decorations(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->decorations_stripped) return;

    SetWindowLongPtrW(hwnd, GWL_STYLE,   mw->orig_style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, mw->orig_exstyle);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    mw->decorations_stripped = false;
    frame_props_init();
    RemovePropW(hwnd, PROP_ORIG_STYLE);
    RemovePropW(hwnd, PROP_ORIG_EXSTYLE);
}

void window_restore_all_decorations(void) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        window_restore_flat(mw->hwnd);
        window_restore_decorations(mw->hwnd);

        if (mw->made_topmost && IsWindow(mw->hwnd)) {
            mw->made_topmost = false;
            window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
        }
    }
}

static HRESULT dwm_set_cloaked(HWND hwnd, bool on) {
    BOOL v = on ? TRUE : FALSE;
    return DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &v, sizeof(v));
}

static bool window_set_cloaked(HWND hwnd, bool on) {
    HRESULT hr = dwm_set_cloaked(hwnd, on);
    if (SUCCEEDED(hr)) return true;
    if (helper_set_cloak(hwnd, on)) return true;

    static bool warned;
    if (!warned) {
        warned = true;
        log_msg(LOG_INFO,
                L"cloak: refused for %p (DwmSetWindowAttribute returned 0x%08lX). "
                L"Expected, and not an integrity boundary the helper can cross: "
                L"DWMWA_CLOAK is owner-only, so no process cloaks another's "
                L"window with it. The shell cloak that Windows' own virtual "
                L"desktops use is IApplicationView::SetCloak, reached through "
                L"CLSID_ImmersiveShell — which explorer.exe registers at "
                L"runtime and mshell replaced, so it is unreachable here "
                L"(tools/probe_shellcloak.c measures this). Hiding sinks the "
                L"window under the backdrop instead, which is what it does by "
                L"default anyway.",
                (void *)hwnd, (unsigned long)hr);
    }
    return false;
}

bool window_on_screen(const ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (mw->wm_hidden || mw->app_hidden) return false;
    return IsWindowVisible(mw->hwnd) != 0;
}

static bool window_sink(ManagedWindow *mw) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg) || !IsWindowVisible(bg)) return false;

    if (mw->made_topmost) {
        mw->made_topmost = false;
        window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
    }
    if (GetWindowLongPtrW(mw->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) return false;

    if (!SetWindowPos(mw->hwnd, bg, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        return false;

    mw->sunk = true;
    return true;
}

static void window_unsink(ManagedWindow *mw) {
    if (!mw->sunk) return;
    mw->sunk = false;
    SetWindowPos(mw->hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

#define STASH_GAP 4000

static bool stash_position(int *x, int *y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    if (vw <= 0) return false;

    *x = vx + vw + STASH_GAP;
    *y = vy;
    return *x < 30000;
}

static bool rect_off_screen(RECT r) {
    RECT vs = { GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    vs.right  = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    RECT hit;
    return !IntersectRect(&hit, &r, &vs);
}

static bool window_stash(ManagedWindow *mw) {
    if (IsIconic(mw->hwnd)) return false;

    RECT r;
    int  x, y;
    if (!stash_position(&x, &y) || !GetWindowRect(mw->hwnd, &r)) return false;
    if (rect_off_screen(r)) return false;

    if (window_set_pos(mw->hwnd, x, y, r.right - r.left, r.bottom - r.top,
                       SWP_NOZORDER | SWP_NOACTIVATE) == PLACE_REFUSED)
        return false;

    mw->stash_rect = r;
    mw->stashed    = true;
    return true;
}

static void window_unstash(ManagedWindow *mw) {
    if (!mw->stashed) return;
    mw->stashed = false;

    RECT r = mw->stash_rect;

    if (rect_off_screen(r)) rect_clamp_into_monitor(&r, mw->monitor);

    window_set_pos(mw->hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                   SWP_NOCOPYBITS);
}

static bool window_defer_if_hung(ManagedWindow *mw, const wchar_t *what) {
    if (!IsHungAppWindow(mw->hwnd)) return false;

    mw->vis_deferred = true;

    static bool warned;
    if (!warned) {
        warned = true;
        log_msg(LOG_WARN, L"%ls: %p is not answering — skipped rather than "
                          L"blocking the shell on it. window_verify_visibility "
                          L"retries once its thread starts pumping again.",
                what, (void *)mw->hwnd);
    }
    return true;
}

void window_hide(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return;
    if (mw->wm_hidden) return;
    if (mw->app_hidden) return;
    if (window_defer_if_hung(mw, L"hide")) return;

    mw->wm_hidden = true;
    mw->cloaked   = false;
    mw->sunk      = false;
    mw->stashed   = false;

    if (g.cfg.hide_policy == HIDE_CLOAK) {
        if (!window_sink(mw)) {
            mw->cloaked = window_set_cloaked(mw->hwnd, true);
            if (!mw->cloaked) mw->stashed = window_stash(mw);
        }
    }

    if (!mw->cloaked && !mw->sunk && !mw->stashed) {
        ShowWindow(mw->hwnd, SW_HIDE);

        if (IsWindowVisible(mw->hwnd)) {
            mw->wm_hidden = false;
            static bool warned;
            if (!warned) {
                warned = true;
                log_err(L"hide: %p could not be taken off the screen by any "
                        L"means — not sunk, not cloaked, not stashed, and "
                        L"SW_HIDE was refused. It will be visible on every "
                        L"desktop. This is what mshelld.exe exists for: run "
                        L"`install.bat /helper` from an administrator prompt "
                        L"(see INSTALL.md).", (void *)mw->hwnd);
            }
            return;
        }
    }

    mw->vis_deferred = false;

    log_msg(LOG_DEBUG, L"hide: %p (%ls)", (void *)mw->hwnd,
            mw->sunk ? L"sunk" : mw->cloaked ? L"cloaked"
                               : mw->stashed ? L"stashed" : L"SW_HIDE");
}

void window_show(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return;
    if (mw->app_hidden) return;
    if (window_defer_if_hung(mw, L"show")) return;

    bool was_off_screen = mw->wm_hidden || mw->cloaked || mw->sunk ||
                          mw->stashed;
    bool was_cloaked    = mw->cloaked;
    bool was_stashed    = mw->stashed;
    bool was_sunk       = mw->sunk;

    if (mw->cloaked) {
        if (!window_set_cloaked(mw->hwnd, false))
            log_msg(LOG_WARN, L"show: could not uncloak %p — the window stays "
                              L"invisible", (void *)mw->hwnd);
        mw->cloaked = false;
    }
    window_unsink(mw);
    window_unstash(mw);

    if (!IsWindowVisible(mw->hwnd)) {
        ShowWindow(mw->hwnd, IsIconic(mw->hwnd) ? SW_SHOWMINNOACTIVE
                                                : SW_SHOWNOACTIVATE);
    }

    if (was_off_screen) {
        RedrawWindow(mw->hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        mw->has_applied   = false;
        mw->needs_repaint = true;

        if (mw->is_floating && !was_stashed && !was_sunk) {
            RECT r;
            if (GetWindowRect(mw->hwnd, &r))
                window_set_pos(mw->hwnd, r.left, r.top,
                               r.right - r.left, r.bottom - r.top,
                               SWP_NOZORDER | SWP_NOACTIVATE |
                               SWP_FRAMECHANGED | SWP_NOCOPYBITS);
            mw->needs_repaint = false;
        } else if (mw->is_floating) {
            mw->needs_repaint = false;
        }

        log_msg(LOG_DEBUG, L"show: %p (was %ls)", (void *)mw->hwnd,
                was_sunk ? L"sunk" : was_cloaked ? L"cloaked"
                                   : was_stashed ? L"stashed" : L"hidden");
    }

    mw->wm_hidden    = false;
    mw->vis_deferred = false;

    if (was_off_screen && mw->is_floating) window_float_keep_reachable(mw);
}

void window_restore_all_visibility(void) {
    int shown = 0;

    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!IsWindow(mw->hwnd)) continue;
        if (mw->app_hidden) continue;

        if (mw->cloaked || mw->sunk || mw->stashed ||
            !IsWindowVisible(mw->hwnd)) shown++;

        window_set_cloaked(mw->hwnd, false);
        mw->cloaked   = false;

        window_unsink(mw);
        window_unstash(mw);

        mw->wm_hidden = false;

        if (IsWindowVisible(mw->hwnd)) continue;

        ShowWindow(mw->hwnd, IsIconic(mw->hwnd) ? SW_SHOWMINNOACTIVE : SW_SHOWNA);
        shown++;
    }

    if (shown) log_err(L"shutdown: re-showed %d hidden window(s)", shown);
}

static void window_grant_full(ManagedWindow *mw, const WindowRule *rule) {
    HWND hwnd = mw->hwnd;

    mw->no_ring    = rule ? rule->no_ring    : false;
    mw->no_decor   = rule ? rule->no_decor   : false;
    mw->fullscreen = rule ? rule->fullscreen : false;
    mw->center_float = (rule && rule->set_center)
                       ? rule->center
                       : (g.cfg.float_placement == FLOAT_PLACE_CENTER);

    window_apply_flat(hwnd);

    Desktop *dt = desktop_by_id(mw->desktop_id);
    mw->is_floating =
        (rule && rule->action == RULE_FLOAT && g.cfg.float_policy != FLOAT_NEVER)
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

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

bool window_frame_rect(HWND hwnd, RECT *out) {
    if (!hwnd || !out) return false;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        out, sizeof(*out))))
        return true;
    return GetWindowRect(hwnd, out) != 0;
}

RECT window_adjust_for_frame(HWND hwnd, RECT want) {
    RECT wr, fr;
    if (GetWindowRect(hwnd, &wr) &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &fr, sizeof(fr)))) {
        int l = fr.left   - wr.left;
        int t = fr.top    - wr.top;
        int r = wr.right  - fr.right;
        int b = wr.bottom - fr.bottom;
        if (l < 0 || l > 64) l = 0;
        if (t < 0 || t > 64) t = 0;
        if (r < 0 || r > 64) r = 0;
        if (b < 0 || b > 64) b = 0;
        want.left   -= l;
        want.top    -= t;
        want.right  += r;
        want.bottom += b;
    }
    return want;
}

PlaceResult window_set_pos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
    if (SetWindowPos(hwnd, NULL, x, y, w, h, flags)) return PLACE_OK;

    DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        log_w(L"SetWindowPos(%p) failed: %lu", (void *)hwnd, err);
        return PLACE_REFUSED;
    }

    return helper_set_window_pos(hwnd, x, y, w, h, flags) ? PLACE_VIA_HELPER
                                                          : PLACE_REFUSED;
}

static void window_placement_refused(ManagedWindow *mw) {
    if (mw->place_refused) return;
    mw->place_refused = true;

    if (!mw->is_floating && helper_available()) {
        log_msg(LOG_WARN, L"placement refused for %p with mshelld.exe connected "
                          L"— leaving it in the layout", (void *)mw->hwnd);
        return;
    }

    if (!mw->is_floating && g.cfg.float_policy == FLOAT_NEVER) {
        log_msg(LOG_WARN, L"%p cannot be placed (UIPI) and float_policy is "
                          L"'never' — it stays in the layout without moving",
                (void *)mw->hwnd);
        return;
    }

    if (!mw->is_floating) {
        log_err(L"%p belongs to a higher-integrity process and cannot be placed "
                L"— floating it. Run `install.bat /helper` from an administrator "
                L"prompt to tile it instead (see INSTALL.md).", (void *)mw->hwnd);
        window_set_floating(mw->hwnd, true);
    }
}

#define DPI_SETTLE_TRIES 4
#define DPI_SETTLE_TICKS 4
#define PLACE_SETTLE_EPS 4

static bool rect_settled_at(RECT got, RECT want) {
    return abs((int)(got.left - want.left)) <= PLACE_SETTLE_EPS &&
           abs((int)(got.top  - want.top))  <= PLACE_SETTLE_EPS &&
           abs((int)((got.right  - got.left) -
                     (want.right  - want.left)))  <= PLACE_SETTLE_EPS &&
           abs((int)((got.bottom - got.top) -
                     (want.bottom - want.top)))   <= PLACE_SETTLE_EPS;
}

bool window_placement_crosses_dpi(HWND hwnd, RECT want) {
    if (!hwnd) return false;
    HMONITOR from = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    HMONITOR to   = MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST);
    if (!from || !to || from == to) return false;
    return monitor_dpi_of(from) != monitor_dpi_of(to);
}

static int monitor_of_rect(RECT r) {
    HMONITOR hmon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    for (int i = 0; i < g.monitor_count; i++)
        if (g.monitors[i].handle == hmon) return i;
    return g.primary_monitor;
}

static void window_settle_onto_monitor(HWND hwnd, RECT want, UINT flags) {
    RECT got;
    if (!window_frame_rect(hwnd, &got)) return;

    RECT fixed = got;
    if (!rect_clamp_into_monitor(&fixed, monitor_of_rect(want))) return;
    if (fixed.left == got.left && fixed.top == got.top) return;

    RECT adj = window_adjust_for_frame(hwnd, fixed);
    window_set_pos(hwnd, adj.left, adj.top, 0, 0,
                   (flags & ~(UINT)SWP_NOMOVE) | SWP_NOSIZE);

    log_msg(LOG_INFO, L"%p kept its own size and was moved back onto monitor "
                      L"%d at %ld,%ld", (void *)hwnd, monitor_of_rect(want),
            (long)fixed.left, (long)fixed.top);
}

PlaceResult window_place_settled(HWND hwnd, RECT want, UINT flags) {
    if (!hwnd || !IsWindow(hwnd)) return PLACE_REFUSED;

    int tries = 1;
    if (window_placement_crosses_dpi(hwnd, want)) {
        tries  = DPI_SETTLE_TRIES;
        flags |= SWP_NOCOPYBITS;
    }

    PlaceResult res       = PLACE_REFUSED;
    bool        unsettled = true;

    for (int i = 0; i < tries; i++) {
        RECT adj = window_adjust_for_frame(hwnd, want);
        res = window_set_pos(hwnd, adj.left, adj.top, adj.right - adj.left,
                             adj.bottom - adj.top, flags);
        if (res == PLACE_REFUSED) return res;
        if (tries == 1) break;

        RECT got;
        if (!window_frame_rect(hwnd, &got) || rect_settled_at(got, want)) {
            unsettled = false;
            break;
        }
    }

    if (tries > 1) {
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        if (unsettled) {
            log_msg(LOG_WARN, L"%p would not settle at %ldx%ld after crossing "
                              L"to a display of a different scale — it keeps "
                              L"re-sizing itself to the DPI it was given.",
                    (void *)hwnd, (long)(want.right - want.left),
                    (long)(want.bottom - want.top));
            window_settle_onto_monitor(hwnd, want, flags);
        }
    }

    return res;
}

PlaceResult window_apply_rect(ManagedWindow *mw, RECT want, UINT flags) {
    HWND hwnd = mw ? mw->hwnd : NULL;
    if (!hwnd || !IsWindow(hwnd)) return PLACE_REFUSED;

    bool crossed_dpi = window_placement_crosses_dpi(hwnd, want);

    PlaceResult res = window_place_settled(hwnd, want, flags);

    if (res == PLACE_REFUSED) {
        window_placement_refused(mw);
        return res;
    }

    mw->applied_rect     = want;
    mw->has_applied      = true;
    mw->place_refused    = false;
    mw->dpi_settle_left  = crossed_dpi ? DPI_SETTLE_TICKS : 0;
    mw->needs_helper  = (res == PLACE_VIA_HELPER);
    return res;
}

void window_center_float(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->is_floating || !mw->center_float) return;

    if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

    if (mw->fullscreen || window_is_screen_fullscreen(mw)) return;

    RECT cur;
    if (!window_frame_rect(hwnd, &cur)) return;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT area = (g.monitor_count > 0) ? g.monitors[mon].work_area : g.work_area;

    int aw = (int)(area.right - area.left);
    int ah = (int)(area.bottom - area.top);
    int w  = (int)(cur.right - cur.left);
    int h  = (int)(cur.bottom - cur.top);
    if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) return;

    UINT from_dpi = monitor_dpi_of(MonitorFromWindow(hwnd,
                                                     MONITOR_DEFAULTTONEAREST));
    UINT to_dpi   = monitor_dpi(mon);
    if (from_dpi > 0 && to_dpi > 0 && from_dpi != to_dpi) {
        w = MulDiv(w, (int)to_dpi, (int)from_dpi);
        h = MulDiv(h, (int)to_dpi, (int)from_dpi);
    }

    if (w > aw) w = aw;
    if (h > ah) h = ah;

    RECT want = { center_axis(area.left, aw, w), center_axis(area.top, ah, h),
                  0, 0 };
    want.right  = want.left + w;
    want.bottom = want.top  + h;

    events_suppress_begin();
    window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
    events_suppress_end();
}

static bool rect_clamp_into_monitor(RECT *r, int mon) {
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;
    RECT area = (mon >= 0 && mon < g.monitor_count) ? g.monitors[mon].work_area
                                                    : g.work_area;

    int aw = (int)(area.right - area.left);
    int ah = (int)(area.bottom - area.top);
    int w  = (int)(r->right - r->left);
    int h  = (int)(r->bottom - r->top);
    if (aw <= 0 || ah <= 0 || w <= 0 || h <= 0) return false;
    if (w > aw) w = aw;
    if (h > ah) h = ah;

    r->left   = clamp_axis(area.left, aw, r->left, w);
    r->top    = clamp_axis(area.top,  ah, r->top,  h);
    r->right  = r->left + w;
    r->bottom = r->top  + h;
    return true;
}

#define FLOAT_REACHABLE_MIN 48

static void window_float_keep_reachable(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd) || IsIconic(mw->hwnd)) return;
    if (mw->stashed || mw->fullscreen || window_is_screen_fullscreen(mw)) return;

    RECT r;
    if (!window_frame_rect(mw->hwnd, &r)) return;

    int mon = window_home_monitor(mw);
    if (mon < 0) mon = monitor_of_window(mw->hwnd);
    if (mon < 0 || mon >= g.monitor_count) return;

    RECT area = g.monitors[mon].work_area;
    RECT hit;
    bool grabbable = IntersectRect(&hit, &r, &area) &&
                     (hit.right - hit.left) >= FLOAT_REACHABLE_MIN &&
                     (hit.bottom - hit.top) >= FLOAT_REACHABLE_MIN;

    if (r.top >= area.top && grabbable) return;

    RECT fixed = r;
    if (!rect_clamp_into_monitor(&fixed, mon)) return;
    if (fixed.left == r.left && fixed.top == r.top) return;

    events_suppress_begin();
    window_apply_rect(mw, fixed, SWP_NOZORDER | SWP_NOACTIVATE);
    events_suppress_end();

    log_msg(LOG_INFO, L"%p came back at %ld,%ld, off monitor %d — moved to "
                      L"%ld,%ld so it can be reached", (void *)mw->hwnd,
            (long)r.left, (long)r.top, mon,
            (long)fixed.left, (long)fixed.top);
}

bool window_clamp_into_monitor(ManagedWindow *mw, int mon) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (IsIconic(mw->hwnd)) return false;

    if (mw->stashed) {
        rect_clamp_into_monitor(&mw->stash_rect, mon);
        return false;
    }

    RECT cur;
    if (!window_frame_rect(mw->hwnd, &cur)) return false;
    if (!rect_clamp_into_monitor(&cur, mon)) return false;

    events_suppress_begin();
    window_apply_rect(mw, cur,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();
    return true;
}

bool window_rescue_offscreen(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (IsIconic(mw->hwnd)) return false;
    if (mw->stashed) return false;

    RECT cur;
    if (!window_frame_rect(mw->hwnd, &cur)) return false;
    if (!rect_off_screen(cur)) return false;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;

    if (!window_clamp_into_monitor(mw, mon)) return false;

    log_msg(LOG_INFO, L"rescued %p from off-screen onto monitor %d",
            (void *)mw->hwnd, mon);
    return true;
}

void window_park_over_monitor(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT want = (g.monitor_count > 0) ? g.monitors[mon].full : g.work_area;

    events_suppress_begin();

    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    window_apply_rect(mw, want,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    events_suppress_end();
}

void window_apply_fullscreen(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->fullscreen || !mw->is_floating) return;
    window_park_over_monitor(hwnd);
}

static void window_park_float_if_fullscreen(ManagedWindow *mw) {
    if (!mw || !mw->is_floating) return;
    if (!mw->fullscreen && !window_is_screen_fullscreen(mw)) return;

    if (!mw->fullscreen && !mw->fs_has_prev)
        mw->fs_has_prev = window_frame_rect(mw->hwnd, &mw->fs_prev_rect);

    window_park_over_monitor(mw->hwnd);
}

static void window_place_float(ManagedWindow *mw) {
    if (!mw || !mw->is_floating) return;

    if (mw->fullscreen || window_is_screen_fullscreen(mw)) {
        window_park_float_if_fullscreen(mw);
        return;
    }

    window_center_float(mw->hwnd);
}

int window_home_monitor(const ManagedWindow *mw) {
    if (!mw) return -1;

    int shown = desktop_monitor_showing(mw->desktop_id);
    if (shown >= 0 && shown < g.monitor_count) return shown;

    const Desktop *dt = desktop_by_id(mw->desktop_id);
    if (dt && dt->monitor >= 0 && dt->monitor < g.monitor_count)
        return dt->monitor;

    return -1;
}

bool window_follow_monitor(ManagedWindow *mw, int mon) {
    if (!mw || !IsWindow(mw->hwnd)) return false;

    window_set_monitor(mw, mon);
    if (!mw->is_floating) return false;

    mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) return false;

    if (mw->stashed) return window_clamp_into_monitor(mw, mon);
    if (IsIconic(mw->hwnd)) return false;

    if (mw->fullscreen || window_is_screen_fullscreen(mw)) {
        if (monitor_of_window(mw->hwnd) == mon &&
            window_covers_monitor(mw->hwnd)) return false;
        window_park_over_monitor(mw->hwnd);
        return true;
    }

    if (monitor_of_window(mw->hwnd) == mon) return false;
    return window_clamp_into_monitor(mw, mon);
}

void window_float_moved(ManagedWindow *mw) {
    if (!mw || !mw->is_floating || !IsWindow(mw->hwnd)) return;
    if (IsIconic(mw->hwnd)) return;

    int at   = monitor_of_window(mw->hwnd);
    int home = mw->fullscreen ? window_home_monitor(mw) : -1;

    if (home < 0 || at == home) {
        window_set_monitor(mw, at);
        return;
    }

    ULONGLONG now = GetTickCount64();
    if (now - mw->snap_first_at > 1000) {
        mw->snap_first_at = now;
        mw->snap_tries    = 0;
    }
    if (++mw->snap_tries > 3) {
        if (mw->snap_tries == 4)
            log_msg(LOG_WARN, L"%p keeps putting itself on monitor %d when its "
                              L"desktop is on %d — leaving it there rather "
                              L"than fighting it.", (void *)mw->hwnd, at, home);
        window_set_monitor(mw, at);
        return;
    }

    window_follow_monitor(mw, home);
}

static void fs_forget_prev(ManagedWindow *mw) {
    if (mw) mw->fs_has_prev = false;
}

bool window_covers_monitor(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    RECT cur;
    if (!mw || !window_frame_rect(hwnd, &cur)) return false;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT full = (g.monitor_count > 0) ? g.monitors[mon].full : g.work_area;

    const int EPS = 8;
    return cur.left   <= full.left   + EPS && cur.top    <= full.top    + EPS &&
           cur.right  >= full.right  - EPS && cur.bottom >= full.bottom - EPS;
}

static void fs_restore_floating(ManagedWindow *mw) {
    if (!mw || !mw->is_floating || !mw->fs_has_prev || !IsWindow(mw->hwnd)) return;

    RECT p = mw->fs_prev_rect;

    events_suppress_begin();
    window_apply_rect(mw, p,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();

    mw->fs_has_prev = false;
}

void window_set_fullscreen(HWND hwnd, FullscreenMode mode) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    FullscreenMode next = (mw->fs_mode == mode) ? FS_OFF : mode;

    if (mw->is_floating && !mw->fs_has_prev &&
        (next == FS_WINDOW || next == FS_BOTH))
        mw->fs_has_prev = window_frame_rect(hwnd, &mw->fs_prev_rect);

    if (next == FS_WINDOW || next == FS_BOTH) {
        for (int i = 0; i < g.managed_count; i++) {
            ManagedWindow *o = &g.managed[i];
            if (o == mw || o->desktop_id != mw->desktop_id ||
                o->monitor != mw->monitor)
                continue;
            if (window_is_screen_fullscreen(o)) {
                o->fs_mode        = FS_OFF;
                o->app_fullscreen = false;
                o->has_applied    = false;
                fs_restore_floating(o);
            }
        }
    }

    mw->fs_mode        = next;
    mw->app_fullscreen = false;
    mw->has_applied    = false;

    log_w(L"fullscreen: %p mode=%d (float=%d)", (void *)hwnd, (int)next,
          mw->is_floating);

    if (mw->is_floating) {
        if (next == FS_WINDOW || next == FS_BOTH) {
            window_park_over_monitor(hwnd);
        } else if (next == FS_OFF) {
            fs_restore_floating(mw);
        }
    }

    tile_current();
}

void window_reassert_rule(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    if (mw->no_decor) window_strip_decorations(hwnd);

    if (!mw->fullscreen || !mw->is_floating) return;

    if (mw->has_applied) {
        RECT cur, a = mw->applied_rect;
        if (window_frame_rect(hwnd, &cur)) {
            const int EPS = 4;
            if (abs((int)(cur.left - a.left)) <= EPS &&
                abs((int)(cur.top  - a.top))  <= EPS &&
                abs((int)((cur.right - cur.left) - (a.right - a.left))) <= EPS &&
                abs((int)((cur.bottom - cur.top) - (a.bottom - a.top))) <= EPS)
                return;
        }
    }

    window_apply_fullscreen(hwnd);
}

static bool zorder_wants_topmost(const ManagedWindow *mw) {
    return window_is_screen_fullscreen(mw) || mw->always_on_top ||
           (g.cfg.float_on_top && window_is_float_tier(mw));
}

static bool window_set_band(HWND hwnd, HWND after, bool topmost) {
    if (SetWindowPos(hwnd, after, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
        return true;

    DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        log_w(L"SetWindowPos(%p, z-order) failed: %lu", (void *)hwnd, err);
        return false;
    }
    return helper_set_topmost(hwnd, topmost);
}

static void zorder_mark_promoted(ManagedWindow *mw) {
    if (!mw->made_topmost &&
        !(GetWindowLongPtrW(mw->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        mw->made_topmost = true;
}

static void zorder_raise_over_floats(void) {
    for (int m = 0; m < g.monitor_count; m++) {
        Desktop *dt = desktop_by_id(desktop_on_monitor(m));
        if (!dt) continue;

        for (int i = 0; i < dt->count; i++) {
            ManagedWindow *mw = window_find(dt->windows[i]);
            if (!mw || !IsWindow(mw->hwnd)) continue;
            if (!window_is_screen_fullscreen(mw) && !mw->always_on_top) continue;
            if (!window_on_screen(mw)) continue;

            zorder_mark_promoted(mw);
            window_set_band(mw->hwnd, HWND_TOPMOST, true);
        }
    }
}

void window_raise_floats(void) {
    if (!g.cfg.float_on_top) { zorder_raise_over_floats(); return; }

    HWND floats[MAX_WINDOWS_PER_DESKTOP];
    int  n = 0;

    int wanted = 0;
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!window_is_float_tier(mw)) continue;
        if (!desktop_is_visible(mw->desktop_id)) continue;
        if (!window_on_screen(mw) || IsIconic(mw->hwnd)) continue;
        wanted++;
    }
    if (wanted == 0) { zorder_raise_over_floats(); return; }
    if (wanted > MAX_WINDOWS_PER_DESKTOP) wanted = MAX_WINDOWS_PER_DESKTOP;

    int steps = 0;
    for (HWND h = GetTopWindow(NULL);
         h && n < wanted && steps < ZORDER_WALK_MAX;
         h = GetWindow(h, GW_HWNDNEXT), steps++) {
        ManagedWindow *mw = window_find(h);
        if (!window_is_float_tier(mw)) continue;
        if (!desktop_is_visible(mw->desktop_id)) continue;
        if (!window_on_screen(mw) || IsIconic(h)) continue;
        floats[n++] = h;
    }
    if (n == 0) { zorder_raise_over_floats(); return; }

    HWND focused = desktop_get_focused();
    for (int i = 1; i < n; i++) {
        if (floats[i] != focused) continue;
        memmove(&floats[1], &floats[0], (size_t)i * sizeof floats[0]);
        floats[0] = focused;
        break;
    }

    HWND after = HWND_TOPMOST;
    for (int i = 0; i < n; i++) {
        ManagedWindow *mw = window_find(floats[i]);
        if (mw) zorder_mark_promoted(mw);
        if (!window_set_band(floats[i], after, true)) continue;
        after = floats[i];
    }

    overlay_raise_all();
    zorder_raise_over_floats();
}

int window_sunk_count(void) {
    int n = 0;
    for (int i = 0; i < g.managed_count; i++)
        if (g.managed[i].sunk && IsWindow(g.managed[i].hwnd)) n++;
    return n;
}

void window_resink(void) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg)) return;

    int sunk = window_sunk_count();

    if (sunk == 0) {
        SetWindowPos(bg, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (!window_sink_intact()) {
        for (int i = 0; i < g.managed_count; i++) {
            ManagedWindow *mw = &g.managed[i];
            if (!mw->sunk || !IsWindow(mw->hwnd)) continue;
            SetWindowPos(mw->hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    HWND top      = GetTopWindow(NULL);
    HWND top_sunk = NULL;
    int  steps    = 0;
    for (HWND h = top ? GetWindow(top, GW_HWNDLAST) : NULL;
         h && steps < SINK_WALK_MAX;
         h = GetWindow(h, GW_HWNDPREV), steps++) {
        if (h == bg) continue;
        ManagedWindow *mw = window_find(h);
        if (mw && mw->sunk) { top_sunk = h; continue; }
        break;
    }
    if (!top_sunk) return;

    HWND anchor = GetWindow(top_sunk, GW_HWNDPREV);
    if (anchor && anchor != bg)
        SetWindowPos(bg, anchor, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static bool window_sink_intact(void) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg)) return true;

    int sunk = window_sunk_count();
    if (sunk == 0) return true;

    HWND top = GetTopWindow(NULL);
    if (!top) return true;

    int seen = 0, steps = 0;
    for (HWND h = GetWindow(top, GW_HWNDLAST);
         h && steps < SINK_WALK_MAX;
         h = GetWindow(h, GW_HWNDPREV), steps++) {
        if (h == bg) return seen >= sunk;
        ManagedWindow *mw = window_find(h);
        if (mw && mw->sunk) seen++;
    }

    return true;
}

void window_verify_visibility(void) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!mw->vis_deferred) continue;
        if (!IsWindow(mw->hwnd)) { mw->vis_deferred = false; continue; }
        if (IsHungAppWindow(mw->hwnd)) continue;
        if (mw->app_hidden || mw->user_hidden) { mw->vis_deferred = false; continue; }

        bool here = desktop_is_visible(mw->desktop_id);

        events_suppress_begin();
        if (!here)                        window_hide(mw);
        else if (!mw->layout_hidden)      window_show(mw);
        else                              mw->vis_deferred = false;
        events_suppress_end();
    }
}

void window_verify_placement(void) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (mw->dpi_settle_left <= 0) continue;

        if (!IsWindow(mw->hwnd) || !mw->has_applied || mw->is_floating ||
            mw->layout_hidden || mw->wm_hidden || mw->app_hidden ||
            mw->user_hidden) {
            mw->dpi_settle_left = 0;
            continue;
        }

        if (IsHungAppWindow(mw->hwnd) || anim_is_animating(mw->hwnd)) continue;

        RECT got;
        if (!window_frame_rect(mw->hwnd, &got)) { mw->dpi_settle_left = 0; continue; }
        if (rect_settled_at(got, mw->applied_rect)) {
            mw->dpi_settle_left = 0;
            continue;
        }

        if (--mw->dpi_settle_left <= 0) {
            log_msg(LOG_WARN, L"%p is still %ldx%ld after crossing to a display "
                              L"of a different scale, where the layout gave it "
                              L"%ldx%ld — recording the size it insists on "
                              L"rather than fighting it.", (void *)mw->hwnd,
                    (long)(got.right - got.left), (long)(got.bottom - got.top),
                    (long)(mw->applied_rect.right - mw->applied_rect.left),
                    (long)(mw->applied_rect.bottom - mw->applied_rect.top));
            mw->applied_rect = got;
            continue;
        }

        window_place_settled(mw->hwnd, mw->applied_rect,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                             SWP_NOCOPYBITS);
    }
}

static void window_rehide_surfaced(void) {
    HWND bg = g.background_window;
    if (!bg || !IsWindow(bg)) return;

    int steps = 0;
    for (HWND h = GetWindow(bg, GW_HWNDPREV);
         h && steps < SINK_WALK_MAX;
         h = GetWindow(h, GW_HWNDPREV), steps++) {
        ManagedWindow *mw = window_find(h);
        if (!mw || !mw->sunk) continue;
        if (desktop_is_visible(mw->desktop_id)) continue;

        log_msg(LOG_WARN, L"sink: %p will not stay under the backdrop — "
                          L"taking it off the screen another way", (void *)h);

        mw->sunk      = false;
        mw->wm_hidden = false;

        events_suppress_begin();
        window_hide(mw);
        events_suppress_end();
    }
}

void window_verify_sink(void) {
    if (window_sink_intact()) return;

    log_msg(LOG_DEBUG, L"sink: a hidden window surfaced above the backdrop — "
                       L"re-asserting");
    window_resink();

    if (!window_sink_intact()) window_rehide_surfaced();
}

void window_enforce_zorder(void) {
    window_resink();

    window_raise_floats();

    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!mw->made_topmost || !IsWindow(mw->hwnd)) continue;
        if (zorder_wants_topmost(mw) && window_on_screen(mw)) continue;

        mw->made_topmost = false;
        window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
    }
}

void window_set_floating(HWND hwnd, bool floating) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    mw->is_floating = floating;
    mw->has_applied = false;

    mw->place_refused = false;

    if (mw->layout_hidden) {
        mw->layout_hidden = false;
        if (floating && desktop_is_visible(mw->desktop_id)) {
            events_suppress_begin();
            window_show(mw);
            events_suppress_end();
        }
    }
    if (floating) {
        if (mw->no_decor) window_strip_decorations(hwnd);
        else              window_restore_decorations(hwnd);
        window_place_float(mw);
    } else {
        window_strip_decorations(hwnd);

        fs_forget_prev(mw);

        if (mw->made_topmost && !window_is_screen_fullscreen(mw) &&
            !mw->always_on_top) {
            mw->made_topmost = false;
            window_set_band(hwnd, HWND_NOTOPMOST, false);
        }
    }
}

static void claim_foreground_rights(void) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type           = INPUT_KEYBOARD;
    in.ki.wVk         = 0;
    in.ki.wScan       = 0;
    in.ki.dwExtraInfo = MSHELL_INPUT_TAG;
    SendInput(1, &in, sizeof(in));
}

void window_focus(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    claim_foreground_rights();
    SetForegroundWindow(hwnd);

    if (GetForegroundWindow() != hwnd) {
        spi_set_broadcast(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)(UINT_PTR)0);
        claim_foreground_rights();
        SetForegroundWindow(hwnd);

        if (GetForegroundWindow() != hwnd)
            SwitchToThisWindow(hwnd, TRUE);
    }

    SetFocus(hwnd);

    {
        ManagedWindow *mw = window_find(hwnd);
        int mon = mw ? desktop_monitor_of_window(mw) : monitor_of_window(hwnd);
        if (mon >= 0 && mon < g.monitor_count) {
            bool crossed = (mon != g.focused_monitor);
            g.focused_monitor = mon;

            if (crossed) desktop_sync_current();
        }

        if (mw) mw->urgent = false;
    }

    window_raise_floats();

    border_refresh();
    bar_refresh();

    {
        static HWND s_last_fired = NULL;
        if (hwnd != s_last_fired) {
            s_last_fired = hwnd;
            lua_fire(LUA_EVENT_FOCUS, hwnd, NULL);
        }
    }

    HWND fg = GetForegroundWindow();
    if (fg == hwnd) {
        log_w(L"focus -> %p ok", (void *)hwnd);
    } else {
        wchar_t cls[128] = {0};
        if (fg) GetClassNameW(fg, cls, 128);
        log_w(L"focus -> %p FAILED — foreground is still %p [%ls]",
              (void *)hwnd, (void *)fg, cls);
    }
}

void window_focus_none(void) {
    border_hide();

    HWND sink = g.background_window;
    if (!sink) return;

    HWND fg = GetForegroundWindow();
    if (fg == sink) return;
    if (fg && !window_find(fg)) return;

    LONG_PTR ex = GetWindowLongPtrW(sink, GWL_EXSTYLE);
    SetWindowLongPtrW(sink, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_NOACTIVATE);

    claim_foreground_rights();
    SetForegroundWindow(sink);

    SetWindowLongPtrW(sink, GWL_EXSTYLE, ex);
}

void window_close(HWND hwnd) {
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) return;

    if (GetLastError() == ERROR_ACCESS_DENIED) helper_close_window(hwnd);
    else log_w(L"close: PostMessage(WM_CLOSE) on %p failed: %lu",
               (void *)hwnd, GetLastError());
}

void window_kill(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;

    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hp) return;

    TerminateProcess(hp, 1);
    CloseHandle(hp);
}

static BOOL CALLBACK uncloak_stray_proc(HWND hwnd, LPARAM lp) {
    int *n = (int *)lp;

    if (!IsWindowVisible(hwnd)) return TRUE;

    RECT r;
    if (window_is_manageable(hwnd) && !IsIconic(hwnd) &&
        GetWindowRect(hwnd, &r) && rect_off_screen(r)) {
        RECT area = g.work_area;
        int  w    = r.right - r.left, h = r.bottom - r.top;
        int  aw   = (int)(area.right - area.left);
        int  ah   = (int)(area.bottom - area.top);
        if (aw > 0 && ah > 0 && w > 0 && h > 0) {
            if (w > aw) w = aw;
            if (h > ah) h = ah;
            window_set_pos(hwnd, center_axis(area.left, aw, w),
                           center_axis(area.top, ah, h), w, h,
                           SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            (*n)++;
            return TRUE;
        }
    }

    int cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                     &cloaked, sizeof(cloaked))))
        return TRUE;
    if (cloaked != DWM_CLOAKED_SHELL) return TRUE;

    if (window_set_cloaked(hwnd, false)) (*n)++;
    return TRUE;
}

static BOOL CALLBACK recover_frame_proc(HWND hwnd, LPARAM lp) {
    HANDLE style = GetPropW(hwnd, PROP_ORIG_STYLE);
    if (!style) return TRUE;

    SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      (LONG_PTR)GetPropW(hwnd, PROP_ORIG_EXSTYLE));
    RemovePropW(hwnd, PROP_ORIG_STYLE);
    RemovePropW(hwnd, PROP_ORIG_EXSTYLE);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);

    (*(int *)lp)++;
    return TRUE;
}

void window_recover_frames(void) {
    frame_props_init();

    int n = 0;
    EnumWindows(recover_frame_proc, (LPARAM)&n);
    if (n) log_err(L"startup: handed back the frame of %d window(s) a previous "
                   L"mshell stripped and did not live to restore", n);
}

void window_uncloak_strays(void) {
    if (g.test_mode) return;

    int n = 0;
    EnumWindows(uncloak_stray_proc, (LPARAM)&n);
    if (n) log_err(L"startup: recovered %d window(s) a previous mshell left "
                   L"hidden — cloaked, or stashed off every display", n);
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
