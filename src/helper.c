#include "mshell.h"
#include "proto.h"

#define HELPER_IO_TIMEOUT_MS   250
#define HELPER_FAIL_LIMIT      3
#define HELPER_BACKOFF_MS      5000

static CRITICAL_SECTION g_cs;
static INIT_ONCE        g_cs_once = INIT_ONCE_STATIC_INIT;

static HANDLE    g_pipe = INVALID_HANDLE_VALUE;
static HANDLE    g_event;
static bool      g_tried;
static int       g_timeouts;
static ULONGLONG g_blocked_until;

static BOOL CALLBACK helper_cs_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_cs);
    return TRUE;
}

static void helper_lock(void) {
    InitOnceExecuteOnce(&g_cs_once, helper_cs_init, NULL, NULL);
    EnterCriticalSection(&g_cs);
}

static void helper_unlock(void) {
    LeaveCriticalSection(&g_cs);
}

static bool helper_backed_off(void) {
    if (!g_blocked_until) return false;

    if (GetTickCount64() < g_blocked_until) return true;

    g_blocked_until = 0;
    g_timeouts      = 0;
    log_msg(LOG_INFO, L"helper: trying mshelld.exe again");
    return false;
}

static void helper_note_timeout(void) {
    if (++g_timeouts < HELPER_FAIL_LIMIT) return;

    g_blocked_until = GetTickCount64() + HELPER_BACKOFF_MS;
    log_err(L"helper: mshelld.exe accepted a connection but stopped answering "
            L"(%d timeouts at %d ms). Not asking again for %d seconds — "
            L"elevated windows will float and stay put until it recovers, "
            L"which is the same as running without the helper.",
            g_timeouts, HELPER_IO_TIMEOUT_MS, HELPER_BACKOFF_MS / 1000);
}

static void helper_pipe_name(wchar_t *out, size_t cap) {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    _snwprintf(out, cap, L"%ls%lu", MSHELLD_PIPE_PREFIX, (unsigned long)sid);
    out[cap - 1] = L'\0';
}

static void helper_disconnect(void) {
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

static int helper_io(void *buf, DWORD len, bool write) {
    OVERLAPPED ov = {0};
    ov.hEvent = g_event;
    ResetEvent(g_event);

    BOOL ok = write ? WriteFile(g_pipe, buf, len, NULL, &ov)
                    : ReadFile(g_pipe, buf, len, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return 0;

    DWORD n = 0;
    if (WaitForSingleObject(g_event, HELPER_IO_TIMEOUT_MS) != WAIT_OBJECT_0) {
        CancelIoEx(g_pipe, &ov);
        GetOverlappedResult(g_pipe, &ov, &n, TRUE);
        return -1;
    }

    if (!GetOverlappedResult(g_pipe, &ov, &n, FALSE)) return 0;
    return (n == len) ? 1 : 0;
}

static bool helper_connect(void) {
    if (g_pipe != INVALID_HANDLE_VALUE) return true;
    if (helper_backed_off()) return false;

    wchar_t name[MAX_PATH];
    helper_pipe_name(name, MAX_PATH);

    if (!WaitNamedPipeW(name, 1)) return false;

    HANDLE p = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (p == INVALID_HANDLE_VALUE) return false;

    if (!g_event) {
        g_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_event) {
            CloseHandle(p);
            log_err(L"helper: CreateEvent failed: %lu — the helper cannot be "
                    L"used safely without a way to time its I/O out",
                    GetLastError());
            return false;
        }
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(p, &mode, NULL, NULL);

    g_pipe = p;

    ProtoMsg hello = { .type = PROTO_HELLO, .version = MSHELLD_PROTO_VERSION };
    ProtoMsg reply = {0};

    if (helper_io(&hello, sizeof hello, true) != 1 ||
        helper_io(&reply, sizeof reply, false) != 1 ||
        reply.type != PROTO_OK) {
        helper_disconnect();
        log_err(L"helper: mshelld.exe did not complete the handshake — are the "
                L"two binaries from the same build, and is it responding?");
        return false;
    }

    log_err(L"helper: connected to mshelld.exe — windows owned by elevated "
            L"processes can now be tiled, hidden and closed");
    return true;
}

static void helper_open(void) {
    g_tried = true;
    if (!helper_connect())
        log_w(L"helper: mshelld.exe is not running. Windows owned by elevated "
              L"processes will float instead of tiling and will stay on every "
              L"desktop. Run `install.bat /helper` from an administrator "
              L"prompt to change that. Cloaking is NOT among the things the "
              L"helper fixes: DWMWA_CLOAK is owner-only for every process, and "
              L"the shell cloak needs an immersive shell that only explorer.exe "
              L"provides, so hiding sinks the window under the backdrop "
              L"regardless (see window_hide).");
}

void helper_init(void) {
    helper_lock();
    helper_open();
    helper_unlock();
}

void helper_shutdown(void) {
    helper_lock();
    helper_disconnect();
    if (g_event) { CloseHandle(g_event); g_event = NULL; }
    helper_unlock();
}

bool helper_available(void) {
    helper_lock();
    bool up = g_pipe != INVALID_HANDLE_VALUE;
    helper_unlock();
    return up;
}

static bool helper_exchange(ProtoMsg *req) {
    ProtoMsg reply = {0};

    int r = helper_io(req, sizeof *req, true);
    if (r == 1) r = helper_io(&reply, sizeof reply, false);

    if (r != 1) {
        helper_disconnect();
        if (r < 0) helper_note_timeout();
        return false;
    }

    g_timeouts = 0;
    return reply.type == PROTO_OK;
}

static bool helper_ready(const wchar_t *op, bool *warned) {
    if (!g_tried) helper_open();

    if (helper_connect()) return true;

    if (!*warned) {
        *warned = true;
        log_err(L"helper: %ls (the window belongs to a higher-integrity "
                L"process) and no mshelld.exe is running to do it — see "
                L"INSTALL.md. This is logged once.", op);
    }
    return false;
}

static bool helper_request(ProtoMsg *req, const wchar_t *op, bool *warned) {
    helper_lock();
    bool ok = helper_ready(op, warned) && helper_exchange(req);
    helper_unlock();
    return ok;
}

bool helper_set_window_pos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
    static bool warned;

    ProtoMsg req = {
        .type    = PROTO_SETPOS,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .x = x, .y = y, .w = w, .h = h,
        .flags   = flags,
    };
    return helper_request(&req, L"a window could not be placed", &warned);
}

bool helper_set_topmost(HWND hwnd, bool on) {
    static bool warned;

    ProtoMsg req = {
        .type    = PROTO_ZORDER,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .flags   = on ? 1u : 0u,
    };
    return helper_request(&req, L"a floating window could not be kept on top",
                          &warned);
}

bool helper_set_cloak(HWND hwnd, bool on) {
    static bool warned;

    ProtoMsg req = {
        .type    = PROTO_CLOAK,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .flags   = on ? 1u : 0u,
    };
    return helper_request(&req, L"a window could not be hidden", &warned);
}

bool helper_close_window(HWND hwnd) {
    static bool warned;

    ProtoMsg req = {
        .type    = PROTO_CLOSE,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
    };
    return helper_request(&req, L"a window could not be closed", &warned);
}

#define HELPER_TASK_NAME  L"mshelld"
#define HELPER_SETTLE_MS  1500
#define HELPER_SCHTASKS_TIMEOUT_MS 15000

static LONG s_restart_running;

static void helper_notify(NotifyKind kind, const wchar_t *fmt, ...) {
    wchar_t msg[NOTIFY_TEXT_CAP];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(msg, NOTIFY_TEXT_CAP - 1, fmt, ap);
    va_end(ap);
    msg[NOTIFY_TEXT_CAP - 1] = L'\0';

    log_msg(kind == NOTIFY_ERROR ? LOG_ERROR : LOG_INFO, L"helper: %ls", msg);

    if (g.message_window)
        PostMessageW(g.message_window, WM_MSHELL_UPDATE,
                     MAKEWPARAM((WORD)kind, (WORD)8000), (LPARAM)_wcsdup(msg));
}

static bool helper_schtasks(const wchar_t *verb, DWORD *exit_code) {
    wchar_t cmd[256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    _snwprintf(cmd, ARRAYSIZE(cmd) - 1, L"schtasks.exe /%ls /tn \"%ls\"",
               verb, HELPER_TASK_NAME);
    cmd[ARRAYSIZE(cmd) - 1] = L'\0';

    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb          = sizeof si;
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return false;

    bool ok = (WaitForSingleObject(pi.hProcess, HELPER_SCHTASKS_TIMEOUT_MS)
               == WAIT_OBJECT_0);
    if (ok && exit_code) GetExitCodeProcess(pi.hProcess, exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

static DWORD WINAPI helper_restart_thread(LPVOID unused) {
    (void)unused;

    helper_lock();
    helper_disconnect();
    helper_unlock();

    DWORD rc = 1;
    if (!helper_schtasks(L"end", &rc))
        helper_notify(NOTIFY_ERROR, L"could not run schtasks to stop the "
                                    L"helper — is the mshelld task registered? "
                                    L"`install.bat /helper` creates it.");
    Sleep(HELPER_SETTLE_MS);

    rc = 1;
    if (!helper_schtasks(L"run", &rc) || rc != 0) {
        helper_notify(NOTIFY_ERROR, L"schtasks could not start the mshelld "
                                    L"task (exit %lu). Elevated windows will "
                                    L"float until your next sign-in.", rc);
        InterlockedExchange(&s_restart_running, 0);
        return 0;
    }
    Sleep(HELPER_SETTLE_MS);

    helper_lock();
    g_tried = false;
    bool reconnected = helper_connect();
    helper_unlock();

    if (reconnected)
        helper_notify(NOTIFY_INFO, L"helper restarted — the mshelld.exe on "
                                   L"disk is now the one running.");
    else
        helper_notify(NOTIFY_WARN, L"the mshelld task was started but has not "
                                   L"answered yet. It should connect on the "
                                   L"next window it is needed for.");

    InterlockedExchange(&s_restart_running, 0);
    return 0;
}

void helper_restart_async(void) {
    if (InterlockedCompareExchange(&s_restart_running, 1, 0) != 0) {
        log_msg(LOG_INFO, L"helper: a restart is already in progress");
        return;
    }

    HANDLE t = CreateThread(NULL, 0, helper_restart_thread, NULL, 0, NULL);
    if (!t) {
        InterlockedExchange(&s_restart_running, 0);
        log_err(L"helper: could not start the restart thread: %lu",
                GetLastError());
        return;
    }
    CloseHandle(t);
}
