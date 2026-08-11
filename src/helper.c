/*
 * helper.c — the shell's side of the privileged helper (see proto.h, mshelld.c).
 *
 * The whole of the integration is one idea: TRY LOCALLY FIRST, ASK ONLY ON
 * REFUSAL. mshell attempts every SetWindowPos itself; if Windows refuses
 * because the target belongs to a higher-integrity process (UIPI), and only
 * then, the request is forwarded to mshelld.exe.
 *
 * That shape is deliberate and worth stating, because the obvious alternative —
 * work out up front which windows are elevated and route those — would mean
 * opening every window's process to ask, on the busiest path in the program,
 * to answer a question the failing call already answers for free. It would also
 * be wrong more often: UIPI is not the only reason a placement can fail.
 *
 * When no helper is running, nothing changes: the failure is reported the way
 * it always was and the window simply is not moved. The helper is opt-in and
 * absent by default.
 */

#include "mshell.h"
#include "proto.h"

/* ---------------------------------------------------------------------------
 * Nothing here may block the shell.
 *
 * Every call below runs on the thread that pumps messages — window_set_pos is
 * reached from the tiling pass, the animation timer and the drag handler — and
 * this process IS the shell. There is no taskbar behind it to recover with.
 *
 * A helper that DIES was always handled: the read fails and the handle is
 * dropped. A helper that is alive and simply not reading was not, and it is the
 * easier state to reach — suspended, sitting in a Windows Error Reporting
 * dialog, or blocked inside its own cross-process DwmSetWindowAttribute call. A
 * synchronous ReadFile on a PIPE_WAIT pipe then never returns and the session
 * is gone.
 *
 * So the I/O is overlapped and bounded. 250 ms is roughly a thousand times what
 * a local pipe round trip to a process doing one SetWindowPos actually costs,
 * and deliberately not IPC_WAIT_MS (5 s), which is the timeout a human waits at
 * a command line rather than one the message loop can afford.
 *
 * And a bound alone is not enough. At one window per timeout, per tiling pass,
 * a wedged helper still costs seconds per keystroke — so consecutive timeouts
 * trip a breaker and mshell stops asking for a while. What that degrades to is
 * exactly the documented no-helper behaviour: elevated windows float and stay
 * put. That is a fallback; a frozen shell is not.
 * --------------------------------------------------------------------------- */
#define HELPER_IO_TIMEOUT_MS   250
#define HELPER_FAIL_LIMIT      3      /* consecutive timeouts before backing off */
#define HELPER_BACKOFF_MS      5000

static HANDLE    g_pipe = INVALID_HANDLE_VALUE;
static HANDLE    g_event;           /* completion event for the overlapped I/O */
static bool      g_tried;           /* we have attempted a connection at least once */
static int       g_timeouts;        /* consecutive, reset by any clean exchange */
static ULONGLONG g_blocked_until;   /* GetTickCount64 deadline; 0 = not backed off */

/* True while the breaker is open. Also closes it again once the wait is up, so
 * the caller does not have to know the breaker exists. */
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

/* One overlapped transfer, bounded. Returns 1 on success, 0 on error, and -1
 * specifically on TIMEOUT, which is the only outcome the breaker counts.
 *
 * The cancel path matters: `ov` lives on this stack frame, and the kernel may
 * still write to it after CancelIoEx returns. GetOverlappedResult with bWait
 * blocks until the cancelled operation has actually been reaped, so the frame
 * cannot go away underneath it. That wait is bounded by the cancellation
 * itself, not by the peer. */
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

/* Connect and shake hands. Cheap to call repeatedly: it returns immediately
 * when already connected, and gives up quickly when the helper simply is not
 * there.
 *
 * NB the 1 ms: WaitNamedPipeW takes NMPWAIT_USE_DEFAULT_WAIT for 0, which is
 * the SERVER's default timeout, not "do not wait". This used to pass 0 and
 * claim in a comment that it was a zero timeout; it returned promptly only
 * because a missing pipe fails immediately regardless. Against a pipe that
 * exists but is busy it was the server's default — 50 ms here — once per
 * window, per pass, on the message thread. */
static bool helper_connect(void) {
    if (g_pipe != INVALID_HANDLE_VALUE) return true;
    if (helper_backed_off()) return false;

    wchar_t name[MAX_PATH];
    helper_pipe_name(name, MAX_PATH);

    if (!WaitNamedPipeW(name, 1)) return false;   /* not running */

    /* FILE_FLAG_OVERLAPPED so every exchange can be given a deadline. */
    HANDLE p = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (p == INVALID_HANDLE_VALUE) return false;

    if (!g_event) {
        /* Manual-reset, unnamed, one for the life of the process: every
         * exchange is synchronous from this thread's point of view, so there is
         * never more than one operation in flight to wait on. */
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

    g_pipe = p;   /* helper_io works on g_pipe, so publish it before the shake */

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

void helper_init(void) {
    g_tried = true;
    if (!helper_connect())
        log_w(L"helper: mshelld.exe is not running. Windows owned by elevated "
              L"processes will float instead of tiling and will stay on every "
              L"desktop — and, less obviously, NO window can be cloaked: DWM "
              L"refuses DWMWA_CLOAK on another process's window whoever owns "
              L"it, so hiding falls back to ShowWindow(SW_HIDE) for everything "
              L"(see window_hide). Run `install.bat /helper` from an "
              L"administrator prompt to change both.");
}

void helper_shutdown(void) {
    helper_disconnect();
    if (g_event) { CloseHandle(g_event); g_event = NULL; }
}

bool helper_available(void) {
    return g_pipe != INVALID_HANDLE_VALUE;
}

/* ===========================================================================
 * The fallback itself.
 *
 * Called ONLY after the local attempt has already failed. Returns true if the
 * helper performed it.
 * =========================================================================== */

/* The request/response round trip, for a caller that holds the connection.
 * A failed conversation drops the handle so the next call reconnects rather
 * than failing forever on a dead pipe. */
static bool helper_exchange(ProtoMsg *req) {
    ProtoMsg reply = {0};

    int r = helper_io(req, sizeof *req, true);
    if (r == 1) r = helper_io(&reply, sizeof reply, false);

    if (r != 1) {
        helper_disconnect();
        /* Only a TIMEOUT counts towards the breaker. An ordinary error means
         * the helper went away, which the reconnect on the next call handles
         * and which costs nothing to retry. */
        if (r < 0) helper_note_timeout();
        return false;
    }

    g_timeouts = 0;   /* it answered; whatever went before is not a pattern */
    return reply.type == PROTO_OK;
}

/* Connect, or explain once (per call site) why the operation is not going to
 * happen. `warned` is the caller's static, so each operation gets its own
 * one-shot message naming what was refused. */
static bool helper_ready(const wchar_t *op, bool *warned) {
    if (!g_tried) helper_init();

    if (helper_connect()) return true;

    if (!*warned) {
        *warned = true;
        log_err(L"helper: %ls (the window belongs to a higher-integrity "
                L"process) and no mshelld.exe is running to do it — see "
                L"INSTALL.md. This is logged once.", op);
    }
    return false;
}

bool helper_set_window_pos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
    static bool warned;
    if (!helper_ready(L"a window could not be placed", &warned)) return false;

    ProtoMsg req = {
        .type    = PROTO_SETPOS,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .x = x, .y = y, .w = w, .h = h,
        .flags   = flags,
    };
    return helper_exchange(&req);
}

bool helper_set_topmost(HWND hwnd, bool on) {
    static bool warned;
    if (!helper_ready(L"a floating window could not be kept on top", &warned))
        return false;

    ProtoMsg req = {
        .type    = PROTO_ZORDER,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .flags   = on ? 1u : 0u,
    };
    return helper_exchange(&req);
}

bool helper_set_cloak(HWND hwnd, bool on) {
    static bool warned;
    if (!helper_ready(L"a window could not be hidden", &warned)) return false;

    ProtoMsg req = {
        .type    = PROTO_CLOAK,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .flags   = on ? 1u : 0u,
    };
    return helper_exchange(&req);
}

bool helper_close_window(HWND hwnd) {
    static bool warned;
    if (!helper_ready(L"a window could not be closed", &warned)) return false;

    ProtoMsg req = {
        .type    = PROTO_CLOSE,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
    };
    return helper_exchange(&req);
}
