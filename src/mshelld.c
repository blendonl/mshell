/*
 * mshelld.c — mshell's privileged helper.
 *
 * A separate, elevated process whose entire job is to touch windows that the
 * unelevated shell is not allowed to touch (UIPI): move them, put them in or
 * out of the always-on-top band, cloak/uncloak them, ask them to close. See
 * proto.h for why this exists and, just as importantly, for what was
 * deliberately left out of it.
 *
 * The design rule this file exists to honour: THIS PROCESS MUST STAY STUPID.
 * It has no config file, no Lua, no window rules, no layout, no keyboard hook
 * and no state beyond the pipe. It cannot be extended from a config, because it
 * never reads one. Every decision about which window gets what is made in the
 * unelevated shell, and arrives here as a rectangle, a cloak flag or a close.
 *
 * That matters because the helper is, unavoidably, a small privilege boundary
 * of its own: anything that can talk to its pipe can move, hide or close any
 * window on the desktop. The DACL restricts that to the interactive user,
 * which is the same user who could already do those things to their own
 * windows — so the granted capability is "manage windows of your own elevated
 * processes", not "become admin".
 *
 * Build: produced by the same `make` as mshell.exe. Run elevated, e.g. from a
 * logon scheduled task with "Run with highest privileges" — see INSTALL.md.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdbool.h>

#include "proto.h"
#include "log.h"
#include "pipe_sd.h"    /* the DACL, shared with the shell — see that header */

/* Cloaking postdates the mingw-w64 headers, same as in window.c: the id is
 * passed as a DWORD, so the raw value is all that is needed. */
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

/* ---------------------------------------------------------------------------
 * Logging — mshelld.log, deliberately its own file. The helper may run as a
 * different token than the shell, so sharing mshell.log would mean two
 * processes appending to one file under different ACLs.
 *
 * log.c is shared with mshell.exe (it deliberately depends on nothing but the
 * Win32 API, so it links into a binary that has no MShell global). Everything
 * the helper logs is lifecycle or failure, so INFO is the right level for it
 * and there is nothing here that wants gating.
 * --------------------------------------------------------------------------- */
#define logf_w(...) log_msg(LOG_INFO, __VA_ARGS__)

/* ---------------------------------------------------------------------------
 * Pipe name and security — identical reasoning to ipc.c: per session, and a
 * DACL naming the interactive user rather than the default, which would let any
 * local account drive it.
 * --------------------------------------------------------------------------- */
static void pipe_name(wchar_t *out, size_t cap) {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    _snwprintf(out, cap, L"%ls%lu", MSHELLD_PIPE_PREFIX, (unsigned long)sid);
    out[cap - 1] = L'\0';
}

/* The DACL now comes from pipe_sd.c, shared with the shell's own pipe.
 *
 * It used to be the SDDL literal "D:(A;;GA;;;IU)(A;;GA;;;BA)(A;;GA;;;SY)", on
 * the reasoning that the helper's elevated token is the Administrators one and
 * so granting that would not let the unelevated shell in. The first half of
 * that is true and the conclusion was not: elevation changes a token's
 * integrity level and groups, NOT its user, so the helper's own user SID is
 * already exactly the interactive user the shell runs as.
 *
 * "IU" is every interactively logged-on user, which is a different and much
 * larger set. The pipe name is per-session and entirely predictable, so with
 * fast user switching a second signed-in user could open this pipe and move,
 * hide or close the first user's windows — including the elevated ones this
 * process exists to reach. */

/* ---------------------------------------------------------------------------
 * The privileged operations. All three stay inside the same authority the DACL
 * already grants: the interactive user driving windows of their own session.
 * --------------------------------------------------------------------------- */
static bool do_setpos(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    /* The flags the shell may ask for are masked to the ones that only move or
     * resize. Not because the shell is untrusted, but because this is the
     * boundary: a request that could activate a window, or change its owner or
     * z-band, is more authority than "tile it" needs, and the narrowest surface
     * that does the job is the one worth exposing.
     *
     * SWP_NOCOPYBITS is on that list even though it looks like a rendering
     * detail, because it IS one and grants nothing: it only says "do not blit
     * the old client area into the new position". The shell adds it to exactly
     * the moves where those saved bits are stale — a window coming back from
     * being hidden, or unstashed from off-screen. Masking it away meant the
     * elevated windows this helper exists to serve were the only ones that kept
     * the bug it fixes, coming back blank or mis-composited. */
    UINT flags = m->flags & (SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                             SWP_NOMOVE   | SWP_NOSIZE     | SWP_NOCOPYBITS);
    flags |= SWP_NOACTIVATE | SWP_NOZORDER;

    return SetWindowPos(h, NULL, m->x, m->y, m->w, m->h, flags) != 0;
}

/* Put a window into the always-on-top band, or take it back out.
 *
 * do_setpos above refuses every z-order change, and that is still right for a
 * placement: tiling has no business restacking anything. This is the one
 * z-order question the shell genuinely cannot answer for itself — float_on_top
 * keeps floating windows above the grid by parking them in the topmost band,
 * and for an elevated window that SetWindowPos is refused like any other, so
 * Task Manager was the one float a click could still bury.
 *
 * Narrow on purpose, and narrower than do_setpos: the band is a boolean, the
 * geometry is untouched (SWP_NOMOVE | SWP_NOSIZE) and the window cannot be
 * activated. What is deliberately NOT offered is an arbitrary hWndInsertAfter
 * — "put this window above that one" is the power to order the whole desktop,
 * and no caller here needs it. Floats are ordered among themselves by the
 * shell, which can place the ones it is allowed to place. */
static bool do_zorder(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    HWND band = (m->flags & 1u) ? HWND_TOPMOST : HWND_NOTOPMOST;
    return SetWindowPos(h, band, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != 0;
}

/* Take a window off the screen (or put it back) the way mshell does for its
 * own windows: DWM cloaking. A hidden window is just a moved one nobody can
 * see — the same UIPI boundary, so it belongs on the same side of it. */
static bool do_cloak(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    BOOL v = (m->flags & 1u) ? TRUE : FALSE;
    return SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_CLOAK, &v, sizeof(v)));
}

/* Ask a window to close itself. A posted WM_CLOSE is the polite close — the
 * app still gets to veto or clean up — and posting it crosses the same
 * integrity boundary moving does. */
static bool do_close(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    return PostMessageW(h, WM_CLOSE, 0, 0) != 0;
}

/* ---------------------------------------------------------------------------
 * Serve one connected client until it disconnects.
 * --------------------------------------------------------------------------- */
static void serve(HANDLE pipe) {
    bool greeted = false;

    for (;;) {
        ProtoMsg in  = {0};
        DWORD    read = 0;

        if (!ReadFile(pipe, &in, sizeof in, &read, NULL) || read != sizeof in)
            return;   /* disconnected or a short/oversized frame */

        if (in.version != MSHELLD_PROTO_VERSION) {
            logf_w(L"rejecting client: protocol %u, expected %u — mshell.exe "
                   L"and mshelld.exe are from different builds",
                   in.version, MSHELLD_PROTO_VERSION);
            return;
        }

        ProtoMsg out = { .type = PROTO_OK, .version = MSHELLD_PROTO_VERSION };

        switch (in.type) {
        case PROTO_HELLO:
            greeted = true;
            logf_w(L"client connected");
            break;

        case PROTO_SETPOS:
            /* HELLO first, so a client that has not agreed the protocol cannot
             * reach the privileged operations at all. */
            if (!greeted)                 out.type = PROTO_FAIL;
            else if (!do_setpos(&in))     out.type = PROTO_FAIL;
            break;

        case PROTO_ZORDER:
            if (!greeted)                 out.type = PROTO_FAIL;
            else if (!do_zorder(&in))     out.type = PROTO_FAIL;
            break;

        case PROTO_CLOAK:
            if (!greeted)                 out.type = PROTO_FAIL;
            else if (!do_cloak(&in))      out.type = PROTO_FAIL;
            break;

        case PROTO_CLOSE:
            if (!greeted)                 out.type = PROTO_FAIL;
            else if (!do_close(&in))      out.type = PROTO_FAIL;
            break;

        default:
            out.type = PROTO_FAIL;
            break;
        }

        DWORD written = 0;
        if (!WriteFile(pipe, &out, sizeof out, &written, NULL)) return;
    }
}

/* ---------------------------------------------------------------------------
 * Entry point. No window, no message loop: this process only ever waits on a
 * pipe.
 * --------------------------------------------------------------------------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hInstance; (void)hPrev; (void)cmd; (void)show;

    log_init(L"mshelld", LOG_INFO);
    logf_w(L"=== mshelld starting (protocol %u) ===", MSHELLD_PROTO_VERSION);

    /* One helper per session, same reasoning as the shell's own guard. */
    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\mshelld_singleton");
    if (!once || GetLastError() == ERROR_ALREADY_EXISTS) {
        logf_w(L"another mshelld is already running — exiting");
        return 0;
    }

    wchar_t sid[256];
    PSECURITY_DESCRIPTOR sd = pipe_sd_for_current_user(sid, 256);
    if (!sd) {
        logf_w(L"FATAL: could not build the pipe's security descriptor — "
               L"refusing to create an unrestricted pipe");
        return 1;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };

    /* Say WHICH identity the pipe was handed to. When the shell cannot connect,
     * this line is the whole diagnosis: the logon task INSTALL.md registers has
     * no /ru and so runs as the interactive user, whose SID this should be. A
     * task edited to run as SYSTEM grants SYSTEM instead, and no shell will
     * ever reach it — that configuration is unsupported and would be in session
     * 0 anyway, unable to touch these windows at all. */
    logf_w(L"granting pipe access to %ls (and SYSTEM)", sid);

    wchar_t name[MAX_PATH];
    pipe_name(name, MAX_PATH);
    logf_w(L"listening on %ls", name);

    /* FILE_FLAG_FIRST_PIPE_INSTANCE: refuse to start beside a pipe of this name
     * that somebody else created, rather than quietly adding an instance to it
     * and taking turns serving the shell's requests. PIPE_REJECT_REMOTE_CLIENTS:
     * this pipe is otherwise reachable over SMB as \\host\pipe\mshelld-1, and
     * nothing on the network has any business driving these operations. */
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(ProtoMsg), sizeof(ProtoMsg), 0, &sa);

        if (pipe == INVALID_HANDLE_VALUE) {
            logf_w(L"CreateNamedPipe failed: %lu", GetLastError());
            break;
        }

        if (ConnectNamedPipe(pipe, NULL) ||
            GetLastError() == ERROR_PIPE_CONNECTED)
            serve(pipe);

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    LocalFree(sd);
    log_shutdown();
    return 0;
}
