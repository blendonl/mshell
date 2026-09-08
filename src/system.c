#include "mshell.h"

#include <powrprof.h>

static bool enable_shutdown_privilege(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    bool ok = LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME,
                                    &tp.Privileges[0].Luid) != 0;
    if (ok) {
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(tok);
    return ok;
}

static void do_exit_windows(UINT flags, const wchar_t *what) {
    if (!enable_shutdown_privilege())
        log_msg(LOG_WARN, L"%ls: could not enable SE_SHUTDOWN_NAME — "
                          L"the request will probably be refused", what);

    log_msg(LOG_INFO, L"session: %ls requested", what);
    if (!ExitWindowsEx(flags | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER |
                                                SHTDN_REASON_MINOR_OTHER |
                                                SHTDN_REASON_FLAG_PLANNED))
        log_err(L"session: %ls FAILED: %lu", what, GetLastError());
}

void system_lock(void) {
    log_msg(LOG_INFO, L"session: lock");
    if (!LockWorkStation())
        log_err(L"session: lock FAILED: %lu (DisableLockWorkstation policy?)",
                GetLastError());
}

void system_logoff(void)   { do_exit_windows(EWX_LOGOFF,   L"log off");  }
void system_reboot(void)   { do_exit_windows(EWX_REBOOT,   L"reboot");   }
void system_shutdown(void) { do_exit_windows(EWX_SHUTDOWN | EWX_POWEROFF,
                                             L"shut down"); }

static void do_suspend(BOOLEAN hibernate, const wchar_t *what) {
    log_msg(LOG_INFO, L"session: %ls", what);
    if (!SetSuspendState(hibernate, FALSE, FALSE))
        log_err(L"session: %ls FAILED: %lu", what, GetLastError());
}

void system_sleep(void)     { do_suspend(FALSE, L"sleep");     }
void system_hibernate(void) { do_suspend(TRUE,  L"hibernate"); }

static void tap_vk(WORD vk) {
    INPUT in[2];
    memset(in, 0, sizeof(in));

    in[0].type           = INPUT_KEYBOARD;
    in[0].ki.wVk         = vk;
    in[0].ki.dwExtraInfo = MSHELL_INPUT_TAG;

    in[1]                = in[0];
    in[1].ki.dwFlags     = KEYEVENTF_KEYUP;

    SendInput(2, in, sizeof(INPUT));
}

void system_media_key(Action action) {
    switch (action) {
    case ACTION_VOLUME_UP:     tap_vk(VK_VOLUME_UP);        break;
    case ACTION_VOLUME_DOWN:   tap_vk(VK_VOLUME_DOWN);      break;
    case ACTION_VOLUME_MUTE:   tap_vk(VK_VOLUME_MUTE);      break;
    case ACTION_MEDIA_PLAY:    tap_vk(VK_MEDIA_PLAY_PAUSE); break;
    case ACTION_MEDIA_NEXT:    tap_vk(VK_MEDIA_NEXT_TRACK); break;
    case ACTION_MEDIA_PREV:    tap_vk(VK_MEDIA_PREV_TRACK); break;
    case ACTION_MEDIA_STOP:    tap_vk(VK_MEDIA_STOP);       break;
    default: break;
    }
}
