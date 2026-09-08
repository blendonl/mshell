#include "mshell.h"
#include "window_internal.h"

void window_apply_flat(HWND hwnd) {
    DWORD corner = (DWORD)g.cfg.corner_pref;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));
    BOOL disable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED,
                          &disable, sizeof(disable));
}

void window_restore_flat(HWND hwnd) {
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

void window_claim_saved_frame(ManagedWindow *mw) {
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
