/* ===========================================================================
 * system.c — session/power actions and media keys.
 *
 * Two things a shell is expected to provide that mshell had no answer for.
 *
 * Session actions exist because replacing Explorer removes every route to them:
 * there is no Start menu to pick "Shut down" from, and `quit` is not a
 * substitute — as the shell, exiting ends the session, which looks like a
 * logoff whatever you meant by it. Being explicit about lock / logoff / reboot
 * / shutdown / sleep means the destructive ones are things you asked for rather
 * than things that happened.
 *
 * Media keys go the other way. A keyboard with dedicated volume keys already
 * works — Windows handles those below our hook — but a keyboard without them
 * had no way to reach volume at all, because every Win+key combination belongs
 * to mshell. These actions synthesise the same virtual keys, so any binding can
 * drive them, and the OS's own handling (including its on-screen indicator)
 * does the rest. Synthesising rather than calling IAudioEndpointVolume directly
 * is what keeps that indicator, and keeps per-app volume behaving normally.
 *
 * Note this file is NOT session.c, which is persistence of per-desktop settings
 * across restarts and has nothing to do with the Windows session.
 * =========================================================================== */
#include "mshell.h"

#include <powrprof.h>

/* ---------------------------------------------------------------------------
 * Shutdown privilege
 *
 * ExitWindowsEx fails with ERROR_PRIVILEGE_NOT_HELD unless SE_SHUTDOWN_NAME is
 * enabled in the process token. It is present-but-disabled for an ordinary
 * interactive user, so this is an enable rather than an escalation — nothing
 * here needs mshell to be elevated.
 * --------------------------------------------------------------------------- */
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
        /* AdjustTokenPrivileges reports success even when it changed nothing,
         * so the real answer is in GetLastError. */
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(tok);
    return ok;
}

/* EWX_FORCEIFHUNG rather than EWX_FORCE: an app that is merely slow to close
 * still gets to save, while one that has stopped answering cannot veto the
 * whole thing. A bare ExitWindowsEx can be silently cancelled by any app, which
 * from a shell with no taskbar looks exactly like the keybinding not working. */
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

/* SetSuspendState's first argument picks hibernate over sleep. The second
 * (bForce) is documented as having no effect on modern Windows, and the third
 * disables wake events, which is not what anyone binding a sleep key wants. */
static void do_suspend(BOOLEAN hibernate, const wchar_t *what) {
    log_msg(LOG_INFO, L"session: %ls", what);
    if (!SetSuspendState(hibernate, FALSE, FALSE))
        log_err(L"session: %ls FAILED: %lu", what, GetLastError());
}

void system_sleep(void)     { do_suspend(FALSE, L"sleep");     }
void system_hibernate(void) { do_suspend(TRUE,  L"hibernate"); }

/* ---------------------------------------------------------------------------
 * Media / volume keys
 *
 * Tagged with MSHELL_INPUT_TAG so our own hook passes them straight through
 * instead of running them past the keymaps — the same marker window_focus()
 * uses. Without it a config that bound VK_VOLUME_UP to something would have
 * these recurse into it.
 * --------------------------------------------------------------------------- */
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
