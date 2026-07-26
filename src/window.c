/*
 * window.c — window management: enumeration, manage/unmanage,
 *            decoration stripping, rules engine.
 */

#include "mshell.h"

/* ---------------------------------------------------------------------------
 * DWM attribute ids that postdate the mingw-w64 headers we build against.
 * DwmSetWindowAttribute takes the id as a DWORD, so defining the raw values
 * is enough — no enum/header dependency. The Win11-only corner id is simply
 * rejected by DWM on Windows 10 (the call returns a failure HRESULT we
 * ignore), so one binary stays correct on both. Values match the official
 * enums, so the #ifndef guards are harmless if a newer header already has them.
 * --------------------------------------------------------------------------- */
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

/* ===========================================================================
 * Helper: full image path of the process owning a window
 * ("C:\SteamLibrary\steamapps\common\Elden Ring\Game\eldenring.exe")
 * =========================================================================== */
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

/* The file name inside a path, or the whole string when there is no separator. */
static const wchar_t *path_basename(const wchar_t *path) {
    const wchar_t *back = wcsrchr(path, L'\\');
    const wchar_t *fwd  = wcsrchr(path, L'/');
    const wchar_t *sep  = (back > fwd) ? back : fwd;
    return sep ? sep + 1 : path;
}

/* ===========================================================================
 * Case-insensitive wildcard match: `*` = any run, `?` = one character. `/` and
 * `\` fold together so a rule can spell a path either way. A pattern with no
 * wildcards is an exact match, which is what class/process rules were before
 * patterns existed — so old configs keep matching exactly what they used to.
 *
 * Iterative with a single backtrack point (no recursion): the greedy `*`
 * remembers where it started swallowing, and a later mismatch resumes from
 * there having eaten one more character.
 * =========================================================================== */
static wchar_t fold_ch(wchar_t c) {
    return (c == L'/') ? L'\\' : (wchar_t)towlower(c);
}

bool wildcard_match(const wchar_t *pat, const wchar_t *str) {
    const wchar_t *star = NULL;   /* last '*' in the pattern, if any     */
    const wchar_t *back = NULL;   /* where that '*' resumes from in str  */

    while (*str) {
        if (*pat == L'*') {
            star = pat++;         /* match zero characters, for now */
            back = str;
        } else if (*pat == L'?' || (*pat && fold_ch(*pat) == fold_ch(*str))) {
            pat++; str++;
        } else if (star) {
            pat = star + 1;       /* backtrack: let the '*' eat one more */
            str = ++back;
        } else {
            return false;
        }
    }

    while (*pat == L'*') pat++;   /* trailing '*'s can match nothing */
    return *pat == L'\0';
}

/* ===========================================================================
 * Helper: get window class name
 * =========================================================================== */
static void get_class_name(HWND hwnd, wchar_t *out, size_t out_len) {
    GetClassNameW(hwnd, out, (int)out_len);
}

/* ===========================================================================
 * window_is_dialog — "is this a dialog?", asked of Windows rather than of a
 * name, because dialogs have no name worth matching: a file picker is the host
 * app's own process (the Open box in Firefox *is* firefox.exe) wearing a class
 * the OS handed it, so class/process/path rules can't separate it from the
 * window that opened it.
 *
 * A dialog must first have a title bar, and then be any one of three things:
 *
 *   #32770                  the class Win32 gives every common dialog — Open /
 *                           Save As / Select Folder, MessageBox, TaskDialog,
 *                           Print, Properties, and the shell's own prompts.
 *   an owner window         a window opened on behalf of another one. This is
 *                           what GTK, wxWidgets and most cross-platform dialogs
 *                           look like, and it is also what Windows itself uses
 *                           for the majority of app-modal prompts.
 *   WS_EX_DLGMODALFRAME     the modal dialog frame, used by Qt, WinUI and .NET
 *   without a maximize box  for dialogs. The maximize test is what keeps this
 *                           branch honest: setting DLGMODALFRAME is also the
 *                           documented trick for hiding a window's title-bar
 *                           icon, so ordinary main windows wear it too — and
 *                           those keep the maximize box a dialog never has.
 * =========================================================================== */
bool window_is_dialog(HWND hwnd) {
    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    /* The caption test comes first and is not optional: without it the owner
     * test below sweeps up every menu, dropdown, tooltip and autocomplete list
     * on the system, all of which are owned popups too. A title bar is what
     * separates a window you answer from a transient an app draws over itself.
     * WS_CAPTION is two bits (WS_BORDER | WS_DLGFRAME) — compare, don't mask,
     * or a plain bordered popup passes for a dialog. */
    if ((style & WS_CAPTION) != WS_CAPTION) return false;

    wchar_t cls[256] = {0};
    get_class_name(hwnd, cls, 256);
    if (_wcsicmp(cls, L"#32770") == 0) return true;

    if (GetWindow(hwnd, GW_OWNER) != NULL) return true;

    return (exstyle & WS_EX_DLGMODALFRAME) && !(style & WS_MAXIMIZEBOX);
}

/* Does any rule ask about dialogs at all? Owned windows are dropped early and
 * cheaply (below); consulting the rules for each one would put a process query
 * behind every menu and tooltip on the system. This bool buys that back: with
 * no `dialog` rule in the config — the shipped state before this existed —
 * nothing changes and nothing is looked up. */
static bool any_dialog_rule(void) {
    for (int i = 0; i < g.rule_count; i++)
        if (g.rules[i].set_dialog) return true;
    return false;
}

/* ===========================================================================
 * is_manageable — filter out windows we should never touch
 * =========================================================================== */
bool window_is_manageable(HWND hwnd) {
    if (!IsWindowVisible(hwnd))   return false;
    if (!IsWindowEnabled(hwnd))   return false;
    if (IsIconic(hwnd))           return false;  /* minimized */

    /* Must be a root window */
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    /* Owned windows are dialogs/pop-ups that normally float. Managing them is
     * opt-in (mshell.set_manage_owned(true)) because modal/fixed-size dialogs
     * tile poorly.
     *
     * A `dialog` rule is the second, narrower opt-in: it says "manage these,
     * but floating", which is the whole difference between a file picker mshell
     * has never heard of — untouched, and therefore still sitting over whatever
     * desktop you switch to next — and one it hides, focuses and closes like
     * any other window. Only a rule that explicitly asked about dialogs may
     * rescue an owned window: a broad path rule (the game-library ones) must
     * not start pulling in every splash screen and error box a game owns. */
    if (!g.manage_owned && GetWindow(hwnd, GW_OWNER) != NULL) {
        if (!any_dialog_rule()) return false;
        const WindowRule *r = window_rule_lookup(hwnd);
        if (!r || !r->set_dialog || !r->dialog || r->action == RULE_IGNORE)
            return false;
    }

    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    /* Tool windows are popups; ignore */
    if (exstyle & WS_EX_TOOLWINDOW) return false;

    /* Must be an overlapped or popup window (not a child) */
    if (!(style & WS_OVERLAPPEDWINDOW) && !(style & WS_POPUP)) return false;

    /* Cloaked windows (Windows 8+ DWM feature) */
    int cloaked = 0;
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr) && cloaked) return false;

    /* Ignore the desktop window and explorer's shell UI. The shell classes
     * matter in --test mode (explorer is running); harmless otherwise. */
    wchar_t cls[256];
    get_class_name(hwnd, cls, 256);
    static const wchar_t *ignore_classes[] = {
        L"Progman",             /* desktop                    */
        L"WorkerW",             /* desktop wallpaper host     */
        L"Shell_TrayWnd",       /* taskbar                    */
        L"Shell_SecondaryTrayWnd", /* taskbar on 2nd monitor  */
        L"TrayNotifyWnd",       /* system tray                */
        L"NotifyIconOverflowWindow",
        L"Windows.UI.Core.CoreWindow", /* start menu / search */
        L"ForegroundStaging",
        L"MultitaskingViewFrame",
        L"XamlExplorerHostIslandWindow",
        L"mshell_Background",   /* our own desktop backdrop   */
        L"mshell_FocusBorder",  /* our own focus ring         */
        L"mshell_MessageWindow",/* our own message window     */
        NULL
    };
    for (const wchar_t **p = ignore_classes; *p; p++) {
        if (_wcsicmp(cls, *p) == 0) return false;
    }

    /* Minimum size (configurable) */
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    int w = r.right  - r.left;
    int h = r.bottom - r.top;
    if (w < g.min_win_w || h < g.min_win_h) return false;

    /* Rules have the last word, and they are consulted *before* the caption-less
     * popup heuristic below — a rule that names a window is the user telling us
     * what it is, which beats guessing from styles. That matters for exactly one
     * class of window: a borderless-fullscreen game is a WS_POPUP with no
     * caption and no sizebox, i.e. byte-for-byte the style a menu or tooltip
     * wears. Without this, such a game is dropped here and its rule never runs:
     * it stays on screen across every desktop switch and Win+Shift+c can't
     * reach it. Rules match in order, so an "ignore" rule placed ahead of a
     * broad path rule still carves exceptions out of it. */
    const WindowRule *rule = window_rule_lookup(hwnd);
    if (rule) return rule->action != RULE_IGNORE;

    /* Ignore invisible/system popups */
    if ((style & WS_POPUP) && !(style & WS_CAPTION) && !(style & WS_SIZEBOX))
        return false;  /* likely a menu / tooltip */

    return true;
}

/* ===========================================================================
 * Rule lookup — the first rule whose class/process/path criteria all match, or
 * NULL when none do (in which case the caller applies the RULE_MANAGE default).
 * Criteria are wildcard patterns; an empty one is skipped, i.e. matches any.
 * =========================================================================== */
const WindowRule *window_rule_lookup(HWND hwnd) {
    if (g.rule_count <= 0) return NULL;   /* skip the process query entirely */

    wchar_t cls[256]       = {0};
    wchar_t path[MAX_PATH] = {0};
    get_class_name(hwnd, cls, 256);
    get_process_path(hwnd, path, MAX_PATH);
    const wchar_t *proc = path_basename(path);

    int is_dlg = -1;   /* answered once, and only if a rule actually asks */

    for (int i = 0; i < g.rule_count; i++) {
        WindowRule *r = &g.rules[i];

        /* Every string criterion is a pattern; an empty one matches anything. */
        if (r->class_match[0]   && !wildcard_match(r->class_match, cls))    continue;
        if (r->process_match[0] && !wildcard_match(r->process_match, proc)) continue;
        if (r->path_match[0]    && !wildcard_match(r->path_match, path))    continue;

        if (r->set_dialog) {
            if (is_dlg < 0) is_dlg = window_is_dialog(hwnd) ? 1 : 0;
            if (r->dialog != (is_dlg == 1)) continue;
        }

        return r;
    }

    return NULL;
}

/* ===========================================================================
 * Find a ManagedWindow by HWND
 * =========================================================================== */
ManagedWindow *window_find(HWND hwnd) {
    int idx = window_index_of(hwnd);
    return (idx >= 0) ? &g.managed[idx] : NULL;
}

/* ===========================================================================
 * Flatten Windows' own window chrome.
 *
 * Two things the OS adds that a tiling shell never wants, and that stripping
 * WS_CAPTION does NOT remove because they're drawn by DWM, not by the frame:
 *   - Win11 rounds the corners of every top-level window (including the
 *     borderless ones like Chrome/Electron that we deliberately don't strip).
 *   - DWM animates window show/hide, minimise and restore, which shows up as
 *     a fade/scale on every desktop switch and app launch.
 * Both are per-window DWM attributes. We apply them to every managed window
 * and reverse them on unmanage / shutdown so apps look normal again.
 * =========================================================================== */
static void window_apply_flat(HWND hwnd) {
    DWORD corner = DWMWCP_DONOTROUND;
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

/* ===========================================================================
 * Strip window decorations
 * =========================================================================== */
void window_strip_decorations(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    LONG_PTR style   = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    /* Nothing to strip — either we already did it and it stayed stripped, or
     * the app draws its own decorations with WS_POPUP and no WS_CAPTION
     * (Electron, Chrome) and there was never a frame to take. Note this is NOT
     * gated on decorations_stripped: an app that re-adds its caption later
     * (games do, flipping between windowed and borderless) gets stripped again
     * by window_reassert_rule(). */
    if (!(style & WS_CAPTION)) return;

    /* Remember what the app chose, once. On a re-strip the "original" still
     * means the styles it had when we first found it, so unmanaging hands back
     * a normal window however many times the app rebuilt its frame. */
    if (!mw->orig_style) {
        mw->orig_style   = style;
        mw->orig_exstyle = exstyle;
    }

    /* Remove caption, system menu, thick frame, min/max boxes */
    style &= ~(WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
               WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

    /* A thin border separates adjacent tiled windows. A window whose rule asked
     * for no decoration at all (decorate = false — games) gets nothing, so the
     * image reaches the screen edge with no line drawn over it. */
    if (mw->no_decor) style &= ~WS_BORDER;
    else              style |= WS_BORDER;

    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    /* Remove WS_EX_WINDOWEDGE and WS_EX_CLIENTEDGE to eliminate
     * any remaining chrome, then add a thin static edge */
    exstyle &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exstyle);

    /* Force a full frame recalculation */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    mw->decorations_stripped = true;
}

/* ===========================================================================
 * Restore window decorations
 * =========================================================================== */
void window_restore_decorations(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->decorations_stripped) return;

    SetWindowLongPtrW(hwnd, GWL_STYLE,   mw->orig_style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, mw->orig_exstyle);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    mw->decorations_stripped = false;
}

/* ===========================================================================
 * Restore decorations on ALL managed windows
 * =========================================================================== */
void window_restore_all_decorations(void) {
    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        window_restore_flat(mw->hwnd);
        window_restore_decorations(mw->hwnd);

        /* Also hand back the topmost band to any window we parked there for
         * fullscreen: mshell is going away, and a window left pinned above
         * everything else outlives us with no way to fix it from the keyboard. */
        if (mw->made_topmost && IsWindow(mw->hwnd)) {
            mw->made_topmost = false;
            SetWindowPos(mw->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

/* ===========================================================================
 * Manage a window — bring it under WM control
 * =========================================================================== */
void window_manage(HWND hwnd) {
    /* Already managed? */
    if (window_index_of(hwnd) >= 0) return;

    if (!window_is_manageable(hwnd)) return;

    const WindowRule *rule = window_rule_lookup(hwnd);
    RuleAction action = rule ? rule->action : RULE_MANAGE;
    if (action == RULE_IGNORE) return;

    /* Grow managed array if needed */
    if (g.managed_count >= MAX_MANAGED_WINDOWS) return;

    /* New windows land on the desktop you are looking at. */
    int      slot = desktop_current_slot();
    Desktop *dt   = &g.desktops[slot];

    ManagedWindow *mw = &g.managed[g.managed_count++];
    memset(mw, 0, sizeof(*mw));
    mw->hwnd       = hwnd;
    mw->desktop_id = dt->id;
    /* Tile it where it opened — unless the desktop is pinned to a display, in
     * which case that is where the desktop's windows go. */
    mw->monitor    = (dt->monitor >= 0 && dt->monitor < g.monitor_count)
                       ? dt->monitor : monitor_of_window(hwnd);
    mw->cfact      = 1.0f;
    mw->no_ring    = rule ? rule->no_ring    : false;
    mw->no_decor   = rule ? rule->no_decor   : false;
    mw->fullscreen = rule ? rule->fullscreen : false;

    /* Square corners + no open/close animation, for tiled and floating alike. */
    window_apply_flat(hwnd);

    /* Two ways to end up floating: the window's own rule says so, or the
     * desktop it opened on floats everything (desktop_rule float = true). The
     * desktop's answer is deliberately checked AFTER FLOAT_NEVER's veto of the
     * window rule, because it is the more specific statement of intent: a
     * config that tiles aggressively and then carves out one floating desktop
     * means it. toggle_float still works there either way — `float` sets what
     * new windows START as, it doesn't pin them. */
    bool floating = (action == RULE_FLOAT && g.float_policy != FLOAT_NEVER)
                    || dt->float_all;

    if (floating) {
        mw->is_floating = true;
        /* Floating normally means "hands off the frame": the window keeps
         * whatever chrome it came with. `decorate = false` opts out of that —
         * float for the geometry, borderless for the looks. */
        if (mw->no_decor) window_strip_decorations(hwnd);
    } else {
        window_strip_decorations(hwnd);
    }

    desktop_add_window(hwnd, slot);
    tile_current();

    /* The tiler never places floating windows, so a fullscreen rule does it
     * here. No-op for tiled windows — the layout already owns their geometry. */
    window_apply_fullscreen(hwnd);

    log_w(L"Managed: %p (desktop '%ls', float=%d, no_decor=%d, fullscreen=%d)",
          (void *)hwnd, dt->name, mw->is_floating, mw->no_decor,
          mw->fullscreen);
}

/* ===========================================================================
 * Unmanage a window — remove from WM control
 * =========================================================================== */
void window_unmanage(HWND hwnd) {
    int idx = window_index_of(hwnd);
    if (idx < 0) return;

    int desk_id = g.managed[idx].desktop_id;

    window_restore_flat(hwnd);
    window_restore_decorations(hwnd);

    /* Hand back the topmost band if we took it for fullscreen. A window we stop
     * managing while it is still alive would otherwise stay pinned above
     * everything with nothing left to ever put it back. */
    if (g.managed[idx].made_topmost && IsWindow(hwnd)) {
        g.managed[idx].made_topmost = false;
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    desktop_remove_window(hwnd);

    /* Remove from managed array (swap with last) */
    g.managed[idx] = g.managed[g.managed_count - 1];
    g.managed_count--;

    /* That may have been the desktop's last window. If it was a BACKGROUND
     * desktop, it has now served its purpose and goes away — closing the last
     * window on a desktop you can't see is exactly the case dynamic desktops
     * exist to clean up. The one you are on survives (desktop_gc's rule), so
     * closing everything in front of you leaves you somewhere, not nowhere. */
    bool was_current = (desk_id == g.current_desktop_id);
    desktop_gc(desktop_slot_by_id(desk_id));

    tile_current();

    /* If the closed window was on the visible desktop, hand focus to a
     * surviving sibling so the keyboard doesn't drop onto the backdrop. */
    if (was_current) {
        HWND next = desktop_get_focused();
        if (next) window_focus(next);
        else      border_hide();
    }
}

/* ===========================================================================
 * DWM visible-frame bounds (excludes the invisible resize border). Used by the
 * event layer to tell a real drag apart from the rect we last assigned.
 * =========================================================================== */
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

/* ===========================================================================
 * Expand a desired visible-frame rect into the window rect SetWindowPos wants,
 * compensating for DWM's invisible border so the *visible* frame lands exactly
 * where the caller asked. Stripped windows have ~0 border; browsers ~7px.
 * =========================================================================== */
RECT window_adjust_for_frame(HWND hwnd, RECT want) {
    RECT wr, fr;
    if (GetWindowRect(hwnd, &wr) &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &fr, sizeof(fr)))) {
        int l = fr.left   - wr.left;
        int t = fr.top    - wr.top;
        int r = wr.right  - fr.right;
        int b = wr.bottom - fr.bottom;
        /* Only trust small, non-negative insets (a sane resize border). */
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

/* ===========================================================================
 * Park a window over the whole monitor it lives on.
 *
 * Deliberately the monitor's *full* bounds, not its work area, and with no gap
 * applied: this is the one window that should reach every screen edge. Shared
 * by the `fullscreen = true` rule (games) and the fullscreen keybindings.
 * =========================================================================== */
void window_park_over_monitor(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = 0;
    RECT want = (g.monitor_count > 0) ? g.monitors[mon].full : g.work_area;

    events_suppress_begin();

    /* A maximized window ignores the geometry handed to SetWindowPos, so drop
     * it out of that state first. */
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    RECT adj = window_adjust_for_frame(hwnd, want);
    SetWindowPos(hwnd, NULL, adj.left, adj.top,
                 adj.right - adj.left, adj.bottom - adj.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    events_suppress_end();

    mw->applied_rect = want;
    mw->has_applied  = true;
}

/* Rule-driven variant: only for a floating window whose rule asked for it, so
 * a tiled window still belongs to the layout (and FLOAT_NEVER still wins). */
void window_apply_fullscreen(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->fullscreen || !mw->is_floating) return;
    window_park_over_monitor(hwnd);
}

/* ===========================================================================
 * Does this window's visible frame reach every edge of its monitor?
 *
 * This is how mshell recognises an app that fullscreened *itself*: a browser
 * going fullscreen for a video keeps the same HWND, drops its chrome and
 * resizes to the display. A few pixels of slop, because apps round monitor
 * bounds in both directions.
 * =========================================================================== */
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

/* Put a floating window back where it was before we parked it over the monitor.
 * A no-op for a tiled window (the layout re-places that one) and when there is
 * nothing saved, so callers don't have to check either. */
static void fs_restore_floating(ManagedWindow *mw) {
    if (!mw || !mw->is_floating || !mw->fs_has_prev || !IsWindow(mw->hwnd)) return;

    RECT p   = mw->fs_prev_rect;
    RECT adj = window_adjust_for_frame(mw->hwnd, p);

    events_suppress_begin();
    SetWindowPos(mw->hwnd, NULL, adj.left, adj.top,
                 adj.right - adj.left, adj.bottom - adj.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();

    mw->fs_has_prev  = false;
    mw->applied_rect = p;
    mw->has_applied  = true;
}

/* ===========================================================================
 * Set (or, for the mode it is already in, clear) a window's fullscreen mode.
 *
 * Every fullscreen binding routes through here, so each one is its own toggle
 * and pressing a different one switches modes without an intermediate "off".
 * The geometry itself is applied by the tiler for tiled windows — this only
 * records the mode and re-tiles — except for floating windows, which the tiler
 * never places, so those are parked (and put back) directly.
 * =========================================================================== */
void window_set_fullscreen(HWND hwnd, FullscreenMode mode) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    FullscreenMode next = (mw->fs_mode == mode) ? FS_OFF : mode;

    /* Remember where a floating window was before it grew: the layout re-places
     * a tiled window on the way out, but a floating one would keep the
     * monitor-sized rect we gave it for good.
     *
     * The test is "about to be parked, with nothing saved yet" rather than
     * "coming from FS_OFF". FS_CONTENT leaves a floating window's geometry
     * alone, so arriving from it the current rect is still the user's and is
     * exactly what we want to save — keying on FS_OFF missed that and stranded
     * the window at monitor size for good. Guarding on fs_has_prev also stops
     * FS_WINDOW -> FS_BOTH from overwriting the saved rect with our own. */
    if (mw->is_floating && !mw->fs_has_prev &&
        (next == FS_WINDOW || next == FS_BOTH))
        mw->fs_has_prev = window_frame_rect(hwnd, &mw->fs_prev_rect);

    /* One screen-covering window per monitor: a second one would just sit
     * invisibly underneath the first. */
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
                /* Demoting only clears the flags for a tiled window — the
                 * tiler puts it back. A floating one is never placed by the
                 * layout, so without this it keeps the monitor-sized rect we
                 * gave it and sits full-screen behind the new fullscreen
                 * window forever. */
                fs_restore_floating(o);
            }
        }
    }

    mw->fs_mode        = next;
    mw->app_fullscreen = false;   /* an explicit mode supersedes detection */
    mw->has_applied    = false;   /* force the next pass to reposition it  */

    log_w(L"fullscreen: %p mode=%d (float=%d)", (void *)hwnd, (int)next,
          mw->is_floating);

    if (mw->is_floating) {
        if (next == FS_WINDOW || next == FS_BOTH) {
            window_park_over_monitor(hwnd);
        } else if (next == FS_OFF) {
            fs_restore_floating(mw);
        }
        /* FS_CONTENT is meaningless while floating — a floating window already
         * keeps whatever geometry the app gives it. The mode is still recorded
         * so un-floating it later lands in the right state. */
    }

    tile_current();   /* places tiled fullscreen windows, re-tiles the rest */
}

/* ===========================================================================
 * Re-assert a rule's chrome and geometry on a window that changed behind our
 * back. Games rebuild their window long after it first appears — the graphics
 * device comes up, the user flips windowed/borderless in the options menu, a
 * resolution change resizes it — and a rule applied once at manage time is
 * silently undone by any of that. Called from the LOCATIONCHANGE handler.
 * =========================================================================== */
void window_reassert_rule(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    /* No-op unless a caption actually came back. */
    if (mw->no_decor) window_strip_decorations(hwnd);

    if (!mw->fullscreen || !mw->is_floating) return;

    /* Only re-place on genuine drift. Our own SetWindowPos comes back through
     * this same hook asynchronously — after the suppression counter has been
     * released — so acting on that echo would loop forever. Same tolerance the
     * tiled path uses. */
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

/* ===========================================================================
 * Enforce a sane z-order after a tiling pass:
 *   - the solid backdrop stays pinned to the very bottom so it can never rise
 *     up and black out the tiled windows;
 *   - optionally (float_on_top) floating windows are raised above the tiled
 *     grid so they read as overlays instead of sinking behind a tiled window.
 * Tiled windows never overlap each other, so their relative order is moot.
 * =========================================================================== */
void window_enforce_zorder(void) {
    if (g.background_window)
        SetWindowPos(g.background_window, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    Desktop *dt = desktop_current();

    if (g.float_on_top) {
        for (int i = 0; i < dt->count; i++) {
            ManagedWindow *mw = window_find(dt->windows[i]);
            if (mw && mw->is_floating && IsWindow(mw->hwnd) &&
                IsWindowVisible(mw->hwnd))
                SetWindowPos(mw->hwnd, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    /* A window covering the whole monitor goes above everything else — raised
     * last, so it beats the floating pass above too: an overlay stranded in the
     * middle of a fullscreen video is exactly what fullscreen is meant to end.
     *
     * It goes into the TOPMOST band, not just to the top of the ordinary one.
     * HWND_TOP cannot do this job: Windows sorts every WS_EX_TOPMOST window
     * above every non-topmost one no matter how recently the latter was raised,
     * so an always-on-top utility — or, in --test mode, explorer's taskbar —
     * keeps a strip of itself over content that asked for the whole display.
     * That reads both as "something is on top" and, when it is the taskbar
     * along an edge, as the window not quite reaching that edge.
     *
     * The promotion is recorded per window so leaving fullscreen can undo
     * exactly the ones we made, and a window that was already topmost on its
     * own account is left flagged false — we must not demote it later. */
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw || !IsWindow(mw->hwnd)) continue;

        if (window_is_screen_fullscreen(mw) && IsWindowVisible(mw->hwnd)) {
            if (!mw->made_topmost &&
                !(GetWindowLongPtrW(mw->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
                mw->made_topmost = true;
            SetWindowPos(mw->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (mw->made_topmost) {
            mw->made_topmost = false;
            SetWindowPos(mw->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
}

/* ===========================================================================
 * Set floating state
 * =========================================================================== */
void window_set_floating(HWND hwnd, bool floating) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw) return;

    mw->is_floating = floating;
    /* A window may have been dragged around while floating; forget its last
     * tiled rect so re-tiling actually repositions it instead of assuming it's
     * still parked where we left it. */
    mw->has_applied = false;
    if (floating) {
        /* Give the frame back — unless the rule says this window is meant to
         * stay borderless, in which case floating it must not re-decorate it. */
        if (mw->no_decor) window_strip_decorations(hwnd);
        else              window_restore_decorations(hwnd);
        window_apply_fullscreen(hwnd);   /* no-op without a fullscreen rule */
    } else {
        window_strip_decorations(hwnd);
    }
}

/* ===========================================================================
 * Claim foreground rights for this process.
 *
 * Windows only honors SetForegroundWindow from a process that is *entitled* to
 * the foreground. The entitlement that matters to us is "the process received
 * the last input event" — and a window manager never qualifies. Our keys reach
 * us through a low-level hook, which observes input on its way past rather than
 * receiving it, so at the exact moment we want to move focus win32k considers us
 * an idle background process. SetForegroundWindow then quietly downgrades to
 * flashing the window and still reports success: the focus ring moves, the
 * keyboard doesn't. That is the bug this exists to kill.
 *
 * Injecting one keystroke makes us the last process to touch the input stream,
 * which is precisely the condition win32k checks. VK 0 is not a real key — no
 * scan code, no character, nothing an application acts on — and, unlike the
 * AttachThreadInput trick this deliberately replaces, injecting never merges
 * input queues, so it cannot leave the held Win key stuck down in another
 * thread's key state. The event is tagged so kb_hook_proc lets it straight
 * through (swallowing it would deny us the very credit we're after).
 * =========================================================================== */
static void claim_foreground_rights(void) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type           = INPUT_KEYBOARD;
    in.ki.wVk         = 0;
    in.ki.wScan       = 0;
    in.ki.dwExtraInfo = MSHELL_INPUT_TAG;
    SendInput(1, &in, sizeof(in));
}

/* ===========================================================================
 * Focus a window — reliably, even from our background process.
 * =========================================================================== */
void window_focus(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);

    /* Raise without activating first. Even if activation is refused below, the
     * window is at least on top — and HWND_TOP is what the focus ring stacks
     * itself against in border_refresh(). */
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    /* NB: deliberately NO AttachThreadInput here.
     *
     * The usual trick to make SetForegroundWindow reliable from a background
     * thread is to AttachThreadInput to the foreground thread. But that MERGES
     * the two threads' input state — including which keys are currently down —
     * and we run this focus change while the user is physically HOLDING the Win
     * key (Win+h/l to move focus). Attaching/detaching while a modifier is held
     * leaves that modifier stuck DOWN in the other thread's key state, so
     * afterwards a bare key reads as Win+key. That was the "Win key acts held"
     * bug, and it accumulates over repeated focus changes.
     *
     * Instead we earn the foreground the way the OS wants it earned — by being
     * the process that last touched the input stream — and never touch anyone's
     * key state. See claim_foreground_rights() above.
     *
     * NB: no AllowSetForegroundWindow(ASFW_ANY) either. That call grants
     * *another* process the right to take the foreground from us; it never
     * helped our own SetForegroundWindow, and handing our rights away is
     * actively counterproductive one line after acquiring them. */
    claim_foreground_rights();
    SetForegroundWindow(hwnd);

    /* SetForegroundWindow is advisory and reports success even when it only
     * flashed the window, so the only way to know is to look. If it didn't
     * take, re-assert the zero foreground-lock timeout (a *global* setting a
     * session unlock, a policy refresh or another process can put back), claim
     * rights again and retry. Still nothing: fall back to SwitchToThisWindow,
     * the entry point Alt+Tab itself uses, which activates across processes
     * without merging input queues. */
    if (GetForegroundWindow() != hwnd) {
        SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0,
                              (PVOID)(UINT_PTR)0, SPIF_SENDCHANGE);
        claim_foreground_rights();
        SetForegroundWindow(hwnd);

        if (GetForegroundWindow() != hwnd)
            SwitchToThisWindow(hwnd, TRUE);
    }

    /* Now that the window's thread owns the foreground, its queue is where the
     * keyboard focus lives; SetFocus is a no-op across threads but harmless. */
    SetFocus(hwnd);

    /* Keep "the monitor we're on" with the focus, so Win+,/. steps from where
     * the user actually is rather than from the last monitor keybind. */
    {
        ManagedWindow *mw = window_find(hwnd);
        int mon = mw ? mw->monitor : monitor_of_window(hwnd);
        if (mon >= 0 && mon < g.monitor_count) g.focused_monitor = mon;
    }

    /* Keep the focus ring on the newly-focused window. */
    border_refresh();

    /* Name what won instead of just "FAILED" — the window that kept the
     * foreground identifies the cause (a UIPI-protected elevated app, an app
     * holding the foreground lock, our own overlay) far faster than a bare
     * failure flag does. */
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

/* ===========================================================================
 * Close a window gracefully (WM_CLOSE)
 * =========================================================================== */
void window_close(HWND hwnd) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

/* ===========================================================================
 * Kill a window forcefully (TerminateProcess)
 * =========================================================================== */
void window_kill(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return;

    HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hp) return;

    TerminateProcess(hp, 1);
    CloseHandle(hp);
}

/* ===========================================================================
 * Enumerate & manage all existing windows at startup
 * =========================================================================== */
static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lp) {
    (void)lp;
    if (window_is_manageable(hwnd)) {
        window_manage(hwnd);
    }
    return TRUE;
}

void window_manage_existing(void) {
    EnumWindows(enum_windows_proc, 0);
}
