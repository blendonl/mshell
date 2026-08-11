/*
 * ipc.c — control a running mshell from the command line.
 *
 *     mshell.exe --msg "switch_desktop web"
 *     mshell.exe --msg "layout_monocle"
 *     mshell.exe --query                     (prints JSON state)
 *
 * Two halves in one binary: a SERVER thread inside the running shell, and a
 * CLIENT path taken when mshell.exe is invoked with --msg/--query, which
 * connects to the already-running instance and exits.
 *
 * THREADING follows the pattern config.c's watcher established: the pipe thread
 * does no window-manager work of its own. It hands the command to the main
 * thread by PostMessage and waits for it to finish, because everything the
 * command can touch — keymaps, desktops, tiling — is main-thread state, and the
 * WinEvent suppression counter is not thread-safe.
 *
 * SECURITY is deliberate rather than incidental, because Phase 3 puts a
 * privilege boundary here: the plan is to split the elevated work (the keyboard
 * hook, UIPI-privileged SetWindowPos) into a small helper with no config and no
 * scripting, talking over this same channel. So the pipe is created with an
 * explicit DACL granting only the owning user and SYSTEM — not the default,
 * which would let any local account connect — and the command surface is a
 * fixed vocabulary of actions rather than anything that can be extended from
 * the config.
 */

#include "mshell.h"
#include "pipe_sd.h"    /* the DACL, shared with mshelld.exe — see that header */

#define IPC_REPLY_MAX    16384
#define IPC_CMD_MAX      1024
#define IPC_WAIT_MS      5000   /* how long a client waits for the shell    */

/* ===========================================================================
 * The request handed from the pipe thread to the main thread.
 * =========================================================================== */
typedef struct {
    wchar_t cmd[IPC_CMD_MAX];
    char    reply[IPC_REPLY_MAX];   /* UTF-8, so the client can print it raw */
    HANDLE  done;
} IpcRequest;

static HANDLE g_ipc_thread;
static HANDLE g_ipc_stop;
static bool   g_ipc_running;

/* Pipe name is per-session: the shell is per-session, and two users signed in
 * at once each run their own mshell. */
static void ipc_pipe_name(wchar_t *out, size_t cap) {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    _snwprintf(out, cap, L"\\\\.\\pipe\\mshell-%lu", (unsigned long)sid);
    out[cap - 1] = L'\0';
}

/* ===========================================================================
 * Server: JSON state
 * =========================================================================== */
static void json_escape(const wchar_t *w, char *out, size_t cap) {
    char u8[1024];
    if (WideCharToMultiByte(CP_UTF8, 0, w ? w : L"", -1, u8, (int)sizeof u8,
                            NULL, NULL) <= 0) {
        if (cap) out[0] = '\0';
        return;
    }

    size_t o = 0;
    for (size_t i = 0; u8[i] && o + 2 < cap; i++) {
        unsigned char c = (unsigned char)u8[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         { o += (size_t)snprintf(out + o, cap - o,
                                                           "\\u%04x", c); }
        else                        out[o++] = (char)c;
    }
    out[o < cap ? o : cap - 1] = '\0';
}

static void ipc_build_state(char *out, size_t cap) {
    size_t o = 0;
    char   esc[1024];

    o += (size_t)snprintf(out + o, cap - o, "{\"version\":\"%s\",", MSHELL_VERSION);

    /* desktops */
    o += (size_t)snprintf(out + o, cap - o, "\"desktops\":[");
    for (int i = 0; i < g.desktop_count && o < cap; i++) {
        const Desktop *d = &g.desktops[i];
        json_escape(d->name, esc, sizeof esc);
        o += (size_t)snprintf(out + o, cap - o,
                 "%s{\"name\":\"%s\",\"current\":%s,\"windows\":%d,"
                 "\"layout\":\"%s\",\"monitor\":%d}",
                 i ? "," : "", esc,
                 d->id == g.current_desktop_id ? "true" : "false",
                 d->count, layout_to_name(d->layout), d->monitor);
    }
    o += (size_t)snprintf(out + o, cap - o, "],");

    /* monitors */
    o += (size_t)snprintf(out + o, cap - o, "\"monitors\":[");
    for (int i = 0; i < g.monitor_count && o < cap; i++) {
        const Monitor *m = &g.monitors[i];

        /* The device name is what a monitor rule is written against, and the
         * mode is what one asks for — so a script (or a person) can read both
         * here instead of guessing. `refresh` and `hdr` come from the display
         * itself rather than from anything cached, so they stay right when the
         * user changes them outside mshell. */
        json_escape(m->device, esc, sizeof esc);
        DisplayMode mode = {0};
        display_current_mode(m->device, &mode);
        int hdr = display_hdr_state(m->device);

        o += (size_t)snprintf(out + o, cap - o,
                 "%s{\"index\":%d,\"device\":\"%s\",\"x\":%ld,\"y\":%ld,"
                 "\"width\":%ld,\"height\":%ld,\"dpi\":%u,\"refresh\":%d,"
                 "\"hdr\":%s,\"focused\":%s}",
                 i ? "," : "", i, esc,
                 (long)m->full.left, (long)m->full.top,
                 (long)(m->full.right - m->full.left),
                 (long)(m->full.bottom - m->full.top),
                 monitor_dpi(i), mode.refresh,
                 hdr == HDR_UNSUPPORTED ? "null" : hdr == HDR_ON ? "true"
                                                                 : "false",
                 i == g.focused_monitor ? "true" : "false");
    }
    o += (size_t)snprintf(out + o, cap - o, "],");

    /* focused window */
    HWND f = desktop_get_focused();
    if (f && IsWindow(f)) {
        wchar_t title[256] = {0};
        GetWindowTextW(f, title, 256);
        json_escape(title, esc, sizeof esc);
        o += (size_t)snprintf(out + o, cap - o, "\"focused\":\"%s\"}", esc);
    } else {
        o += (size_t)snprintf(out + o, cap - o, "\"focused\":null}");
    }

    if (o >= cap) out[cap - 1] = '\0';
}

/* ===========================================================================
 * Server: run one command. MAIN THREAD ONLY (see the file header).
 * =========================================================================== */
void ipc_handle_request(void *req_ptr) {
    IpcRequest *r = (IpcRequest *)req_ptr;
    if (!r) return;

    /* Whatever happens below, the pipe thread is waiting on r->done and must be
     * released — including on the early-return paths. One place to get right. */
    #define IPC_DONE() do { if (r->done) SetEvent(r->done); } while (0)

    /* Split "<action> [argument]" — the argument keeps its spaces, since a
     * desktop name or a spawn command may contain them. */
    wchar_t  verb[64] = {0};
    const wchar_t *arg = NULL;
    {
        const wchar_t *p = r->cmd;
        while (*p == L' ') p++;
        const wchar_t *start = p;
        while (*p && *p != L' ') p++;
        size_t n = (size_t)(p - start);
        if (n >= 64) n = 63;
        memcpy(verb, start, n * sizeof(wchar_t));
        verb[n] = L'\0';
        while (*p == L' ') p++;
        if (*p) arg = p;
    }

    if (!verb[0]) {
        snprintf(r->reply, IPC_REPLY_MAX, "error: empty command");
        IPC_DONE();
        return;
    }

    /* query — the read-only half of the surface */
    if (_wcsicmp(verb, L"query") == 0 || _wcsicmp(verb, L"state") == 0) {
        ipc_build_state(r->reply, IPC_REPLY_MAX);
        IPC_DONE();
        return;
    }

    /* Everything else is an ACTION, by the same names the config uses. Reusing
     * that table rather than inventing a second vocabulary means the two can
     * never drift, and it keeps the surface to exactly what a keybinding can
     * already do — which is the property Phase 3's privilege split needs. */
    char verb_u8[64];
    WideCharToMultiByte(CP_UTF8, 0, verb, -1, verb_u8, 64, NULL, NULL);

    Action action = action_name_to_enum(verb_u8);
    if (action == ACTION_NONE) {
        snprintf(r->reply, IPC_REPLY_MAX,
                 "error: unknown command '%s' (try an action name, or 'query')",
                 verb_u8);
        IPC_DONE();
        return;
    }

    execute_action(action, arg ? _wtoi(arg) : 0, arg, NULL, NULL);
    snprintf(r->reply, IPC_REPLY_MAX, "ok");
    IPC_DONE();
    #undef IPC_DONE
}

/* ===========================================================================
 * Server: the pipe thread
 * =========================================================================== */
static DWORD WINAPI ipc_thread_proc(LPVOID param) {
    (void)param;

    wchar_t name[MAX_PATH];
    ipc_pipe_name(name, MAX_PATH);

    PSECURITY_DESCRIPTOR sd = pipe_sd_for_current_user(NULL, 0);
    if (!sd) {
        log_err(L"ipc: could not build the pipe's security descriptor — "
                L"refusing to create an unrestricted pipe. --msg is unavailable.");
        return 1;
    }

    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };

    /* FILE_FLAG_FIRST_PIPE_INSTANCE: fail loudly if this name is already taken
     * rather than quietly adding an instance beside somebody else's. Anything
     * holding FILE_CREATE_PIPE_INSTANCE on an existing pipe can add one, and a
     * squatter that got there first would take our clients' connections.
     * PIPE_REJECT_REMOTE_CLIENTS: a named pipe is otherwise reachable over SMB
     * as \\host\pipe\mshell-1. Nothing here should ever answer the network. */
    for (;;) {
        if (WaitForSingleObject(g_ipc_stop, 0) == WAIT_OBJECT_0) break;

        HANDLE pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            IPC_REPLY_MAX, IPC_CMD_MAX * sizeof(wchar_t), 0, &sa);

        if (pipe == INVALID_HANDLE_VALUE) {
            log_err(L"ipc: CreateNamedPipe failed: %lu", GetLastError());
            break;
        }

        /* Blocking accept. kb_shutdown-style teardown wakes it by connecting
         * to its own pipe (see ipc_stop). */
        BOOL connected = ConnectNamedPipe(pipe, NULL) ||
                         GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) { CloseHandle(pipe); continue; }

        if (WaitForSingleObject(g_ipc_stop, 0) == WAIT_OBJECT_0) {
            CloseHandle(pipe);
            break;
        }

        IpcRequest *req = (IpcRequest *)calloc(1, sizeof *req);
        if (!req) { CloseHandle(pipe); continue; }

        DWORD read = 0;
        if (ReadFile(pipe, req->cmd, (IPC_CMD_MAX - 1) * sizeof(wchar_t),
                     &read, NULL) && read) {
            req->cmd[read / sizeof(wchar_t)] = L'\0';

            req->done = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (req->done && g.message_window) {
                PostMessageW(g.message_window, WM_MSHELL_IPC, 0, (LPARAM)req);
                /* Bounded: if the main thread is wedged, the client gets an
                 * error instead of hanging forever. */
                if (WaitForSingleObject(req->done, IPC_WAIT_MS) != WAIT_OBJECT_0)
                    snprintf(req->reply, IPC_REPLY_MAX,
                             "error: mshell did not respond within %d ms",
                             IPC_WAIT_MS);
            } else {
                snprintf(req->reply, IPC_REPLY_MAX, "error: mshell is not ready");
            }

            DWORD written = 0;
            WriteFile(pipe, req->reply, (DWORD)strlen(req->reply),
                      &written, NULL);
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (req->done) CloseHandle(req->done);
        free(req);
    }

    LocalFree(sd);
    return 0;
}

void ipc_start(void) {
    if (g_ipc_running) return;

    g_ipc_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ipc_stop) return;

    g_ipc_thread = CreateThread(NULL, 0, ipc_thread_proc, NULL, 0, NULL);
    if (!g_ipc_thread) {
        log_err(L"ipc: CreateThread failed: %lu — --msg is unavailable",
                GetLastError());
        CloseHandle(g_ipc_stop);
        g_ipc_stop = NULL;
        return;
    }
    g_ipc_running = true;

    wchar_t name[MAX_PATH];
    ipc_pipe_name(name, MAX_PATH);
    log_w(L"ipc: listening on %ls", name);
}

void ipc_stop(void) {
    if (!g_ipc_running) return;

    SetEvent(g_ipc_stop);

    /* The thread is parked in ConnectNamedPipe; a throwaway connection wakes
     * it so it can notice the stop event and exit. */
    wchar_t name[MAX_PATH];
    ipc_pipe_name(name, MAX_PATH);
    HANDLE poke = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);

    if (WaitForSingleObject(g_ipc_thread, 2000) != WAIT_OBJECT_0)
        log_w(L"ipc: server thread did not exit; leaking its handle");
    else
        CloseHandle(g_ipc_thread);

    CloseHandle(g_ipc_stop);
    g_ipc_thread  = NULL;
    g_ipc_stop    = NULL;
    g_ipc_running = false;
}

/* ===========================================================================
 * Client
 *
 * mshell is a GUI-subsystem binary, so it has no console of its own and a bare
 * printf goes nowhere. Attaching to the parent console is what makes
 * `mshell.exe --query` usable from a terminal; when there is no parent console
 * (double-clicked) the output is simply dropped, which is the best available
 * outcome and better than popping a message box.
 * =========================================================================== */
void console_print(const char *s) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(out, s, (DWORD)strlen(s), &written, NULL);
        WriteFile(out, "\r\n", 2, &written, NULL);
    }
    FreeConsole();
}

/* Send one command to the running shell and print its reply. */
static int ipc_client_send(const wchar_t *cmd) {
    wchar_t name[MAX_PATH];
    ipc_pipe_name(name, MAX_PATH);

    if (!WaitNamedPipeW(name, 2000)) {
        console_print("error: no mshell is running in this session");
        return 1;
    }

    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        console_print("error: could not connect to mshell");
        return 1;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, NULL, NULL);

    DWORD written = 0;
    WriteFile(pipe, cmd, (DWORD)(wcslen(cmd) * sizeof(wchar_t)), &written, NULL);

    char  reply[IPC_REPLY_MAX] = {0};
    DWORD read = 0;
    if (ReadFile(pipe, reply, IPC_REPLY_MAX - 1, &read, NULL) && read) {
        reply[read] = '\0';
        console_print(reply);
    }
    CloseHandle(pipe);

    return (strncmp(reply, "error:", 6) == 0) ? 1 : 0;
}

/* Called at the very top of WinMain. True if this invocation was a client
 * command, in which case *exit_code is set and the shell must not start. */
bool ipc_client_try(int *exit_code) {
    int      argc = 0;
    LPWSTR  *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;

    bool handled = false;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--msg") == 0 && i + 1 < argc) {
            *exit_code = ipc_client_send(argv[i + 1]);
            handled = true;
            break;
        }
        if (wcscmp(argv[i], L"--query") == 0) {
            *exit_code = ipc_client_send(L"query");
            handled = true;
            break;
        }
        if (wcscmp(argv[i], L"--msg") == 0) {
            console_print("error: --msg needs a command, e.g. "
                         "--msg \"switch_desktop web\"");
            *exit_code = 1;
            handled = true;
            break;
        }
    }

    LocalFree(argv);
    return handled;
}
