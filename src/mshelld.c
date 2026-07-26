/*
 * mshelld.c — mshell's privileged helper.
 *
 * A separate, elevated process whose entire job is to move windows that the
 * unelevated shell is not allowed to move (UIPI). See proto.h for why this
 * exists and, just as importantly, for what was deliberately left out of it.
 *
 * The design rule this file exists to honour: THIS PROCESS MUST STAY STUPID.
 * It has no config file, no Lua, no window rules, no layout, no keyboard hook
 * and no state beyond the pipe. It cannot be extended from a config, because it
 * never reads one. Every decision about which window goes where is made in the
 * unelevated shell, and arrives here as a rectangle.
 *
 * That matters because the helper is, unavoidably, a small privilege boundary
 * of its own: anything that can talk to its pipe can move any window on the
 * desktop. The DACL restricts that to the interactive user, which is the same
 * user who could already move their own windows — so the granted capability is
 * "reposition windows of your own elevated processes", not "become admin".
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
#include <sddl.h>
#include <stdio.h>
#include <stdbool.h>

#include "proto.h"

/* ---------------------------------------------------------------------------
 * Logging — %TEMP%\mshelld.log, deliberately its own file. The helper may run
 * as a different token than the shell, so sharing mshell.log would mean two
 * processes truncating one file.
 * --------------------------------------------------------------------------- */
static FILE *g_log;

static void logf_w(const wchar_t *fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = L'\0';

    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    if (g_log) { fputws(buf, g_log); fputwc(L'\n', g_log); fflush(g_log); }
}

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

/* The helper runs elevated, so its OWN token is the Administrators-flavoured
 * one — granting access to that would not let the unelevated shell connect.
 * What is wanted is the interactive user of this session, which is what the
 * shell runs as. SDDL's "IU" (INTERACTIVE) names exactly that. */
static PSECURITY_DESCRIPTOR build_sd(void) {
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;IU)(A;;GA;;;BA)(A;;GA;;;SY)",
            SDDL_REVISION_1, &sd, NULL))
        return NULL;
    return sd;
}

/* ---------------------------------------------------------------------------
 * The one privileged operation.
 * --------------------------------------------------------------------------- */
static bool do_setpos(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    /* The flags the shell may ask for are masked to the ones that only move or
     * resize. Not because the shell is untrusted, but because this is the
     * boundary: a request that could activate a window, or change its owner or
     * z-band, is more authority than "tile it" needs, and the narrowest surface
     * that does the job is the one worth exposing. */
    UINT flags = m->flags & (SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                             SWP_NOMOVE   | SWP_NOSIZE);
    flags |= SWP_NOACTIVATE | SWP_NOZORDER;

    return SetWindowPos(h, NULL, m->x, m->y, m->w, m->h, flags) != 0;
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
             * reach the privileged operation at all. */
            if (!greeted)                 out.type = PROTO_FAIL;
            else if (!do_setpos(&in))     out.type = PROTO_FAIL;
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

    {
        wchar_t path[MAX_PATH];
        if (GetTempPathW(MAX_PATH, path)) {
            wcsncat(path, L"mshelld.log", MAX_PATH - wcslen(path) - 1);
            g_log = _wfopen(path, L"w, ccs=UTF-8");
        }
    }
    logf_w(L"=== mshelld starting (protocol %u) ===", MSHELLD_PROTO_VERSION);

    /* One helper per session, same reasoning as the shell's own guard. */
    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\mshelld_singleton");
    if (!once || GetLastError() == ERROR_ALREADY_EXISTS) {
        logf_w(L"another mshelld is already running — exiting");
        return 0;
    }

    PSECURITY_DESCRIPTOR sd = build_sd();
    if (!sd) {
        logf_w(L"FATAL: could not build the pipe's security descriptor — "
               L"refusing to create an unrestricted pipe");
        return 1;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };

    wchar_t name[MAX_PATH];
    pipe_name(name, MAX_PATH);
    logf_w(L"listening on %ls", name);

    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
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
    if (g_log) fclose(g_log);
    return 0;
}
