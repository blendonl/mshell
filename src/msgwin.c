#include "mshell.h"
#include "msgwin.h"
#include <wtsapi32.h>

static int s_resink_left;

static int s_spi_broadcast_depth;

BOOL spi_set_broadcast(UINT action, UINT ui_param, PVOID pv_param) {
    s_spi_broadcast_depth++;
    BOOL ok = SystemParametersInfoW(action, ui_param, pv_param, SPIF_SENDCHANGE);
    if (s_spi_broadcast_depth > 0) s_spi_broadcast_depth--;
    return ok;
}

static bool setting_change_affects_layout(WPARAM wp, LPARAM lp) {
    if (s_spi_broadcast_depth > 0) return false;

    switch (wp) {
    case SPI_SETWORKAREA:
    case SPI_SETNONCLIENTMETRICS:
        return true;
    default:
        break;
    }

    if (wp != 0) return false;

    const wchar_t *area = (const wchar_t *)lp;
    return area && _wcsicmp(area, L"WindowMetrics") == 0;
}

LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MSHELL_ACTION: {
        Action  action;
        int     arg;
        wchar_t cmd[MAX_PATH];
        wchar_t args[SPAWN_ARGS_MAX];
        wchar_t cwd[MAX_PATH];
        int     count = 0;

        if (kb_take_pending((unsigned)lp, &action, &arg,
                            cmd, MAX_PATH, args, SPAWN_ARGS_MAX,
                            cwd, MAX_PATH, &count)) {
            log_w(L"hook match: vk=0x%02X mods=0x%X -> action=%d",
                  (unsigned)(wp & 0xFFFF), (unsigned)((wp >> 16) & 0xFFFF),
                  (int)action);
            int reps = (count > 1 && action_is_repeatable(action)) ? count : 1;
            for (int i = 0; i < reps; i++)
                execute_action(action, arg, cmd[0] ? cmd : NULL,
                               args[0] ? args : NULL,
                               cwd[0] ? cwd : NULL);
        }
        return 0;
    }

    case WM_MSHELL_SUBMAP:
        whichkey_notify();
        return 0;

    case WM_MSHELL_IPC:
        ipc_handle_request((void *)lp);
        return 0;

    case WM_MSHELL_CONFIG_CHANGED:
        config_on_file_changed((unsigned)wp);
        return 0;

    case WM_DISPLAYCHANGE:
        update_work_area();
        displays_apply_rules(false);
        desktop_monitors_changed();
        background_update();
        bar_reconfigure();
        tile_current();
        s_resink_left = RESINK_RETRIES;
        SetTimer(hwnd, TIMER_RESINK, RESINK_RETRY_MS, NULL);
        return 0;

    case WM_SETTINGCHANGE:
        if (!setting_change_affects_layout(wp, lp)) {
            const wchar_t *area = (wp == 0 && lp) ? (const wchar_t *)lp : L"";
            log_w(L"settings: ignoring WM_SETTINGCHANGE wParam=%u area='%ls'%ls",
                  (unsigned)wp, area,
                  s_spi_broadcast_depth > 0 ? L" (our own broadcast)" : L"");
            return 0;
        }
        update_work_area();
        desktop_monitors_changed();
        background_update();
        bar_reconfigure();
        tile_current();
        return 0;

    case WM_DPICHANGED:
        update_work_area();
        background_update();
        whichkey_hide();
        bar_reconfigure();
        tile_current();
        return 0;

    case WM_MSHELL_UPDATE: {
        wchar_t *msg = (wchar_t *)lp;
        if (msg) {
            NotifyKind kind = (NotifyKind)LOWORD(wp);
            int        ms   = (int)HIWORD(wp);
            notify_show(msg, kind, ms > 0 ? ms : 15000);
            free(msg);
        }
        return 0;
    }

    case WM_MSHELL_RESTART:
        log_w(L"update: handing over to the build just installed");
        g.running = false;
        PostQuitMessage(0);
        return 0;

    case WM_MSHELL_CAPTURE_KEY:
        launcher_key((DWORD)wp, (wchar_t)lp);
        return 0;

    case WM_MSHELL_MOUSE:
        mouse_mod_drag_apply((int)(LONG)wp, (int)(LONG)lp);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_FOLLOW_MOUSE) { mouse_poll_focus();  return 0; }
        if (wp == TIMER_SINK_VERIFY) {
            window_verify_visibility();
            window_verify_placement();
            window_verify_sink();
            return 0;
        }
        if (wp == TIMER_ANIM)          { anim_tick();        return 0; }
        if (wp == TIMER_RESINK) {
            update_work_area();
            background_update();
            bar_reconfigure();
            tile_current();
            window_resink();
            if (--s_resink_left <= 0) KillTimer(hwnd, TIMER_RESINK);
            return 0;
        }
        if (wp == TIMER_CRASHLOOP_HEALTHY) {
            KillTimer(hwnd, TIMER_CRASHLOOP_HEALTHY);
            crashloop_mark_healthy();
            return 0;
        }
        break;

    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_UNLOCK || wp == WTS_SESSION_LOGON ||
            wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT)
            kb_reset_state();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {
            g.running = false;
            PostQuitMessage(0);
        }
        return 0;

    case WM_CLOSE:
        g.running = false;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

HWND create_message_window(HINSTANCE hinst) {
    const wchar_t *class_name = L"mshell_MessageWindow";

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MessageWndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    return CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"mshell", WS_POPUP,
                           0, 0, 0, 0, NULL, NULL, hinst, NULL);
}
