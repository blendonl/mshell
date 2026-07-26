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

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static bool   g_tried;      /* we have attempted a connection at least once */
static bool   g_warned;     /* the "no helper" note is logged once, not per call */

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

/* Connect and shake hands. Cheap to call repeatedly: it returns immediately
 * when already connected, and does not retry in a tight loop when the helper
 * simply is not there (WaitNamedPipe with a zero timeout). */
static bool helper_connect(void) {
    if (g_pipe != INVALID_HANDLE_VALUE) return true;

    wchar_t name[MAX_PATH];
    helper_pipe_name(name, MAX_PATH);

    if (!WaitNamedPipeW(name, 0)) return false;   /* not running */

    HANDLE p = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (p == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(p, &mode, NULL, NULL);

    ProtoMsg hello = { .type = PROTO_HELLO, .version = MSHELLD_PROTO_VERSION };
    ProtoMsg reply = {0};
    DWORD    n     = 0;

    if (!WriteFile(p, &hello, sizeof hello, &n, NULL) ||
        !ReadFile(p, &reply, sizeof reply, &n, NULL)  ||
        n != sizeof reply || reply.type != PROTO_OK) {
        CloseHandle(p);
        log_err(L"helper: mshelld.exe rejected the handshake — are the two "
                L"binaries from the same build?");
        return false;
    }

    g_pipe = p;
    log_err(L"helper: connected to mshelld.exe — windows owned by elevated "
            L"processes can now be tiled");
    return true;
}

void helper_init(void) {
    g_tried = true;
    if (!helper_connect())
        log_w(L"helper: mshelld.exe is not running; windows owned by elevated "
              L"processes will float instead of tiling (this is the default "
              L"and is fine unless you want them tiled)");
}

void helper_shutdown(void) {
    helper_disconnect();
}

bool helper_available(void) {
    return g_pipe != INVALID_HANDLE_VALUE;
}

/* ===========================================================================
 * The fallback itself.
 *
 * Called ONLY after a local SetWindowPos has already failed. Returns true if
 * the helper performed it.
 * =========================================================================== */
bool helper_set_window_pos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
    if (!g_tried) helper_init();

    if (!helper_connect()) {
        if (!g_warned) {
            g_warned = true;
            log_err(L"helper: a window could not be placed (it belongs to a "
                    L"higher-integrity process) and no mshelld.exe is running "
                    L"to do it — see INSTALL.md. This is logged once.");
        }
        return false;
    }

    ProtoMsg req = {
        .type    = PROTO_SETPOS,
        .version = MSHELLD_PROTO_VERSION,
        .hwnd    = (uint64_t)(uintptr_t)hwnd,
        .x = x, .y = y, .w = w, .h = h,
        .flags   = flags,
    };
    ProtoMsg reply = {0};
    DWORD    n     = 0;

    if (!WriteFile(g_pipe, &req, sizeof req, &n, NULL) ||
        !ReadFile(g_pipe, &reply, sizeof reply, &n, NULL) ||
        n != sizeof reply) {
        /* The helper went away mid-conversation. Drop the handle so the next
         * call reconnects rather than failing forever on a dead pipe. */
        helper_disconnect();
        return false;
    }

    return reply.type == PROTO_OK;
}
