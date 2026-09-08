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
#include <string.h>
#include <wchar.h>

#include "proto.h"
#include "log.h"
#include "pipe_sd.h"

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#define MSHELLD_CLIENT_EXE     L"mshell.exe"
#define MSHELLD_HANDSHAKE_MS   5000
#define MSHELLD_WRITE_MS       5000

#define logf_w(...)    log_msg(LOG_INFO, __VA_ARGS__)
#define logf_warn(...) log_msg(LOG_WARN, __VA_ARGS__)

static HANDLE  g_io_event;
static wchar_t g_client_path[MAX_PATH];

static void pipe_name(wchar_t *out, size_t cap) {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    _snwprintf(out, cap, L"%ls%lu", MSHELLD_PIPE_PREFIX, (unsigned long)sid);
    out[cap - 1] = L'\0';
}

static void canonical_path(const wchar_t *in, wchar_t *out, DWORD cap) {
    DWORD n = GetLongPathNameW(in, out, cap);
    if (n == 0 || n >= cap) {
        wcsncpy(out, in, cap - 1);
        out[cap - 1] = L'\0';
    }
}

static bool resolve_client_path(wchar_t *out, size_t cap) {
    wchar_t self[MAX_PATH];
    DWORD   n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash) return false;
    slash[1] = L'\0';

    wchar_t joined[MAX_PATH];
    int     written = _snwprintf(joined, MAX_PATH, L"%ls%ls", self,
                                 MSHELLD_CLIENT_EXE);
    if (written < 0 || written >= MAX_PATH) return false;
    joined[written] = L'\0';

    canonical_path(joined, out, (DWORD)cap);
    return true;
}

static bool client_is_mshell(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) {
        logf_warn(L"refusing client: GetNamedPipeClientProcessId failed: %lu",
                  GetLastError());
        return false;
    }

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        logf_warn(L"refusing client pid %lu: OpenProcess failed: %lu",
                  (unsigned long)pid, GetLastError());
        return false;
    }

    wchar_t image[MAX_PATH];
    DWORD   len = MAX_PATH;
    BOOL    ok  = QueryFullProcessImageNameW(proc, 0, image, &len);
    DWORD   err = GetLastError();
    CloseHandle(proc);

    if (!ok) {
        logf_warn(L"refusing client pid %lu: QueryFullProcessImageName "
                  L"failed: %lu", (unsigned long)pid, err);
        return false;
    }

    wchar_t resolved[MAX_PATH];
    canonical_path(image, resolved, MAX_PATH);

    if (_wcsicmp(resolved, g_client_path) != 0) {
        logf_warn(L"refusing client pid %lu: it is %ls, and only %ls may "
                  L"drive this helper", (unsigned long)pid, resolved,
                  g_client_path);
        return false;
    }

    return true;
}

static bool connect_client(HANDLE pipe) {
    OVERLAPPED ov = {0};
    ov.hEvent = g_io_event;
    ResetEvent(g_io_event);

    if (ConnectNamedPipe(pipe, &ov)) return true;

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return true;
    if (err != ERROR_IO_PENDING) {
        logf_warn(L"ConnectNamedPipe failed: %lu", err);
        return false;
    }

    if (WaitForSingleObject(g_io_event, INFINITE) != WAIT_OBJECT_0) return false;

    DWORD n = 0;
    return GetOverlappedResult(pipe, &ov, &n, FALSE) != 0;
}

static int pipe_io(HANDLE pipe, void *buf, DWORD len, bool write, DWORD wait_ms) {
    OVERLAPPED ov = {0};
    ov.hEvent = g_io_event;
    ResetEvent(g_io_event);

    BOOL ok = write ? WriteFile(pipe, buf, len, NULL, &ov)
                    : ReadFile(pipe, buf, len, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return 0;

    DWORD n = 0;
    if (WaitForSingleObject(g_io_event, wait_ms) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &n, TRUE);
        return -1;
    }

    if (!GetOverlappedResult(pipe, &ov, &n, FALSE)) return 0;
    return (n == len) ? 1 : 0;
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
        ProtoMsg in = {0};

        int r = pipe_io(pipe, &in, sizeof in, false,
                        greeted ? INFINITE : MSHELLD_HANDSHAKE_MS);
        if (r < 0) {
            logf_warn(L"dropping client: it connected but sent no handshake "
                      L"within %u ms", (unsigned)MSHELLD_HANDSHAKE_MS);
            return;
        }
        if (r != 1) return;

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

        if (pipe_io(pipe, &out, sizeof out, true, MSHELLD_WRITE_MS) != 1) return;
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

    g_io_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_io_event) {
        logf_w(L"FATAL: CreateEvent failed: %lu — without a way to time client "
               L"I/O out, one silent client would wedge the helper",
               GetLastError());
        return 1;
    }

    if (!resolve_client_path(g_client_path, MAX_PATH)) {
        logf_w(L"FATAL: could not work out the path of the mshell.exe beside "
               L"this helper — refusing to serve clients it cannot identify");
        return 1;
    }
    logf_w(L"only %ls may connect", g_client_path);

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
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE |
                  FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(ProtoMsg), sizeof(ProtoMsg), 0, &sa);

        if (pipe == INVALID_HANDLE_VALUE) {
            logf_w(L"CreateNamedPipe failed: %lu", GetLastError());
            break;
        }

        if (connect_client(pipe) && client_is_mshell(pipe))
            serve(pipe);

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    LocalFree(sd);
    log_shutdown();
    return 0;
}
