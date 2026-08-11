/*
 * window.c — window management: enumeration, manage/unmanage,
 *            decoration stripping, rules engine.
 */

#include "mshell.h"
#include "layout_math.h"   /* center_axis()/clamp_axis() — where a float sits */
#include "overlay.h"       /* overlay_raise_all() — our own surfaces stay up */

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
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
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

/* Exposed for the Lua state queries (lua_api.c), which need to describe a
 * window to the config without duplicating the process-handle dance. */
void window_process_path(HWND hwnd, wchar_t *out, size_t out_len) {
    get_process_path(hwnd, out, out_len);
}

/* Defined with the rest of the z-order pass, declared here because handing the
 * topmost band back happens on paths that come long before it — unmanaging a
 * window, and shutting down. */
static bool window_set_band(HWND hwnd, HWND after, bool topmost);

/* Defined with the rest of the float placement logic, declared here because
 * adoption and promotion both finish by asking it — and both come earlier in
 * the file than the floats do. */
static void window_park_float_if_fullscreen(ManagedWindow *mw);

/* The file name inside a path, or the whole string when there is no separator. */
static const wchar_t *path_basename(const wchar_t *path) {
    const wchar_t *back = wcsrchr(path, L'\\');
    const wchar_t *fwd  = wcsrchr(path, L'/');
    const wchar_t *sep  = (back > fwd) ? back : fwd;
    return sep ? sep + 1 : path;
}

/* wildcard_match now lives in match.c — it is pure C with no Windows
 * dependency, which is what lets `make test` cover it on the host. */

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
 * window_adopt_tier — how much control a window gets
 *
 * Every window that shows up is adopted at one of three tiers:
 *
 *   ADOPT_NO     left completely alone: invisible or child windows, the
 *                shell's own UI, menus and tooltips, cloaked windows, and
 *                anything an "ignore" rule names. These must stay out of the
 *                desktop machinery — and "ignore" stays the way to claim a
 *                window for every desktop at once (overlays).
 *   ADOPT_TRACK  desktop membership only. The window is hidden and shown with
 *                its desktop, focusable, closable and movable between
 *                desktops — but it keeps its own chrome and geometry and is
 *                never tiled. This is the tier for windows full management
 *                would only fight: an owned dialog nobody opted into, a window
 *                disabled by its own modal (Steam's updater holding its main
 *                window), or one too small to tile. Crucially it is adopted
 *                ALL THE SAME — a window rejected here is the window that ends
 *                up visible on every desktop, which is the bug this tier
 *                exists to kill. toggle_float promotes one to full.
 *   ADOPT_FULL   the layout owns it: tiled (or rule-floated), decorations
 *                stripped, focus ring.
 *
 * `rule_out` reports the matched rule (or NULL) so the caller does not have to
 * look it up a second time. window_rule_lookup() opens the owning process and
 * queries its image path, and this runs for every window that SHOWS anywhere on
 * the system — every menu, tooltip, dropdown and IME candidate list — so doing
 * it twice per window was a measurable waste on the busiest path there is.
 * Within this function the lookup is likewise done at most once, memoised in
 * `rule` / `looked_up`.
 * =========================================================================== */
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

    /* A window disabled by its own modal dialog cannot be tiled sensibly, but
     * it still belongs to the desktop it opened on. */
    if (!IsWindowEnabled(hwnd))   return ADOPT_TRACK;

    /* Must be a root window */
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return ADOPT_NO;

    /* Owned windows are dialogs/pop-ups that normally float. FULLY managing
     * them is opt-in (mshell.set_manage_owned(true)) because modal/fixed-size
     * dialogs tile poorly — but tracking them is not: an owned window belongs
     * to the desktop it opened on like any other.
     *
     * A `dialog` rule is the second, narrower opt-in: it says "manage these,
     * but floating", which is the whole difference between a file picker mshell
     * merely hides and shows with its desktop and one it also floats, focuses
     * and decorates like any other window. Only a rule that explicitly asked
     * about dialogs may rescue an owned window: a broad path rule (the
     * game-library ones) must not start pulling in every splash screen and
     * error box a game owns. */
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

    /* Tool windows are popups; ignore */
    if (exstyle & WS_EX_TOOLWINDOW) return ADOPT_NO;

    /* Must be an overlapped or popup window (not a child) */
    if (!(style & WS_OVERLAPPEDWINDOW) && !(style & WS_POPUP)) return ADOPT_NO;

    /* Ignore the desktop window and explorer's shell UI. The shell classes
     * matter in --test mode (explorer is running); harmless otherwise.
     *
     * This sits ABOVE the cloaked test on purpose: GetClassNameW is a local
     * read, while DwmGetWindowAttribute is an RPC to dwm.exe. Rejecting the
     * cheap way first keeps the expensive question off the common path. */
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
        L"mshell_Bar",          /* our own status bar         */
        L"mshell_WhichKey",     /* our own submap hint        */
        L"mshell_Notify",       /* our own toasts             */
        L"mshell_Launcher",     /* our own app launcher       */
        L"mshell_Dim",          /* our own unfocused scrim    */
        NULL
    };
    for (const wchar_t **p = ignore_classes; *p; p++) {
        if (_wcsicmp(cls, *p) == 0) return ADOPT_NO;
    }

    /* Minimum size (configurable). Skipped while the window is minimized: an
     * iconic window reports a near-zero rect, and its real size comes back
     * with the restore — rejecting one here used to strand apps that open
     * minimized outside management for good. */
    if (!IsIconic(hwnd)) {
        RECT r;
        if (!GetWindowRect(hwnd, &r)) return ADOPT_TRACK;
        int w = r.right  - r.left;
        int h = r.bottom - r.top;
        if (w < g.min_win_w || h < g.min_win_h) return ADOPT_TRACK;
    }

    /* Cloaked windows (Windows 8+ DWM feature). Last of the cheap rejections
     * because it is the only one that leaves the process. */
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloaked, sizeof(cloaked))) && cloaked)
        return ADOPT_NO;

    /* Rules have the last word, and they are consulted *before* the caption-less
     * popup heuristic below — a rule that names a window is the user telling us
     * what it is, which beats guessing from styles. That matters for exactly one
     * class of window: a borderless-fullscreen game is a WS_POPUP with no
     * caption and no sizebox, i.e. byte-for-byte the style a menu or tooltip
     * wears. Without this, such a game is dropped here and its rule never runs:
     * it stays on screen across every desktop switch and Win+Shift+c can't
     * reach it. Rules match in order, so an "ignore" rule placed ahead of a
     * broad path rule still carves exceptions out of it. */
    if (!looked_up) rule = window_rule_lookup(hwnd);
    if (rule_out) *rule_out = rule;
    if (rule) return rule->action != RULE_IGNORE ? ADOPT_FULL : ADOPT_NO;

    /* Ignore invisible/system popups */
    if ((style & WS_POPUP) && !(style & WS_CAPTION) && !(style & WS_SIZEBOX))
        return ADOPT_NO;  /* likely a menu / tooltip */

    return ADOPT_FULL;
}

bool window_is_manageable(HWND hwnd) {
    return window_adopt_tier(hwnd, NULL) == ADOPT_FULL;
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

    /* Fetched lazily for the same reason is_dlg is: this runs for every window
     * that appears anywhere on the system, and most configs have no title rule
     * at all. GetWindowTextW can block on a hung app, so it is also the one
     * criterion worth not asking for speculatively. */
    wchar_t title[256];
    int     have_title = 0;

    for (int i = 0; i < g.rule_count; i++) {
        WindowRule *r = &g.rules[i];

        /* Every string criterion is a pattern; an empty one matches anything. */
        if (r->class_match[0]   && !wildcard_match(r->class_match, cls))    continue;
        if (r->process_match[0] && !wildcard_match(r->process_match, proc)) continue;
        if (r->path_match[0]    && !wildcard_match(r->path_match, path))    continue;
        if (r->title_match[0]) {
            if (!have_title) {
                title[0] = L'\0';
                /* SendMessageTimeout rather than GetWindowText: the latter can
                 * hang us on a window whose thread is not pumping, and this runs
                 * on the thread that also services keybinds. */
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

/* ===========================================================================
 * Find a ManagedWindow by HWND
 * =========================================================================== */
/* Move a window to a display and remember which display that IS, by name.
 *
 * Every assignment to mw->monitor goes through here, because the index alone
 * does not survive a hotplug: unplug a display and the rest renumber, so a
 * remembered index comes to mean a different screen. The name is what
 * monitors_update matches on to put the window back.
 *
 * Returns true when something changed, so callers can skip a needless re-tile. */
bool window_set_monitor(ManagedWindow *mw, int mon) {
    if (!mw) return false;
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;
    if (mon < 0 || mon >= g.monitor_count) return false;   /* no monitors yet */

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
    /* Square by default, but configurable: a tiled grid with rounded corners
     * has a gap at every junction that the gap setting did not ask for, while a
     * mostly-floating setup may want Windows' own look back. */
    DWORD corner = (DWORD)g.corner_pref;
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

    /* Did it actually take? Neither SetWindowLongPtrW above can be trusted to
     * have worked: UIPI refuses a style write aimed at a window owned by a
     * higher-integrity process, and it refuses it silently as far as the return
     * value is concerned. Setting the flag anyway meant `decorations_stripped`
     * claimed a frame had been taken off a window that still wears its caption,
     * and mshell then had no idea the rule had not applied.
     *
     * Read the style back rather than checking a return: it is one call, it is
     * authoritative, and it is the same question the flag is supposed to answer.
     * orig_style is left recorded either way — it is what the window HAD, which
     * is true whether or not we managed to change it. */
    bool stripped = !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION);
    if (!stripped && !mw->decor_strip_refused) {
        mw->decor_strip_refused = true;
        log_msg(LOG_WARN, L"could not strip decorations from %p — the window "
                          L"belongs to a higher-integrity process and keeps its "
                          L"title bar", (void *)hwnd);
    }
    mw->decorations_stripped = stripped;
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

        /* Also hand back the topmost band to any window we parked there —
         * fullscreen, pinned, or floating: mshell is going away, and a window
         * left pinned above everything else outlives us with no way to fix it
         * from the keyboard. Through window_set_band, so an elevated one goes
         * back down too rather than staying over the desktop for good. */
        if (mw->made_topmost && IsWindow(mw->hwnd)) {
            mw->made_topmost = false;
            window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
        }
    }
}

/* ===========================================================================
 * Taking a window off the screen, and putting it back.
 *
 * Desktop switching, monocle and the scratchpad all need a window gone from
 * the screen while it stays alive and managed, and this is the one place that
 * decides HOW. See the HidePolicy comment in mshell.h for why the default is
 * cloaking and not ShowWindow(SW_HIDE) — the short version is that SW_HIDE
 * makes DWM destroy the window's redirection surface and makes Chromium-class
 * apps shut their compositor down, so the window comes back black.
 * =========================================================================== */

/* Ask DWM to stop compositing this window (or to resume). Cross-process, like
 * every other DwmSetWindowAttribute call here. Fails on a window whose process
 * has gone, which is why the result is checked rather than assumed. */
static HRESULT dwm_set_cloaked(HWND hwnd, bool on) {
    BOOL v = on ? TRUE : FALSE;
    return DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &v, sizeof(v));
}

/* Cloak (or uncloak) with the helper as the second try. Returns true when the
 * window ended up in the requested state by either route.
 *
 * The helper is asked on ANY failure, not on E_ACCESSDENIED alone. That test
 * was written from the placement path, where a refusal really does arrive as
 * ERROR_ACCESS_DENIED — but DWM does not answer the same way. Measured on
 * Windows 11, from an unelevated shell:
 *
 *     ordinary same-user window (a browser, a terminal)   0x80070005
 *     window owned by a higher-integrity process          0x80070006
 *
 * So the one case the helper exists for — Task Manager, an admin terminal —
 * was the case the old test let through, and the fallback never ran for it.
 * Nothing here can tell the failures apart usefully anyway: cloaking another
 * process's window is privileged whoever owns it, and the only question worth
 * asking is whether it worked. A missing helper costs one WaitNamedPipe with a
 * zero timeout, so trying and failing is cheap. */
static bool window_set_cloaked(HWND hwnd, bool on) {
    HRESULT hr = dwm_set_cloaked(hwnd, on);
    if (SUCCEEDED(hr)) return true;
    if (helper_set_cloak(hwnd, on)) return true;

    /* Both routes refused. Report it here, where the HRESULT and whether the
     * helper was even listening are both still in hand — window_hide only
     * knows that it has to fall back.
     *
     * Whether the helper was running is the whole diagnosis. Elevated, it is
     * supposed to be the way across this boundary; if it was connected and the
     * cloak STILL did not happen, then DWM is not refusing on integrity at all
     * and will not cloak another process's window from any process on this
     * build of Windows — in which case no amount of installing changes it and
     * SW_HIDE, with everything that costs a Chromium window, is all there is.
     * Say which of the two it is rather than sending the user to INSTALL.md for
     * a helper they may already have running. */
    static bool warned;
    if (!warned) {
        warned = true;
        log_err(L"cloak: refused for %p — DwmSetWindowAttribute returned "
                L"0x%08lX and mshelld.exe %ls. Hiding falls back to "
                L"ShowWindow(SW_HIDE), which Chromium-based apps come back from "
                L"blank or mis-composited.%ls",
                (void *)hwnd, (unsigned long)hr,
                helper_available() ? L"could not do it either"
                                   : L"is not running",
                helper_available()
                    ? L" The helper IS connected, so this is not a missing "
                      L"install: DWM will not cloak a foreign window at all here."
                    : L" Run `install.bat /helper` from an administrator prompt "
                      L"— see INSTALL.md.");
    }
    return false;
}

bool window_on_screen(const ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    if (mw->wm_hidden || mw->app_hidden) return false;
    return IsWindowVisible(mw->hwnd) != 0;
}

/* ===========================================================================
 * STASHING — taking a window off the screen without taking it off the screen.
 *
 * The third mechanism, and the one a refused cloak falls back to. The window is
 * MOVED clear of every display: still WS_VISIBLE, still composited, still
 * drawing, simply nowhere you can look at it.
 *
 * Why this rather than SW_HIDE, which is what the fallback used to be: clearing
 * WS_VISIBLE makes DWM drop the window's redirection surface, and a
 * Chromium-class app rebuilds its compositor from that — blank until something
 * makes it draw, or offset by its client origin, once per hide/show round trip
 * until you restart the app. Nothing mshell can do on the way back reliably
 * repairs it; window_show's nudge exists to try and does not always win. A
 * window that was never torn down has nothing to rebuild.
 *
 * The way back is a real SetWindowPos from off-screen to where it was, which is
 * the other half of the point: the old show path re-applied the rect a window
 * already had, and an app is free to do nothing at all with a move that moves
 * it nowhere.
 *
 * WHAT IT COSTS. A stashed window is still a window: Alt+Tab lists it (mshell
 * eats Alt+Tab, but the OS still knows), a screen recorder that captures a
 * window by handle still captures it, and it still costs whatever it costs to
 * render. Against a browser that quietly corrupts itself on every desktop
 * switch, that is a trade worth making.
 *
 * STRANDING. A window stashed when mshell dies is off-screen with nothing left
 * to bring it back, so the startup sweep in window_uncloak_strays pulls those
 * in too, and window_restore_all_visibility unstashes on the way out. An iconic window is never stashed: it has no surface to protect and
 * moving one only edits the rect it will restore to.
 * =========================================================================== */

/* Clear of the rightmost display, and clear of a display appearing there while
 * the window is away. Window coordinates are signed 16-bit at the boundary, so
 * this stays well inside ±32767 even on a wide multi-monitor desktop. */
#define STASH_GAP 4000

static bool stash_position(int *x, int *y) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    if (vw <= 0) return false;

    *x = vx + vw + STASH_GAP;
    *y = vy;                     /* same band, so the trip back is one axis */
    return *x < 30000;           /* refuse rather than wrap into the desktop */
}

/* True when nothing of this rect is on any display — the test the startup
 * sweep uses to recognise a window a dead mshell left stashed. */
static bool rect_off_screen(RECT r) {
    RECT vs = { GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    vs.right  = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    RECT hit;
    return !IntersectRect(&hit, &r, &vs);
}

static bool window_stash(ManagedWindow *mw) {
    if (IsIconic(mw->hwnd)) return false;   /* already gone, and un-moveable */

    RECT r;
    int  x, y;
    if (!stash_position(&x, &y) || !GetWindowRect(mw->hwnd, &r)) return false;
    if (rect_off_screen(r)) return false;   /* not ours to file away */

    /* Anything but a flat refusal is a stash: it does not matter to us whether
     * the move went locally or through the helper, only that the window is
     * gone from the screen and we know where it came from. */
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
    /* FRAMECHANGED | NOCOPYBITS for the same reason window_show's nudge carries
     * them: whatever the app had cached for this position is stale, and the
     * saved bits are the ones we do not want blitted forward. */
    window_set_pos(mw->hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                   SWP_NOCOPYBITS);
}

void window_hide(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return;
    if (mw->wm_hidden) return;      /* already ours and already gone */
    /* The app hid it itself (minimise-to-tray). It is already off the screen,
     * and claiming it would mean un-hiding it later on the app's behalf — so
     * leave it alone and let the EVENT_OBJECT_SHOW handler deal with it when
     * the app brings it back. */
    if (mw->app_hidden) return;

    /* The flags go up BEFORE the window is touched, never after.
     *
     * The EVENT_OBJECT_HIDE handler tells our own hide from an app's
     * minimise-to-tray by reading exactly these two, and getting that wrong
     * loses the window for good (see the comment there). The out-of-context
     * hook queues the event rather than delivering it inline, so setting them
     * afterwards happens to work — but an invariant that holds only by timing
     * is not one to lean on, and it costs nothing to make it hold outright. */
    mw->wm_hidden = true;
    mw->cloaked   = false;
    mw->stashed   = false;

    if (g.hide_policy == HIDE_CLOAK) {
        /* Cloak, then stash, then — only if the window cannot be moved either —
         * SW_HIDE. The refusal itself is logged once, with its HRESULT, by
         * window_set_cloaked; this is only about what to do instead.
         *
         * Stashing is preferred over SW_HIDE rather than the other way round
         * because SW_HIDE is the mechanism that damages the window: WS_VISIBLE
         * comes off, DWM drops the redirection surface, and a Chromium-class
         * app rebuilds its compositor blank or offset on the way back, every
         * round trip, until it is restarted. */
        mw->cloaked = window_set_cloaked(mw->hwnd, true);
        if (!mw->cloaked) mw->stashed = window_stash(mw);
    }

    if (!mw->cloaked && !mw->stashed) {
        /* Last resort: an iconic window (nothing to protect, and moving one
         * only edits the rect it will restore to), no room left in the
         * coordinate space to stash it in, or the "hide" policy outright. */
        ShowWindow(mw->hwnd, SW_HIDE);

        /* ...and SW_HIDE can be refused too, on the same integrity boundary
         * that just refused the cloak and the stash. Every mechanism having
         * failed is the case that must NOT be recorded as a hide: wm_hidden is
         * what window_on_screen() answers from, so a window still sitting on
         * the display while the flag says otherwise is the "elevated window
         * stares at you from every desktop" bug — silently, with the desktop
         * switch believing it did its job.
         *
         * Clearing the flag here does not violate the set-it-first rule above.
         * That rule exists so the queued EVENT_OBJECT_HIDE is not mistaken for
         * the app traying itself — and if nothing hid the window, no such
         * event was ever generated to be mistaken. */
        if (IsWindowVisible(mw->hwnd)) {
            mw->wm_hidden = false;
            static bool warned;
            if (!warned) {
                warned = true;
                log_err(L"hide: %p could not be taken off the screen by any "
                        L"means — not cloaked, not stashed, and SW_HIDE was "
                        L"refused. It will be visible on every desktop. This is "
                        L"what mshelld.exe exists for: run `install.bat /helper` "
                        L"from an administrator prompt (see INSTALL.md).",
                        (void *)mw->hwnd);
            }
            return;
        }
    }

    log_msg(LOG_DEBUG, L"hide: %p (%ls)", (void *)mw->hwnd,
            mw->cloaked ? L"cloaked" : mw->stashed ? L"stashed" : L"SW_HIDE");
}

void window_show(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return;
    if (mw->app_hidden) return;     /* not ours to reveal */

    /* Was it actually off the screen? Asked before anything is undone, and of
     * our own bookkeeping rather than of IsWindowVisible — a cloaked window is
     * still WS_VISIBLE, so the window we most need to nudge is exactly the one
     * IsWindowVisible would call fine. Getting this wrong is what shipped:
     * the repaint below sat inside an `if (!IsWindowVisible)` and therefore
     * never ran for a cloaked window at all. */
    bool was_off_screen = mw->wm_hidden || mw->cloaked || mw->stashed;
    bool was_cloaked    = mw->cloaked;
    bool was_stashed    = mw->stashed;

    /* Uncloak whenever we cloaked it, even if wm_hidden is already clear: the
     * two can drift apart if the policy changed under a hidden window, and a
     * window left cloaked is invisible with no way back. Unstashing is the same
     * promise for the other mechanism, and unconditional for the same reason:
     * the flag is the only record of where the window belongs. */
    if (mw->cloaked) {
        if (!window_set_cloaked(mw->hwnd, false))
            log_msg(LOG_WARN, L"show: could not uncloak %p — the window stays "
                              L"invisible", (void *)mw->hwnd);
        mw->cloaked = false;
    }
    window_unstash(mw);

    if (!IsWindowVisible(mw->hwnd)) {
        /* SW_SHOWNOACTIVATE restores a minimized window, which would quietly
         * un-minimize everything every time you came back to a desktop. Hiding
         * does not clear WS_MINIMIZE, so IsIconic still answers correctly. */
        ShowWindow(mw->hwnd, IsIconic(mw->hwnd) ? SW_SHOWMINNOACTIVE
                                                : SW_SHOWNOACTIVATE);
    }

    if (was_off_screen) {
        /* Make the app repaint, whichever way it was hidden.
         *
         * A window coming back on screen is the case that renders black. The
         * app has not been asked for a frame — it was told (by the hide, or by
         * its own occlusion tracking noticing the cloak) that nobody was
         * looking — and what is left in front of DWM is an empty or stale
         * surface. Nothing else asks either: the window is by definition
         * exactly where the layout already wants it, so flush_placements'
         * rect_eq skip means it gets no SetWindowPos at all.
         *
         * So both halves of the nudge, always:
         *   RedrawWindow  queues WM_PAINT on the window AND its children —
         *                 RDW_ALLCHILDREN matters, because a Chromium window's
         *                 content lives in a child HWND, not the top-level one.
         *                 No RDW_UPDATENOW: that paints synchronously and would
         *                 hang the WM on an app that is not answering.
         *   has_applied   dropped so the next tiling pass issues a real
         *                 SetWindowPos, and needs_repaint so that pass cannot
         *                 skip it and adds SWP_NOCOPYBITS. */
        RedrawWindow(mw->hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        mw->has_applied   = false;
        mw->needs_repaint = true;

        /* The tiler is what consumes needs_repaint, and the tiler never places
         * a floating window — so for those, nobody would. Re-apply the rect it
         * already has: a no-op move, but it carries SWP_FRAMECHANGED and
         * SWP_NOCOPYBITS, which is the whole point.
         *
         * Skipped for a window that was stashed: the unstash above WAS the move,
         * with the same flags and a real distance behind it, which is strictly
         * the better version of this. */
        if (mw->is_floating && !was_stashed) {
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
                was_cloaked ? L"cloaked" : was_stashed ? L"stashed" : L"hidden");
    }

    mw->wm_hidden = false;
}

/* ===========================================================================
 * Re-show every window mshell hid.
 *
 * Both virtual desktops (desktop.c) and monocle (tiling.c) are implemented by
 * hiding windows, so at any moment most managed windows are invisible — every
 * window on a desktop you are not looking at, plus every monocle window that
 * is not the focused one.
 *
 * Nothing else ever brings those back. A hidden top-level window has no
 * taskbar button and no Alt+Tab entry, and a CLOAKED one is worse still — it
 * keeps its taskbar button and gives you nothing when you click it. So the
 * instant mshell stops running they are unreachable: as the shell there is
 * nothing left to reveal them, and in --test mode explorer comes back to a
 * taskbar of buttons that do nothing. Quitting is a bound key, and a crash
 * lands in the same place (see the crash handler in main.c).
 *
 * Uncloaking is therefore unconditional — not gated on wm_hidden and not on
 * the current policy. This runs once, on the way out, in a process that may
 * already be broken, and the only failure that matters is leaving a window
 * cloaked; asking DWM to uncloak a window that was not cloaked costs an RPC
 * and does nothing.
 *
 * Call this on the way out, BEFORE window_restore_all_decorations(), so the
 * frames are handed back to windows the user can actually see.
 * =========================================================================== */
void window_restore_all_visibility(void) {
    int shown = 0;

    for (int i = 0; i < g.managed_count; i++) {
        ManagedWindow *mw = &g.managed[i];
        if (!IsWindow(mw->hwnd)) continue;
        /* Undo mshell's hiding, not the app's: a window sitting in the tray
         * because the user closed it there should stay there. */
        if (mw->app_hidden) continue;

        if (mw->cloaked || mw->stashed || !IsWindowVisible(mw->hwnd)) shown++;

        /* Unconditional, not `if (mw->cloaked)`: this is the last chance any
         * of these windows get, and a bad flag here costs the user a window
         * they cannot see and cannot reach. Goes through the helper too when
         * the window belongs to a higher-integrity process — leaving an
         * elevated window cloaked on the way out is the same lost window. */
        window_set_cloaked(mw->hwnd, false);
        mw->cloaked   = false;

        /* And the same for the other mechanism: a stashed window is off every
         * display, and once this process is gone nothing else knows where it
         * came from. This is the only thing that does. */
        window_unstash(mw);

        mw->wm_hidden = false;

        if (IsWindowVisible(mw->hwnd)) continue;

        /* SW_SHOWNA reveals without activating: we are tearing down and have no
         * business deciding which window ends up focused.
         *
         * A window that was minimized when we hid it is shown minimized again
         * rather than restored — that state is the USER's, not ours, and this
         * function exists to undo mshell's hiding, not to override their
         * choices on the way out. */
        ShowWindow(mw->hwnd, IsIconic(mw->hwnd) ? SW_SHOWMINNOACTIVE : SW_SHOWNA);
        shown++;
    }

    if (shown) log_err(L"shutdown: re-showed %d hidden window(s)", shown);
}

/* ===========================================================================
 * window_grant_full — the full-management half of adoption
 *
 * Everything the layout, the ring and the rules engine need, decided from the
 * matched rule (or NULL). Shared by window_manage(), adopting a window that
 * just appeared, and window_promote(), upgrading a tracked one — the callers
 * own placement, which is the only thing that differs between them.
 * =========================================================================== */
static void window_grant_full(ManagedWindow *mw, const WindowRule *rule) {
    HWND hwnd = mw->hwnd;

    mw->no_ring    = rule ? rule->no_ring    : false;
    mw->no_decor   = rule ? rule->no_decor   : false;
    mw->fullscreen = rule ? rule->fullscreen : false;
    /* Resolved once, here, rather than read from the globals at each placement:
     * the rule is already in hand, and pinning the answer to the window means a
     * later reload cannot change where a window that is already open jumps to
     * the next time it floats. */
    mw->center_float = (rule && rule->set_center)
                       ? rule->center
                       : (g.float_placement == FLOAT_PLACE_CENTER);

    /* Square corners + no open/close animation, for tiled and floating alike. */
    window_apply_flat(hwnd);

    /* Two ways to end up floating: the window's own rule says so, or the
     * desktop it opened on floats everything (desktop_rule float = true). The
     * desktop's answer is deliberately checked AFTER FLOAT_NEVER's veto of the
     * window rule, because it is the more specific statement of intent: a
     * config that tiles aggressively and then carves out one floating desktop
     * means it. toggle_float still works there either way — `float` sets what
     * new windows START as, it doesn't pin them. */
    Desktop *dt = desktop_by_id(mw->desktop_id);
    mw->is_floating =
        (rule && rule->action == RULE_FLOAT && g.float_policy != FLOAT_NEVER)
        || (dt && dt->float_all);

    if (mw->is_floating) {
        /* Floating normally means "hands off the frame": the window keeps
         * whatever chrome it came with. `decorate = false` opts out of that —
         * float for the geometry, borderless for the looks. */
        if (mw->no_decor) window_strip_decorations(hwnd);
    } else {
        window_strip_decorations(hwnd);
    }

    /* Recorded before anything places the window, because it is one of the
     * answers to "who owns this rect": a window that starts fullscreen is
     * claiming the whole monitor, and neither the centring nor the tile pass
     * should put it somewhere else first. */
    if (rule && rule->start_fullscreen) mw->fs_mode = FS_WINDOW;
}

/* ===========================================================================
 * Manage a window — bring it under WM control
 * =========================================================================== */
void window_manage(HWND hwnd) {
    /* Already adopted? */
    if (window_index_of(hwnd) >= 0) return;

    /* One lookup for both questions — the tier test already resolved the rule
     * (opening the owning process to do it), so asking again here would double
     * the cost of every window that appears anywhere on the system. */
    const WindowRule *rule = NULL;
    AdoptTier tier = window_adopt_tier(hwnd, &rule);
    if (tier == ADOPT_NO) return;

    /* Grow managed array if needed */
    if (g.managed_count >= MAX_MANAGED_WINDOWS) return;

    /* New windows land on the desktop you are looking at — unless a rule says
     * otherwise, in which case that desktop is created if it does not exist,
     * exactly as switching to a name would. The window is added there and, since
     * only the current desktop is ever tiled, is hidden on the way if you are
     * not standing on it. */
    int slot = desktop_current_slot();
    if (rule && rule->desktop[0]) {
        int target = desktop_ensure(rule->desktop);
        if (target >= 0) slot = target;
        else log_msg(LOG_WARN, L"rule: cannot place a window on '%ls' — the "
                               L"name is unusable or there are too many "
                               L"desktops", rule->desktop);
    }
    Desktop *dt = &g.desktops[slot];

    ManagedWindow *mw = &g.managed[g.managed_count++];
    memset(mw, 0, sizeof(*mw));
    mw->hwnd       = hwnd;
    mw->desktop_id = dt->id;
    mw->cfact      = 1.0f;

    if (tier == ADOPT_TRACK) {
        /* Desktop membership and window control, nothing else: the window
         * keeps its chrome and the rect it opened with. Floating by
         * construction, so the layout never offers it a tile, and no ring —
         * it would claim the window for a layout it is not in. */
        mw->tracked_only = true;
        mw->is_floating  = true;
        mw->no_ring      = true;
        /* Record where it opened (window_set_monitor only records; it does
         * not move anything). */
        window_set_monitor(mw, monitor_of_window(hwnd));
    } else {
        window_grant_full(mw, rule);

        /* Tile it where it opened — unless the desktop is pinned to a display,
         * in which case that is where the desktop's windows go. */
        {
            /* A desktop pinned to a display wins: it moves ALL its windows
             * there, so honouring a per-window pin against it would just be
             * undone on the next reload. Otherwise the rule's pin, then
             * wherever it opened. */
            int mon = monitor_of_window(hwnd);
            if (rule && rule->set_monitor) mon = rule->monitor;
            if (dt->monitor >= 0 && dt->monitor < g.monitor_count)
                mon = dt->monitor;
            window_set_monitor(mw, mon);
        }

        /* A fixed rect only means anything for a window the layout is not
         * going to place. Applied before the tile pass so the first frame is
         * already right, and recorded as applied so the drift detector reads
         * it as ours. */
        if (mw->is_floating && rule && rule->set_geometry) {
            RECT want = { rule->x, rule->y, rule->x + rule->w,
                          rule->y + rule->h };
            window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
            /* A rule is a coordinate someone typed, and displays come and go
             * after it was typed. If it just put the window somewhere the user
             * cannot look at, pull it back — a window that opens onto no
             * display at all cannot be recovered from the keyboard. */
            window_rescue_offscreen(mw);
        } else if (mw->is_floating) {
            /* Nothing more specific asked for a rect, so the placement policy
             * gets it. Also before the tile pass, for the same reason: the
             * first frame the user sees should already be in the right place,
             * not a frame in the corner followed by a jump to the middle. */
            window_center_float(hwnd);
        }
    }

    desktop_add_window(hwnd, slot);

    /* Hide it if it landed somewhere you are not looking — desktop membership
     * is show/hide here, and only the current desktop is ever tiled. */
    if (dt->id != g.current_desktop_id) {
        events_suppress_begin();
        window_hide(mw);
        events_suppress_end();
    }

    tile_current();

    /* The tiler never places floating windows, so anything that wants one to
     * fill its monitor has to do it here — the `fullscreen = true` rule, and
     * `start_fullscreen`, which sets the MODE and which nothing used to act on
     * for a float. No-op for tiled windows (the layout already owns their
     * geometry) and for tracked ones, which carry neither flag.
     *
     * Parking only, deliberately not window_place_float: a `geometry` rule has
     * already put this window exactly where the config asked. */
    window_park_float_if_fullscreen(mw);

    log_w(L"%ls: %p (desktop '%ls', float=%d, no_decor=%d, fullscreen=%d)",
          tier == ADOPT_TRACK ? L"Tracking" : L"Managed",
          (void *)hwnd, dt->name, mw->is_floating, mw->no_decor,
          mw->fullscreen);

    /* Last, once the window is fully set up, so a handler sees its final
     * state (desktop, floating, monitor) rather than a half-built record. */
    lua_fire(LUA_EVENT_WINDOW_OPEN, hwnd, NULL);
}

/* ===========================================================================
 * Promote a tracked window to full management.
 *
 * The window is already adopted — this flips the tier, re-resolving the rule
 * so no_decor/no_ring/floating land the way they would have had the window
 * been fully manageable when it arrived (Steam's main window, adopted while
 * its updater held it disabled, is the case this exists for). Placement is
 * deliberately NOT re-run: the window keeps the desktop, monitor and rect it
 * already has — promoting must not teleport a window you are looking at.
 * =========================================================================== */
void window_promote(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->tracked_only) return;

    mw->tracked_only = false;
    window_grant_full(mw, window_rule_lookup(hwnd));

    /* It may join the grid now — forget the rect bookkeeping the tracked tier
     * never maintained, so the next pass actually places it. */
    mw->has_applied = false;

    /* Same tail as window_manage: the tiler never places floating windows, so
     * a window that should fill its monitor is parked here. A no-op without a
     * fullscreen rule or mode, and promotion must not otherwise move a window
     * you are looking at. */
    window_park_float_if_fullscreen(mw);

    log_w(L"promoted to full management: %p (float=%d)", (void *)hwnd,
          mw->is_floating);
}

/* ===========================================================================
 * Unmanage a window — remove from WM control
 * =========================================================================== */
void window_unmanage(HWND hwnd) {
    int idx = window_index_of(hwnd);
    if (idx < 0) return;

    /* Fired FIRST, while the window is still managed, so a handler can still
     * ask which desktop it was on and what it was. By the time we return it is
     * gone from every list. The HWND may already be destroyed — the describe
     * path tolerates that and reports what it can. */
    lua_fire(LUA_EVENT_WINDOW_CLOSE, hwnd, NULL);

    /* Re-resolve: `idx` is used below to compact the managed array, and it was
     * read before the handler ran. Nothing the config can call from a handler
     * mutates that array today, but "today" is doing a lot of work in a line
     * that would otherwise corrupt memory. */
    idx = window_index_of(hwnd);
    if (idx < 0) return;

    int desk_id = g.managed[idx].desktop_id;

    window_restore_flat(hwnd);
    window_restore_decorations(hwnd);

    /* Hand back the topmost band if we took it — for fullscreen, for a pin, or
     * for a float. A window we stop managing while it is still alive would
     * otherwise stay pinned above everything with nothing left to put it back. */
    if (g.managed[idx].made_topmost && IsWindow(hwnd)) {
        g.managed[idx].made_topmost = false;
        window_set_band(hwnd, HWND_NOTOPMOST, false);
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
/* NB: the insets computed below are deliberately NOT cached on ManagedWindow.
 * It looks tempting — two cross-process calls per placed window per pass — but
 * a tiling pass is user-initiated, not per-frame, and flush_placements already
 * skips windows that haven't moved, so the real cost is well under a
 * millisecond on a pass a human just asked for. Against that, the cache would
 * have to be invalidated every time a window's frame changes behind our back,
 * which is precisely what games do (see window_reassert_rule) — and a stale
 * inset produces a wrong rect, which the drift detector then re-tiles with the
 * same stale inset. Bad trade: real geometry bugs to buy invisible time. */
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
 * Place a window, falling back to the privileged helper when refused.
 *
 * UIPI stops a lower-integrity process from moving a higher-integrity window,
 * so an unelevated mshell simply cannot place Task Manager or an elevated
 * terminal. Rather than work out in advance which windows those are — a process
 * open per window, on the busiest path, to answer a question the failing call
 * answers for free — the call is attempted and the refusal is what triggers the
 * fallback.
 *
 * With no mshelld.exe running this is exactly the old behaviour: the placement
 * fails and the window is left where it is.
 * =========================================================================== */
PlaceResult window_set_pos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
    if (SetWindowPos(hwnd, NULL, x, y, w, h, flags)) return PLACE_OK;

    DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        log_w(L"SetWindowPos(%p) failed: %lu", (void *)hwnd, err);
        return PLACE_REFUSED;
    }

    /* NB: which window needs the helper is deliberately NOT recorded here any
     * more. This function does the I/O and says what happened; window_apply_rect
     * owns the bookkeeping. Keeping the two apart is what lets the flag be
     * CLEARED again — it used to be set here and nowhere unset, so one transient
     * refusal pinned a window to the pipe for the rest of its life. */
    return helper_set_window_pos(hwnd, x, y, w, h, flags) ? PLACE_VIA_HELPER
                                                          : PLACE_REFUSED;
}

/* ===========================================================================
 * A window nobody can place does not get to keep a tile.
 *
 * UIPI stops an unelevated shell from moving a window owned by a
 * higher-integrity process, and without mshelld.exe running there is nothing
 * further to try. Such a window used to stay a full tiled client: the layout
 * kept handing it a cell it could never occupy, so the cell read as a hole and
 * the window sat wherever it opened, over its neighbours.
 *
 * Floating it is what the documentation has always claimed happens — see
 * helper.c's startup warning, proto.h's opening note and MANUAL-TESTS.md — and
 * it is the better behaviour anyway: the window is left alone, and the cell it
 * was never going to fill goes back to the windows that can use it.
 *
 * Once per window, not once per pass: `place_refused` is both the record that
 * this already happened and the guard against recursing, since floating a
 * window places it again and that placement is refused too.
 * =========================================================================== */
static void window_placement_refused(ManagedWindow *mw) {
    if (mw->place_refused) return;      /* already dealt with, and no recursion */
    mw->place_refused = true;

    if (!mw->is_floating && helper_available()) {
        /* The helper IS connected and still could not place it. Floating would
         * be the wrong lesson to draw — the boundary is not the problem here. */
        log_msg(LOG_WARN, L"placement refused for %p with mshelld.exe connected "
                          L"— leaving it in the layout", (void *)mw->hwnd);
        return;
    }

    if (!mw->is_floating && g.float_policy == FLOAT_NEVER) {
        /* The config said every window tiles. Honour it, and say what that
         * costs, because the symptom (a window sitting outside its cell) is
         * otherwise indistinguishable from a bug. */
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

/* ===========================================================================
 * Place a managed window and record the result. See the contract in mshell.h.
 * =========================================================================== */
PlaceResult window_apply_rect(ManagedWindow *mw, RECT want, UINT flags) {
    HWND hwnd = mw ? mw->hwnd : NULL;
    if (!hwnd || !IsWindow(hwnd)) return PLACE_REFUSED;

    RECT adj = window_adjust_for_frame(hwnd, want);
    PlaceResult res = window_set_pos(hwnd, adj.left, adj.top,
                                     adj.right - adj.left,
                                     adj.bottom - adj.top, flags);

    if (res == PLACE_REFUSED) {
        /* Record nothing. has_applied is left exactly as it was, so the next
         * pass tries again rather than skipping this window forever as
         * "already where we want it". */
        window_placement_refused(mw);
        return res;
    }

    mw->applied_rect  = want;
    mw->has_applied   = true;
    mw->place_refused = false;
    /* Set when the helper had to do it, cleared when the local call worked
     * again — an elevated window that closes and reopens unelevated, or one
     * whose refusal was transient, rejoins the batch on its own. */
    mw->needs_helper  = (res == PLACE_VIA_HELPER);
    return res;
}

/* ===========================================================================
 * Centre a floating window on its monitor.
 *
 * Where a floating window SITS is the one thing about it nobody owns. Its size
 * is the app's business and the layout never touches its rect, so left alone it
 * opens wherever that app last happened to be, or at the next step of Windows'
 * cascade — which on a shell with no taskbar and no desktop behind it reads as
 * "somewhere near the top left, for no reason". The window deliberately kept
 * out of the grid is also the one being looked at, so it goes in the middle.
 *
 * The monitor's WORK AREA, not its full bounds: the bar is reserved space, and
 * a centred window that slid under it would be centred against something the
 * user cannot see. The size is only ever clamped down to fit — this decides
 * where a float is, never how big it is.
 *
 * Safe to call on anything: it is a no-op for a tiled window, for one that
 * opted out, and for every state where an explicit rect is the wrong answer.
 * =========================================================================== */
void window_center_float(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->is_floating || !mw->center_float) return;

    /* Both states ignore the rect handed to SetWindowPos, and both are a
     * deliberate act by the user or the app. Restoring one just to centre it
     * is not what "put floating windows in the middle" asks for. */
    if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

    /* Never fight the fullscreen paths. A window parked over its monitor — by
     * rule, by keybinding, or by the app fullscreening itself — is already
     * exactly where it belongs, and centring it there is at best a no-op and at
     * worst undoes the park. */
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
    if (w > aw) w = aw;
    if (h > ah) h = ah;

    RECT want = { center_axis(area.left, aw, w), center_axis(area.top, ah, h),
                  0, 0 };
    want.right  = want.left + w;
    want.bottom = want.top  + h;

    /* Recorded as ours by window_apply_rect — but only if it actually took, so
     * the drift detector reads the LOCATIONCHANGE this causes as an echo rather
     * than as the app moving itself. */
    events_suppress_begin();
    window_apply_rect(mw, want, SWP_NOZORDER | SWP_NOACTIVATE);
    events_suppress_end();
}

/* ===========================================================================
 * Put a managed window back where it can be reached.
 *
 * A window can end up on no display at all without anybody moving it: the
 * monitor it lived on was unplugged, or a `geometry` rule was written for an
 * arrangement this machine no longer has. There is no taskbar under mshell, so
 * off every display means gone — no button to click, and for a floating window
 * not even a tiling pass that would put it back, because the tiler never places
 * floats. `has_applied = false` is what the hotplug path sets, and for a float
 * it is inert.
 *
 * CLAMPED rather than centred, unlike the startup stray sweep further down.
 * The sweep deals with one window at a time, at startup, whose position means
 * nothing (it was parked 4000px clear of the desktop by a dead mshell), so the
 * middle of the screen is the friendliest answer. This runs on every display
 * change and can rescue SEVERAL windows at once — centring would stack every
 * one of them in exactly the same spot, which is a worse outcome than the
 * stranding. Moving each the least distance that gets it back on screen keeps
 * them apart and roughly where they were, which is what a dock/undock cycle
 * wants.
 *
 * A no-op unless the window really is off every display, so callers do not have
 * to check. Returns true when it moved something.
 * =========================================================================== */
bool window_rescue_offscreen(ManagedWindow *mw) {
    if (!mw || !IsWindow(mw->hwnd)) return false;
    /* An iconic window has no on-screen rect to judge, and moving one only
     * edits the rect it will restore to. */
    if (IsIconic(mw->hwnd)) return false;

    RECT cur;
    if (!window_frame_rect(mw->hwnd, &cur)) return false;
    if (!rect_off_screen(cur)) return false;

    int mon = mw->monitor;
    if (mon < 0 || mon >= g.monitor_count) mon = g.primary_monitor;
    RECT area = (mon >= 0 && mon < g.monitor_count) ? g.monitors[mon].work_area
                                                    : g.work_area;

    int aw = (int)(area.right - area.left);
    int ah = (int)(area.bottom - area.top);
    int w  = (int)(cur.right - cur.left);
    int h  = (int)(cur.bottom - cur.top);
    if (aw <= 0 || ah <= 0 || w <= 0 || h <= 0) return false;
    if (w > aw) w = aw;
    if (h > ah) h = ah;

    RECT want = { clamp_axis(area.left, aw, cur.left, w),
                  clamp_axis(area.top,  ah, cur.top,  h), 0, 0 };
    want.right  = want.left + w;
    want.bottom = want.top  + h;

    log_msg(LOG_INFO, L"rescued %p from off-screen onto monitor %d",
            (void *)mw->hwnd, mon);

    events_suppress_begin();
    window_apply_rect(mw, want,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();
    return true;
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

    window_apply_rect(mw, want,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    events_suppress_end();
}

/* Rule-driven variant: only for a floating window whose rule asked for it, so
 * a tiled window still belongs to the layout (and FLOAT_NEVER still wins). */
void window_apply_fullscreen(HWND hwnd) {
    ManagedWindow *mw = window_find(hwnd);
    if (!mw || !mw->fullscreen || !mw->is_floating) return;
    window_park_over_monitor(hwnd);
}

/* ===========================================================================
 * Where does a floating window sit?
 *
 * The single answer, because there are two of them and they used to be asked
 * as two independent questions that each no-opped when the other applied:
 * window_apply_fullscreen (which needs the RULE flag) followed by
 * window_center_float (which bails on any kind of fullscreen). A window
 * fullscreened from the KEYBOARD while tiled satisfies neither — fs_mode says
 * FS_WINDOW but mw->fullscreen is false — so floating one did nothing at all
 * and left it sitting at the tile it no longer owned, with every flag claiming
 * it was a fullscreen float. Pressing fullscreen again then restored a rect
 * that had never been saved, which is to say it did nothing either.
 *
 * Asking once, in order of who outranks whom, is the whole fix.
 * =========================================================================== */
/* The parking half on its own, because one caller wants it WITHOUT the
 * centring: a window whose rule gave it an explicit `geometry` has already been
 * put exactly where the config asked, and centring it afterwards would throw
 * that away.
 *
 * Two independent things can ask for a screen-filling float and only one of
 * them used to be honoured here. `fullscreen = true` is a rule and is permanent
 * — there is nothing to restore, so nothing is saved. A fullscreen MODE is the
 * keyboard state machine, and `start_fullscreen` puts a window into FS_WINDOW
 * at manage time without setting the rule flag at all; that window was never
 * parked by anyone, because the tiler skips floats and this path only asked
 * about the rule. */
static void window_park_float_if_fullscreen(ManagedWindow *mw) {
    if (!mw || !mw->is_floating) return;
    if (!mw->fullscreen && !window_is_screen_fullscreen(mw)) return;

    /* Save what it had before we take the whole monitor, or leaving fullscreen
     * has nothing to go back to. Guarded, so arriving here already parked does
     * not overwrite the user's rect with our own. Not saved for the rule case:
     * that window is fullscreen for as long as it lives. */
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

    window_center_float(mw->hwnd);   /* no-op unless it should be centred */
}

/* A saved pre-fullscreen rect belongs to the float it was saved from. A window
 * that leaves the floating tier must forget it: the layout owns a tiled
 * window's geometry, so the rect is meaningless there, and keeping it made the
 * NEXT float refuse to save its own (window_set_fullscreen only saves when
 * nothing is saved) and then restore a rect from two arrangements ago. */
static void fs_forget_prev(ManagedWindow *mw) {
    if (mw) mw->fs_has_prev = false;
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

    RECT p = mw->fs_prev_rect;

    events_suppress_begin();
    window_apply_rect(mw, p,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    events_suppress_end();

    mw->fs_has_prev = false;
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

/* Does this window belong in the topmost band, and if so why? Three
 * independent reasons, and the demotion pass has to wait for all of them to
 * lapse. Floats are here only while float_on_top asks for it. */
static bool zorder_wants_topmost(const ManagedWindow *mw) {
    return window_is_screen_fullscreen(mw) || mw->always_on_top ||
           (g.float_on_top && window_is_float_tier(mw));
}

/* Move a window between the ordinary and the topmost band.
 *
 * `after` is what the local call wants: HWND_TOPMOST / HWND_NOTOPMOST, or
 * another window to sit directly beneath when the floats are being chained in
 * order. `topmost` says which band that amounts to, because the helper fallback
 * cannot be told about the chain — it takes a band and nothing else (see
 * proto.h).
 *
 * Same shape as window_set_pos: try locally, and forward to mshelld only on the
 * refusal that means UIPI. Without this an elevated window — Task Manager is
 * the one everybody meets — was the single float that stayed buriable, because
 * every SetWindowPos we aimed at it failed silently. With no helper running,
 * that is still the outcome; helper_set_topmost says so once. */
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

/* Record that WE promoted this window, so the demotion pass knows it may take
 * the band back. A window already topmost on its own account is left flagged
 * false — demoting that one would break its app's own choice. */
static void zorder_mark_promoted(ManagedWindow *mw) {
    if (!mw->made_topmost &&
        !(GetWindowLongPtrW(mw->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
        mw->made_topmost = true;
}

/* The two kinds of window that outrank a float inside the topmost band:
 * one covering its whole monitor (an overlay stranded in the middle of a
 * fullscreen video is exactly what fullscreen is meant to end), and one the
 * user explicitly pinned. Raised after the floats, so they end up above them.
 * Fullscreen beats mshell's own surfaces too — the bar included — which is why
 * this runs after overlay_raise_all(). */
static void zorder_raise_over_floats(void) {
    Desktop *dt = desktop_current();

    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw || !IsWindow(mw->hwnd)) continue;
        if (!window_is_screen_fullscreen(mw) && !mw->always_on_top) continue;
        if (!window_on_screen(mw)) continue;

        zorder_mark_promoted(mw);
        window_set_band(mw->hwnd, HWND_TOPMOST, true);
    }
}

/* ===========================================================================
 * Raise the floating windows of the current desktop above everything else, so
 * they read as overlays instead of sinking behind a tiled window.
 *
 * Its own entry point rather than part of window_enforce_zorder() because a
 * tiling pass is not the only thing that disturbs the order — and not even the
 * common one. Activating a window raises it, so focusing a *tiled* window, by
 * keybind or by click, drops every float behind it with no tiling pass in
 * sight. Every focus change therefore has to re-assert this.
 *
 * The floats go into the TOPMOST band, not merely to the top of the ordinary
 * one. HWND_TOP was the old answer and it cannot hold: Windows sorts every
 * WS_EX_TOPMOST window above every non-topmost one, and — worse — a
 * re-assertion only ever happens on an event we hear about. A window that
 * raises itself after its activation event (apps do, on WM_ACTIVATE and when a
 * child takes focus), or an unmanaged window activating (which the foreground
 * handler filters out), buried the float with nothing left to fix it. Sitting
 * in the topmost band is the only "on top no matter what" Windows offers: the
 * OS keeps it there with no event of ours in the loop.
 *
 * Ranking inside that band is then ours to arrange, and the tail of this
 * function does it: our own overlays above the floats, fullscreen above both.
 * =========================================================================== */
void window_raise_floats(void) {
    if (!g.float_on_top) { zorder_raise_over_floats(); return; }

    HWND floats[MAX_WINDOWS_PER_DESKTOP];
    int  n = 0;

    /* Walk the system z-order (top first) rather than Desktop.windows[], so a
     * pass preserves how the floats are already stacked against each other.
     * Raising them in desktop order would re-shuffle two overlapping floats on
     * every focus change, which is a worse tic than the bug being fixed. */
    for (HWND h = GetTopWindow(NULL); h && n < MAX_WINDOWS_PER_DESKTOP;
         h = GetWindow(h, GW_HWNDNEXT)) {
        ManagedWindow *mw = window_find(h);
        /* The TIER, not the exemption: a tracked window is floating only in the
         * sense that the layout leaves it alone, and putting one in the topmost
         * band is doing something to a window the tier promises not to touch. */
        if (!window_is_float_tier(mw)) continue;
        if (mw->desktop_id != g.current_desktop_id) continue;
        /* window_on_screen, not IsWindowVisible: a window mshell has cloaked —
         * a stowed scratchpad is the floating case — keeps its visible bit, so
         * IsWindowVisible would hand it a place in the chain and leave an
         * invisible window sitting above real ones. */
        if (!window_on_screen(mw) || IsIconic(h)) continue;
        floats[n++] = h;
    }
    if (n == 0) { zorder_raise_over_floats(); return; }

    /* Among the floats, the focused one belongs on top — it is the window the
     * user just asked to look at. */
    HWND focused = desktop_get_focused();
    for (int i = 1; i < n; i++) {
        if (floats[i] != focused) continue;
        memmove(&floats[1], &floats[0], (size_t)i * sizeof floats[0]);
        floats[0] = focused;
        break;
    }

    /* Chain them: the first goes to the top of the topmost band, each of the
     * rest directly under its predecessor. Inserting after a topmost window
     * makes the inserted window topmost too, so one HWND_TOPMOST at the head
     * carries the whole chain up while keeping the order intact. */
    HWND after = HWND_TOPMOST;
    for (int i = 0; i < n; i++) {
        ManagedWindow *mw = window_find(floats[i]);
        if (mw) zorder_mark_promoted(mw);
        /* An elevated float goes up through the helper, which knows only the
         * band, so it lands at the top of it rather than under its predecessor
         * — the chain's order is approximate for those. That is a nicety;
         * being coverable at all was the bug. A float that could not be moved
         * (no helper) is skipped as the anchor: chaining the next one under a
         * window that never went up would drag it down with it. */
        if (!window_set_band(floats[i], after, true)) continue;
        after = floats[i];
    }

    /* The band is shared with mshell's own surfaces, which the chain above just
     * jumped over: the status bar is furniture, not a window a float may bury.
     * Then the windows that outrank a float go back over everything. */
    overlay_raise_all();
    zorder_raise_over_floats();
}

/* ===========================================================================
 * Enforce a sane z-order after a tiling pass:
 *   - the solid backdrop stays pinned to the very bottom so it can never rise
 *     up and black out the tiled windows;
 *   - optionally (float_on_top) floating windows go into the topmost band,
 *     under mshell's own surfaces;
 *   - fullscreen and always-on-top windows go into the topmost band, over both;
 *   - anything of ours left up there without a reason comes back down.
 * Tiled windows never overlap each other, so their relative order is moot.
 * =========================================================================== */
void window_enforce_zorder(void) {
    if (g.background_window)
        SetWindowPos(g.background_window, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    Desktop *dt = desktop_current();

    /* Everything that goes UP happens in here: the floats, then our overlays,
     * then the fullscreen and pinned windows over both. */
    window_raise_floats();

    /* What is left is giving the band back. A window we promoted and that has
     * since run out of reasons to be up there — it stopped floating, left
     * fullscreen, was unpinned, or went off-screen onto another desktop — is
     * demoted here, and only here.
     *
     * made_topmost records whether WE promoted it, so a window that was topmost
     * on its own account is never demoted out from under its app. */
    for (int i = 0; i < dt->count; i++) {
        ManagedWindow *mw = window_find(dt->windows[i]);
        if (!mw || !IsWindow(mw->hwnd)) continue;
        if (zorder_wants_topmost(mw) && window_on_screen(mw)) continue;
        if (!mw->made_topmost) continue;

        mw->made_topmost = false;
        window_set_band(mw->hwnd, HWND_NOTOPMOST, false);
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

    /* The refusal record belongs to the arrangement it happened in. A float
     * whose centring was refused must not carry that forward and rob itself of
     * the "cannot be tiled, so float it" answer when it is next put in the
     * grid — that answer is exactly what this transition may need. */
    mw->place_refused = false;

    /* A floating window is not in the layout, so it cannot be layout-hidden.
     * Saying so here is not tidiness: the tiler skips floating windows on BOTH
     * the hide and the show side, so a window that monocle was holding back
     * when it started floating would keep the flag with nothing left that ever
     * clears it — off the screen for good. Being off-screen because it lives on
     * another desktop is a different question, and is left alone. */
    if (mw->layout_hidden) {
        mw->layout_hidden = false;
        if (floating && mw->desktop_id == g.current_desktop_id) {
            events_suppress_begin();
            window_show(mw);
            events_suppress_end();
        }
    }
    if (floating) {
        /* Give the frame back — unless the rule says this window is meant to
         * stay borderless, in which case floating it must not re-decorate it. */
        if (mw->no_decor) window_strip_decorations(hwnd);
        else              window_restore_decorations(hwnd);
        /* A window leaving the grid keeps the size of the tile it just left,
         * which is a fine size and a meaningless position — the tile is about
         * to be given away to the windows that stayed. Where it goes instead is
         * one question with one answer; see window_place_float. */
        window_place_float(mw);
    } else {
        window_strip_decorations(hwnd);

        /* Leaving the float tier: the pre-fullscreen rect it was holding
         * describes an arrangement that no longer exists. */
        fs_forget_prev(mw);

        /* Back into the grid, so out of the topmost band we put it in — right
         * now, not at the next tiling pass. The window is about to be given a
         * tile, and one frame of it hanging over its new neighbours is exactly
         * the flicker the band is meant to avoid. Only if WE promoted it, and
         * not while another reason (fullscreen, pinned) still holds. */
        if (mw->made_topmost && !window_is_screen_fullscreen(mw) &&
            !mw->always_on_top) {
            mw->made_topmost = false;
            window_set_band(hwnd, HWND_NOTOPMOST, false);
        }
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

        /* Looking at it is what "attention given" means. */
        if (mw) mw->urgent = false;
    }

    /* Activation just raised this window to the top of its band. If it is a
     * tiled one, that put it over any float it overlaps, so put the floats
     * back — before the ring is placed, which stacks itself at HWND_TOP. */
    window_raise_floats();

    /* Keep the focus ring on the newly-focused window. */
    border_refresh();
    bar_refresh();   /* the title section follows the focus */

    /* Only when the focus actually moved. window_focus is also called to
     * re-assert focus that is already correct — after a re-tile, on a desktop
     * switch that lands on the same window — and firing on those would make a
     * handler see a stream of events for something that never changed. */
    {
        static HWND s_last_fired = NULL;
        if (hwnd != s_last_fired) {
            s_last_fired = hwnd;
            lua_fire(LUA_EVENT_FOCUS, hwnd, NULL);
        }
    }

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
 * Focus nothing — park the foreground on the backdrop.
 *
 * Needed because cloaking, unlike ShowWindow(SW_HIDE), does not disturb the
 * foreground at all: hiding a window makes Windows hand the foreground to
 * somebody else, cloaking one leaves it exactly where it was. So switching to
 * an EMPTY desktop used to defocus by accident and now would not — the window
 * you just left would keep the keyboard while being invisible, and you would
 * type into it without seeing a thing.
 *
 * The backdrop is the natural sink: it already covers the whole virtual screen
 * and is already pinned to the bottom of the z-order, so activating it is
 * visually a no-op. WS_EX_NOACTIVATE is the only obstacle, and it is there to
 * stop a click in a gap between tiles from stealing focus — a different
 * question from whether WE may hand it the foreground deliberately. Lifting it
 * for the length of this one call answers both correctly.
 *
 * Windows' own virtual desktops do the same thing, parking the foreground on
 * the shell's desktop window when you switch to a desktop with nothing on it.
 * =========================================================================== */
void window_focus_none(void) {
    /* First, unconditionally: there is no focused window, so there is no ring.
     * This is what every caller here used to do on its own, and it must happen
     * whatever the foreground turns out to be. */
    border_hide();

    HWND sink = g.background_window;
    if (!sink) return;

    HWND fg = GetForegroundWindow();
    if (fg == sink) return;                     /* already nowhere */
    /* The foreground is a window we do not manage — one of our own overlays, or
     * something outside mshell's control. Leave it: we are here to stop a
     * window WE hid from keeping the keyboard, not to seize the foreground from
     * whatever else happens to hold it. */
    if (fg && !window_find(fg)) return;

    LONG_PTR ex = GetWindowLongPtrW(sink, GWL_EXSTYLE);
    SetWindowLongPtrW(sink, GWL_EXSTYLE, ex & ~(LONG_PTR)WS_EX_NOACTIVATE);

    claim_foreground_rights();
    SetForegroundWindow(sink);

    SetWindowLongPtrW(sink, GWL_EXSTYLE, ex);
}

/* ===========================================================================
 * Close a window gracefully (WM_CLOSE)
 * =========================================================================== */
void window_close(HWND hwnd) {
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) return;

    /* UIPI refuses to carry even WM_CLOSE into a higher-integrity process —
     * the helper runs on the other side of that boundary. */
    if (GetLastError() == ERROR_ACCESS_DENIED) helper_close_window(hwnd);
    else log_w(L"close: PostMessage(WM_CLOSE) on %p failed: %lu",
               (void *)hwnd, GetLastError());
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
 * Undo cloaking left behind by a previous mshell that did not shut down.
 *
 * Hiding a desktop's windows by cloaking them means a crash — or a kill that
 * never reaches the crash handler — can leave windows cloaked with nobody to
 * uncloak them. That is a window the user cannot get back: it is not in
 * Alt+Tab (Windows excludes cloaked windows, which is how its own virtual
 * desktops work), and is_manageable rejects cloaked windows, so the fresh
 * mshell coming up behind the dead one would not adopt it either. Winlogon's
 * AutoRestartShell makes that restart the normal case, so this runs first,
 * before the enumeration that would otherwise skip them.
 *
 * Only DWM_CLOAKED_SHELL is touched. That is what a cloak applied from outside
 * the owning process reads back as — i.e. ours. DWM_CLOAKED_APP is the app
 * hiding itself (a suspended store app) and DWM_CLOAKED_INHERITED is a window
 * following its owner; both belong to somebody else.
 *
 * Skipped in --test mode, and this is the whole reason for the flag here:
 * explorer is alive there, and Windows' own virtual desktops cloak the windows
 * of every desktop you are not on with exactly the same DWM_CLOAKED_SHELL.
 * Sweeping would haul them all onto the desktop you are looking at.
 * =========================================================================== */
static BOOL CALLBACK uncloak_stray_proc(HWND hwnd, LPARAM lp) {
    int *n = (int *)lp;

    /* Cheap rejections first: the sweep visits every top-level window on the
     * system, and DwmGetWindowAttribute is an RPC into dwm.exe. */
    if (!IsWindowVisible(hwnd)) return TRUE;

    /* A window a dead mshell left STASHED is the same lost window as a cloaked
     * one, and worse to find: it is visible, so nothing about it looks wrong
     * except that it is nowhere. Pull anything sitting entirely off every
     * display back onto the primary work area, and let the tiling pass that
     * follows adoption put it where it belongs.
     *
     * Gated on window_is_manageable so this only ever touches windows mshell
     * would have stashed in the first place: an app is perfectly entitled to
     * keep a visible window off-screen (that IS how some of them hide things),
     * and one that mshell would never manage is none of our business. */
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

void window_uncloak_strays(void) {
    if (g.test_mode) return;

    int n = 0;
    EnumWindows(uncloak_stray_proc, (LPARAM)&n);
    if (n) log_err(L"startup: recovered %d window(s) a previous mshell left "
                   L"hidden — cloaked, or stashed off every display", n);
}

/* ===========================================================================
 * Enumerate & manage all existing windows at startup
 * =========================================================================== */
static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lp) {
    (void)lp;
    /* window_manage() runs the manageability test itself, so testing here too
     * just paid for every check — including the process query — twice per
     * window on the system. */
    window_manage(hwnd);
    return TRUE;
}

void window_manage_existing(void) {
    window_uncloak_strays();
    EnumWindows(enum_windows_proc, 0);
}
