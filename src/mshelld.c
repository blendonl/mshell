#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <dwmapi.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
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
#define MSHELLD_MAX_CLIENTS    4
#define SIGNER_HASH_LEN        32

#define logf_w(...)    log_msg(LOG_INFO, __VA_ARGS__)
#define logf_warn(...) log_msg(LOG_WARN, __VA_ARGS__)

typedef struct {
    HANDLE pipe;
    HANDLE io_event;
    DWORD  pid;
} Client;

static wchar_t g_client_path[MAX_PATH];
static LONG    g_client_count;
static bool    g_signer_pinned;
static BYTE    g_signer_hash[SIGNER_HASH_LEN];

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

static bool self_path(wchar_t *out, DWORD cap) {
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    return n > 0 && n < cap;
}

static LONG authenticode_signer(const wchar_t *path, BYTE *hash) {
    GUID verify_v2 = { 0x00aac56b, 0xcd44, 0x11d0,
                       { 0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee } };

    WINTRUST_FILE_INFO file = {
        .cbStruct      = sizeof file,
        .pcwszFilePath = path,
    };
    WINTRUST_DATA data = {
        .cbStruct            = sizeof data,
        .dwUIChoice          = WTD_UI_NONE,
        .fdwRevocationChecks = WTD_REVOKE_NONE,
        .dwUnionChoice       = WTD_CHOICE_FILE,
        .pFile               = &file,
        .dwStateAction       = WTD_STATEACTION_VERIFY,
        .dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL,
    };
    HWND no_ui = (HWND)INVALID_HANDLE_VALUE;

    LONG status = WinVerifyTrust(no_ui, &verify_v2, &data);

    if (status == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA *prov   =
            WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR *signer =
            prov ? WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0) : NULL;
        CRYPT_PROVIDER_CERT *leaf   =
            signer ? WTHelperGetProvCertFromChain(signer, 0) : NULL;
        DWORD len = SIGNER_HASH_LEN;

        if (!leaf || !leaf->pCert ||
            !CertGetCertificateContextProperty(leaf->pCert,
                                               CERT_SHA256_HASH_PROP_ID,
                                               hash, &len) ||
            len != SIGNER_HASH_LEN)
            status = TRUST_E_NO_SIGNER_CERT;
    }

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(no_ui, &verify_v2, &data);
    return status;
}

static void pin_signer(void) {
    wchar_t self[MAX_PATH];
    if (!self_path(self, MAX_PATH)) {
        logf_warn(L"could not find mshelld.exe's own path, so clients are "
                  L"identified by path only");
        return;
    }

    LONG status = authenticode_signer(self, g_signer_hash);
    if (status == ERROR_SUCCESS) {
        g_signer_pinned = true;
        logf_w(L"mshelld.exe is signed — clients must be signed by the same "
               L"certificate");
    } else if (status == TRUST_E_NOSIGNATURE) {
        logf_w(L"mshelld.exe is not Authenticode-signed, so clients are "
               L"identified by path only");
    } else {
        logf_warn(L"mshelld.exe's signature did not verify (0x%08lX), so "
                  L"clients are identified by path only",
                  (unsigned long)status);
    }
}

static bool client_signed_like_helper(DWORD pid, const wchar_t *image) {
    if (!g_signer_pinned) return true;

    BYTE hash[SIGNER_HASH_LEN];
    LONG status = authenticode_signer(image, hash);
    if (status != ERROR_SUCCESS) {
        logf_warn(L"refusing client pid %lu: %ls has no valid signature "
                  L"(0x%08lX) and mshelld.exe is signed",
                  (unsigned long)pid, image, (unsigned long)status);
        return false;
    }

    if (memcmp(hash, g_signer_hash, SIGNER_HASH_LEN) != 0) {
        logf_warn(L"refusing client pid %lu: %ls is signed by a different "
                  L"certificate than mshelld.exe", (unsigned long)pid, image);
        return false;
    }

    return true;
}

static bool resolve_client_path(wchar_t *out, size_t cap) {
    wchar_t self[MAX_PATH];
    if (!self_path(self, MAX_PATH)) return false;

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

static bool client_is_mshell(HANDLE pipe, DWORD *pid_out) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) {
        logf_warn(L"refusing client: GetNamedPipeClientProcessId failed: %lu",
                  GetLastError());
        return false;
    }
    *pid_out = pid;

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

    return client_signed_like_helper(pid, resolved);
}

static bool connect_client(HANDLE pipe, HANDLE event) {
    OVERLAPPED ov = {0};
    ov.hEvent = event;
    ResetEvent(event);

    if (ConnectNamedPipe(pipe, &ov)) return true;

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return true;
    if (err != ERROR_IO_PENDING) {
        logf_warn(L"ConnectNamedPipe failed: %lu", err);
        return false;
    }

    if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) return false;

    DWORD n = 0;
    return GetOverlappedResult(pipe, &ov, &n, FALSE) != 0;
}

static int pipe_io(const Client *c, void *buf, DWORD len, bool write,
                   DWORD wait_ms) {
    OVERLAPPED ov = {0};
    ov.hEvent = c->io_event;
    ResetEvent(c->io_event);

    BOOL ok = write ? WriteFile(c->pipe, buf, len, NULL, &ov)
                    : ReadFile(c->pipe, buf, len, NULL, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return 0;

    DWORD n = 0;
    if (WaitForSingleObject(c->io_event, wait_ms) != WAIT_OBJECT_0) {
        CancelIoEx(c->pipe, &ov);
        GetOverlappedResult(c->pipe, &ov, &n, TRUE);
        return -1;
    }

    if (!GetOverlappedResult(c->pipe, &ov, &n, FALSE)) return 0;
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

static void serve(const Client *c) {
    bool greeted = false;

    for (;;) {
        ProtoMsg in = {0};

        int r = pipe_io(c, &in, sizeof in, false,
                        greeted ? INFINITE : MSHELLD_HANDSHAKE_MS);
        if (r < 0) {
            logf_warn(L"dropping client pid %lu: it connected but sent no "
                      L"handshake within %u ms", (unsigned long)c->pid,
                      (unsigned)MSHELLD_HANDSHAKE_MS);
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
            logf_w(L"client pid %lu connected", (unsigned long)c->pid);
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

        if (pipe_io(c, &out, sizeof out, true, MSHELLD_WRITE_MS) != 1) return;
    }
}

static DWORD WINAPI client_thread(LPVOID param) {
    Client *c = (Client *)param;

    serve(c);

    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
    CloseHandle(c->io_event);
    free(c);

    InterlockedDecrement(&g_client_count);
    return 0;
}

static bool admit_client(HANDLE pipe, DWORD pid) {
    if (InterlockedIncrement(&g_client_count) > MSHELLD_MAX_CLIENTS) {
        logf_warn(L"refusing client pid %lu: %d clients are already connected",
                  (unsigned long)pid, MSHELLD_MAX_CLIENTS);
        InterlockedDecrement(&g_client_count);
        return false;
    }

    Client *c = (Client *)calloc(1, sizeof *c);
    HANDLE  ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE  t  = NULL;

    if (c && ev) {
        c->pipe     = pipe;
        c->io_event = ev;
        c->pid      = pid;
        t = CreateThread(NULL, 0, client_thread, c, 0, NULL);
    }

    if (!t) {
        logf_warn(L"refusing client pid %lu: could not start a thread for it: "
                  L"%lu", (unsigned long)pid, GetLastError());
        if (ev) CloseHandle(ev);
        free(c);
        InterlockedDecrement(&g_client_count);
        return false;
    }

    CloseHandle(t);
    return true;
}

static HANDLE create_instance(const wchar_t *name, SECURITY_ATTRIBUTES *sa,
                              bool first) {
    DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

    HANDLE pipe = CreateNamedPipeW(
        name, open_mode,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
        PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        sizeof(ProtoMsg), sizeof(ProtoMsg), 0, sa);

    if (pipe == INVALID_HANDLE_VALUE)
        logf_w(L"CreateNamedPipe failed: %lu", GetLastError());
    return pipe;
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

    HANDLE listen_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!listen_event) {
        logf_w(L"FATAL: CreateEvent failed: %lu", GetLastError());
        return 1;
    }

    if (!resolve_client_path(g_client_path, MAX_PATH)) {
        logf_w(L"FATAL: could not work out the path of the mshell.exe beside "
               L"this helper — refusing to serve clients it cannot identify");
        return 1;
    }
    logf_w(L"only %ls may connect", g_client_path);
    pin_signer();

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

    HANDLE listening = create_instance(name, &sa, true);

    while (listening != INVALID_HANDLE_VALUE) {
        bool   connected = connect_client(listening, listen_event);
        HANDLE next      = create_instance(name, &sa, false);
        DWORD  pid       = 0;

        bool admitted = connected && next != INVALID_HANDLE_VALUE &&
                        client_is_mshell(listening, &pid) &&
                        admit_client(listening, pid);

        if (!admitted) {
            DisconnectNamedPipe(listening);
            CloseHandle(listening);
        }
        listening = next;
    }

    CloseHandle(listen_event);
    LocalFree(sd);
    log_shutdown();
    return 0;
}
