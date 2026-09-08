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
#include "pipe_sd.h"

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#define logf_w(...) log_msg(LOG_INFO, __VA_ARGS__)

static void pipe_name(wchar_t *out, size_t cap) {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    _snwprintf(out, cap, L"%ls%lu", MSHELLD_PIPE_PREFIX, (unsigned long)sid);
    out[cap - 1] = L'\0';
}

static bool do_setpos(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    UINT flags = m->flags & (SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                             SWP_NOMOVE   | SWP_NOSIZE     | SWP_NOCOPYBITS);
    flags |= SWP_NOACTIVATE | SWP_NOZORDER;

    return SetWindowPos(h, NULL, m->x, m->y, m->w, m->h, flags) != 0;
}

static bool do_zorder(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    HWND band = (m->flags & 1u) ? HWND_TOPMOST : HWND_NOTOPMOST;
    return SetWindowPos(h, band, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != 0;
}

static bool do_cloak(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    BOOL v = (m->flags & 1u) ? TRUE : FALSE;
    return SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_CLOAK, &v, sizeof(v)));
}

static bool do_close(const ProtoMsg *m) {
    HWND h = (HWND)(uintptr_t)m->hwnd;
    if (!h || !IsWindow(h)) return false;

    return PostMessageW(h, WM_CLOSE, 0, 0) != 0;
}

static void serve(HANDLE pipe) {
    bool greeted = false;

    for (;;) {
        ProtoMsg in  = {0};
        DWORD    read = 0;

        if (!ReadFile(pipe, &in, sizeof in, &read, NULL) || read != sizeof in)
            return;

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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hInstance; (void)hPrev; (void)cmd; (void)show;

    log_init(L"mshelld", LOG_INFO);
    logf_w(L"=== mshelld starting (protocol %u) ===", MSHELLD_PROTO_VERSION);

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

    logf_w(L"granting pipe access to %ls (and SYSTEM)", sid);

    wchar_t name[MAX_PATH];
    pipe_name(name, MAX_PATH);
    logf_w(L"listening on %ls", name);

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
